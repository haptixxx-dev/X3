# EXECUTION SPEC — Phase 1b: Replace the GL-shaped interfaces with a Vulkan-native resource layer

**Scope:** `X3/src/Renderer/I*`, `X3/src/Platform/Vulkan/*`, and every call site in `Renderer.cpp`, `RenderLayer.cpp`, `X3-Editor`, `X3-Runtime`.
**Preconditions:** Phase 1a is done — `X3/src/Platform/OpenGL/` deleted, `X3_USE_OPENGL` and all `#ifdef X3_USE_OPENGL` branches removed, `X3_GRAPHICS_API` option dropped. This spec assumes those `#ifdef`s no longer exist and does not re-list them.
**Non-goals:** no portable RHI, no virtual dispatch, no `I*` factories, no second backend. `VkStructs` appear in the interface deliberately. Do not design for Slang (Phase 3); do keep the descriptor tables in one place so codegen can replace them later.
**Build note:** `X3/CMakeLists.txt:47` globs `src/*.cpp` recursively, so new `.cpp` files need no CMake edit. Line 50-54 filters `Platform/OpenGL` — that filter goes away in 1a.
**You cannot compile on the dev machine** (vulkan-headers missing). Every step below must be reasoned through by reading; the ordering in §6 is designed so that each step is independently reviewable.

---

## 1. Inventory of GL-isms, with what each forces the Vulkan side to do wrong

### 1.1 `Bind()` / `Unbind()`

| Declaration | Vulkan implementation |
|---|---|
| `IComputeShader::Bind/Unbind` — `Renderer/IComputeShader.h:13-14` | `VulkanComputeShader.cpp:89-97` — two empty functions, comment "placeholder for API compatibility with OpenGL" |
| `IUniformBuffer::Bind/Unbind` — `Renderer/IUniformBuffer.h:22-23` | `VulkanUniformBuffer.cpp:51-57` / `:59-62` |
| `IShaderStorageBuffer::Bind/Unbind` — `Renderer/IShaderStorageBuffer.h:25-26` | `VulkanShaderStorageBuffer.cpp:52-58` / `:60-63` |

**What it forces wrong:** `Bind()` on the two buffer classes is not a no-op — it is the *only* path by which a buffer ever reaches a descriptor set. `VulkanUniformBuffer::Bind()` calls `context->registerUniformBuffer(m_BindingPoint, m_Buffer, m_Size)` (`VulkanUniformBuffer.cpp:55`) and `VulkanShaderStorageBuffer::Bind()` calls `registerStorageBuffer` (`VulkanShaderStorageBuffer.cpp:56`). Descriptor membership is therefore a side effect of a GL verb, invisible at the call site, ordered by whoever last called `Bind()`. Miss a `Bind()` and the binding silently keeps whatever `VkBuffer` was registered last frame — including a destroyed one. `Unbind()` does *not* unregister, so the registry can only ever grow stale.

### 1.2 `GetID() → int` / `GetID() → uint32_t`

| Declaration | Vulkan implementation |
|---|---|
| `IImage2D::GetID() const → int` — `Renderer/IImage2D.h:19` | `VulkanImage2D.h:16` returns `m_ID`, a `static int s_NextID` counter (`VulkanImage2D.cpp:8, 18`) |
| `ITexture2D::GetID() const → int` — `Renderer/ITexture2D.h:13` | `VulkanTexture2D.h:16`: `return static_cast<int>(reinterpret_cast<uintptr_t>(m_Image));` |
| `IComputeShader::GetID() → uint32_t` — `Renderer/IComputeShader.h:21` | `VulkanComputeShader.h:34` returns `m_ShaderID`, a counter (`VulkanComputeShader.h:57`) |

**What it forces wrong:**
- `VulkanTexture2D.h:16` truncates a 64-bit `VkImage` to 32 bits. On any driver that hands out handles above 2^32 (common — they are frequently pointers) two distinct textures collide or the value goes negative. Nothing currently reads it, but it is a live trap.
- The compute-shader ID is used as a *validity* test: `Renderer.cpp:32` (`it->second->GetID() != 0`) and `Renderer.cpp:44` (`shader->GetID() == 0`). That is `glCreateProgram` semantics — a compute shader whose `vkCreateComputePipelines` failed still has a nonzero counter ID, so `GetOrLoadShader` reports success for a broken pipeline (it can't actually reach that state today because `createPipeline()` throws at `VulkanComputeShader.cpp:443`, but the check is meaningless either way).
- `IImage2D::GetID()` is the cache key in `ViewportPanel.cpp:49` / `:99` for the ImGui descriptor. It works only because it is a counter, not a handle.

### 1.3 `ChangeImageUnit()` / `ChangeTextureUnit()` and the global binding registry

| Declaration | Vulkan implementation |
|---|---|
| `IImage2D::ChangeImageUnit(int)` — `Renderer/IImage2D.h:18` | `VulkanImage2D.cpp:49-56` → `context->registerStorageImage(imageUnit, m_ImageView)` |
| `ITexture2D::ChangeTextureUnit(int)` — `Renderer/ITexture2D.h:12` | `VulkanTexture2D.cpp:36-43` → `context->registerSampledImage(textureUnit, m_ImageView, m_Sampler)` |
| Registry — `VulkanContext.h:39-52`, implementations `VulkanContext.cpp:810-824`, storage `VulkanContext.h:175-178` | Four `std::unordered_map<uint32_t, …>` keyed on **binding number only** |
| Consumer — `VulkanComputeShader.cpp:194-279` | For each `(set, binding)` in the hand-written table, looks up `boundX.find(binding.binding)` — **`setNum` is never part of the key** |

**What it forces wrong:** correctness here is coincidental. The shader layout (`X3/res/shaders/PathTracing.comp:83-129`) is:

```
set 0 binding 0  storage image   rayTracingTexture
set 0 binding 1  combined sampler skyboxTexture
set 1 binding 0  UBO CameraUBO
set 1 binding 1  UBO SettingsUBO
set 2 binding 0..6  SSBOs
```

It happens that within each *descriptor type* the binding numbers are unique across sets: storage images only ever use binding 0, samplers only binding 1, UBOs only 0-1 (all in set 1), SSBOs only 0-6 (all in set 2). Add any second storage image, or a UBO in set 2, and the wrong resource is written into the wrong set with no diagnostic. `VulkanComputeShader.cpp:276` even has the "no resource bound" warning commented out, so a missing binding is completely silent.

Second consequence: `registerStorageImage(0, view)` is called every frame from `Renderer.cpp:209` with the *current write* frame's view, and `VulkanImage2D`'s destructor (`VulkanImage2D.cpp:35-47`) never unregisters. On a resolution change (`Renderer.cpp:201-204`) both images are destroyed and the registry holds a dangling `VkImageView` until the next `ChangeImageUnit`.

Third: the registry is global, so two compute pipelines can never have different resources on the same binding number.

### 1.4 `AddData(offset, size, data)`

| Declaration | Vulkan implementation |
|---|---|
| `IUniformBuffer::AddData` — `Renderer/IUniformBuffer.h:25` | `VulkanUniformBuffer.cpp:68-74` — `memcpy` into `m_MappedData` |
| `IShaderStorageBuffer::AddData` — `Renderer/IShaderStorageBuffer.h:30` | `VulkanShaderStorageBuffer.cpp:69-75` — same |

**What it forces wrong:** `glBufferSubData` semantics require a *single* persistently-mapped allocation, which is exactly what both classes create (`VulkanUniformBuffer.cpp:23-37`, `VulkanShaderStorageBuffer.cpp:23-38`, both `VMA_ALLOCATION_CREATE_MAPPED_BIT`, `m_MappedData = allocationInfo.pMappedData`). `Renderer::SetupGPUResources` then memcpys frame N+1's camera matrix, settings, transforms, materials and lights straight over the bytes the GPU is reading for frame N — up to `MAX_FRAMES_IN_FLIGHT = 2` (`VulkanContext.h:146`) command buffers are in flight and only the fence in `beginFrame` (`VulkanContext.cpp:346`) gates anything. There is no way to fix this without changing the API: an offset-into-one-allocation signature cannot express "write into this frame's slot."

Additionally, buffer *recreation* on size change (`Renderer.cpp:258, 270, 282, 295, 318, 332, 346`) reassigns a `shared_ptr`, which runs `~VulkanShaderStorageBuffer` → `vmaDestroyBuffer` (`VulkanShaderStorageBuffer.cpp:42-50`) immediately, while the previous frame's command buffer may still reference that `VkBuffer`.

### 1.5 `Image2DType` (`LR_READ` / `LR_WRITE` / `LR_READ_WRITE`)

Declared `Renderer/IImage2D.h:8-12`; passed through `IImage2D::Create` (`Renderer/IImage2D.h:16`) and `IImage2D.cpp:14-18`. Stored at `VulkanImage2D.h:41` and exposed by `getImageType()` (`VulkanImage2D.h:23`).

**What it forces wrong:** `VulkanImage2D::createImage` (`VulkanImage2D.cpp:58-152`) never reads `m_ImageType`. Every image gets `VK_IMAGE_USAGE_STORAGE_BIT | SAMPLED_BIT | TRANSFER_SRC_BIT | TRANSFER_DST_BIT` (`VulkanImage2D.cpp:77`) and `VK_FORMAT_R32G32B32A32_SFLOAT` (`:74`, `:163`) regardless. `getImageType()` has zero callers. It is a GLSL access-qualifier concept with no Vulkan meaning and it hides the field that actually matters (`VkImageUsageFlags`) from the caller.

### 1.6 `IRenderingContext::swapBuffers()`

Declared `Renderer/IRenderingContext.h:9`; the only implementation is `VulkanContext::swapBuffers` (`VulkanContext.h:21`, `VulkanContext.cpp:639-661`). Reached from `Application::run` → `IWindow::swapBuffers` (`Core/IWindow.h:37`) → `GLFWWindowIMPL::swapBuffers` (`GLFWWindow.cpp:89-91`) at `application.cpp:69`.

**What it forces wrong:** one GL verb has to mean submit + present + acquire + begin-render-pass. `swapBuffers()` calls `endFrame()` then `beginFrame()` (`VulkanContext.cpp:649-651`), so the frame for iteration N+1 is opened at the *end* of iteration N. That inversion requires the `m_FirstFrame` special case (`VulkanContext.h:168`, `VulkanContext.cpp:42`, `:644-647`) plus a second escape hatch, `ensureFrameStarted()` (`VulkanContext.h:64`, `VulkanContext.cpp:492-500`), which `ImGuiContext::EndFrame` has to call defensively at `ImGuiContext.cpp:278`. `beginFrame` also unconditionally begins the clear render pass (`VulkanContext.cpp:380-393`) which the runtime path immediately throws away — `blitImageToSwapchain` ends it at `VulkanContext.cpp:832-835` and does its own `vkCmdClearColorImage` at `:887`. And the retry-on-failure at `VulkanContext.cpp:651-659` calls `beginFrame()` twice in a row, which on the `VK_ERROR_OUT_OF_DATE_KHR` path (`:354-357`) recreates the swapchain twice.

### 1.7 Dead surface — delete, do not port

Verified zero callers outside the interface/implementation hierarchy:

- `IShaderStorageBuffer::ReadData` (`Renderer/IShaderStorageBuffer.h:32`, `VulkanShaderStorageBuffer.cpp:77-93`). *This deletes Phase 1c item 3 outright* — the "a memory barrier should be issued before this" comment at `VulkanShaderStorageBuffer.cpp:89-90` followed by an unsynchronized `memcpy` is unreachable. If a readback is needed later, add it as a fenced staging copy, not as a mapped read.
- `IUniformBuffer::SetBindingPoint` / `IShaderStorageBuffer::SetBindingPoint` (`Renderer/IUniformBuffer.h:24`, `Renderer/IShaderStorageBuffer.h:28`).
- `IComputeShader::getWorkGroupSizes` / `getFilePath` (`Renderer/IComputeShader.h:22-23`).
- `IRendererAPI::SetViewportSize` (`Renderer/IRendererAPI.h:20`, `VulkanRendererAPI.cpp:19-50`) — records `vkCmdSetViewport`/`vkCmdSetScissor` for a graphics pipeline the engine does not have.
- `VulkanRendererAPI::GetClearColor` (`VulkanRendererAPI.h:15`).
- `IRendererAPI::Clear` (`Renderer/IRendererAPI.h:19`) — called at `application.cpp:55/57`, stores into `m_ClearColor` (`VulkanRendererAPI.cpp:16`) which nothing reads; `beginFrame` hardcodes black at `VulkanContext.cpp:388`.
- `IRendererAPI::API` enum + `s_API` (`Renderer/IRendererAPI.h:11-15, 23-27`) and its consumer `ProjectManager.cpp:113-119` — the 1a "API-selection trap". `RenderSettings::rendererAPI` (`RenderSettings.h:14-17, 28-32, 45, 61`) goes with it.

⇒ **`IRendererAPI`, `VulkanRendererAPI`, and `Application::_RendererAPI` are entirely dead once `Clear` is dropped. Delete all three.**

---

## 2. Proposed new API surface

All new code lives in `X3/src/Platform/Vulkan/`. New class names deliberately do **not** collide with the old ones (`VulkanBuffer` vs `VulkanUniformBuffer`, `VulkanImage` vs `VulkanImage2D`, `VulkanTexture` vs `VulkanTexture2D`, `VulkanComputePipeline` vs `VulkanComputeShader`) so old and new can coexist during the incremental migration in §6.

Conventions used throughout: every resource class is **move-only** (deleted copy ctor/assign), default-constructible into an invalid state, and RAII-owning. There are no `shared_ptr`s in the layer. There is no virtual dispatch anywhere.

**Vulkan version note:** `VulkanContext` requires 1.2 (`VulkanContext.cpp:52`, `:78`, `:139`). The sketches below use `VkAccessFlags`/`VkPipelineStageFlags` and `vkCmdPipelineBarrier` (1.0 core), **not** `VkAccessFlags2`/synchronization2. Do not bump to 1.3 as part of this phase.

### 2.0 `X3/src/Platform/Vulkan/VulkanCommon.h` (new)

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <cstddef>

namespace X3 {

// MUST equal VulkanContext::MAX_FRAMES_IN_FLIGHT (VulkanContext.h:146).
// A static_assert in VulkanContext.cpp enforces this.
inline constexpr uint32_t FRAMES_IN_FLIGHT = 2;

// Logs file/line and throws std::runtime_error on non-VK_SUCCESS.
void VkCheck(VkResult result, const char* expr, const char* file, int line);
#define VK_CHECK(x) ::X3::VkCheck((x), #x, __FILE__, __LINE__)

} // namespace X3
```

### 2.1 `X3/src/Platform/Vulkan/FrameContext.h` (new)

This is the single object that threads the frame index. It is a non-owning view, valid only between `beginFrame()` and `endFrame()`, and is never stored by a resource.

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"

namespace X3 {

class VulkanContext;

class FrameContext {
public:
    // 0 .. FRAMES_IN_FLIGHT-1. Indexes every per-frame ring in the engine.
    uint32_t        index()  const { return m_Index; }
    // The primary command buffer for this frame. Already in the recording state.
    VkCommandBuffer cmd()    const { return m_Cmd; }
    // Monotonic frame counter since context init. Used by the deletion queue.
    uint64_t        number() const { return m_Number; }
    VulkanContext&  context() const { return *m_Context; }

private:
    friend class VulkanContext;
    VulkanContext*  m_Context = nullptr;
    VkCommandBuffer m_Cmd     = VK_NULL_HANDLE;
    uint32_t        m_Index   = 0;
    uint64_t        m_Number  = 0;
};

} // namespace X3
```

**Ownership:** owned by `VulkanContext` as a single member. `beginFrame()` returns `const FrameContext*`; callers pass `const FrameContext&` down. Never copy it into a member.

### 2.2 `X3/src/Platform/Vulkan/VulkanBuffer.h` (new) — static buffer + per-frame ring

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/FrameContext.h"
#include <type_traits>

namespace X3 {

class VulkanContext;

enum class BufferKind : uint8_t { Uniform, Storage };

// ---------------------------------------------------------------------------
// Device-local buffer whose contents change rarely (mesh / BVH node / index).
// Uploads are staged through the context's per-frame staging arena and recorded
// into the frame command buffer, so they are ordered with the dispatch that
// reads them and need no queue wait.
// ---------------------------------------------------------------------------
class VulkanBuffer {
public:
    VulkanBuffer() = default;
    VulkanBuffer(VulkanContext& ctx, BufferKind kind, VkDeviceSize size, const char* debugName);
    ~VulkanBuffer();

    VulkanBuffer(VulkanBuffer&&) noexcept;
    VulkanBuffer& operator=(VulkanBuffer&&) noexcept;
    VulkanBuffer(const VulkanBuffer&)            = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    // Grows (never shrinks) to at least `size`. If reallocation happens, the old
    // VkBuffer/VmaAllocation go to ctx.deferDestroy() and are freed only after
    // FRAMES_IN_FLIGHT frames have retired. Returns true if reallocated, which
    // is the caller's signal that any descriptor referencing this buffer must be
    // rewritten. Growth is geometric (next power of two).
    bool ensureCapacity(const FrameContext& frame, VkDeviceSize size);

    // Records vkCmdCopyBuffer from the frame staging arena into this buffer,
    // plus a TRANSFER_WRITE -> SHADER_READ buffer barrier. Asserts
    // dstOffset + size <= capacity().
    void upload(const FrameContext& frame, const void* data,
                VkDeviceSize size, VkDeviceSize dstOffset = 0);

    VkBuffer     handle()   const { return m_Buffer; }
    VkDeviceSize capacity() const { return m_Capacity; }
    bool         valid()    const { return m_Buffer != VK_NULL_HANDLE; }

    VkDescriptorBufferInfo descriptor(VkDeviceSize offset = 0,
                                      VkDeviceSize range  = VK_WHOLE_SIZE) const;

private:
    VulkanContext* m_Ctx        = nullptr;
    VkBuffer       m_Buffer     = VK_NULL_HANDLE;
    VmaAllocation  m_Allocation = VK_NULL_HANDLE;
    VkDeviceSize   m_Capacity   = 0;
    BufferKind     m_Kind       = BufferKind::Storage;
    const char*    m_DebugName  = nullptr;
};

// ---------------------------------------------------------------------------
// Host-visible, persistently mapped, FRAMES_IN_FLIGHT slots inside ONE VkBuffer.
// Slot i occupies [i*stride(), i*stride() + sizePerFrame()).
// Writing slot frame.index() is safe because VulkanContext::beginFrame() waited
// on that frame's fence before handing out the FrameContext.
// ---------------------------------------------------------------------------
class VulkanRingBuffer {
public:
    VulkanRingBuffer() = default;
    VulkanRingBuffer(VulkanContext& ctx, BufferKind kind,
                     VkDeviceSize sizePerFrame, const char* debugName);
    ~VulkanRingBuffer();

    VulkanRingBuffer(VulkanRingBuffer&&) noexcept;
    VulkanRingBuffer& operator=(VulkanRingBuffer&&) noexcept;
    VulkanRingBuffer(const VulkanRingBuffer&)            = delete;
    VulkanRingBuffer& operator=(const VulkanRingBuffer&) = delete;

    // Same contract as VulkanBuffer::ensureCapacity, but the argument is the
    // per-frame slot size; the underlying allocation is FRAMES_IN_FLIGHT * stride.
    bool ensureCapacity(const FrameContext& frame, VkDeviceSize sizePerFrame);

    // memcpy into slot frame.index() + vmaFlushAllocation for that range
    // (a no-op on coherent memory). Asserts offsetInSlot+size <= sizePerFrame().
    void write(const FrameContext& frame, const void* data,
               VkDeviceSize size, VkDeviceSize offsetInSlot = 0);

    template <typename T>
    void writeStruct(const FrameContext& frame, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        write(frame, &value, sizeof(T));
    }

    // Pointer to the start of slot frame.index(), for scatter writes.
    // Caller must call flush(frame) afterwards. Never null on a valid buffer.
    std::byte* mapped(const FrameContext& frame);
    void       flush(const FrameContext& frame);

    // Descriptor aimed at frame.index()'s slot. offset = index*stride,
    // range = sizePerFrame (NOT VK_WHOLE_SIZE).
    VkDescriptorBufferInfo descriptor(const FrameContext& frame) const;

    VkBuffer     handle()       const { return m_Buffer; }
    VkDeviceSize sizePerFrame() const { return m_SizePerFrame; }
    VkDeviceSize stride()       const { return m_Stride; }
    bool         valid()        const { return m_Buffer != VK_NULL_HANDLE; }

private:
    VulkanContext* m_Ctx          = nullptr;
    VkBuffer       m_Buffer       = VK_NULL_HANDLE;
    VmaAllocation  m_Allocation   = VK_NULL_HANDLE;
    std::byte*     m_Mapped       = nullptr;
    VkDeviceSize   m_SizePerFrame = 0;
    VkDeviceSize   m_Stride       = 0;   // sizePerFrame rounded up (see §3)
    BufferKind     m_Kind         = BufferKind::Storage;
    const char*    m_DebugName    = nullptr;
};

} // namespace X3
```

**Implementation notes for `VulkanRingBuffer`:**
- Usage flags: `Uniform → VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`, `Storage → VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`.
- VMA flags: `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT` (same as `VulkanUniformBuffer.cpp:25-26`). **Do not** use `HOST_ACCESS_RANDOM` (`VulkanShaderStorageBuffer.cpp:26`) — it existed only for `ReadData`, which is being deleted.
- Always call `vmaFlushAllocation(allocator, alloc, slotOffset, size)` in `write()`/`flush()`. VMA no-ops on coherent memory; without it the non-coherent case is silently broken.
- `sizePerFrame == 0` must be legal and must allocate a minimum of one alignment unit, so a zero-element SSBO still yields a valid descriptor (see §1.4 / the light-buffer hole at `Renderer.cpp:291-301`).

**Ownership:** `Renderer` owns all `VulkanBuffer`/`VulkanRingBuffer` by value. Destruction happens at `Renderer` teardown, after `vkDeviceWaitIdle`.

### 2.3 `X3/src/Platform/Vulkan/VulkanImage.h` (new) — replaces `IImage2D`/`VulkanImage2D`

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/FrameContext.h"
#include <glm/glm.hpp>

namespace X3 {

class VulkanContext;

struct ImageDesc {
    uint32_t          width      = 0;
    uint32_t          height     = 0;
    VkFormat          format     = VK_FORMAT_R32G32B32A32_SFLOAT;
    // Explicit, replaces Image2DType entirely.
    VkImageUsageFlags usage      = 0;
    uint32_t          mipLevels  = 1;
    const char*       debugName  = nullptr;
};

// Owns VkImage + VmaAllocation + one default VkImageView.
// Tracks its own layout and last access/stage so barriers are derivable.
class VulkanImage {
public:
    VulkanImage() = default;
    VulkanImage(VulkanContext& ctx, const ImageDesc& desc);
    ~VulkanImage();

    VulkanImage(VulkanImage&&) noexcept;
    VulkanImage& operator=(VulkanImage&&) noexcept;
    VulkanImage(const VulkanImage&)            = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;

    // Destroys via ctx.deferDestroy() and recreates. Any descriptor referencing
    // this image must be rewritten afterwards; id() changes.
    void recreate(const FrameContext& frame, const ImageDesc& desc);

    // Records an image memory barrier into frame.cmd() if the layout differs,
    // using the tracked (oldLayout, lastAccess, lastStage) as the source.
    // Idempotent: a no-op if already in newLayout with a compatible access mask.
    void transition(const FrameContext& frame, VkImageLayout newLayout,
                    VkAccessFlags dstAccess, VkPipelineStageFlags dstStage);

    // Same-layout execution+memory barrier. REQUIRED for the accumulation
    // read-modify-write across frames (PathTracing.comp:520-521 does
    // imageLoad then imageStore on the same image), which transition() would
    // skip because the layout is unchanged.
    void barrier(const FrameContext& frame,
                 VkAccessFlags dstAccess, VkPipelineStageFlags dstStage);

    VkImage       handle()     const { return m_Image; }
    VkImageView   view()       const { return m_View; }
    VkImageLayout layout()     const { return m_Layout; }
    VkExtent2D    extent()     const { return m_Extent; }
    VkFormat      format()     const { return m_Format; }
    glm::ivec2    dimensions() const { return { int(m_Extent.width), int(m_Extent.height) }; }
    bool          valid()      const { return m_Image != VK_NULL_HANDLE; }

    // Stable 64-bit identity from a process-wide atomic counter.
    // Replaces IImage2D::GetID()->int as a cache key. Never truncates,
    // never a raw handle, never reused after destruction.
    uint64_t id() const { return m_Id; }

    VkDescriptorImageInfo storageDescriptor() const; // { VK_NULL_HANDLE, view, GENERAL }

private:
    VulkanContext*        m_Ctx        = nullptr;
    VkImage               m_Image      = VK_NULL_HANDLE;
    VmaAllocation         m_Allocation = VK_NULL_HANDLE;
    VkImageView           m_View       = VK_NULL_HANDLE;
    VkExtent2D            m_Extent     {};
    VkFormat              m_Format     = VK_FORMAT_UNDEFINED;
    VkImageLayout         m_Layout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags         m_LastAccess = 0;
    VkPipelineStageFlags  m_LastStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    uint64_t              m_Id         = 0;
};

} // namespace X3
```

**Behaviour change vs `VulkanImage2D`:** the constructor must **not** call `beginSingleTimeCommands`/`endSingleTimeCommands` the way `VulkanImage2D.cpp:26-30` does — that path ends in `vkQueueWaitIdle` (`VulkanContext.cpp:530`) and is invoked from inside a recording frame at `Renderer.cpp:202-203`. Instead, leave the image in `VK_IMAGE_LAYOUT_UNDEFINED` and let the first `transition(frame, VK_IMAGE_LAYOUT_GENERAL, …)` in `Renderer::Draw` do the work inside the frame command buffer. `UNDEFINED → GENERAL` is always legal.

The `unsigned char* data` upload path of `VulkanImage2D::createImage` (`VulkanImage2D.cpp:92-152`, the RGBA8→RGBA32F conversion loop) has exactly one caller passing `nullptr` (`Renderer.cpp:202-203`). Do not port it. Storage images are always created empty.

### 2.4 `X3/src/Platform/Vulkan/VulkanTexture.h` (new) — replaces `ITexture2D`/`VulkanTexture2D`

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/FrameContext.h"

namespace X3 {

class VulkanContext;

struct SamplerDesc {
    VkFilter             filter      = VK_FILTER_LINEAR;
    VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    bool operator==(const SamplerDesc&) const = default;
};

struct TextureDesc {
    uint32_t    width     = 0;
    uint32_t    height    = 0;
    VkFormat    format    = VK_FORMAT_R8G8B8A8_SRGB;
    SamplerDesc sampler   {};
    const char* debugName = nullptr;
};

// Immutable sampled texture: image + view + a sampler borrowed from the
// context's sampler cache (NOT owned - do not destroy it).
class VulkanTexture {
public:
    VulkanTexture() = default;
    // `pixels` must be width*height*bytesPerPixel(format) bytes and is copied
    // immediately into the frame staging arena. The upload + the
    // UNDEFINED->TRANSFER_DST->SHADER_READ_ONLY_OPTIMAL transitions are recorded
    // into frame.cmd(), so no queue wait and no mid-frame submit.
    VulkanTexture(VulkanContext& ctx, const FrameContext& frame,
                  const TextureDesc& desc, const void* pixels);
    ~VulkanTexture();

    VulkanTexture(VulkanTexture&&) noexcept;
    VulkanTexture& operator=(VulkanTexture&&) noexcept;
    VulkanTexture(const VulkanTexture&)            = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    VkImageView view()    const { return m_View; }
    VkSampler   sampler() const { return m_Sampler; }
    bool        valid()   const { return m_Image != VK_NULL_HANDLE; }
    uint64_t    id()      const { return m_Id; }

    // { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }
    VkDescriptorImageInfo descriptor() const;

private:
    VulkanContext* m_Ctx        = nullptr;
    VkImage        m_Image      = VK_NULL_HANDLE;
    VmaAllocation  m_Allocation = VK_NULL_HANDLE;
    VkImageView    m_View       = VK_NULL_HANDLE;
    VkSampler      m_Sampler    = VK_NULL_HANDLE;  // borrowed from ctx cache
    uint64_t       m_Id         = 0;
};

} // namespace X3
```

**Bug to fix while porting:** `VulkanTexture2D::transitionImageLayout` sets `destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` for the `TRANSFER_DST → SHADER_READ_ONLY_OPTIMAL` transition (`VulkanTexture2D.cpp:203-207`), but the skybox is sampled from a **compute** shader (`PathTracing.comp:85`). The new class must use `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`.

**Sampler ownership:** the per-texture `vkCreateSampler` at `VulkanTexture2D.cpp:154-179` becomes `VulkanContext::getSampler(const SamplerDesc&)`, backed by a `std::unordered_map<SamplerDesc, VkSampler>` on the context, destroyed in `VulkanContext::cleanup()`.

### 2.5 `X3/src/Platform/Vulkan/VulkanDescriptors.h` (new)

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/FrameContext.h"
#include <array>
#include <span>
#include <vector>

namespace X3 {

class VulkanContext;
class VulkanBuffer;
class VulkanRingBuffer;
class VulkanImage;
class VulkanTexture;

struct DescriptorBindingDesc {
    uint32_t           binding = 0;
    VkDescriptorType   type    = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t           count   = 1;
    VkShaderStageFlags stages  = VK_SHADER_STAGE_COMPUTE_BIT;
};

// Owns one VkDescriptorSetLayout. Keeps the binding table so DescriptorWriter
// can validate completeness. In Phase 3 this table becomes Slang-generated.
class VulkanDescriptorSetLayout {
public:
    VulkanDescriptorSetLayout() = default;
    VulkanDescriptorSetLayout(VulkanContext& ctx,
                              std::span<const DescriptorBindingDesc> bindings);
    ~VulkanDescriptorSetLayout();

    VulkanDescriptorSetLayout(VulkanDescriptorSetLayout&&) noexcept;
    VulkanDescriptorSetLayout& operator=(VulkanDescriptorSetLayout&&) noexcept;
    VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout&) = delete;

    VkDescriptorSetLayout handle() const { return m_Layout; }
    std::span<const DescriptorBindingDesc> bindings() const { return m_Bindings; }
    const DescriptorBindingDesc* find(uint32_t binding) const; // nullptr if absent

private:
    VulkanContext*                     m_Ctx    = nullptr;
    VkDescriptorSetLayout              m_Layout = VK_NULL_HANDLE;
    std::vector<DescriptorBindingDesc> m_Bindings;
};

// FRAMES_IN_FLIGHT descriptor sets sharing one layout. THIS IS THE REPLACEMENT
// FOR THE SINGLE SET AT VulkanComputeShader.cpp:113-118.
// Set i is only ever written while frame i's fence is signalled.
class VulkanDescriptorSetRing {
public:
    VulkanDescriptorSetRing() = default;
    VulkanDescriptorSetRing(VulkanContext& ctx, const VulkanDescriptorSetLayout& layout);
    ~VulkanDescriptorSetRing();   // ctx.deferFreeDescriptorSets(...)

    VulkanDescriptorSetRing(VulkanDescriptorSetRing&&) noexcept;
    VulkanDescriptorSetRing& operator=(VulkanDescriptorSetRing&&) noexcept;
    VulkanDescriptorSetRing(const VulkanDescriptorSetRing&) = delete;

    VkDescriptorSet get(const FrameContext& frame) const { return m_Sets[frame.index()]; }
    bool valid() const { return m_Sets[0] != VK_NULL_HANDLE; }

private:
    VulkanContext*                                  m_Ctx = nullptr;
    std::array<VkDescriptorSet, FRAMES_IN_FLIGHT>   m_Sets{};
};

// Accumulates writes for ONE descriptor set and flushes them in one
// vkUpdateDescriptorSets. Owns its VkDescriptor*Info storage, which is why
// VulkanComputeShader's m_BufferInfos/m_ImageInfos keep-alive maps
// (VulkanComputeShader.h:80-82) disappear.
// Stack-allocate one per set per frame.
class DescriptorWriter {
public:
    // Capacity is fixed at construction: the info vectors are reserve()d and
    // MUST NOT reallocate, because VkWriteDescriptorSet holds raw pointers into
    // them. Every add asserts against overflow.
    DescriptorWriter(VulkanContext& ctx, VkDescriptorSet dst, uint32_t maxWrites = 16);

    DescriptorWriter& uniformBuffer(uint32_t binding, const VulkanRingBuffer&, const FrameContext&);
    DescriptorWriter& uniformBuffer(uint32_t binding, const VulkanBuffer&);
    DescriptorWriter& storageBuffer(uint32_t binding, const VulkanRingBuffer&, const FrameContext&);
    DescriptorWriter& storageBuffer(uint32_t binding, const VulkanBuffer&);
    DescriptorWriter& storageImage (uint32_t binding, const VulkanImage&);
    DescriptorWriter& sampledImage (uint32_t binding, const VulkanTexture&);
    DescriptorWriter& raw(uint32_t binding, VkDescriptorType, const VkDescriptorBufferInfo&);
    DescriptorWriter& raw(uint32_t binding, VkDescriptorType, const VkDescriptorImageInfo&);

    // Debug builds: asserts that every binding in `layout` was written exactly
    // once and that the descriptor types match. Release: skips validation.
    // Then calls vkUpdateDescriptorSets once.
    void flush(const VulkanDescriptorSetLayout& layout);

private:
    VkDevice                            m_Device;
    VkDescriptorSet                     m_Dst;
    std::vector<VkDescriptorBufferInfo> m_BufferInfos;
    std::vector<VkDescriptorImageInfo>  m_ImageInfos;
    std::vector<VkWriteDescriptorSet>   m_Writes;
};

} // namespace X3
```

The "every binding written exactly once" assertion is the whole point — it turns the currently silent failure at `VulkanComputeShader.cpp:273-277` into a hard error, and it is what forces the light-buffer hole (`Renderer.cpp:291-301`, binding 6 skipped when `count == 0`) to be fixed rather than tolerated.

### 2.6 `X3/src/Platform/Vulkan/VulkanComputePipeline.h` (new) — replaces `IComputeShader`/`VulkanComputeShader`

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/VulkanDescriptors.h"
#include "Platform/Vulkan/FrameContext.h"
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace X3 {

class VulkanContext;

struct ComputePipelineDesc {
    // FULL path to the .spv. Unlike VulkanComputeShader::loadShaderFromFile
    // (VulkanComputeShader.cpp:302), ".spv" is NOT appended here.
    std::filesystem::path spirvPath;
    std::string           entryPoint = "main";
    // index == descriptor set number; empty inner vector == empty set.
    std::vector<std::vector<DescriptorBindingDesc>> setLayouts;
    uint32_t              pushConstantSize = 0;  // 0 == none
    const char*           debugName        = nullptr;
};

class VulkanComputePipeline {
public:
    VulkanComputePipeline() = default;
    // Does NOT throw on failure; logs and leaves valid() == false.
    VulkanComputePipeline(VulkanContext& ctx, const ComputePipelineDesc& desc);
    ~VulkanComputePipeline();

    VulkanComputePipeline(VulkanComputePipeline&&) noexcept;
    VulkanComputePipeline& operator=(VulkanComputePipeline&&) noexcept;
    VulkanComputePipeline(const VulkanComputePipeline&) = delete;

    // Replaces IComputeShader::GetID() != 0 as the validity test
    // (Renderer.cpp:32, :44).
    bool     valid()    const { return m_Pipeline != VK_NULL_HANDLE; }
    uint32_t setCount() const { return uint32_t(m_SetLayouts.size()); }
    const VulkanDescriptorSetLayout& setLayout(uint32_t set) const { return m_SetLayouts[set]; }

    // vkCmdBindPipeline + vkCmdBindDescriptorSets(firstSet=0) + vkCmdDispatch.
    // `sets` must be exactly setCount() entries, index == set number, already
    // written for this frame. Group COUNTS, not local sizes.
    // Inserts NO barriers - the caller owns synchronisation.
    void dispatch(const FrameContext& frame,
                  std::span<const VkDescriptorSet> sets,
                  uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) const;

    void pushConstants(const FrameContext& frame, const void* data, uint32_t size) const;

    VkPipeline                   handle() const { return m_Pipeline; }
    VkPipelineLayout             layout() const { return m_PipelineLayout; }
    const std::filesystem::path& path()   const { return m_Path; }

private:
    VulkanContext*                         m_Ctx            = nullptr;
    std::filesystem::path                  m_Path;
    VkShaderModule                         m_Module         = VK_NULL_HANDLE;
    VkPipeline                             m_Pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout                       m_PipelineLayout = VK_NULL_HANDLE;
    std::vector<VulkanDescriptorSetLayout> m_SetLayouts;
    uint32_t                               m_PushConstantSize = 0;
};

} // namespace X3
```

**Removed vs `VulkanComputeShader`:** `Bind()`/`Unbind()`; the `m_WorkGroupSizes` member and `setWorkGroupSizes` (`VulkanComputeShader.h:39`) — misnamed, it actually held *group counts* (`Renderer.cpp:372-376`) and is now a dispatch parameter; `updateDescriptorSets()` and the registry lookup (`VulkanComputeShader.cpp:182-284`); the keep-alive info maps (`VulkanComputeShader.h:80-82`); the post-dispatch memory barrier (`VulkanComputeShader.cpp:139-154`) which is now the caller's job; the hardcoded three-set table in the constructor (`VulkanComputeShader.cpp:16-42`), which moves to `Renderer` as a single named constant (see §5).

**Ownership:** `Renderer` owns pipelines by value in `std::unordered_map<ShaderType, VulkanComputePipeline>`. `GetOrLoadShader` returns a raw `VulkanComputePipeline*` into that map — stable across rehash because `unordered_map` does not move elements.

### 2.7 `VulkanContext` — modifications (`X3/src/Platform/Vulkan/VulkanContext.h/.cpp`)

Delete: `#include "Renderer/IRenderingContext.h"` and `: public IRenderingContext` (`VulkanContext.h:10, 15`); `void swapBuffers() override` (`.h:21`, `.cpp:639-661`); `ensureFrameStarted()` (`.h:64`, `.cpp:492-500`); `m_FirstFrame` (`.h:168`, `.cpp:42, 644-647`); the entire registry block (`.h:38-52`, `.h:174-178`, `.cpp:810-824`); `beginRenderPass()` (`.h:72`, `.cpp:535-576`) — superseded by `beginOverlayRenderPass()`, which duplicates it.

Add / change:

```cpp
class VulkanContext {                              // no base class
public:
    VulkanContext(GLFWwindow* window);
    ~VulkanContext();
    void init();                                   // no longer sets m_FirstFrame
    static void setWindowHints();
    static VulkanContext* Get() { return s_Instance; }

    // ---- Frame lifecycle. Replaces swapBuffers(). ----------------------------
    // Waits on the fence for frame m_CurrentFrame, drains the deletion queue and
    // resets the staging arena for that frame, acquires a swapchain image, resets
    // and begins m_CommandBuffers[m_CurrentFrame]. Does NOT begin a render pass.
    // Returns nullptr if the swapchain was out of date (already recreated;
    // caller must skip the whole frame). No retry loop.
    const FrameContext* beginFrame();

    // Ends any active render pass; ensures the acquired swapchain image is in
    // PRESENT_SRC_KHR (transitioning + clearing it if nothing rendered);
    // ends the command buffer; vkQueueSubmit with the acquire/render semaphores
    // and the frame fence. Precondition: a successful beginFrame().
    void endFrame();

    // vkQueuePresentKHR; on OUT_OF_DATE/SUBOPTIMAL calls recreateSwapchain();
    // then advances m_CurrentFrame, m_CurrentSemaphoreIndex and m_FrameNumber.
    void present();

    // Non-null only between beginFrame() and endFrame().
    const FrameContext* currentFrame() const;
    uint32_t getMaxFramesInFlight() const { return MAX_FRAMES_IN_FLIGHT; }

    // ---- Render passes (explicit; beginFrame no longer starts one) ----------
    void beginSwapchainRenderPass();   // m_RenderPass, LOAD_OP_CLEAR
    void beginOverlayRenderPass();     // m_OverlayRenderPass, LOAD_OP_LOAD (ImGui)
    void endRenderPass();
    bool isRenderPassActive() const { return m_RenderPassActive; }

    // ---- Per-frame staging arena (Phase 1c item 6) --------------------------
    // Bump allocator inside one host-visible buffer per frame in flight.
    // Reset in beginFrame() after the fence wait. Returns the region to memcpy
    // into plus the (buffer, offset) to use as a vkCmdCopyBuffer source.
    struct StagingAlloc { VkBuffer buffer; VkDeviceSize offset; std::byte* ptr; };
    StagingAlloc stage(const FrameContext& frame, VkDeviceSize size, VkDeviceSize alignment = 16);

    // ---- Deferred destruction ----------------------------------------------
    // Freed once m_FrameNumber has advanced FRAMES_IN_FLIGHT past the retire
    // frame. Drained at the top of beginFrame().
    void deferDestroy(VkBuffer, VmaAllocation);
    void deferDestroy(VkImage, VmaAllocation, VkImageView);
    void deferFreeDescriptorSets(std::span<const VkDescriptorSet>);

    // ---- Shared resources ---------------------------------------------------
    VkSampler getSampler(const SamplerDesc&);           // cached, owned by context
    const VulkanBuffer&  dummyStorageBuffer() const;    // 256B device-local, zeroed
    const VulkanBuffer&  dummyUniformBuffer() const;    // 256B device-local, zeroed
    const VulkanTexture& dummyTexture() const;          // 1x1 opaque black

    // ---- Presentation / settings -------------------------------------------
    // Phase 1c item 5. Stores the desired present mode and, if it changed,
    // sets a flag that recreateSwapchain() honours; call recreateSwapchain()
    // to apply. FIFO when true, MAILBOX (fallback IMMEDIATE) when false.
    void setVSync(bool enabled);
    bool vsync() const;

    // unchanged getters: getInstance/getPhysicalDevice/getDevice/getGraphicsQueue/
    // getSurface/getCommandPool/getRenderPass/getOverlayRenderPass/getAllocator/
    // getDescriptorPool/getGraphicsQueueFamily/getSwapchainExtent/
    // getSwapchainImageCount/getMinImageCount/wasSwapchainRecreated/
    // clearSwapchainRecreatedFlag/recreateSwapchain/blitImageToSwapchain
private:
    static_assert(MAX_FRAMES_IN_FLIGHT == int(FRAMES_IN_FLIGHT));
    FrameContext m_Frame;
    bool         m_FrameActive = false;
    uint64_t     m_FrameNumber = 0;
    VkImageLayout m_SwapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // + staging arena, deletion queue, sampler cache, dummy resources
};
```

`beginSingleTimeCommands` / `endSingleTimeCommands` (`.h:67-68`, `.cpp:502-533`) stay, but their only remaining legitimate callers are startup and teardown paths outside a frame. All in-frame uploads go through `stage()` + `frame.cmd()`. Add a debug assert in `beginSingleTimeCommands` that `m_FrameActive == false`.

Also, Phase 1c item 4 belongs here and is one line: `VulkanContext.h:166` becomes

```cpp
#ifdef NDEBUG
    bool m_EnableValidationLayers = false;
#else
    bool m_EnableValidationLayers = true;
#endif
```

### 2.8 Files deleted at the end of the migration

```
X3/src/Renderer/IComputeShader.h        X3/src/Renderer/IComputeShader.cpp
X3/src/Renderer/IImage2D.h              X3/src/Renderer/IImage2D.cpp
X3/src/Renderer/ITexture2D.h            X3/src/Renderer/ITexture2D.cpp
X3/src/Renderer/IUniformBuffer.h        X3/src/Renderer/IUniformBuffer.cpp
X3/src/Renderer/IShaderStorageBuffer.h  X3/src/Renderer/IShaderStorageBuffer.cpp
X3/src/Renderer/IRenderingContext.h     X3/src/Renderer/IRenderingContext.cpp
X3/src/Renderer/IRendererAPI.h          X3/src/Renderer/IRendererAPI.cpp
X3/src/Platform/Vulkan/VulkanComputeShader.h       .cpp
X3/src/Platform/Vulkan/VulkanImage2D.h             .cpp
X3/src/Platform/Vulkan/VulkanTexture2D.h           .cpp
X3/src/Platform/Vulkan/VulkanUniformBuffer.h       .cpp
X3/src/Platform/Vulkan/VulkanShaderStorageBuffer.h .cpp
X3/src/Platform/Vulkan/VulkanRendererAPI.h         .cpp
```

`X3/src/X3.h:16-21` loses all six `Renderer/I*.h` includes.

---

## 3. Per-frame ring design in detail

**Copies:** exactly `FRAMES_IN_FLIGHT = 2`, matching `VulkanContext::MAX_FRAMES_IN_FLIGHT` (`VulkanContext.h:146`). One `VkBuffer` per ring, not N — `FRAMES_IN_FLIGHT` slots inside a single VMA allocation. Reasons: one allocation, one persistent map pointer, one destroy, and the descriptor for slot `i` is just `{buffer, i*stride, sizePerFrame}`.

**Stride:** `stride = alignUp(sizePerFrame, A)` where
`A = max(sizePerFrame == 0 ? 1 : 1,
        kind == Uniform ? limits.minUniformBufferOffsetAlignment
                        : limits.minStorageBufferOffsetAlignment,
        memory is non-coherent ? limits.nonCoherentAtomSize : 1)`.
Query `VkPhysicalDeviceLimits` once in `VulkanContext::pickPhysicalDevice` (`VulkanContext.cpp:90-92` already calls `vkGetPhysicalDeviceProperties`) and cache it. Total allocation size is `FRAMES_IN_FLIGHT * stride`.

**Threading the frame index — the rule:** the frame index is *never* read from a global. It travels as `const FrameContext&`, obtained exactly twice in the whole engine:

1. `Application::run` calls `VulkanContext::beginFrame()` and holds the `const FrameContext*` for the iteration (see §5.6).
2. `ImGuiContext::EndFrame` and `RuntimeLayer::onUpdate` call `VulkanContext::Get()->currentFrame()`, because `ILayer::onUpdate()` has no parameter and changing that signature across five layers is churn this phase does not need.

Below those two points every function that touches GPU state takes `const FrameContext&` as its first parameter: `Renderer::Render`, `Renderer::SetupGPUResources`, `Renderer::Draw`, and every method on the resource classes that writes or records. There is no `VulkanContext::getCurrentFrame()` returning a bare `uint32_t` any more (the current one is `VulkanContext.h:58`) — deleting it is what prevents the index from leaking back into ad-hoc call sites.

**How a caller writes data for the current frame:**

```cpp
// whole-struct case
CameraUBOData cam{ pScene->CameraTransform, pScene->CameraFocalLength, {0,0,0} };
m_CameraUBO.writeStruct(frame, cam);

// dynamically sized case
const uint32_t bytes = uint32_t(sizeof(MeshEntityHandle) * pScene->MeshEntityLookupTable.size());
if (m_EntityLookupSSBO.ensureCapacity(frame, bytes)) { /* descriptors get rewritten below anyway */ }
m_EntityLookupSSBO.write(frame, pScene->MeshEntityLookupTable.data(), bytes);
```

`write()` computes `m_Mapped + frame.index() * m_Stride + offsetInSlot`, memcpys, and flushes that exact range.

**Why this is safe, precisely:** `VulkanContext::beginFrame()` does `vkWaitForFences(m_InFlightFences[m_CurrentFrame])` (`VulkanContext.cpp:346`) *before* returning the `FrameContext`. That fence was signalled by the submit of the previous use of frame slot `m_CurrentFrame` (`VulkanContext.cpp:429`). So when the caller receives the `FrameContext`, the GPU has provably finished every command that referenced slot `frame.index()`. The CPU can then overwrite it with no further synchronisation. This is the *entire* correctness argument — it is why `FrameContext` is produced by `beginFrame` and by nothing else, and why it must never be cached across frames.

**Interaction with `MAX_FRAMES_IN_FLIGHT`:** `FRAMES_IN_FLIGHT` in `VulkanCommon.h` and `MAX_FRAMES_IN_FLIGHT` in `VulkanContext.h:146` must be the same number. Enforce with `static_assert(VulkanContext::MAX_FRAMES_IN_FLIGHT == int(FRAMES_IN_FLIGHT))` in `VulkanContext.cpp`. Make `MAX_FRAMES_IN_FLIGHT` public (it is currently private at `VulkanContext.h:146`) or, better, define it *as* `FRAMES_IN_FLIGHT` and delete the duplicate. Note that fences are per-frame-in-flight while semaphores are per-swapchain-image (`VulkanContext.cpp:310-315`, `:333-339`) — that distinction is correct and stays.

**Growth:** `ensureCapacity` never shrinks and grows to the next power of two, so a scene with a fluctuating entity count does not reallocate every frame. Reallocation hands the old `(VkBuffer, VmaAllocation)` to `ctx.deferDestroy()`, which frees it only after `FRAMES_IN_FLIGHT` further frames have retired. This is what fixes the immediate-`vmaDestroyBuffer`-while-in-flight bug at `Renderer.cpp:258/270/282/295/318/332/346`.

**Deletion queue mechanics:** a `std::vector<PendingDelete>` on the context where `PendingDelete { uint64_t retireFrame; enum Kind; VkBuffer/VkImage/VkImageView; VmaAllocation; }`. `deferDestroy` sets `retireFrame = m_FrameNumber`. `beginFrame()` — immediately after the fence wait, before anything else — erases and destroys every entry with `m_FrameNumber - retireFrame >= FRAMES_IN_FLIGHT`.

**Staging arena:** same shape. One host-visible `VkBuffer` per frame in flight, default 8 MiB, bump-allocated by `stage()`, offset reset to 0 in `beginFrame()` after the fence wait. If a single request exceeds the remaining space, grow the arena (defer-destroying the old one) and log a warning — the mesh/node/index uploads at `Renderer.cpp:317/331/345` can be large.

---

## 4. Descriptor management design

### 4.1 What is wrong today, restated as requirements

`VulkanComputeShader::Dispatch` (`VulkanComputeShader.cpp:99-155`) allocates **one** set per layout on first dispatch (`:113-115` → `allocateDescriptorSets`, `.cpp:157-180`) and then calls `updateDescriptorSets()` on **every** dispatch (`:118`), i.e. `vkUpdateDescriptorSets` on a set that frame N-1's still-executing command buffer has bound. That is undefined behaviour and the validation layer flags it as soon as it is enabled.

Requirements for the replacement:
1. `FRAMES_IN_FLIGHT` sets per (pipeline, set index), indexed by `frame.index()`.
2. Sets updated only after the fence wait for that frame index.
3. Binding identity is `(set, binding)`, not `binding`.
4. Every declared binding written every frame, or a hard assert.
5. Descriptor info storage owned by the writer, not smuggled in member maps.

### 4.2 Allocation

- Pool: reuse the existing `m_DescriptorPool` (`VulkanContext.cpp:460-490`) — `maxSets = 1000`, 1000 of each type, `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`. Three shaders × three sets × two frames = 18 sets, well inside budget alongside ImGui's usage (`ImGuiContext.cpp:169`) and `ImGui_ImplVulkan_AddTexture` (`ViewportPanel.cpp:93`).
- `VulkanDescriptorSetRing`'s constructor does one `vkAllocateDescriptorSets` with `descriptorSetCount = FRAMES_IN_FLIGHT` and `pSetLayouts` = the same layout repeated `FRAMES_IN_FLIGHT` times.
- Allocation happens **once**, at pipeline creation time in `Renderer::GetOrLoadShader`, never lazily inside dispatch. This removes `m_DescriptorSetsAllocated` (`VulkanComputeShader.h:75`) and the branch at `VulkanComputeShader.cpp:113-115`.
- Destruction: `~VulkanDescriptorSetRing` calls `ctx.deferFreeDescriptorSets(m_Sets)`, which `vkFreeDescriptorSets` them after `FRAMES_IN_FLIGHT` retired frames.

### 4.3 When sets are updated

Once per frame, in `Renderer::Draw`, immediately before `dispatch()`, after all buffer writes and image transitions have been recorded. Concretely (full code in §5.5):

```cpp
DescriptorWriter(ctx, sets0.get(frame))
    .storageImage(0, outputImage)
    .sampledImage(1, m_SkyboxTexture.valid() ? m_SkyboxTexture : ctx.dummyTexture())
    .flush(pipeline->setLayout(0));
```

Rewriting all sets every frame is deliberate for this phase: it is what the old code effectively did, it is cheap at 11 descriptors, and dirty-tracking is a Phase 5 render-graph concern. The *correctness* fix is that the set being written is the one belonging to a frame the GPU has finished, not the one it is reading.

### 4.4 Replacing the global registry

Delete `VulkanContext.h:38-52` (the four `Bound*` structs, the four `register*` declarations, the four `getBound*` getters), `VulkanContext.h:174-178` (the four maps), and `VulkanContext.cpp:810-824` (the four implementations).

What replaces it, mechanism by mechanism:

| Old | New |
|---|---|
| `registerStorageImage(unit, view)` from `VulkanImage2D::ChangeImageUnit` | `DescriptorWriter::storageImage(binding, image)` at the call site that knows the set |
| `registerSampledImage(unit, view, sampler)` from `VulkanTexture2D::ChangeTextureUnit` | `DescriptorWriter::sampledImage(binding, texture)` |
| `registerUniformBuffer(bp, buf, size)` from `VulkanUniformBuffer::Bind` | `DescriptorWriter::uniformBuffer(binding, ringBuffer, frame)` |
| `registerStorageBuffer(bp, buf, size)` from `VulkanShaderStorageBuffer::Bind` | `DescriptorWriter::storageBuffer(binding, ringOrStaticBuffer[, frame])` |
| Lookup `boundX.find(binding.binding)` in `VulkanComputeShader.cpp:214/228/242/256` | Nothing. There is no lookup — the writer is told the resource directly. |
| Silent skip when not found (`VulkanComputeShader.cpp:273-277`) | `DescriptorWriter::flush` asserts completeness against `VulkanDescriptorSetLayout::bindings()` |

Set identity is now structural: `sets0` is a different `VulkanDescriptorSetRing` object from `sets2`, and `flush()` validates against `pipeline->setLayout(0)` vs `setLayout(2)`. `(set, binding)` collision is not possible because a binding number is only ever interpreted relative to the layout it is flushed against.

The single binding table (currently `VulkanComputeShader.cpp:19-42`, matched to shader source by comment) moves to one named constant in `Renderer.cpp` — see §5.1. That is the object Phase 3's Slang reflection codegen will generate; keeping it in exactly one place is the "don't paint into a corner" requirement.

### 4.5 The always-write rule and dummy resources

`Renderer.cpp:292` skips the light SSBO entirely when `count == 0`, leaving set 2 binding 6 unwritten. The `flush()` assert makes that an immediate failure. Two acceptable fixes, in preference order:

1. `m_LightSSBO.ensureCapacity(frame, std::max(bytes, VkDeviceSize(sizeof(LightData))))` and write whatever is there. `u_LightCount` (already uploaded in the settings UBO, `Renderer.cpp:227`) tells the shader not to read it. **Use this.**
2. `ctx.dummyStorageBuffer()` for the binding. Reserve this for cases where no real buffer exists at all.

`ctx.dummyTexture()` (1×1 opaque black, `VK_FORMAT_R8G8B8A8_SRGB`, `SHADER_READ_ONLY_OPTIMAL`) is required for set 0 binding 1 when the scene has no skybox — `Renderer.cpp:247` sets `m_SkyboxTexture = nullptr` and the binding is then unwritten today.

---

## 5. Call-site migration table

### 5.1 New constants to introduce in `Renderer.cpp`

```cpp
namespace {
// Mirrors res/shaders/PathTracing.comp:83-129 (identical in PBR.comp / Phong.comp).
// Phase 3 replaces this with Slang-reflection-generated code.
constexpr X3::DescriptorBindingDesc kSet0[] = {
    {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT}, // rayTracingTexture
    {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // skyboxTexture
};
constexpr X3::DescriptorBindingDesc kSet1[] = {
    {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // CameraUBO
    {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // SettingsUBO
};
constexpr X3::DescriptorBindingDesc kSet2[] = {
    {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // EntityLookup
    {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // Transform
    {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // Material
    {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // MeshBuffer
    {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // NodeBuffer
    {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // IndexBuffer
    {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // LightBuffer
};

// std140. Matches PathTracing.comp:87-90. Total 80 bytes, as Renderer.cpp:17.
struct CameraUBOData { glm::mat4 transform; float focalLength; float _pad[3]; };
static_assert(sizeof(CameraUBOData) == 80);

// std140. Matches PathTracing.comp:92-101. 32 bytes used; Renderer.cpp:18 allocates 64.
struct SettingsUBOData {
    uint32_t raysPerPixel, bouncesPerRay, accumulatedFrames, entityCount;
    uint32_t debugMode, aabbHeatmapCutoff, triHeatmapCutoff, lightCount;
};
static_assert(sizeof(SettingsUBOData) == 32);
} // namespace
```

### 5.2 `X3/src/Renderer/Renderer.h`

| Current | Becomes |
|---|---|
| `:5` `#include "Renderer/IRendererAPI.h"` | deleted |
| `:13-17` forward decls of `IComputeShader/ITexture2D/IImage2D/IUniformBuffer/IShaderStorageBuffer` | forward decls of `VulkanContext, VulkanImage, VulkanTexture, VulkanBuffer, VulkanRingBuffer, VulkanComputePipeline, VulkanDescriptorSetRing, FrameContext` — but these are held **by value**, so the concrete headers must be included instead |
| `:34-38` `Cache::entityLookupSize/transformSize/materialSize/lightSize` | deleted — `ensureCapacity` subsumes them |
| `:87-88` `GetAPI()/SetAPI()` | deleted with `IRendererAPI` |
| `:92` `void Init()` | `void Init(VulkanContext& ctx)` |
| `:93-94` `std::shared_ptr<IImage2D> Render(...)` | `VulkanImage* Render(const FrameContext& frame, const Scene*, const AssetPool*, const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f)` |
| `:99` `bool SetupGPUResources(pScene, scene, assetPool)` | `bool SetupGPUResources(const FrameContext&, std::shared_ptr<const ParsedScene>, const Scene*, const AssetPool*)` |
| `:100` `void Draw()` | `void Draw(const FrameContext&)` |
| `:101` `std::shared_ptr<IComputeShader> GetOrLoadShader(ShaderType)` | `VulkanComputePipeline* GetOrLoadShader(ShaderType)` (nullptr on failure) |
| `:106-107` `m_CurrentShader` / `m_ShaderCache` | `VulkanComputePipeline* m_CurrentShader = nullptr;` / `std::unordered_map<ShaderType, VulkanComputePipeline> m_ShaderCache;` |
| — (new) | `std::unordered_map<ShaderType, std::array<VulkanDescriptorSetRing,3>> m_DescriptorRings;` |
| `:111-113` `m_Frames[2]`, `m_WriteFrameIndex`, `m_WasDoubleBuffering` | `std::array<VulkanImage, FRAMES_IN_FLIGHT> m_Frames;` — index selection explained below; the other two are deleted |
| `:115` `std::shared_ptr<ITexture2D> m_SkyboxTexture` | `VulkanTexture m_SkyboxTexture;` |
| `:116` `m_CameraUBO, m_SettingsUBO` | `VulkanRingBuffer m_CameraUBO, m_SettingsUBO;` |
| `:117` seven `IShaderStorageBuffer` | `VulkanRingBuffer m_EntityLookupSSBO, m_TransformSSBO, m_MaterialSSBO, m_LightSSBO;` + `VulkanBuffer m_MeshBufferSSBO, m_NodeBufferSSBO, m_IndexBufferSSBO;` |
| — (new) | `VulkanContext* m_Ctx = nullptr;` and `uint32_t m_PrevMeshVersion=0, m_PrevNodeVersion=0, m_PrevIndexVersion=0;` (see `Renderer.cpp:306-309`) |

**Output-image indexing (replaces the `m_WriteFrameIndex` dance at `Renderer.cpp:65-90, 209`).** The accumulation path is a read-modify-write of the *same* image: `PathTracing.comp:520-521` does `imageLoad(rayTracingTexture, …)` then `imageStore(rayTracingTexture, …)`. So:

```cpp
VulkanImage& outputImage(const FrameContext& f) {
    return m_RenderSettings.accumulate ? m_Frames[0] : m_Frames[f.index()];
}
```

That preserves both behaviours: accumulate ⇒ one persistent image (frames serialise on it, which is inherent); no accumulate ⇒ one image per frame in flight, which is what `useDoubleBuffering` was hand-rolling. Consequently `RenderSettings::useDoubleBuffering` (`RenderSettings.h:24`) and its two setters (`WindowTitleBar.cpp:131, 139`) are deleted — with correct fencing, double buffering is always safe when not accumulating.
When `accumulate` is true, `m_Frames[0]` needs the cross-frame RMW barrier before the dispatch: `outputImage.barrier(frame, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)`.

### 5.3 `X3/src/Renderer/Renderer.cpp` — `Init` and `GetOrLoadShader`

| Line(s) | Current | Becomes |
|---|---|---|
| `:5-9` | five `Renderer/I*.h` includes | `Platform/Vulkan/VulkanContext.h`, `VulkanComputePipeline.h`, `VulkanBuffer.h`, `VulkanImage.h`, `VulkanTexture.h`, `VulkanDescriptors.h` |
| `:17` | `m_CameraUBO = IUniformBuffer::Create(80, 0, DYNAMIC_DRAW)` | `m_CameraUBO = VulkanRingBuffer(ctx, BufferKind::Uniform, sizeof(CameraUBOData), "CameraUBO")` |
| `:18` | `m_SettingsUBO = IUniformBuffer::Create(64, 1, DYNAMIC_DRAW)` | `m_SettingsUBO = VulkanRingBuffer(ctx, BufferKind::Uniform, sizeof(SettingsUBOData), "SettingsUBO")` |
| `:21` | `m_CurrentShader = GetOrLoadShader(PATH_TRACING)` | unchanged, type is now `VulkanComputePipeline*` |
| `:26` | `m_CurrentShader->Bind()` | **deleted** |
| — | | add `m_Ctx = &ctx;` at the top of `Init` |
| `:32` | `it->second && it->second->GetID() != 0` | `it->second.valid()` — return `&it->second` |
| `:43` | `IComputeShader::Create(pathIt->second.string(), glm::uvec3(1))` | `ComputePipelineDesc d; d.spirvPath = pathIt->second; d.spirvPath += ".spv"; d.setLayouts = {{kSet0...}, {kSet1...}, {kSet2...}}; d.debugName = …;` then `auto [it2,_] = m_ShaderCache.try_emplace(type, *m_Ctx, d);` |
| `:44` | `if (!shader \|\| shader->GetID() == 0)` | `if (!it2->second.valid()) { m_ShaderCache.erase(it2); return nullptr; }` |
| `:50` | `m_ShaderCache[type] = shader; return shader;` | allocate the rings — `m_DescriptorRings[type] = { VulkanDescriptorSetRing(*m_Ctx, p.setLayout(0)), VulkanDescriptorSetRing(*m_Ctx, p.setLayout(1)), VulkanDescriptorSetRing(*m_Ctx, p.setLayout(2)) };` — then `return &it2->second;` |

Note `.spv` must now be appended by the caller (`Renderer.h:121-125` stores paths without it); `VulkanComputeShader.cpp:302` used to do it internally.

### 5.4 `Renderer.cpp` — `Render` and `Parse`

| Line(s) | Current | Becomes |
|---|---|---|
| `:54-55` | signature | `VulkanImage* Renderer::Render(const FrameContext& frame, const Scene*, const AssetPool*, const glm::mat4*, float)` |
| `:58-61` | `Parse(...)`; `return nullptr` | unchanged |
| `:62` | `SetupGPUResources(pScene, scene, assetPool)` | `SetupGPUResources(frame, pScene, scene, assetPool)` |
| `:63` | `Draw()` | `Draw(frame)` |
| `:65-90` | the whole `canDoubleBuffer` / `m_WasDoubleBuffering` / `m_WriteFrameIndex` block | **deleted**, replaced by `return &outputImage(frame);` |
| `:93-193` | `Parse` | **unchanged** — pure CPU, no GPU types |

### 5.5 `Renderer.cpp` — `SetupGPUResources` and `Draw`

| Line(s) | Current | Becomes |
|---|---|---|
| `:201-205` | resolution change → two `IImage2D::Create(nullptr, w, h, 0, LR_READ_WRITE)` | `for (auto& img : m_Frames) img.recreate(frame, ImageDesc{ w, h, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT \| VK_IMAGE_USAGE_SAMPLED_BIT \| VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, "RenderTarget" });` — `TRANSFER_DST` from `VulkanImage2D.cpp:77` is no longer needed (nothing uploads into it) |
| `:209` | `m_Frames[m_WriteFrameIndex]->ChangeImageUnit(0)` | **deleted** — descriptor write moves to `Draw` |
| `:212` | accumulation counter | unchanged |
| `:217-218` | `entityCount`, `lightCount` | unchanged |
| `:219-228` | `m_SettingsUBO->Bind(); AddData ×8; Unbind()` | one `SettingsUBOData s{ raysPerPixel, bouncesPerRay, AccumulatedFrames, entityCount, uint32_t(debugMode), uint32_t(aabbHeatmapCutoff), uint32_t(triangleHeatmapCutoff), lightCount }; m_SettingsUBO.writeStruct(frame, s);` — note `debugMode`/`aabbHeatmapCutoff`/`triangleHeatmapCutoff` are `int` in `RenderSettings.h:21-23` and were being `AddData`'d as `uint32_t`; the struct makes the conversion explicit |
| `:231-234` | `m_CameraUBO->Bind(); AddData ×2; Unbind()` | `CameraUBOData c{ pScene->CameraTransform, pScene->CameraFocalLength, {} }; m_CameraUBO.writeStruct(frame, c);` |
| `:238-249` | skybox reload; `SKYBOX_TEXTURE_UNIT = 1`; `ITexture2D::Create(data, w, h, 1)` | `m_SkyboxTexture = VulkanTexture(*m_Ctx, frame, TextureDesc{ uint32_t(metadata->width), uint32_t(metadata->height), VK_FORMAT_R8G8B8A8_SRGB, {}, "Skybox" }, data);` — `SKYBOX_TEXTURE_UNIT` deleted; the else branch becomes `m_SkyboxTexture = {};` |
| `:253-264` | EntityLookup: conditional `Create(sizeBytes, 0, DYNAMIC)`, `Bind/AddData/Unbind`, `m_Cache.entityLookupSize` | `m_EntityLookupSSBO.ensureCapacity(frame, bytes); m_EntityLookupSSBO.write(frame, data, bytes);` |
| `:265-276` | Transform, binding 1 | same pattern on `m_TransformSSBO` |
| `:277-288` | Material, binding 2 | same pattern on `m_MaterialSSBO` |
| `:289-302` | Lights, binding 6, **skipped when `count == 0`** | `const VkDeviceSize bytes = std::max<VkDeviceSize>(sizeof(LightData)*count, sizeof(LightData)); m_LightSSBO.ensureCapacity(frame, bytes); if (count) m_LightSSBO.write(frame, data, sizeof(LightData)*count);` — **no early-out**, so binding 6 is always valid |
| `:306-309` | four function-local `static uint32_t prev*Version` | move to members `m_PrevMeshVersion/m_PrevNodeVersion/m_PrevIndexVersion`; `prevSkyboxTextureVersion` (`:309`) is unused — delete |
| `:311-323` | MeshBuffer: `Create(bytes, 3, STATIC)` + `Bind/AddData/Unbind` | `m_MeshBufferSSBO.ensureCapacity(frame, bytes); m_MeshBufferSSBO.upload(frame, assetPool->MeshBuffer.data(), bytes);` — still guarded by the version check |
| `:325-337` | NodeBuffer, binding 4 | same pattern on `m_NodeBufferSSBO` |
| `:339-351` | IndexBuffer, binding 5 | same pattern on `m_IndexBufferSSBO` |
| `:356-357` | `Draw()` signature + profiler | `void Renderer::Draw(const FrameContext& frame)` |
| `:360-369` | shader switch + null check | unchanged (`VulkanComputePipeline*`) |
| `:371` | `m_CurrentShader->Bind()` | **deleted** |
| `:372-376` | `setWorkGroupSizes(uvec3((w+7)/8, (h+3)/4, 1))` | local `const uint32_t gx = (res.x+7)/8, gy = (res.y+3)/4;` (the /8 and /4 must stay in sync with `LOCAL_GROUP_X 8` / `LOCAL_GROUP_Y 4` at `PathTracing.comp:16-17`) |
| `:377` | `m_CurrentShader->Dispatch()` | the block below |

New body of `Draw` from `:371` on:

```cpp
VulkanImage& out = outputImage(frame);
out.transition(frame, VK_IMAGE_LAYOUT_GENERAL,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
if (m_RenderSettings.accumulate) {                 // cross-frame RMW on m_Frames[0]
    out.barrier(frame, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

auto& rings = m_DescriptorRings.at(m_RenderSettings.shaderType);
DescriptorWriter(*m_Ctx, rings[0].get(frame))
    .storageImage(0, out)
    .sampledImage(1, m_SkyboxTexture.valid() ? m_SkyboxTexture : m_Ctx->dummyTexture())
    .flush(m_CurrentShader->setLayout(0));
DescriptorWriter(*m_Ctx, rings[1].get(frame))
    .uniformBuffer(0, m_CameraUBO,   frame)
    .uniformBuffer(1, m_SettingsUBO, frame)
    .flush(m_CurrentShader->setLayout(1));
DescriptorWriter(*m_Ctx, rings[2].get(frame))
    .storageBuffer(0, m_EntityLookupSSBO, frame)
    .storageBuffer(1, m_TransformSSBO,    frame)
    .storageBuffer(2, m_MaterialSSBO,     frame)
    .storageBuffer(3, m_MeshBufferSSBO)
    .storageBuffer(4, m_NodeBufferSSBO)
    .storageBuffer(5, m_IndexBufferSSBO)
    .storageBuffer(6, m_LightSSBO,        frame)
    .flush(m_CurrentShader->setLayout(2));

const VkDescriptorSet sets[3] = { rings[0].get(frame), rings[1].get(frame), rings[2].get(frame) };
m_CurrentShader->dispatch(frame, sets, gx, gy, 1);

// Was VulkanComputeShader.cpp:139-154, now explicit and correctly scoped:
// compute write -> ImGui fragment read (editor) / transfer read (runtime blit).
out.barrier(frame, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
```

### 5.6 `X3/src/Core/application.cpp` and the window layer

| File:line | Current | Becomes |
|---|---|---|
| `application.h:12, 28` | `class IRendererAPI;` / `_RendererAPI` | deleted; add `VulkanContext* _Context = nullptr;` |
| `application.cpp:9` | `#include "Renderer/IRendererAPI.h"` | `#include "Platform/Vulkan/VulkanContext.h"` |
| `application.cpp:27-28` | `_RendererAPI = IRendererAPI::Create(); _RendererAPI->Init();` | `_Context = VulkanContext::Get();` (created by the window at `GLFWWindow.cpp:50`) |
| `application.cpp:53-58` | `_RendererAPI->Clear({...})` under `BUILD_INSTALL` | deleted — the clear colour is `VulkanContext.cpp:388` / `blitImageToSwapchain`'s `vkCmdClearColorImage` at `:880` |
| `application.cpp:60-70` | `_LayerStack->onUpdate();` then `_Window->swapBuffers();` | the loop body below |
| `IWindow.h:37` | `virtual void swapBuffers() = 0;` | deleted |
| `GLFWWindow.h:19` | `void swapBuffers() override;` | deleted |
| `GLFWWindow.h:5, 53` | `#include "Renderer/IRenderingContext.h"`, `IRenderingContext* m_Context;` | `#include "Platform/Vulkan/VulkanContext.h"`, `std::unique_ptr<VulkanContext> m_Context;` (currently leaked — `new` at `:50`, never deleted) |
| `GLFWWindow.cpp:80-83` | `onUpdate()` = `glfwPollEvents(); m_Context->swapBuffers();` | `onUpdate()` is unreferenced dead code; delete it and `IWindow.h:35` |
| `GLFWWindow.cpp:89-91` | `swapBuffers()` | deleted |
| `GLFWWindow.cpp:107-110` | `setVSync` → `glfwSwapInterval(enabled)` (a no-op under Vulkan) | `m_VSync = enabled; m_Context->setVSync(enabled); m_Context->recreateSwapchain();` — **this is Phase 1c item 5** |

New `Application::run` body:

```cpp
while (!_Window->shouldClose()) {
    Time::Update();
    auto t = _Profiler->globalTimer("GLOBAL");
    { auto t2 = _Profiler->timer("PollEvents"); _Window->pollEvents(); }

    const FrameContext* frame = _Context->beginFrame();
    if (!frame) continue;                       // swapchain was out of date

    { auto t2 = _Profiler->timer("LayerStack::onUpdate()"); _LayerStack->onUpdate(); }

    { auto t2 = _Profiler->timer("Present");
      _Context->endFrame();
      _Context->present(); }
}
Shutdown();
```

`vkDeviceWaitIdle` before `Shutdown()` so layer/renderer destructors are safe.

### 5.7 `X3/src/Core/Layers/RenderLayer.cpp` and `RenderEvents.h`

| File:line | Current | Becomes |
|---|---|---|
| `RenderEvents.h:4` | `#include "Renderer/IImage2D.h"` | forward-declare `class VulkanImage;` |
| `RenderEvents.h:12, 14` | `std::shared_ptr<IImage2D> frame;` | `VulkanImage* frame = nullptr;` — non-owning is correct: `LayerStack::dispatchEvent` (`LayerStack.cpp:34-41`) is synchronous, `RenderLayer` is pushed before every consumer (`application.cpp:33`), so consumers read it inside the same `LayerStack::onUpdate` pass in which it was produced |
| `RenderLayer.cpp:30` | `std::shared_ptr<IImage2D> RenderedFrame;` | `VulkanImage* RenderedFrame = nullptr;` |
| `RenderLayer.cpp:31-37` | two `m_Renderer.Render(...)` calls | prepend the frame: `const FrameContext* f = VulkanContext::Get()->currentFrame(); if (!f) return;` then `m_Renderer.Render(*f, scene.get(), assetPool.get(), …)` |
| `RenderLayer.cpp:19` | `m_Renderer.Init();` | `m_Renderer.Init(*VulkanContext::Get());` |
| `RenderLayer.cpp:45` | `NewFrameRenderedEvent(RenderedFrame)` | unchanged shape, raw pointer |

### 5.8 `X3-Editor/src/Panels/ViewportPanel/*`

| File:line | Current | Becomes |
|---|---|---|
| `ViewportPanel.h:46` | `std::weak_ptr<IImage2D> m_LatestRenderedFrame;` | `VulkanImage* m_LatestRenderedFrame = nullptr;` |
| `ViewportPanel.h:65-67` | `VkDescriptorSet m_ImGuiTextureDescriptor; VkSampler m_TextureSampler; int m_LastRegisteredImageID;` | `std::unordered_map<uint64_t, VkDescriptorSet> m_ImGuiDescriptors;` + `VkSampler m_TextureSampler` (keep, or take from `ctx.getSampler`) |
| `ViewportPanel.h:70` | `ImTextureID GetImGuiTextureID(std::shared_ptr<IImage2D>)` | `ImTextureID GetImGuiTextureID(VulkanImage& image)` |
| `ViewportPanel.cpp:13` | `#include "Platform/Vulkan/VulkanImage2D.h"` | `#include "Platform/Vulkan/VulkanImage.h"` |
| `ViewportPanel.cpp:45-52` | single-entry cache on `image->GetID()`; re-registers whenever the id changes | look up `m_ImGuiDescriptors.find(image.id())`; hit ⇒ return. **This matters**: with `m_Frames[frame.index()]` the panel now alternates between two images every frame, so a single-slot cache would `RemoveTexture`/`AddTexture` every frame |
| `ViewportPanel.cpp:57-59` | `std::dynamic_pointer_cast<VulkanImage2D>(image)` | deleted — no hierarchy, no RTTI |
| `ViewportPanel.cpp:63-67` | unconditional `ImGui_ImplVulkan_RemoveTexture` before re-register | only on cache eviction / in `CleanupVulkanResources` |
| `ViewportPanel.cpp:93-97` | `ImGui_ImplVulkan_AddTexture(sampler, vulkanImage->getImageView(), VK_IMAGE_LAYOUT_GENERAL)` | `…, image.view(), VK_IMAGE_LAYOUT_GENERAL)` — layout stays `GENERAL`, matching `Draw`'s transition |
| `ViewportPanel.cpp:34-42` | `CleanupVulkanResources` frees one descriptor | iterate and free the whole map |
| `ViewportPanel.cpp:121` | `m_LatestRenderedFrame = dynamic_pointer_cast<NewFrameRenderedEvent>(event)->frame;` | unchanged shape (now a raw pointer) |
| `ViewportPanel.cpp:284-293` | `#ifdef X3_USE_VULKAN` / `#else` with the OpenGL `(ImTextureID)(intptr_t)…GetID()` path at `:292` | keep only the Vulkan branch; drop the `#ifdef`/`#else`/`#endif` (`:284, 290, 293`) and the entire `:292` line |
| `ViewportPanel.cpp:20-24`, `.h:8-10, 63-71` | `#ifdef X3_USE_VULKAN` guards | unconditional |

Everywhere `latestRenderedFrameShared` is dereferenced (`ViewportPanel.cpp:286, 292` and the `.lock()` sites above `:260`), replace the `weak_ptr::lock()` idiom with a plain null check on the raw pointer.

### 5.9 `X3-Runtime/src/RuntimeLayer.{h,cpp}`

| File:line | Current | Becomes |
|---|---|---|
| `RuntimeLayer.h:10` | `#include "Platform/Vulkan/VulkanImage2D.h"` | `#include "Platform/Vulkan/VulkanImage.h"` |
| `RuntimeLayer.h:47` | `std::shared_ptr<IImage2D> m_CurrentFrame;` | `VulkanImage* m_CurrentFrame = nullptr;` |
| `RuntimeLayer.h:48`, `.cpp:23, 109, 115-117` | `m_Framebuffer` (a GL FBO name) | deleted with the OpenGL branch in 1a |
| `RuntimeLayer.cpp:176-193` | `dynamic_pointer_cast<VulkanImage2D>`, `VulkanContext::Get()`, `blitImageToSwapchain(vulkanImage->getImage(), GENERAL, GetDimensions().x/.y, …)` | `if (m_CurrentFrame) { CalculateViewportCoordinates(); auto* ctx = VulkanContext::Get(); ctx->blitImageToSwapchain(m_CurrentFrame->handle(), m_CurrentFrame->layout(), uint32_t(m_CurrentFrame->dimensions().x), uint32_t(m_CurrentFrame->dimensions().y), m_ViewportCoords, m_WindowSize); }` — pass the *tracked* layout instead of hardcoding `VK_IMAGE_LAYOUT_GENERAL` (`RuntimeLayer.cpp:185`) |
| `RuntimeLayer.cpp:312` | `m_CurrentFrame->GetDimensions()` | `m_CurrentFrame->dimensions()` |
| `RuntimeLayer.cpp:199` | `m_CurrentFrame = dynamic_pointer_cast<NewFrameRenderedEvent>(event)->frame;` | unchanged shape |
| — | | `blitImageToSwapchain` (`VulkanContext.cpp:826-954`) must be updated to write back the new layout into the `VulkanImage` (or accept a `VulkanImage&`), since it leaves the source in `GENERAL` at `:934-953`. Cleanest: change the signature to `void blitImageToSwapchain(const FrameContext&, VulkanImage& src, glm::ivec4 viewport, glm::ivec2 windowSize)` and let it call `src.transition(...)` twice. |

Phase 1d (the OpenGL-only splash at `RuntimeLayer.cpp:35-71, 122-161, 213-304`) is **out of scope for 1b** — with 1a done those blocks are already gone; do not add a Vulkan splash here.

### 5.10 `X3-Editor/src/ImGuiContext.cpp`

| Line(s) | Current | Becomes |
|---|---|---|
| `:129-133` | `#ifndef X3_USE_VULKAN io.ConfigFlags \|= ViewportsEnable` | unchanged — multi-viewport stays off; that is Phase 13 |
| `:278` | `vkContext->ensureFrameStarted();` | **deleted** — `Application::run` guarantees the frame is open before any layer runs |
| `:283` | `vkContext->beginOverlayRenderPass();` | unchanged |
| `:290` | `assert(vkContext->isRenderPassActive())` | unchanged |
| `:220-261` | swapchain-recreation re-init | unchanged |

### 5.11 Other files

| File:line | Change |
|---|---|
| `X3/src/X3.h:16-21` | delete all six `Renderer/I*.h` includes |
| `X3/src/Project/ProjectManager.cpp:4, 113-119` | delete the include and the whole API-selection block |
| `X3/src/Renderer/RenderSettings.h:14-17, 28-32, 45, 61` | delete `enum class RendererAPI`, the member, and its two serialization lines |
| `X3/src/Renderer/RenderSettings.h:24` | delete `useDoubleBuffering` |
| `X3-Editor/src/WindowTitleBar/WindowTitleBar.cpp:130-132, 138-140` | delete the two `useDoubleBuffering` assignments; keep the `UpdateRenderSettingsEvent` dispatches |
| `X3-Editor/src/EditorLayer.cpp:58-64` | `SET_VSYNC_EVENT` → `m_Window->setVSync(...)`, which now actually reaches `VulkanContext::setVSync` |

---

## 6. Ordering — how to do this without a big-bang rewrite

The engine must run after every numbered step. Each step is one commit.

**The key trick that makes this incremental:** during steps 3b-3f the new `DescriptorWriter` and the old global registry coexist. For a binding whose resource has not yet been migrated, the writer takes the raw descriptor info straight out of `VulkanContext::getBoundStorageBuffers()` etc. via `DescriptorWriter::raw(binding, type, info)`. So the set can always be filled completely even though only some of its resources are new. The registry is deleted only in step 3g, when nothing reads it.

**Step 0 — prerequisite.** Phase 1a landed: `Platform/OpenGL/` gone, all `X3_USE_OPENGL` branches gone, `X3_GRAPHICS_API` gone. Nothing in this spec is safe to start before that.

**Step 1 — add the layer, wire nothing.** Purely additive; the engine behaves identically.
 1.1 `VulkanCommon.h`, `FrameContext.h`.
 1.2 `VulkanContext`: deletion queue, staging arena, sampler cache, dummy resources, `m_FrameNumber`, `static_assert` on `FRAMES_IN_FLIGHT`, and the `NDEBUG` gate on `m_EnableValidationLayers` (Phase 1c item 4 — free, do it here).
 1.3 `VulkanBuffer.h/.cpp`.
 1.4 `VulkanImage.h/.cpp`, `VulkanTexture.h/.cpp`.
 1.5 `VulkanDescriptors.h/.cpp`.
 1.6 `VulkanComputePipeline.h/.cpp`.
 *Verify:* builds and links; the new symbols are unreferenced. No behaviour change.

**Step 2 — frame lifecycle.** `beginFrame`/`endFrame`/`present` on `VulkanContext`; delete `swapBuffers`, `ensureFrameStarted`, `m_FirstFrame`, `beginRenderPass`, `IRenderingContext`, `IRendererAPI`, `VulkanRendererAPI`; restructure `Application::run`; drop `IWindow::swapBuffers`/`onUpdate`; make `GLFWWindowIMPL` own the context by `unique_ptr`; drop `ensureFrameStarted` from `ImGuiContext::EndFrame`; wire `setVSync` to the present mode (Phase 1c item 5). Everything else — `VulkanComputeShader`, the registry, the old buffer classes — is untouched and still works, because it only ever needed a recording command buffer.
 *Verify:* editor and runtime render exactly as before; no `m_FirstFrame` black first frame; window resize still recreates the swapchain and ImGui re-inits (`ImGuiContext.cpp:220-261`).
 *This is the highest-risk step.* It is isolated deliberately.

**Step 3 — migrate the Renderer, one binding group at a time.**
 3a. Swap `VulkanComputeShader` → `VulkanComputePipeline` + `VulkanDescriptorSetRing` + `DescriptorWriter`, with **every** binding still sourced from the old registry through `DescriptorWriter::raw`. `Renderer::Draw` gains `const FrameContext&`; `Bind()` calls disappear from `Renderer.cpp:26, 371`. **This alone fixes Phase 1c item 1** (per-frame descriptor sets) — the single biggest correctness bug — while every resource class is still the old one.
 3b. `m_CameraUBO`, `m_SettingsUBO` → `VulkanRingBuffer` + the two POD structs. Set 1 now writes from real objects; sets 0 and 2 still use `raw`. **Fixes item 2 for the UBOs.**
 3c. `m_EntityLookupSSBO`, `m_TransformSSBO`, `m_MaterialSSBO`, `m_LightSSBO` → `VulkanRingBuffer`, with `ensureCapacity` replacing the `m_Cache.*Size` fields and the light-count early-out removed. **Fixes item 2 for the dynamic SSBOs and closes the binding-6 hole.**
 3d. `m_MeshBufferSSBO`, `m_NodeBufferSSBO`, `m_IndexBufferSSBO` → `VulkanBuffer` with staged in-frame uploads; move the three `static` version counters to members. **Fixes Phase 1c item 6** for the large uploads — no more `vkQueueWaitIdle` per buffer.
 3e. `m_Frames` → `std::array<VulkanImage, FRAMES_IN_FLIGHT>`; delete `m_WriteFrameIndex`, `m_WasDoubleBuffering`, `useDoubleBuffering`; update `RenderEvents.h`, `RenderLayer.cpp`, `ViewportPanel` (multi-entry descriptor cache), `RuntimeLayer` (blit takes a `VulkanImage&`). Add the pre-dispatch RMW barrier for the accumulate path.
 3f. Skybox → `VulkanTexture` with the compute-stage transition fix and `dummyTexture()` fallback. **Set 2, then set 1, then set 0 are now fully native — no `raw` calls remain.**
 3g. Delete the registry (`VulkanContext.h:38-52, 174-178`, `VulkanContext.cpp:810-824`) and the thirteen old files listed in §2.8. Delete `DescriptorWriter::raw` if nothing else wants it.
 *Verify after each of 3a-3f:* editor renders; runtime renders; resize is stable; validation layers clean.

**Step 4 — cleanup.** `X3.h` includes; `ProjectManager.cpp:113-119`; `RenderSettings::rendererAPI`; `WindowTitleBar` double-buffering assignments.

**Verification gate before declaring 1b done** (this is a subset of the Phase 1 exit criteria): with validation layers on, run the editor for several minutes with camera motion, a resolution change, a shader-type switch, a skybox change, a scene with zero lights, accumulate on and off, and a window resize including minimise/restore — zero validation errors and zero synchronisation warnings. Then the same in the runtime. Enable `VK_LAYER_KHRONOS_validation`'s synchronization validation (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`) for at least one of those runs; it is the only thing that will catch a missed barrier in `VulkanImage::transition`.

**Remaining Phase 1c items after 1b:** item 1 lands in 3a, item 2 in 3b/3c, item 3 is deleted outright (§1.7), item 4 in 1.2, item 5 in step 2, item 6 in 3d for buffers — the residual `vkQueueWaitIdle` in `endSingleTimeCommands` (`VulkanContext.cpp:530`) then only affects startup paths, which is acceptable to leave.