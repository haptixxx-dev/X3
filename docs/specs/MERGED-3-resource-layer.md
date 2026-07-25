Both builds verified before writing: `cmake --preset vulkan-debug && cmake --build build/vulkan-debug -j 14` → 0 occurrences of `error:` in the log, `build/vulkan-debug/Debug/X3Editor` (39 MB) + `Debug/runtime/X3Runtime` produced. Same for `opengl-debug` → 0 `error:`, `build/opengl-debug/Debug/X3Editor` (31 MB). All line citations below were re-read from the current worktree.

---

# PART 3 — The Vulkan-native resource layer

**Scope:** `X3/src/Platform/Vulkan/*` (new files), `X3/src/Renderer/*`, `X3/src/Core/Events/RenderEvents.h`, `X3/src/Core/Layers/RenderLayer.cpp`, `X3-Editor/src/Panels/ViewportPanel/*`, `X3-Runtime/src/RuntimeLayer.*`.

**Preconditions:** OpenGL is deleted (`X3/src/Platform/OpenGL/` gone, every `#ifdef X3_USE_OPENGL` branch gone, `X3_GRAPHICS_API` option dropped). Part 2 (dynamic rendering + `beginFrame`/`endFrame`/`present`) interleaves with this part — the exact interleaving is §3.9.

**Non-goals:** no portable RHI, no virtual dispatch, no `I*` factories, no second backend. `VkStruct`s appear in interfaces deliberately. Do not design for Slang (Phase 3); do keep every descriptor table in exactly one place so codegen can replace it later.

**Build note (fresh, re-read):** `X3/CMakeLists.txt:54` is `file(GLOB_RECURSE X3_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")`, applied at `:63`. **New `.cpp` files under `X3/src/` need no CMake edit.** The `Platform/OpenGL` / `Platform/Vulkan` filter is `:56-61` and disappears with the OpenGL deletion. (The `:47`/`:50-54` citations in the older spec drafts are stale; this file has been edited since — assimp options at `:17-22`, `JPH_USE_VK/DX12/MTL OFF` at `:39-43`.)

---

## 3.0 Decisions settled here

These were left open or contradictory across the earlier drafts. They are settled now; do not re-litigate them mid-implementation.

| # | Question | Decision |
|---|---|---|
| D1 | `FRAMES_IN_FLIGHT` vs `MAX_FRAMES_IN_FLIGHT` | **One constant: `X3::FRAMES_IN_FLIGHT` in `VulkanCommon.h`.** Delete `static constexpr int MAX_FRAMES_IN_FLIGHT = 2;` (`VulkanContext.h:146`) and `getMaxFramesInFlight()` (`VulkanContext.h:88`). No `static_assert` is needed because there is only one definition left. |
| D2 | How code reaches the context | **`VulkanContext::Get()` (`VulkanContext.h:55`) stays and is the only way to obtain the context, but it may only be called from a constructor / `init()` / `onAttach()`, never from a per-frame path.** Every consumer stores a `VulkanContext*` member. `Renderer::Init(VulkanContext&)` takes an explicit reference. **No file under `X3/src/Platform/Vulkan/Vulkan{Buffer,Image,Texture,Descriptors,ComputePipeline}.{h,cpp}` may contain `VulkanContext::Get()`** — those classes receive `VulkanContext&` in their constructors. `Application` does hold a `VulkanContext* _Context`, obtained once in its constructor. |
| D3 | Staging arena growth | **The arena is a per-frame *list* of blocks, not one block that gets replaced.** It is explicitly multi-buffer within a frame. Blocks are never destroyed mid-frame, so already-recorded `vkCmdCopyBuffer` sources can never dangle, and no defer-destroy is involved in growth. |
| D4 | `DescriptorWriter` construction | **Constructed from the layout**, not from a magic `maxWrites` number. Capacity is derived from `sum(binding.count)`. This is a deliberate change from the earlier draft: a 128-element texture array silently overflows a `maxWrites = 16` reservation. |
| D5 | Descriptor pool | **Reuse the shared `m_DescriptorPool`** built at `VulkanContext.cpp:460-490`. It already has `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` (`:479`), which `deferFreeDescriptorSets` requires, and `maxSets = 1000` (`:480`). **Do not create a per-shader pool with `flags = 0`** — that design cannot free sets. |
| D6 | Frame-image event lifetime | **Raw `VulkanImage*` plus a `uint64_t frameNumber` stamp**, validated by every consumer. See §3.6. |
| D7 | Deletion-queue retire arithmetic | **Explicit `m_CompletedFrame`**, not `m_FrameNumber - retire >= FRAMES_IN_FLIGHT`. It behaves correctly during the first two frames, when the subtraction would underflow. |

---

## 3.1 What is being removed, and what each removal forces the Vulkan side to do wrong

Restated compactly so the implementer needs no other document. Every claim below was read out of the current worktree.

### (a) `Bind()` / `Unbind()` on resources

`IComputeShader::Bind/Unbind` (`X3/src/Renderer/IComputeShader.h:13-14`) is genuinely empty on the Vulkan side (`VulkanComputeShader.cpp:89-92`, `:94-97`, both with the comment *"placeholder for API compatibility with OpenGL"*). **But `Bind()` on the two buffer classes is not a no-op** — it is the only path by which a buffer ever reaches a descriptor set:

- `VulkanUniformBuffer::Bind()` → `context->registerUniformBuffer(m_BindingPoint, m_Buffer, m_Size)` (`VulkanUniformBuffer.cpp:51-57`, call at `:55`)
- `VulkanShaderStorageBuffer::Bind()` → `context->registerStorageBuffer(...)` (`VulkanShaderStorageBuffer.cpp:52-58`, call at `:56`)

`Unbind()` does not unregister (`VulkanUniformBuffer.cpp:59-62`, `VulkanShaderStorageBuffer.cpp:60-63`), so the registry only ever grows stale. Miss a `Bind()` and the binding silently keeps whatever `VkBuffer` was registered last — including a destroyed one.

### (b) `GetID()`

- `IImage2D::GetID() const → int` (`IImage2D.h:19`) → `VulkanImage2D.h:16` returns `m_ID`, a `static int s_NextID` counter (`VulkanImage2D.cpp:8`, assigned `:18`). Used as the ImGui descriptor cache key at `ViewportPanel.cpp:48` and stored at `:99`.
- `ITexture2D::GetID() const → int` (`ITexture2D.h:13`) → `VulkanTexture2D.h:16` does `static_cast<int>(reinterpret_cast<uintptr_t>(m_Image))`. **Truncates a 64-bit `VkImage` to 32 bits.** Zero readers today, but a live trap.
- `IComputeShader::GetID() → uint32_t` (`IComputeShader.h:21`) is used as a *validity* test at `Renderer.cpp:32` and `:44` — `glCreateProgram` semantics. A pipeline whose creation failed still has a nonzero counter ID.

### (c) `ChangeImageUnit` / `ChangeTextureUnit` and the global registry

| Piece | Location |
|---|---|
| `IImage2D::ChangeImageUnit(int)` | `IImage2D.h:18` → `VulkanImage2D.cpp:49-56`, `registerStorageImage(imageUnit, m_ImageView)` at `:54` |
| `ITexture2D::ChangeTextureUnit(int)` | `ITexture2D.h:12` → `VulkanTexture2D.cpp:36-43`, `registerSampledImage(...)` at `:41` |
| Registry declarations | `VulkanContext.h:39-42` (four `Bound*` structs), `:44-47` (four `register*`), `:49-52` (four `getBound*`) |
| Registry storage | `VulkanContext.h:175-178` — four `std::unordered_map<uint32_t, …>` **keyed on binding number only** |
| Consumer | `VulkanComputeShader::updateDescriptorSets`, `VulkanComputeShader.cpp:182-284`; lookups at `:214`, `:228`, `:242`, `:256` — `setNum` is never part of the key |

Correctness here is coincidental. The shader layout (`X3/res/shaders/PathTracing.comp:83-129`) happens to use disjoint binding numbers per descriptor type: storage images only binding 0, samplers only binding 1, UBOs only 0-1 (set 1), SSBOs only 0-6 (set 2). Add a second storage image or a UBO in set 2 and the wrong resource lands in the wrong set with no diagnostic — the "no resource bound" warning at `VulkanComputeShader.cpp:276` is commented out.

**Verified live bug, worth calling out because it changes the migration order:** `VulkanTexture2D`'s constructor (`VulkanTexture2D.cpp:8-17`) calls `createImage`/`createImageView`/`createSampler` and **never** calls `registerSampledImage`. Registration happens only in `ChangeTextureUnit` (`:36-43`), and a repo-wide grep over `X3/src`, `X3-Editor/src`, `X3-Runtime/src` finds **zero callers** of `ChangeTextureUnit` outside the interface/implementation hierarchy. `Renderer.cpp:238-249` creates the skybox with `ITexture2D::Create(data, w, h, SKYBOX_TEXTURE_UNIT)` and never calls `ChangeTextureUnit`. **Therefore set 0 binding 1 is never written today**, on every frame, in both builds. The compute shader samples an uninitialised descriptor. Consequence for §3.7: the migration bridge cannot source binding 1 from the registry, because the registry entry does not exist — it must use `ctx.dummyTexture()` from the very first migration step, which fixes this bug earlier than the skybox migration itself.

### (d) `AddData(offset, size, data)` — **why the signature cannot be salvaged**

`IUniformBuffer::AddData` (`IUniformBuffer.h:25`) → `VulkanUniformBuffer.cpp:68-74`; `IShaderStorageBuffer::AddData` (`IShaderStorageBuffer.h:30`) → `VulkanShaderStorageBuffer.cpp:69-75`. Both are a bare `memcpy` into `m_MappedData`.

`glBufferSubData` semantics require a *single* persistently-mapped allocation, which is exactly what both constructors create: `VulkanUniformBuffer.cpp:23-37` and `VulkanShaderStorageBuffer.cpp:23-38`, both with `VMA_ALLOCATION_CREATE_MAPPED_BIT` and `m_MappedData = allocationInfo.pMappedData` (`:36` / `:37`).

**The argument, stated exactly.** `AddData(offset, size, data)` has one address space: byte `offset` of one allocation. A per-frame ring needs the write to land at `frameIndex * stride + offsetInSlot`. The only ways to express that through this signature are (1) make the caller compute `frameIndex * stride` and pass it as `offset`, which means every call site must know the stride and the frame index — i.e. the API has not changed, the invariant has just been moved into nine call sites where it can be violated silently; or (2) hide `currentFrameOffset()` inside the implementation, which requires the buffer to read the frame index from a global, which is precisely the hidden-global coupling this layer exists to delete, and which makes the buffer's correctness depend on being called between the right pair of `beginFrame`/`endFrame` calls with nothing in the type system saying so. Both are rejected. `ENGINE_PLAN.md:129` orders this API removed and replaced with "Per-frame write into a ring slot"; the replacement signature is `write(const FrameContext& frame, const void* data, VkDeviceSize size, VkDeviceSize offsetInSlot = 0)`, where the frame slot is not addressable by the caller at all and the `FrameContext` is a token that can only be obtained from `beginFrame()`.

The bug this fixes: `Renderer::SetupGPUResources` memcpys frame N+1's camera matrix, settings, transforms, materials and lights straight over the bytes the GPU is reading for frame N. Up to `MAX_FRAMES_IN_FLIGHT = 2` (`VulkanContext.h:146`) command buffers are in flight; only the fence wait in `beginFrame` (`VulkanContext.cpp:346`) gates anything, and it gates the *slot*, not the single shared allocation.

Second bug in the same code: buffer *recreation* on size change (`Renderer.cpp:258, 270, 282, 295, 318, 332, 346`) reassigns a `shared_ptr`, running `~VulkanShaderStorageBuffer` → `vmaDestroyBuffer` (`VulkanShaderStorageBuffer.cpp:42-50`) immediately, while the previous frame's command buffer may still reference that `VkBuffer`.

### (e) `Image2DType`

Declared `IImage2D.h:8-12`, passed through `IImage2D::Create` (`IImage2D.h:16`), stored at `VulkanImage2D.h:41`, exposed by `getImageType()` (`VulkanImage2D.h:23`). `VulkanImage2D::createImage` (`VulkanImage2D.cpp:58-152`) **never reads `m_ImageType`**; every image gets identical usage flags and `VK_FORMAT_R32G32B32A32_SFLOAT`. `getImageType()` has zero callers (grep, both build configurations). It is a GLSL access-qualifier concept with no Vulkan meaning, and it hides the field that actually matters (`VkImageUsageFlags`).

### (f) Dead surface — delete, do not port

Each verified by grepping all three source trees (`X3/src`, `X3-Editor/src`, `X3-Runtime/src`, `*.h` and `*.cpp`) for the identifier; the only hits are the declaration, the override declarations, and the definitions. **Both build configurations compile today**, so nothing reaches these through an OpenGL-only path either — which is the failure mode that bit the `DXC_COMMAND` removal in Phase 0 (grep said dead; it was load-bearing for the Vulkan configuration only). For each of these, the OpenGL implementation is also in the grep output and is also unreferenced, so the "dead in one configuration only" trap does not apply.

- `IShaderStorageBuffer::ReadData` (`IShaderStorageBuffer.h:32`; `VulkanShaderStorageBuffer.cpp:77-93`; `OpenGLShaderStorageBuffer.cpp:41`). **This deletes the "ReadData synchronization" item outright** — the comment at `VulkanShaderStorageBuffer.cpp:89-90` (*"For GPU-written data, a memory barrier should be issued before this"*) followed by an unsynchronised `memcpy` at `:90` is unreachable code. If a readback is needed later, add it as a fenced staging copy, not as a mapped read. Deleting `ReadData` also lets `VulkanRingBuffer` use `HOST_ACCESS_SEQUENTIAL_WRITE` instead of the `HOST_ACCESS_RANDOM` at `VulkanShaderStorageBuffer.cpp:26`, which existed only for it.
- `IUniformBuffer::SetBindingPoint` (`IUniformBuffer.h:24`) / `IShaderStorageBuffer::SetBindingPoint` (`IShaderStorageBuffer.h:28`).
- `IComputeShader::getWorkGroupSizes` (`IComputeShader.h:22`) / `getFilePath` (`IComputeShader.h:23`).
- `ITexture2D::ChangeTextureUnit` (`ITexture2D.h:12`) — see (c).
- `VulkanImage2D::getImageType` (`VulkanImage2D.h:23`).
- `VulkanRendererAPI::GetClearColor` (`VulkanRendererAPI.h:15`, `.cpp:52`).
- `IRendererAPI::SetViewportSize` (`IRendererAPI.h:20`, `VulkanRendererAPI.cpp:19-50`) — records `vkCmdSetViewport`/`vkCmdSetScissor` for a graphics pipeline the engine does not have.
- `VulkanContext::getMaxFramesInFlight` (`VulkanContext.h:88`) — the only textual hit outside the declaration is a **commented-out** line, `ImGuiContext.cpp:179`.
- `IRendererAPI::Clear` (`IRendererAPI.h:19`) — called at `application.cpp:55` and `:57`, stores into `m_ClearColor` (`VulkanRendererAPI.cpp:16`) which nothing reads.

⇒ **`IRendererAPI`, `VulkanRendererAPI` and `Application::_RendererAPI` (`application.h:12, 28`) are entirely dead once `Clear` is dropped. Delete all three.** Under dynamic rendering the clear colour becomes `VkRenderingAttachmentInfo::clearValue`, supplied by `VulkanContext::beginSwapchainRendering(VkClearColorValue)`.

Also delete with them: `IRendererAPI::API` enum and `s_API` (`IRendererAPI.h:11-15, 23-27`), the consumer at `ProjectManager.cpp:113-119`, `Renderer::GetAPI/SetAPI` (`Renderer.h:87-88`), and `RenderSettings::rendererAPI` with its serialization (`RenderSettings.h:13-16` enum, `:28-32` member, `:45` serialize, `:61` deserialize).

---

## 3.2 The new API surface

All new code lives in `X3/src/Platform/Vulkan/`. New class names deliberately do **not** collide with the old ones, so old and new coexist during the incremental migration in §3.9.

Conventions, applied without exception: every resource class is **move-only** (copy ctor and copy assign `= delete`), default-constructible into an invalid state, and RAII-owning. There are no `shared_ptr`s anywhere in the layer. There is no virtual dispatch anywhere in the layer.

### 3.2.1 `VulkanCommon.h` (new)

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <cstddef>

namespace X3 {

// THE frames-in-flight constant. Replaces VulkanContext::MAX_FRAMES_IN_FLIGHT
// (formerly VulkanContext.h:146), which is deleted. Everything that was indexed
// by frame slot -- command buffers, fences, ring-buffer slots, descriptor sets,
// staging arenas -- is sized by this and only this.
inline constexpr uint32_t FRAMES_IN_FLIGHT = 2;

// Logs file/line and throws std::runtime_error on non-VK_SUCCESS.
void VkCheck(VkResult result, const char* expr, const char* file, int line);
#define VK_CHECK(x) ::X3::VkCheck((x), #x, __FILE__, __LINE__)

} // namespace X3
```

Call sites of the deleted `MAX_FRAMES_IN_FLIGHT` to update: `VulkanContext.cpp:293`, `:311`, `:325`, `:456`, `:758`. The two loop counters (`:325`, `:758`) are `size_t i`; leave them or change to `uint32_t i`, either compiles.

### 3.2.2 `FrameContext.h` (new) — the single object that threads the frame index

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"

namespace X3 {

class VulkanContext;

// Non-owning view of the in-progress frame. Valid ONLY between beginFrame()
// and endFrame(). Never stored by a resource, never cached across frames,
// never copied into a member. Obtainable ONLY from VulkanContext.
class FrameContext {
public:
    // 0 .. FRAMES_IN_FLIGHT-1. Indexes every per-frame ring in the engine.
    uint32_t        index()  const { return m_Index; }
    // The primary command buffer for this frame, already in the recording state.
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

**Ownership:** one `FrameContext m_Frame;` member on `VulkanContext`. `beginFrame()` fills it and returns `const FrameContext*`; callers pass `const FrameContext&` downward.

**The correctness argument, in full, because everything else depends on it.** `VulkanContext::beginFrame()` executes `vkWaitForFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame], VK_TRUE, UINT64_MAX)` at `VulkanContext.cpp:346` *before* it returns the `FrameContext`. That fence was signalled by the `vkQueueSubmit` of the previous use of frame slot `m_CurrentFrame` (`VulkanContext.cpp:429`), and it is created signalled at first use (`VulkanContext.cpp:322`, `VK_FENCE_CREATE_SIGNALED_BIT`). So when a caller holds a `FrameContext`, the GPU has provably finished every command that referenced slot `frame.index()`. The CPU may then overwrite that slot with no further synchronisation. This is why `FrameContext` is produced by `beginFrame()` and by nothing else, and why caching it across frames is forbidden rather than merely discouraged.

### 3.2.3 `VulkanBuffer.h` (new) — static buffer + per-frame ring

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

    // Grows (never shrinks) to at least `size`, geometrically (next power of two).
    // On reallocation the old (VkBuffer, VmaAllocation) go to ctx.deferDestroy()
    // and are freed only after FRAMES_IN_FLIGHT frames retire. Returns true if
    // reallocation happened -- the caller's signal that any descriptor referencing
    // this buffer must be rewritten before the next dispatch.
    bool ensureCapacity(const FrameContext& frame, VkDeviceSize size);

    // ctx.stage() + memcpy + vkCmdCopyBuffer into frame.cmd(), followed by a
    // VkBufferMemoryBarrier TRANSFER_WRITE -> SHADER_READ at
    // TRANSFER -> COMPUTE_SHADER. Asserts dstOffset + size <= capacity().
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
// Writing slot frame.index() is safe because beginFrame() waited that frame's
// fence before handing out the FrameContext (see FrameContext.h).
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
    // PER-FRAME slot size; the underlying allocation is FRAMES_IN_FLIGHT * stride.
    bool ensureCapacity(const FrameContext& frame, VkDeviceSize sizePerFrame);

    // memcpy into slot frame.index(), then vmaFlushAllocation over exactly that
    // range. Asserts offsetInSlot + size <= sizePerFrame().
    void write(const FrameContext& frame, const void* data,
               VkDeviceSize size, VkDeviceSize offsetInSlot = 0);

    template <typename T>
    void writeStruct(const FrameContext& frame, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        write(frame, &value, sizeof(T));
    }

    // Start of slot frame.index(), for scatter writes. Caller must flush(frame)
    // afterwards. Never null on a valid buffer.
    std::byte* mapped(const FrameContext& frame);
    void       flush(const FrameContext& frame);

    // Descriptor aimed at frame.index()'s slot:
    // offset = index*stride, range = sizePerFrame (NOT VK_WHOLE_SIZE).
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
    VkDeviceSize   m_Stride       = 0;
    BufferKind     m_Kind         = BufferKind::Storage;
    const char*    m_DebugName    = nullptr;
};

} // namespace X3
```

Implementation rules:

- Usage flags: `Uniform → VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`; `Storage → VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`. `VulkanBuffer` additionally gets `VK_BUFFER_USAGE_TRANSFER_DST_BIT` (it is a copy destination) and `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` with no host-access flags.
- `VulkanRingBuffer` VMA flags: `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT` (as `VulkanUniformBuffer.cpp:25-26`). **Do not** use `HOST_ACCESS_RANDOM` (`VulkanShaderStorageBuffer.cpp:26`) — that existed only for the deleted `ReadData`.
- Always `vmaFlushAllocation(allocator, alloc, slotOffset + offsetInSlot, size)` in `write()` and `flush()`. VMA no-ops on coherent memory; without it the non-coherent case is silently broken.
- **`sizePerFrame == 0` must be legal** and must allocate at least one alignment unit, so a zero-element SSBO still yields a legal descriptor. `VkBufferCreateInfo::size` must never be 0 (`VUID-VkBufferCreateInfo-size-00912`) and `VkDescriptorBufferInfo::range` must never be 0 (`VUID-VkDescriptorBufferInfo-range-00341`). Both classes clamp their effective size to `max(requested, 16)`. This is the fix for the empty-scene crash path at `Renderer.cpp:255-262`, `:268`, `:280`, `:317`, `:331`, `:345`.
- **Construction failure is fatal.** `VulkanUniformBuffer.cpp:31-32` and `VulkanShaderStorageBuffer.cpp:32-33` log and return a half-constructed object with `m_Buffer == VK_NULL_HANDLE`, which the old `Bind()` then skips — leaving the previous, destroyed buffer's handle live in the registry. `VK_CHECK` throws instead.

**Ownership:** `Renderer` owns every `VulkanBuffer` / `VulkanRingBuffer` **by value**. They are destroyed by `Renderer::Shutdown()` (§3.6), which runs from `RenderLayer::onDetach()` after `vkDeviceWaitIdle`.

### 3.2.4 `VulkanImage.h` (new) — replaces `IImage2D` / `VulkanImage2D`

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/FrameContext.h"
#include <glm/glm.hpp>

namespace X3 {

class VulkanContext;

struct ImageDesc {
    uint32_t          width     = 0;
    uint32_t          height    = 0;
    VkFormat          format    = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkImageUsageFlags usage     = 0;   // explicit; replaces Image2DType entirely
    uint32_t          mipLevels = 1;
    const char*       debugName = nullptr;
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

    // Defer-destroys the current image/view and allocates new ones.
    // id() is UNCHANGED; generation() is incremented. Any descriptor or ImGui
    // registration referencing this image must be refreshed -- compare
    // generation(), not id(). See ViewportPanel in section 3.6.
    void recreate(const FrameContext& frame, const ImageDesc& desc);

    // Records a VkImageMemoryBarrier into frame.cmd() from the tracked
    // (m_Layout, m_LastAccess, m_LastStage) to (newLayout, dstAccess, dstStage),
    // then updates the tracked state. No-op if already in newLayout with a
    // superset access mask.
    void transition(const FrameContext& frame, VkImageLayout newLayout,
                    VkAccessFlags dstAccess, VkPipelineStageFlags dstStage);

    // Same-layout execution+memory barrier. REQUIRED for the accumulation
    // read-modify-write across frames (PathTracing.comp does imageLoad then
    // imageStore on the same image), which transition() would skip because the
    // layout is unchanged.
    void barrier(const FrameContext& frame,
                 VkAccessFlags dstAccess, VkPipelineStageFlags dstStage);

    VkImage       handle()     const { return m_Image; }
    VkImageView   view()       const { return m_View; }
    VkImageLayout layout()     const { return m_Layout; }
    VkExtent2D    extent()     const { return m_Extent; }
    VkFormat      format()     const { return m_Format; }
    glm::ivec2    dimensions() const { return { int(m_Extent.width), int(m_Extent.height) }; }
    bool          valid()      const { return m_Image != VK_NULL_HANDLE; }

    // Stable object identity from a process-wide atomic counter. Assigned ONCE,
    // in the constructor. NOT changed by recreate(). Replaces IImage2D::GetID()
    // as a cache key: never truncates, never a raw handle, never reused.
    uint64_t id()         const { return m_Id; }
    // Incremented by every successful recreate(). A cache keyed on id() must
    // also store generation() and refresh when it differs.
    uint64_t generation() const { return m_Generation; }

    VkDescriptorImageInfo storageDescriptor() const; // { VK_NULL_HANDLE, view, GENERAL }

private:
    VulkanContext*       m_Ctx        = nullptr;
    VkImage              m_Image      = VK_NULL_HANDLE;
    VmaAllocation        m_Allocation = VK_NULL_HANDLE;
    VkImageView          m_View       = VK_NULL_HANDLE;
    VkExtent2D           m_Extent     {};
    VkFormat             m_Format     = VK_FORMAT_UNDEFINED;
    VkImageLayout        m_Layout     = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags        m_LastAccess = 0;
    VkPipelineStageFlags m_LastStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    uint64_t             m_Id         = 0;
    uint64_t             m_Generation = 0;
};

} // namespace X3
```

**Behaviour changes vs `VulkanImage2D`, all deliberate:**

1. The constructor must **not** call `beginSingleTimeCommands`/`endSingleTimeCommands` the way `VulkanImage2D.cpp:25-30` does. That path ends in `vkQueueWaitIdle` (`VulkanContext.cpp:530`) and is invoked from inside a recording frame at `Renderer.cpp:202-203`, stalling the whole pipeline on every resolution change. Instead leave the image in `VK_IMAGE_LAYOUT_UNDEFINED` and let the first `transition(frame, VK_IMAGE_LAYOUT_GENERAL, …)` in `Renderer::Draw` do it inside the frame command buffer. `UNDEFINED → GENERAL` is always legal.
2. The `unsigned char* data` upload path of `VulkanImage2D::createImage` (`VulkanImage2D.cpp:92-152`, including the RGBA8→RGBA32F conversion loop) has exactly one caller and it passes `nullptr` (`Renderer.cpp:202-203`). **Do not port it.** Storage images are always created empty.
3. Fix the stage masks while porting: `VulkanImage2D.cpp:209` uses `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` as the source stage when leaving `SHADER_READ_ONLY_OPTIMAL`, and `:230` targets `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`. The consumer in this engine is a **compute** shader. `transition()` takes the stage explicitly, so this class of bug becomes unrepresentable — but the call sites in `Renderer::Draw` must pass `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`, and the post-dispatch barrier must pass `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT` (editor samples it through ImGui; runtime blits it).

### 3.2.5 `VulkanTexture.h` (new) — replaces `ITexture2D` / `VulkanTexture2D`

```cpp
#pragma once
#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/FrameContext.h"
#include <functional>

namespace X3 {

class VulkanContext;

struct SamplerDesc {
    VkFilter             filter      = VK_FILTER_LINEAR;
    VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerMipmapMode  mipmapMode  = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    bool operator==(const SamplerDesc&) const = default;
};

struct TextureDesc {
    uint32_t    width     = 0;
    uint32_t    height    = 0;
    VkFormat    format    = VK_FORMAT_R8G8B8A8_SRGB;
    uint32_t    mipLevels = 1;
    SamplerDesc sampler   {};
    const char* debugName = nullptr;
};

// Immutable sampled texture: image + view + a sampler BORROWED from the
// context's sampler cache (not owned; never destroy it here).
class VulkanTexture {
public:
    VulkanTexture() = default;

    // In-frame construction. `pixels` must be width*height*bytesPerPixel(format)
    // bytes and is copied immediately into the frame staging arena. The copy and
    // the UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL transitions are
    // recorded into frame.cmd(): no queue wait, no mid-frame submit.
    VulkanTexture(VulkanContext& ctx, const FrameContext& frame,
                  const TextureDesc& desc, const void* pixels);

    // Out-of-frame construction for init/teardown paths ONLY (the context's own
    // dummy texture, and anything that runs before the first beginFrame()).
    // Uses beginSingleTimeCommands/endSingleTimeCommands and therefore blocks.
    // Asserts that no frame is active.
    VulkanTexture(VulkanContext& ctx, const TextureDesc& desc, const void* pixels);

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
    VkSampler      m_Sampler    = VK_NULL_HANDLE;   // borrowed from ctx cache
    uint64_t       m_Id         = 0;
};

} // namespace X3
```

Add a `std::hash<X3::SamplerDesc>` specialisation next to it (hash the three enum values), because the context's cache is an `unordered_map`.

**Bug fixed while porting:** `VulkanTexture2D::transitionImageLayout` sets `destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` for the `TRANSFER_DST → SHADER_READ_ONLY_OPTIMAL` transition (`VulkanTexture2D.cpp:203-207`, the assignment at `:207`), but the skybox is sampled from a **compute** shader (`PathTracing.comp:85`, sampled at `:208`). The new class must use `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`. Today this is masked by the `vkQueueWaitIdle` at `VulkanContext.cpp:530`; the in-frame upload path removes that mask, so **this fix must land in the same commit as the skybox migration**, not after it.

**Sampler ownership:** the per-texture `vkCreateSampler` at `VulkanTexture2D.cpp:154-179` (and its `vkDestroySampler` at `:26`) is replaced by `VulkanContext::getSampler(const SamplerDesc&)`, backed by an `unordered_map<SamplerDesc, VkSampler>` on the context, destroyed in `VulkanContext::cleanup()`. Also fix `maxLod = 0.0f` (`VulkanTexture2D.cpp:174`) → `VK_LOD_CLAMP_NONE`; the current value silently disables mips even when they exist. Rationale for centralising: Phase 2 wants 128 material textures, and `maxSamplerAllocationCount` is as low as 4000 on some desktop drivers and lower under MoltenVK. One `VkSampler` handle may legally be repeated across every element of a combined-image-sampler array.

### 3.2.6 `VulkanDescriptors.h` (new)

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
    uint32_t           count   = 1;   // > 1 == array binding
    VkShaderStageFlags stages  = VK_SHADER_STAGE_COMPUTE_BIT;
};

// Owns one VkDescriptorSetLayout and keeps its binding table, which
// DescriptorWriter uses for capacity reservation AND completeness validation.
// In Phase 3 this table becomes Slang-reflection-generated.
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
    const DescriptorBindingDesc* find(uint32_t binding) const;   // nullptr if absent

    // Sum of `count` over bindings of image / buffer descriptor types.
    // DescriptorWriter reserves exactly these.
    uint32_t imageDescriptorCount()  const;
    uint32_t bufferDescriptorCount() const;

private:
    VulkanContext*                     m_Ctx    = nullptr;
    VkDescriptorSetLayout              m_Layout = VK_NULL_HANDLE;
    std::vector<DescriptorBindingDesc> m_Bindings;
};

// FRAMES_IN_FLIGHT descriptor sets sharing one layout.
// THIS IS THE REPLACEMENT FOR THE SINGLE SET AT VulkanComputeShader.cpp:113-118.
// Set i is only ever written while frame i's fence is signalled.
class VulkanDescriptorSetRing {
public:
    VulkanDescriptorSetRing() = default;
    VulkanDescriptorSetRing(VulkanContext& ctx, const VulkanDescriptorSetLayout& layout);
    ~VulkanDescriptorSetRing();   // -> ctx.deferFreeDescriptorSets(m_Sets)

    VulkanDescriptorSetRing(VulkanDescriptorSetRing&&) noexcept;
    VulkanDescriptorSetRing& operator=(VulkanDescriptorSetRing&&) noexcept;
    VulkanDescriptorSetRing(const VulkanDescriptorSetRing&) = delete;

    VkDescriptorSet get(const FrameContext& frame) const { return m_Sets[frame.index()]; }
    bool valid() const { return m_Sets[0] != VK_NULL_HANDLE; }

private:
    VulkanContext*                                m_Ctx = nullptr;
    std::array<VkDescriptorSet, FRAMES_IN_FLIGHT> m_Sets{};
};

// Accumulates the writes for ONE descriptor set and flushes them in a single
// vkUpdateDescriptorSets. Owns its VkDescriptor*Info storage, which is why
// VulkanComputeShader's keep-alive maps (VulkanComputeShader.h:80-82) disappear.
// Stack-allocate one per set per frame; it is a scratch object.
class DescriptorWriter {
public:
    // Capacity is derived from the layout, NOT from a caller-supplied number.
    // Reserves layout.imageDescriptorCount() image infos and
    // layout.bufferDescriptorCount() buffer infos.
    DescriptorWriter(VulkanContext& ctx,
                     const VulkanDescriptorSetLayout& layout,
                     VkDescriptorSet dst);

    DescriptorWriter& uniformBuffer(uint32_t binding, const VulkanRingBuffer&, const FrameContext&);
    DescriptorWriter& uniformBuffer(uint32_t binding, const VulkanBuffer&);
    DescriptorWriter& storageBuffer(uint32_t binding, const VulkanRingBuffer&, const FrameContext&);
    DescriptorWriter& storageBuffer(uint32_t binding, const VulkanBuffer&);
    DescriptorWriter& storageImage (uint32_t binding, const VulkanImage&);
    DescriptorWriter& sampledImage (uint32_t binding, const VulkanTexture&);

    // ---- Array write. Writes layout.find(binding)->count descriptors in ONE
    // VkWriteDescriptorSet. `textures.size()` must equal that count exactly.
    // A null element is replaced by ctx.dummyTexture(); there are no holes.
    // See section 3.5 for the contiguity invariant this depends on.
    DescriptorWriter& sampledImageArray(uint32_t binding,
                                        std::span<const VulkanTexture* const> textures);

    // Migration bridge only. Deleted in the final cleanup commit.
    DescriptorWriter& raw(uint32_t binding, VkDescriptorType, const VkDescriptorBufferInfo&);
    DescriptorWriter& raw(uint32_t binding, VkDescriptorType, const VkDescriptorImageInfo&);

    // Resolves the deferred info pointers, asserts (debug only) that every
    // binding in the layout was written exactly once with the declared type and
    // descriptorCount, then issues one vkUpdateDescriptorSets.
    void flush();

private:
    struct PendingWrite {
        uint32_t         binding;
        VkDescriptorType type;
        uint32_t         count;
        uint32_t         infoBase;   // index into m_ImageInfos or m_BufferInfos
        bool             isImage;
    };

    VkDevice                            m_Device;
    const VulkanDescriptorSetLayout*    m_Layout;
    VkDescriptorSet                     m_Dst;
    VulkanContext*                      m_Ctx;
    std::vector<VkDescriptorBufferInfo> m_BufferInfos;
    std::vector<VkDescriptorImageInfo>  m_ImageInfos;
    std::vector<PendingWrite>           m_Pending;
};

} // namespace X3
```

### 3.2.7 `VulkanComputePipeline.h` (new) — replaces `IComputeShader` / `VulkanComputeShader`

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
    // FULL path to the .spv, extension included. Unlike
    // VulkanComputeShader::loadShaderFromFile (VulkanComputeShader.cpp:302),
    // ".spv" is NOT appended here.
    std::filesystem::path spirvPath;
    std::string           entryPoint = "main";
    // index == descriptor set number; set indices must be contiguous from 0.
    std::vector<std::vector<DescriptorBindingDesc>> setLayouts;
    uint32_t              pushConstantSize = 0;   // 0 == none
    const char*           debugName        = nullptr;
};

class VulkanComputePipeline {
public:
    VulkanComputePipeline() = default;
    // Does NOT throw on failure: logs and leaves valid() == false.
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

    // vkCmdBindPipeline + vkCmdBindDescriptorSets(firstSet = 0) + vkCmdDispatch.
    // `sets` must be exactly setCount() entries, index == set number, already
    // written for this frame. Arguments are group COUNTS, not local sizes.
    // Inserts NO barriers -- the caller owns synchronisation.
    void dispatch(const FrameContext& frame,
                  std::span<const VkDescriptorSet> sets,
                  uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) const;

    void pushConstants(const FrameContext& frame, const void* data, uint32_t size) const;

    VkPipeline                   handle() const { return m_Pipeline; }
    VkPipelineLayout             layout() const { return m_PipelineLayout; }
    const std::filesystem::path& path()   const { return m_Path; }

private:
    VulkanContext*                         m_Ctx              = nullptr;
    std::filesystem::path                  m_Path;
    VkShaderModule                         m_Module           = VK_NULL_HANDLE;
    VkPipeline                             m_Pipeline         = VK_NULL_HANDLE;
    VkPipelineLayout                       m_PipelineLayout   = VK_NULL_HANDLE;
    std::vector<VulkanDescriptorSetLayout> m_SetLayouts;
    uint32_t                               m_PushConstantSize = 0;
};

} // namespace X3
```

**Removed relative to `VulkanComputeShader`:** `Bind()`/`Unbind()` (`VulkanComputeShader.cpp:89-97`); `m_WorkGroupSizes` and `setWorkGroupSizes` (`VulkanComputeShader.h:39`) — misnamed, it actually held *group counts* (`Renderer.cpp:372-376`), and is now a `dispatch()` parameter; `updateDescriptorSets()` and every registry lookup (`VulkanComputeShader.cpp:182-284`); `m_DescriptorSetsAllocated` and the lazy allocation branch (`VulkanComputeShader.h:75`, `.cpp:113-115`); the keep-alive info maps (`VulkanComputeShader.h:80-82`); the post-dispatch memory barrier (`VulkanComputeShader.cpp:139-154`), which becomes the caller's explicit barrier; and the hardcoded three-set table in the constructor (`VulkanComputeShader.cpp:19-42`), which moves to one named constant in `Renderer.cpp` (§3.7.1).

**Ownership:** `Renderer` owns pipelines by value in `std::unordered_map<ShaderType, VulkanComputePipeline>`. `GetOrLoadShader` returns a raw `VulkanComputePipeline*` into that map, which is stable across rehash because `std::unordered_map` never moves its elements.

### 3.2.8 `VulkanContext` — additions owned by this part

Part 2 owns the frame-lifecycle and dynamic-rendering changes. This part adds the following, and nothing else, to `X3/src/Platform/Vulkan/VulkanContext.h`:

```cpp
public:
    // ---- Frame identity -----------------------------------------------------
    // Non-null only between beginFrame() and endFrame().
    const FrameContext* currentFrame() const;
    uint64_t frameNumber()    const { return m_FrameNumber; }
    // Highest frame number whose submission is known complete (from the fence
    // wait at the top of beginFrame). 0 until FRAMES_IN_FLIGHT frames have run.
    uint64_t completedFrame() const { return m_CompletedFrame; }

    // ---- Per-frame staging arena -------------------------------------------
    // Bump allocator over a LIST of host-visible blocks per frame in flight.
    // Reset (offsets only, blocks retained) in beginFrame() after the fence wait.
    struct StagingAlloc { VkBuffer buffer; VkDeviceSize offset; std::byte* ptr; };
    StagingAlloc stage(const FrameContext& frame, VkDeviceSize size,
                       VkDeviceSize alignment = 16);

    // ---- Deferred destruction ----------------------------------------------
    // Recorded with retireFrame = m_FrameNumber; destroyed once
    // retireFrame <= m_CompletedFrame. Drained at the top of beginFrame().
    void deferDestroy(VkBuffer, VmaAllocation);
    void deferDestroy(VkImage, VmaAllocation, VkImageView);
    void deferFreeDescriptorSets(std::span<const VkDescriptorSet>);

    // ---- Shared resources ---------------------------------------------------
    VkSampler            getSampler(const SamplerDesc&);   // cached, context-owned
    const VulkanBuffer&  dummyStorageBuffer() const;       // 256 B device-local, zeroed
    const VulkanBuffer&  dummyUniformBuffer() const;       // 256 B device-local, zeroed
    const VulkanTexture& dummyTexture() const;             // 1x1 opaque black, SRGB

    // ---- Cached device limits ----------------------------------------------
    const VkPhysicalDeviceLimits& limits() const { return m_Limits; }

private:
    FrameContext  m_Frame;
    bool          m_FrameActive    = false;
    uint64_t      m_FrameNumber    = 0;
    uint64_t      m_CompletedFrame = 0;
    VkPhysicalDeviceLimits m_Limits{};

    struct PendingDelete {
        uint64_t      retireFrame  = 0;
        VkBuffer      buffer       = VK_NULL_HANDLE;
        VkImage       image        = VK_NULL_HANDLE;
        VkImageView   view         = VK_NULL_HANDLE;
        VmaAllocation allocation   = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> sets;
    };
    std::vector<PendingDelete> m_DeletionQueue;
    void drainDeletionQueue();       // destroys retireFrame <= m_CompletedFrame
    void drainDeletionQueueFully();  // vkDeviceWaitIdle + destroy everything

    struct StagingBlock { VkBuffer buffer; VmaAllocation alloc; std::byte* mapped;
                          VkDeviceSize size; VkDeviceSize used; };
    std::array<std::vector<StagingBlock>, FRAMES_IN_FLIGHT> m_Staging;
    std::array<uint32_t, FRAMES_IN_FLIGHT>                  m_StagingCursor{};

    std::unordered_map<SamplerDesc, VkSampler> m_SamplerCache;
    std::unique_ptr<VulkanBuffer>  m_DummyStorage, m_DummyUniform;
    std::unique_ptr<VulkanTexture> m_DummyTexture;
```

Frame-number bookkeeping, exactly:

- `m_Limits` is cached in `pickPhysicalDevice`; `VulkanContext.cpp:90-92` already calls `vkGetPhysicalDeviceProperties` — store `.limits` there instead of discarding it.
- In `beginFrame()`, immediately after the fence wait succeeds (`VulkanContext.cpp:346`): `if (m_FrameNumber >= FRAMES_IN_FLIGHT) m_CompletedFrame = m_FrameNumber - FRAMES_IN_FLIGHT;` then `drainDeletionQueue();` then reset `m_Staging[m_CurrentFrame]` cursors, then fill `m_Frame` and set `m_FrameActive = true`.
- `present()` (Part 2) increments `m_CurrentFrame` and `m_FrameNumber` together, at the very end, and clears `m_FrameActive`.
- `cleanup()` (`VulkanContext.cpp:741`) calls `drainDeletionQueueFully()` first, then destroys the sampler cache, the dummy resources and the staging blocks.
- `beginSingleTimeCommands` (`VulkanContext.h:67`, `.cpp:502-533`) **stays**, but gains `assert(!m_FrameActive)`. Its only remaining legitimate callers are init/teardown paths: the `VulkanTexture` out-of-frame constructor and the ImGui font upload at `ImGuiContext.cpp:192-197`. All in-frame uploads go through `stage()` + `frame.cmd()`. After this part, `grep -rn 'vkQueueWaitIdle\|vkDeviceWaitIdle' X3/src/Platform/Vulkan/` should return only: `endSingleTimeCommands` (`VulkanContext.cpp:530`), `recreateSwapchain` (`:665`), `cleanup` (`:743`), and `drainDeletionQueueFully`.

**Staging arena mechanics (resolves the open "arena sizing and growth" item).** Each frame slot owns a `std::vector<StagingBlock>`. `stage(frame, size, alignment)`:

1. Align `m_Staging[i][cursor].used` up to `alignment`.
2. If `used + size <= block.size`, carve it and return `{block.buffer, used, block.mapped + used}`; advance `used`.
3. Otherwise advance `cursor`. If a next block already exists and fits, use it. Otherwise **append a new block** of `max(8 MiB, nextPow2(size))`, log once at INFO, and carve from it.
4. `beginFrame()` resets `cursor = 0` and every block's `used = 0`. **Blocks are never destroyed until `cleanup()`.**

This is explicitly a multi-buffer arena within one frame. That is what makes growth safe: an already-recorded `vkCmdCopyBuffer` names the block's `VkBuffer` by handle, and no block is ever replaced or destroyed while a frame that references it can still be in flight. `StagingAlloc` returns the buffer per call precisely so that a single frame's copies may source from different blocks.

**Descriptor pool budget (concrete numbers, D5).** Reusing `m_DescriptorPool` (`VulkanContext.cpp:460-490`): three pipelines × three sets × two frames = **18 sets**, against `maxSets = 1000` (`:480`). Descriptor counts: `STORAGE_IMAGE` 6, `COMBINED_IMAGE_SAMPLER` 6, `UNIFORM_BUFFER` 12, `STORAGE_BUFFER` 42 — against 1000 of each (`:464-474`). ImGui adds one font texture plus at most `FRAMES_IN_FLIGHT` viewport textures. Enormous headroom; **change nothing in `createDescriptorPool` in this part.** When Phase 2's 128-element material-texture array lands, `COMBINED_IMAGE_SAMPLER` becomes `(1 + 128) × 3 × 2 = 774` plus ImGui — still under 1000 but with no headroom for a fourth pipeline, so at that point raise the `COMBINED_IMAGE_SAMPLER` entry at `VulkanContext.cpp:465` from 1000 to 4096. **`maxSets` is already 1000; do not change it, and specifically do not lower it to 256** — that would be a 4× cut and would break ImGui's per-texture allocations.

### 3.2.9 Files deleted at the end of this part

```
X3/src/Renderer/IComputeShader.h        X3/src/Renderer/IComputeShader.cpp
X3/src/Renderer/IImage2D.h              X3/src/Renderer/IImage2D.cpp
X3/src/Renderer/ITexture2D.h            X3/src/Renderer/ITexture2D.cpp
X3/src/Renderer/IUniformBuffer.h        X3/src/Renderer/IUniformBuffer.cpp
X3/src/Renderer/IShaderStorageBuffer.h  X3/src/Renderer/IShaderStorageBuffer.cpp
X3/src/Renderer/IRenderingContext.h     X3/src/Renderer/IRenderingContext.cpp
X3/src/Renderer/IRendererAPI.h          X3/src/Renderer/IRendererAPI.cpp
X3/src/Platform/Vulkan/VulkanComputeShader.h        .cpp
X3/src/Platform/Vulkan/VulkanImage2D.h              .cpp
X3/src/Platform/Vulkan/VulkanTexture2D.h            .cpp
X3/src/Platform/Vulkan/VulkanUniformBuffer.h        .cpp
X3/src/Platform/Vulkan/VulkanShaderStorageBuffer.h  .cpp
X3/src/Platform/Vulkan/VulkanRendererAPI.h          .cpp
```

**Deletion order matters:** `IRendererAPI.h:4` is `#include "Renderer/IComputeShader.h"`, so `IRendererAPI.h` must be deleted before or with `IComputeShader.h`, never after. Stray includes that break otherwise, neither of which appears in any earlier edit table: `X3/src/Platform/Windows/GLFWWindow.cpp:8` (`#include "Renderer/IRendererAPI.h"`, unguarded and unused) and `X3-Editor/src/ImGuiContext.cpp:20` (same). Remove both. `X3/src/X3.h:16-21` loses all six `Renderer/I*.h` includes.

---

## 3.3 Per-frame rings, in detail

**How many copies:** exactly `FRAMES_IN_FLIGHT = 2`. One `VkBuffer` per ring, not N — `FRAMES_IN_FLIGHT` slots inside a single VMA allocation. One allocation, one persistent map pointer, one destroy, and the descriptor for slot `i` is just `{buffer, i*stride, sizePerFrame}`.

**Stride:**

```
A = max( kind == Uniform ? limits().minUniformBufferOffsetAlignment
                         : limits().minStorageBufferOffsetAlignment,
         limits().nonCoherentAtomSize,
         VkDeviceSize(1) )
stride = alignUp(max(sizePerFrame, 16), A)
total  = FRAMES_IN_FLIGHT * stride
```

`nonCoherentAtomSize` is folded in unconditionally rather than conditioned on the memory type, because VMA may place the allocation in non-coherent memory and `vmaFlushAllocation` rounds to that granularity anyway; over-aligning costs at most a few hundred bytes per ring.

**How the frame index threads through — the rule.** The frame index is never read from a global. It travels as `const FrameContext&`, obtained in exactly two places in the whole engine:

1. `Application::run` calls `_Context->beginFrame()` and holds the `const FrameContext*` for that loop iteration.
2. `ImGuiContext::EndFrame` (editor), `RuntimeLayer::onUpdate` (runtime) and `RenderLayer::onUpdate` call `m_Ctx->currentFrame()`, because `ILayer::onUpdate()` takes no parameter and changing that signature across five layers is churn this phase does not need. Each null-checks the result and returns early if null.

Below those points, every function that touches GPU state takes `const FrameContext&` as its first parameter: `Renderer::Render`, `Renderer::SetupGPUResources`, `Renderer::Draw`, and every recording/writing method on the resource classes. **`VulkanContext::getCurrentFrame()` returning a bare `uint32_t` (`VulkanContext.h:58`) is deleted** — removing it is what stops the index leaking back into ad-hoc call sites. `getCurrentCommandBuffer()` (`VulkanContext.h:59`) survives only until the last non-`FrameContext` caller is gone; then it goes too.

**How a caller writes for the current frame:**

```cpp
// whole-struct case
CameraUBOData cam{ pScene->CameraTransform, pScene->CameraFocalLength, {} };
m_CameraUBO.writeStruct(frame, cam);

// dynamically sized case
const VkDeviceSize bytes = sizeof(MeshEntityHandle) * pScene->MeshEntityLookupTable.size();
m_EntityLookupSSBO.ensureCapacity(frame, bytes);          // may reallocate
m_EntityLookupSSBO.write(frame, pScene->MeshEntityLookupTable.data(), bytes);
```

`write()` computes `m_Mapped + frame.index() * m_Stride + offsetInSlot`, memcpys, and flushes exactly that range. There is no way to name another frame's slot.

**Growth:** `ensureCapacity` never shrinks and grows to the next power of two, so a scene with a fluctuating entity count does not reallocate every frame. Reallocation hands the old `(VkBuffer, VmaAllocation)` to `ctx.deferDestroy()`, freed only after `FRAMES_IN_FLIGHT` further frames retire. **This is what fixes the immediate-`vmaDestroyBuffer`-while-in-flight bug at `Renderer.cpp:258, 270, 282, 295, 318, 332, 346`.** Because all sets are rewritten every frame (§3.4), a reallocation needs no extra descriptor bookkeeping; the `bool` return exists for future dirty-tracking and for asserts.

**Deletion-queue mechanics.** `deferDestroy` pushes with `retireFrame = m_FrameNumber`. `drainDeletionQueue()` — called at the top of `beginFrame()`, immediately after the fence wait and before anything else — erases and destroys every entry with `retireFrame <= m_CompletedFrame`. Using `m_CompletedFrame` rather than `m_FrameNumber - FRAMES_IN_FLIGHT` is deliberate: the subtraction underflows on `uint64_t` during the first two frames.

---

## 3.4 Descriptor management

### 3.4.1 What is wrong today, restated as requirements

`VulkanComputeShader::Dispatch` (`VulkanComputeShader.cpp:99-155`) allocates **one** set per layout on first dispatch (`:113-115` → `allocateDescriptorSets`, `:157-180`) and then calls `updateDescriptorSets()` on **every** dispatch (`:118`), i.e. `vkUpdateDescriptorSets` at `:282` against sets that frame N−1's still-pending command buffer has bound at `:125-134`. Two distinct violations:

1. **Illegal API call.** A descriptor set's contents must not be updated while it is in use by a command buffer in the pending state, unless the binding was created with `UPDATE_AFTER_BIND` or `UPDATE_UNUSED_WHILE_PENDING`. The layouts here are created with no `VkDescriptorSetLayoutBindingFlagsCreateInfo` at all (`VulkanComputeShader.cpp:381-384` — `layoutInfo` has no `pNext`), so neither flag is present. The validation layers report **VUID-vkUpdateDescriptorSets-None-03047**.
2. **Wrong data even if the call were legal.** Descriptors are consumed at *execution* time, not record time. Once per-frame ring slots exist, frame N−1's pending dispatch would read frame N's camera matrix and buffer offsets.

Requirements for the replacement:

1. `FRAMES_IN_FLIGHT` sets per (pipeline, set index), indexed by `frame.index()`.
2. Sets written only after the fence wait for that frame index.
3. Binding identity is `(set, binding)`, never `binding` alone.
4. Every declared binding written every frame, or a hard assert.
5. Descriptor info storage owned by the writer, not smuggled into member maps.

### 3.4.2 Allocation

- Pool: the shared `m_DescriptorPool` (D5, §3.2.8).
- `VulkanDescriptorSetRing`'s constructor does **one** `vkAllocateDescriptorSets` with `descriptorSetCount = FRAMES_IN_FLIGHT` and `pSetLayouts` set to the same layout handle repeated `FRAMES_IN_FLIGHT` times.
- Allocation happens **once**, at pipeline-creation time in `Renderer::GetOrLoadShader`, never lazily inside dispatch.
- Destruction: `~VulkanDescriptorSetRing` calls `ctx.deferFreeDescriptorSets(m_Sets)`, which `vkFreeDescriptorSets` them after `FRAMES_IN_FLIGHT` retired frames. This is why `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` (`VulkanContext.cpp:479`) is load-bearing.
- **Set indices must be contiguous from 0.** `VulkanComputeShader::createDescriptorSetLayouts` resizes with `VK_NULL_HANDLE` fill (`VulkanComputeShader.cpp:363`), so a gap silently produced a null layout. `VulkanComputePipeline`'s constructor fails loudly instead: if any entry of `ComputePipelineDesc::setLayouts` is empty, log and leave `valid() == false`.

### 3.4.3 When sets are updated

Once per frame, in `Renderer::Draw`, immediately before `dispatch()`, after every buffer write and image transition has been recorded. Rewriting all sets every frame is deliberate for this phase: it is what the old code effectively did, it is 11 descriptors, and dirty tracking is a Phase 5 render-graph concern. The *correctness* fix is that the set being written belongs to a frame the GPU has provably finished.

### 3.4.4 Replacing the global registry

Delete `VulkanContext.h:39-42` (four `Bound*` structs), `:44-47` (four `register*` declarations), `:49-52` (four `getBound*` getters), `:175-178` (four maps), and the four implementations at `VulkanContext.cpp:810-824`.

| Old mechanism | New mechanism |
|---|---|
| `registerStorageImage(unit, view)` from `VulkanImage2D::ChangeImageUnit` (`VulkanImage2D.cpp:54`) | `DescriptorWriter::storageImage(binding, image)` at the call site that knows the set |
| `registerSampledImage(unit, view, sampler)` from `VulkanTexture2D::ChangeTextureUnit` (`VulkanTexture2D.cpp:41`) — never actually called | `DescriptorWriter::sampledImage(binding, texture)` |
| `registerUniformBuffer(bp, buf, size)` from `VulkanUniformBuffer::Bind` (`VulkanUniformBuffer.cpp:55`) | `DescriptorWriter::uniformBuffer(binding, ring, frame)` |
| `registerStorageBuffer(bp, buf, size)` from `VulkanShaderStorageBuffer::Bind` (`VulkanShaderStorageBuffer.cpp:56`) | `DescriptorWriter::storageBuffer(binding, ringOrStatic[, frame])` |
| Lookup `boundX.find(binding.binding)` (`VulkanComputeShader.cpp:214, 228, 242, 256`) | Nothing. There is no lookup; the writer is told the resource directly. |
| Silent skip when not found (`VulkanComputeShader.cpp:273-277`, warning commented out at `:276`) | `DescriptorWriter::flush()` asserts completeness against the layout |

Set identity becomes structural: `rings[0]` is a different object from `rings[2]`, and each writer validates against `pipeline->setLayout(0)` vs `setLayout(2)`. A `(set, binding)` collision is not representable, because a binding number is only ever interpreted relative to the layout it is flushed against.

The single binding table moves to one named constant in `Renderer.cpp` (§3.7.1). That is the object Phase 3's Slang reflection codegen will generate; keeping it in exactly one place is the "don't paint into a corner" requirement.

### 3.4.5 The always-write rule and dummy resources

`Renderer.cpp:289-302` skips the light SSBO entirely when `count == 0` (the `if (count > 0)` at `:292`), leaving set 2 binding 6 unwritten. `Renderer.cpp:247` sets `m_SkyboxTexture = nullptr` when a scene has no skybox, and as established in §3.1(c) binding 1 is *never* written at all. The `flush()` assert turns both into immediate, loud failures. Fixes, in preference order:

1. **Lights:** `m_LightSSBO.ensureCapacity(frame, std::max<VkDeviceSize>(sizeof(LightData) * count, sizeof(LightData)))` and write whatever is there. `u_LightCount` is already uploaded in the settings UBO (`Renderer.cpp:227`) and tells the shader not to read it. **Use this** — no early-out, so binding 6 is always valid.
2. **Skybox:** `ctx.dummyTexture()` (1×1 opaque black, `VK_FORMAT_R8G8B8A8_SRGB`, left in `SHADER_READ_ONLY_OPTIMAL`) when `!m_SkyboxTexture.valid()`.
3. `ctx.dummyStorageBuffer()` / `dummyUniformBuffer()` exist for bindings where no real buffer exists at all. Reserve them for that case.

### 3.4.6 `DescriptorWriter::sampledImageArray` and the contiguity invariant — **owned here, not by Phase 2**

Phase 2 needs `layout(set = 0, binding = 2) uniform sampler2D u_MaterialTextures[128]`. No `DescriptorWriter` method as previously drafted could write more than one descriptor: every overload wrote `descriptorCount = 1`. That gap is closed here, in the layer, because the layer owns descriptor writing. Phase 2 consumes it; Phase 2 does not build it.

```cpp
DescriptorWriter& sampledImageArray(uint32_t binding,
                                    std::span<const VulkanTexture* const> textures);
```

Semantics:

- `const DescriptorBindingDesc* b = m_Layout->find(binding); assert(b && b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);`
- `assert(textures.size() == b->count);` — the array is **fully written**, never partially. There is no `PARTIALLY_BOUND`, so an unwritten element is undefined behaviour on access.
- Append `b->count` `VkDescriptorImageInfo` entries to `m_ImageInfos` in one uninterrupted loop, recording `infoBase = m_ImageInfos.size()` before the loop. A null or `!valid()` element becomes `m_Ctx->dummyTexture().descriptor()`.
- Push one `PendingWrite{ binding, type, b->count, infoBase, /*isImage=*/true }`.
- `flush()` builds `VkWriteDescriptorSet{ .dstBinding = binding, .dstArrayElement = 0, .descriptorCount = count, .pImageInfo = m_ImageInfos.data() + infoBase }`.

**The contiguous-info-block invariant, stated as three enforced properties.** `VkWriteDescriptorSet::pImageInfo` is a pointer to an array of `descriptorCount` structs that must be contiguous and must remain valid until `vkUpdateDescriptorSets` returns. Therefore:

1. **No reallocation.** The constructor does `m_ImageInfos.reserve(layout.imageDescriptorCount())` and `m_BufferInfos.reserve(layout.bufferDescriptorCount())`, both derived from `sum(binding.count)` over the layout. Every append asserts `size() < capacity()`. This is the reason `DescriptorWriter` is constructed from the layout rather than from a `maxWrites` number (decision D4): with a `maxWrites = 16` reservation, a 128-element array silently reallocates the vector, every previously-taken `pImageInfo` dangles, and `vkUpdateDescriptorSets` reads freed memory — a bug that would appear as sporadic wrong textures, not as a crash.
2. **No interleaving.** The `count` infos for one binding are appended by a single loop with no other append in between, so `[infoBase, infoBase + count)` is contiguous by construction.
3. **Deferred pointer resolution.** `VkWriteDescriptorSet::pImageInfo` is computed in `flush()` as `m_ImageInfos.data() + infoBase`, never captured at append time. Property 1 already guarantees stability; property 3 means a violation of property 1 cannot corrupt anything silently even so — it will trip the capacity assert first, and if asserts are compiled out the pointers are still consistent with the final vector.

The completeness assert in `flush()` becomes: for every `DescriptorBindingDesc` in the layout, exactly one `PendingWrite` exists with the same `binding`, the same `type`, and `count == b->count`. This is what makes an array binding that Phase 2 forgets to fill a hard error at the first dispatch rather than sampled garbage.

Device features required for the array (Phase 2's job to enable, recorded here so the seam is not lost): `VkPhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing`, and `VkPhysicalDeviceVulkan12Features::{descriptorIndexing, shaderSampledImageArrayNonUniformIndexing}` via `vkb::PhysicalDeviceSelector::set_required_features` / `set_required_features_12`. **Not** needed: `runtimeDescriptorArray`, `descriptorBindingVariableDescriptorCount`, `descriptorBindingPartiallyBound`, or any `updateAfterBind` — keeping the feature set this small is the entire point of the fixed-size choice, and it is what keeps MoltenVK viable.

---

## 3.5 Ownership and lifetime — the complete table

| Object | Owner | Created | Destroyed | Notes |
|---|---|---|---|---|
| `VulkanContext` | `GLFWWindowIMPL` via `std::unique_ptr` | `GLFWWindow.cpp:50` | `~GLFWWindowIMPL` (`GLFWWindow.cpp:76-78`) | Currently `new`'d and **never deleted** — a real leak. `~VulkanContext` → `cleanup()` → `drainDeletionQueueFully()` first. |
| `FrameContext` | `VulkanContext` (one member) | `beginFrame()` | never (value member) | Never copied into any other object. |
| Deletion queue, staging blocks, sampler cache, dummy resources | `VulkanContext` | `init()` | `cleanup()` | Sampler cache destroyed after everything that borrows from it. |
| `VulkanBuffer` / `VulkanRingBuffer` | `Renderer`, by value | `Renderer::Init` / first `ensureCapacity` | `Renderer::Shutdown()` | Destructor routes through `ctx.deferDestroy`. |
| `VulkanImage` (`m_Frames[2]`) | `Renderer`, by value | `Renderer::Init` (invalid) / `SetupGPUResources` on first resolution | `Renderer::Shutdown()` | `recreate()` defer-destroys the old handles. |
| `VulkanTexture` (skybox) | `Renderer`, by value | `SetupGPUResources` on skybox change | `Renderer::Shutdown()` | Sampler is borrowed, not owned. |
| `VulkanComputePipeline` | `Renderer`, `unordered_map` by value | `GetOrLoadShader` | `Renderer::Shutdown()` | Raw `VulkanComputePipeline*` into the map is stable across rehash. |
| `VulkanDescriptorSetRing` | `Renderer`, `unordered_map<ShaderType, array<…,3>>` | `GetOrLoadShader` | `Renderer::Shutdown()` | Frees via `deferFreeDescriptorSets`. |
| `VkDescriptorSet` for ImGui viewport textures | `ViewportPanel` | `ImGui_ImplVulkan_AddTexture` | deferred `RemoveTexture`, see §3.6 | Allocated from `m_DescriptorPool`; ImGui frees them. |

`Renderer` gains `void Shutdown();` which move-assigns every resource member from a default-constructed object, in this order: descriptor rings → pipelines → images → textures → buffers. `RenderLayer::onDetach()` (currently empty, `RenderLayer.cpp:22-23`) becomes `vkDeviceWaitIdle(m_Ctx->getDevice()); m_Renderer.Shutdown();`. This runs from `LayerStack::onDetach()` (`LayerStack.cpp:28-32`), called by `Application::Shutdown()` (`application.cpp:37-39`), which runs before `~Application` destroys `_Window` — member declaration order in `application.h` puts `_Window` at `:25`, before `_LayerStack` at `:27` and `_RenderLayer` at `:33`, so the window and therefore the context outlive the layers. That ordering is already correct; do not reorder those members.

---

## 3.6 The two lifetime regressions this design would otherwise introduce, and their fixes

### 3.6.1 The frame event — dangling `VulkanImage*` where `weak_ptr::lock()` used to return null

**The regression.** `NewFrameRenderedEvent::frame` is `std::shared_ptr<IImage2D>` (`RenderEvents.h:12`, constructor `:14`) and `ViewportPanel::m_LatestRenderedFrame` is `std::weak_ptr<IImage2D>` (`ViewportPanel.h:46`). Moving to a raw `VulkanImage*` is correct *in principle* — `LayerStack::dispatchEvent` is synchronous (`LayerStack.cpp:34-41`) and `RenderLayer` is pushed before every consumer (`application.cpp:33`) — but **both consumers cache the pointer across frames**: `ViewportPanel.cpp:121` stores it in `onEvent` and `:205` reads it in a later `OnImGuiRender`; `RuntimeLayer.cpp:199` stores it and `:176` reads it. And `RenderLayer::onUpdate` only dispatches when a project is open (`RenderLayer.cpp:26`, `if (m_ProjectManager->ProjectIsOpen())`). Close a project or shut down and the panel holds a raw pointer where `weak_ptr::lock()` previously returned null. There is no `PROJECT_CLOSED` event to hang a fix on — the `EventType` enum in `X3/src/Core/Events/IEvent.h` has no such member.

**The fix, in three parts. All three are required; any two alone leave a hole.**

**(a) Stamp the event with the frame number.**

```cpp
// X3/src/Core/Events/RenderEvents.h  -- replace :4 (#include "Renderer/IImage2D.h")
//                                       with a forward declaration.
namespace X3 { class VulkanImage; }

struct NewFrameRenderedEvent : public IEvent {
    // Non-owning. Valid ONLY during the LayerStack::onUpdate pass that produced
    // it. Consumers MUST validate `frameNumber` against
    // VulkanContext::frameNumber() before dereferencing.
    VulkanImage* frame       = nullptr;
    uint64_t     frameNumber = 0;

    NewFrameRenderedEvent(VulkanImage* frame, uint64_t frameNumber)
        : frame(frame), frameNumber(frameNumber) {}

    inline EventType GetType() const override { return EventType::NEW_FRAME_RENDERED_EVENT; }
};
```

**(b) Dispatch unconditionally.** Move the `dispatchEvent` call in `RenderLayer::onUpdate` **out** of the `if (m_ProjectManager->ProjectIsOpen())` block (`RenderLayer.cpp:26-46`), so a null frame is dispatched every frame in which no frame was produced — no project open, no valid camera (`Renderer::Parse` returns nullptr at `Renderer.cpp:60`/`:128`), or no `FrameContext`. Consumers then need no cross-frame caching at all in the normal case.

**(c) Consumers validate the stamp.** `ViewportPanel` and `RuntimeLayer` store both fields and dereference only through an accessor:

```cpp
VulkanImage* CurrentFrameImage() const {
    return (m_Ctx && m_LatestFrameNumber == m_Ctx->frameNumber())
         ? m_LatestRenderedFrame : nullptr;
}
```

`ViewportPanel.cpp:205` (`auto latestRenderedFrameShared = m_LatestRenderedFrame.lock();`) becomes `VulkanImage* frameImage = CurrentFrameImage();` with the same null branch below it (`:206-218`). `ViewportPanel.cpp:221` `->GetDimensions()` becomes `->dimensions()`. `ViewportPanel.cpp:284-293`: keep only the Vulkan branch, delete the `#ifdef X3_USE_VULKAN` / `#else` / `#endif` at `:284`, `:290`, `:293` and the whole OpenGL line at `:292`. `RuntimeLayer.cpp:176` becomes `if (VulkanImage* img = CurrentFrameImage())`.

Why (c) is not redundant given (b): shutdown. `Application::run` exits the loop, `Shutdown()` destroys the layers, and any in-flight ImGui pass that still holds the pointer would dereference freed memory. The frame-number check makes a stale pointer *unreachable* rather than merely unlikely, deterministically and with no extra events. It costs eight bytes and one comparison.

Also downgrade `ViewportPanel.cpp:206`'s `LOG_EDITOR_ERROR("Last Rendered Frame was a nullptr..")` to a debug-level log: with (b) it now fires legitimately every frame while no project is open.

### 3.6.2 The ImGui descriptor map — bounded by construction, not by an eviction heuristic

**The regression.** The current cache is single-entry: `m_ImGuiTextureDescriptor`, `m_TextureSampler`, `m_LastRegisteredImageID` (`ViewportPanel.h:65-67`), keyed on `image->GetID()` (`ViewportPanel.cpp:48`) and re-registered whenever the id changes (`:63-67` removes, `:93-97` re-adds, `:99` restamps). Once `m_Frames` becomes `std::array<VulkanImage, FRAMES_IN_FLIGHT>` indexed by `frame.index()`, the panel alternates between two images every frame, so a single-slot cache would `RemoveTexture`/`AddTexture` on *every* frame. Replacing it with an `unordered_map<uint64_t, VkDescriptorSet>` keyed on a per-allocation id fixes that but grows one entry — one `VkDescriptorSet` out of `maxSets = 1000` — per resolution change, forever.

**The fix: make the key stable and version the value.** This is why `VulkanImage::id()` is assigned once in the constructor and is explicitly **not** changed by `recreate()`, while `generation()` is (§3.2.4). The map is then keyed on object identity, so its size is bounded by the number of distinct `VulkanImage` objects ever displayed — which is `FRAMES_IN_FLIGHT = 2`, forever, regardless of how many times the resolution changes.

```cpp
// ViewportPanel.h -- replaces :65-67
struct ImGuiTextureEntry { VkDescriptorSet set = VK_NULL_HANDLE; uint64_t generation = 0; };
std::unordered_map<uint64_t, ImGuiTextureEntry> m_ImGuiDescriptors;      // key: VulkanImage::id()
std::vector<std::pair<uint64_t, VkDescriptorSet>> m_PendingRemovals;     // (retireFrame, set)
VulkanContext* m_Ctx = nullptr;                                          // set in init()
```

`GetImGuiTextureID(VulkanImage& image)`:

1. **Drain first.** For each entry of `m_PendingRemovals` with `m_Ctx->frameNumber() - retireFrame >= FRAMES_IN_FLIGHT`, call `ImGui_ImplVulkan_RemoveTexture(set)` and erase.
2. `auto& e = m_ImGuiDescriptors[image.id()];`
3. If `e.set != VK_NULL_HANDLE && e.generation == image.generation()`, return it. This is the every-frame path; no Vulkan calls.
4. Otherwise, if `e.set != VK_NULL_HANDLE`, push `{m_Ctx->frameNumber(), e.set}` onto `m_PendingRemovals`.
5. `e.set = ImGui_ImplVulkan_AddTexture(m_Ctx->getSampler(SamplerDesc{VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE}), image.view(), VK_IMAGE_LAYOUT_GENERAL); e.generation = image.generation();` and return it. The layout stays `VK_IMAGE_LAYOUT_GENERAL`, matching the transition `Renderer::Draw` records.

**Why the removal must be deferred.** `ImGui_ImplVulkan_RemoveTexture` calls `vkFreeDescriptorSets` immediately, on the pool it was allocated from — which is our `m_DescriptorPool`, passed as `init_info.DescriptorPool` from `ImGuiContext.cpp:169`. The previous frame's ImGui draw command buffer may still be pending with that set bound. Freeing a descriptor set in use by a pending command buffer is undefined behaviour.

**Why this lives in the editor and not in `VulkanContext`.** `X3Engine` does not link ImGui — `X3/CMakeLists.txt` has no imgui target (`:68-69` link `glm spdlog entt yaml-cpp Jolt assimp stb_image stb_truetype`, `:72-85` add Vulkan/GLFW), and ImGui is linked only by `X3-Editor`. `VulkanContext` therefore cannot call `ImGui_ImplVulkan_RemoveTexture`, so the deferred list belongs to the panel that owns the descriptors.

Corresponding deletions: `m_TextureSampler` (`ViewportPanel.h:66`), the `vkCreateSampler` block at `ViewportPanel.cpp:68-89`, the `vkDestroySampler` at `:38-41`, `m_LastRegisteredImageID` (`ViewportPanel.h:67`, set at `ViewportPanel.cpp:99`), and the `std::dynamic_pointer_cast<VulkanImage2D>` at `:57-59` (no hierarchy, no RTTI). `CleanupVulkanResources` (`ViewportPanel.cpp:26-42`) keeps its `vkDeviceWaitIdle` at `:31`, then removes every entry in both `m_ImGuiDescriptors` and `m_PendingRemovals` and clears both. `ViewportPanel.cpp:13` `#include "Platform/Vulkan/VulkanImage2D.h"` → `VulkanImage.h`. The `#ifdef X3_USE_VULKAN` guards at `ViewportPanel.cpp:12-16`, `:20-24` and `ViewportPanel.h:8-10`, `:63-71` become unconditional.

---

## 3.7 Call-site migration

### 3.7.1 New constants in `Renderer.cpp`

```cpp
namespace {
// Mirrors res/shaders/PathTracing.comp:83-129 (identical in PBR.comp / Phong.comp).
// This is the ONLY copy of the descriptor table in the engine. Phase 3 replaces
// it with Slang-reflection-generated code; keep it in one place.
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

// std140, matches PathTracing.comp:87-90. 80 bytes -- the size Renderer.cpp:17 allocates.
struct CameraUBOData { glm::mat4 transform; float focalLength; float _pad[3]; };
static_assert(sizeof(CameraUBOData) == 80);

// std140, matches PathTracing.comp:92-101. 32 bytes used; Renderer.cpp:18 allocated 64.
struct SettingsUBOData {
    uint32_t raysPerPixel, bouncesPerRay, accumulatedFrames, entityCount;
    uint32_t debugMode, aabbHeatmapCutoff, triHeatmapCutoff, lightCount;
};
static_assert(sizeof(SettingsUBOData) == 32);
} // namespace
```

### 3.7.2 `X3/src/Renderer/Renderer.h`

| Current | Becomes |
|---|---|
| `:5` `#include "Renderer/IRendererAPI.h"` | deleted |
| `:13-17` forward decls `IComputeShader/ITexture2D/IImage2D/IUniformBuffer/IShaderStorageBuffer` | deleted. The new types are held **by value**, so include the concrete headers: `Platform/Vulkan/VulkanBuffer.h`, `VulkanImage.h`, `VulkanTexture.h`, `VulkanDescriptors.h`, `VulkanComputePipeline.h`; forward-declare `class VulkanContext; class FrameContext;` |
| `:34-38` `Cache::entityLookupSize/transformSize/materialSize/lightSize` | deleted — `ensureCapacity` subsumes them (`:34` is the comment; the four fields are `:35-38`) |
| `:87-88` `GetAPI()/SetAPI()` | deleted with `IRendererAPI` |
| `:92` `void Init()` | `void Init(VulkanContext& ctx);` |
| — (new) | `void Shutdown();` |
| `:93-94` `std::shared_ptr<IImage2D> Render(...)` | `VulkanImage* Render(const FrameContext& frame, const Scene*, const AssetPool*, const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f);` |
| `:99` `bool SetupGPUResources(pScene, scene, assetPool)` | `bool SetupGPUResources(const FrameContext&, std::shared_ptr<const ParsedScene>, const Scene*, const AssetPool*);` |
| `:100` `void Draw()` | `void Draw(const FrameContext&);` |
| `:101` `std::shared_ptr<IComputeShader> GetOrLoadShader(ShaderType)` | `VulkanComputePipeline* GetOrLoadShader(ShaderType);` (nullptr on failure) |
| `:106-107` `m_CurrentShader`, `m_ShaderCache` | `VulkanComputePipeline* m_CurrentShader = nullptr;` / `std::unordered_map<ShaderType, VulkanComputePipeline> m_ShaderCache;` |
| — (new) | `std::unordered_map<ShaderType, std::array<VulkanDescriptorSetRing, 3>> m_DescriptorRings;` |
| `:111-113` `m_Frames[2]`, `m_WriteFrameIndex`, `m_WasDoubleBuffering` | `std::array<VulkanImage, FRAMES_IN_FLIGHT> m_Frames;` — the other two deleted |
| `:115` `std::shared_ptr<ITexture2D> m_SkyboxTexture` | `VulkanTexture m_SkyboxTexture;` |
| `:116` `m_CameraUBO, m_SettingsUBO` | `VulkanRingBuffer m_CameraUBO, m_SettingsUBO;` |
| `:117` seven `IShaderStorageBuffer` | `VulkanRingBuffer m_EntityLookupSSBO, m_TransformSSBO, m_MaterialSSBO, m_LightSSBO;` + `VulkanBuffer m_MeshBufferSSBO, m_NodeBufferSSBO, m_IndexBufferSSBO;` |
| — (new) | `VulkanContext* m_Ctx = nullptr;` and `uint32_t m_PrevMeshVersion = 0, m_PrevNodeVersion = 0, m_PrevIndexVersion = 0;` |

**Output-image indexing** replaces the `m_WriteFrameIndex` dance at `Renderer.cpp:65-90` and `:209`. The accumulation path is a read-modify-write of the *same* image (`PathTracing.comp` does `imageLoad` then `imageStore` on `rayTracingTexture`), so:

```cpp
VulkanImage& outputImage(const FrameContext& f) {
    return m_RenderSettings.accumulate ? m_Frames[0] : m_Frames[f.index()];
}
```

That preserves both behaviours: accumulate ⇒ one persistent image (frames serialise on it, which is inherent to accumulation); no accumulate ⇒ one image per frame in flight, which is what `useDoubleBuffering` was hand-rolling without fences. Consequently **`RenderSettings::useDoubleBuffering` (`RenderSettings.h:24`) is deleted**, along with its only readers/writers: `Renderer.cpp:67` and `WindowTitleBar.cpp:131`, `:139` (keep the surrounding `UpdateRenderSettingsEvent` dispatches). With correct fencing, double buffering is always safe when not accumulating.

### 3.7.3 `Renderer.cpp` — `Init` and `GetOrLoadShader`

| Line(s) | Current | Becomes |
|---|---|---|
| `:5-9` | five `Renderer/I*.h` includes | `Platform/Vulkan/VulkanContext.h`, `VulkanComputePipeline.h`, `VulkanBuffer.h`, `VulkanImage.h`, `VulkanTexture.h`, `VulkanDescriptors.h`, `FrameContext.h` |
| `:15` | `void Renderer::Init()` | `void Renderer::Init(VulkanContext& ctx)`; first statement `m_Ctx = &ctx;` |
| `:17` | `m_CameraUBO = IUniformBuffer::Create(80, 0, DYNAMIC_DRAW)` | `m_CameraUBO = VulkanRingBuffer(ctx, BufferKind::Uniform, sizeof(CameraUBOData), "CameraUBO");` |
| `:18` | `m_SettingsUBO = IUniformBuffer::Create(64, 1, DYNAMIC_DRAW)` | `m_SettingsUBO = VulkanRingBuffer(ctx, BufferKind::Uniform, sizeof(SettingsUBOData), "SettingsUBO");` — 32, not 64; the extra 32 bytes were slack |
| `:21` | `m_CurrentShader = GetOrLoadShader(PATH_TRACING)` | unchanged; type is now `VulkanComputePipeline*` |
| `:26` | `m_CurrentShader->Bind();` | **deleted** (safe from step 3a: `VulkanComputeShader::Bind` is empty, `VulkanComputeShader.cpp:89-92`) |
| `:32` | `it->second && it->second->GetID() != 0` | `it->second.valid()` → `return &it->second;` |
| `:43` | `IComputeShader::Create(pathIt->second.string(), glm::uvec3(1))` | `ComputePipelineDesc d; d.spirvPath = pathIt->second; d.spirvPath += ".spv"; d.setLayouts = { {kSet0, std::end(kSet0)}, {kSet1, …}, {kSet2, …} }; d.debugName = …; auto [it2, _] = m_ShaderCache.try_emplace(type, *m_Ctx, d);` |
| `:44` | `if (!shader \|\| shader->GetID() == 0)` | `if (!it2->second.valid()) { m_ShaderCache.erase(it2); return nullptr; }` |
| `:50-51` | `m_ShaderCache[type] = shader; return shader;` | allocate the rings: `m_DescriptorRings[type] = { VulkanDescriptorSetRing(*m_Ctx, it2->second.setLayout(0)), …(1), …(2) };` then `return &it2->second;` |

`.spv` must now be appended by the caller — `Renderer.h:121-125` stores paths without it and `VulkanComputeShader.cpp:302` used to append it internally.

### 3.7.4 `Renderer.cpp` — `Render` and `Parse`

| Line(s) | Current | Becomes |
|---|---|---|
| `:54-55` | signature | `VulkanImage* Renderer::Render(const FrameContext& frame, const Scene*, const AssetPool*, const glm::mat4*, float)` |
| `:58-61` | `Parse(...)`; `return nullptr` | unchanged |
| `:62` | `SetupGPUResources(pScene, scene, assetPool)` | `SetupGPUResources(frame, pScene, scene, assetPool)` |
| `:63` | `Draw()` | `Draw(frame)` |
| `:65-90` | the whole `canDoubleBuffer` / `m_WasDoubleBuffering` / `m_WriteFrameIndex` block | **deleted**, replaced by `return &outputImage(frame);` |
| `:93-193` | `Parse` | **unchanged** — pure CPU, touches no GPU type |

### 3.7.5 `Renderer.cpp` — `SetupGPUResources`

| Line(s) | Current | Becomes |
|---|---|---|
| `:197` | signature | `bool Renderer::SetupGPUResources(const FrameContext& frame, std::shared_ptr<const ParsedScene> pScene, const Scene* scene, const AssetPool* assetPool)` |
| `:198` | `m_Profiler->timer("Renderer::SetupGPUResources()");` | `auto t = m_Profiler->timer(...);` — **`Profiler::timer` returns `const std::shared_ptr<ScopeTimer>` (`Profiler.h:89`) and the result is discarded here, so the timer destructs immediately and the measurement is meaningless.** Fix it now, because it is the measurement used to verify that in-frame staging removed the upload stalls. |
| `:201-205` | resolution change → two `IImage2D::Create(nullptr, w, h, 0, LR_READ_WRITE)` | `for (auto& img : m_Frames) img.recreate(frame, ImageDesc{ res.x, res.y, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT \| VK_IMAGE_USAGE_SAMPLED_BIT \| VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1, "RenderTarget" });` — `VK_IMAGE_USAGE_TRANSFER_DST_BIT` (present at `VulkanImage2D.cpp:77`) is dropped: nothing uploads into these |
| `:209` | `m_Frames[m_WriteFrameIndex]->ChangeImageUnit(0);` | **deleted** — the descriptor write moves to `Draw` |
| `:212` | accumulation counter | unchanged |
| `:217-218` | `entityCount`, `lightCount` | unchanged |
| `:219-228` | `m_SettingsUBO->Bind(); AddData ×8; Unbind();` | `SettingsUBOData s{ uint32_t(m_RenderSettings.raysPerPixel), uint32_t(m_RenderSettings.bouncesPerRay), m_Cache.AccumulatedFrames, entityCount, uint32_t(m_RenderSettings.debugMode), uint32_t(m_RenderSettings.aabbHeatmapCutoff), uint32_t(m_RenderSettings.triangleHeatmapCutoff), lightCount }; m_SettingsUBO.writeStruct(frame, s);` — note `debugMode`/`aabbHeatmapCutoff`/`triangleHeatmapCutoff` are `int` (`RenderSettings.h:20-22`) and were being `AddData`'d as `uint32_t` by aliasing; the struct makes the conversion explicit |
| `:231-234` | `m_CameraUBO->Bind(); AddData ×2; Unbind();` | `CameraUBOData c{ pScene->CameraTransform, pScene->CameraFocalLength, {} }; m_CameraUBO.writeStruct(frame, c);` |
| `:238-249` | skybox reload; `SKYBOX_TEXTURE_UNIT = 1`; `ITexture2D::Create(data, w, h, 1)` | `m_SkyboxTexture = VulkanTexture(*m_Ctx, frame, TextureDesc{ uint32_t(metadata->width), uint32_t(metadata->height), VK_FORMAT_R8G8B8A8_SRGB, 1, {}, "Skybox" }, data);` — `SKYBOX_TEXTURE_UNIT` (`:242`) deleted; the else branch at `:247` becomes `m_SkyboxTexture = {};` |
| `:253-264` | EntityLookup: conditional `Create`, `Bind/AddData/Unbind`, `m_Cache.entityLookupSize` | `const VkDeviceSize bytes = sizeof(MeshEntityHandle) * count; m_EntityLookupSSBO.ensureCapacity(frame, bytes); m_EntityLookupSSBO.write(frame, pScene->MeshEntityLookupTable.data(), bytes);` |
| `:265-276` | Transform | same pattern on `m_TransformSSBO` |
| `:277-288` | Material | same pattern on `m_MaterialSSBO` |
| `:289-302` | Lights, **skipped entirely when `count == 0`** (`:292`) | `const VkDeviceSize bytes = std::max<VkDeviceSize>(sizeof(LightData) * count, sizeof(LightData)); m_LightSSBO.ensureCapacity(frame, bytes); if (count) m_LightSSBO.write(frame, pScene->LightBuffer.data(), sizeof(LightData) * count);` — **no early-out**, so binding 6 is always valid |
| `:306-309` | four function-local `static uint32_t prev*Version` | move to the members `m_PrevMeshVersion` / `m_PrevNodeVersion` / `m_PrevIndexVersion`; `prevSkyboxTextureVersion` (`:309`) has no readers — delete it |
| `:311-323` | MeshBuffer: `Create(bytes, 3, STATIC)` + `Bind/AddData/Unbind` | `m_MeshBufferSSBO.ensureCapacity(frame, bytes); m_MeshBufferSSBO.upload(frame, assetPool->MeshBuffer.data(), bytes);` — still guarded by the version check at `:314` |
| `:325-337` | NodeBuffer | same pattern on `m_NodeBufferSSBO` |
| `:339-351` | IndexBuffer | same pattern on `m_IndexBufferSSBO` |

The `static` → member move at `:306-309` is not cosmetic: function-local statics are shared across every `Renderer` instance and survive project close, so a reopened project with identical version counters skips the upload into buffers that were just destroyed.

### 3.7.6 `Renderer.cpp` — `Draw`

| Line(s) | Current | Becomes |
|---|---|---|
| `:356-357` | `void Renderer::Draw()` + profiler | `void Renderer::Draw(const FrameContext& frame)`; profiler line unchanged (it already binds to `auto t`) |
| `:360-369` | shader switch + null check | unchanged, types are now `VulkanComputePipeline*` |
| `:371` | `m_CurrentShader->Bind();` | **deleted** |
| `:372-376` | `setWorkGroupSizes(uvec3((w+7)/8, (h+3)/4, 1))` | locals `const uint32_t gx = (res.x + 7) / 8, gy = (res.y + 3) / 4;` — the `/8` and `/4` must stay in sync with `LOCAL_GROUP_X 8` / `LOCAL_GROUP_Y 4` at `PathTracing.comp:16-17` |
| `:377` | `m_CurrentShader->Dispatch();` | the block below |

New body from `:371` onward:

```cpp
VulkanImage& out = outputImage(frame);
out.transition(frame, VK_IMAGE_LAYOUT_GENERAL,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
if (m_RenderSettings.accumulate) {          // cross-frame RMW on m_Frames[0]
    out.barrier(frame, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

auto& rings = m_DescriptorRings.at(m_RenderSettings.shaderType);

DescriptorWriter(*m_Ctx, m_CurrentShader->setLayout(0), rings[0].get(frame))
    .storageImage(0, out)
    .sampledImage(1, m_SkyboxTexture.valid() ? m_SkyboxTexture : m_Ctx->dummyTexture())
    .flush();
DescriptorWriter(*m_Ctx, m_CurrentShader->setLayout(1), rings[1].get(frame))
    .uniformBuffer(0, m_CameraUBO,   frame)
    .uniformBuffer(1, m_SettingsUBO, frame)
    .flush();
DescriptorWriter(*m_Ctx, m_CurrentShader->setLayout(2), rings[2].get(frame))
    .storageBuffer(0, m_EntityLookupSSBO, frame)
    .storageBuffer(1, m_TransformSSBO,    frame)
    .storageBuffer(2, m_MaterialSSBO,     frame)
    .storageBuffer(3, m_MeshBufferSSBO)
    .storageBuffer(4, m_NodeBufferSSBO)
    .storageBuffer(5, m_IndexBufferSSBO)
    .storageBuffer(6, m_LightSSBO,        frame)
    .flush();

const VkDescriptorSet sets[3] = { rings[0].get(frame), rings[1].get(frame), rings[2].get(frame) };
m_CurrentShader->dispatch(frame, sets, gx, gy, 1);

// Was VulkanComputeShader.cpp:139-154 (dst stage FRAGMENT_SHADER only).
// Now explicit and correctly scoped: compute write -> ImGui fragment read
// (editor) OR vkCmdBlitImage transfer read (runtime).
out.barrier(frame, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
```

Note the ordering constraint that Part 2 depends on: this dispatch and these barriers are now recorded **outside** any rendering block, which is what makes them legal (`vkCmdDispatch` inside a render pass instance violates VUID-vkCmdDispatch-renderpass, and the barrier at `VulkanComputeShader.cpp:146-153` inside a render pass would require a subpass self-dependency that never existed).

### 3.7.7 `RenderLayer`, `Application`, window layer

| File:line | Current | Becomes |
|---|---|---|
| `RenderLayer.cpp:18-20` | `onAttach() { m_Renderer.Init(); }` | `m_Ctx = VulkanContext::Get(); m_Renderer.Init(*m_Ctx);` (add `VulkanContext* m_Ctx = nullptr;` to `RenderLayer.h`) |
| `RenderLayer.cpp:22-23` | `onDetach()` empty | `vkDeviceWaitIdle(m_Ctx->getDevice()); m_Renderer.Shutdown();` |
| `RenderLayer.cpp:30` | `std::shared_ptr<IImage2D> RenderedFrame;` | `VulkanImage* RenderedFrame = nullptr;` |
| `RenderLayer.cpp:26-46` | dispatch inside `if (ProjectIsOpen())` | obtain `const FrameContext* f = m_Ctx->currentFrame();`; render only when a project is open *and* `f != nullptr`; **dispatch `NewFrameRenderedEvent(RenderedFrame, m_Ctx->frameNumber())` unconditionally**, outside the `if` (§3.6.1(b)) |
| `RenderLayer.cpp:31-37` | two `m_Renderer.Render(...)` calls | prepend `*f` as the first argument to both |
| `application.h:12, 28` | `class IRendererAPI;` / `std::shared_ptr<IRendererAPI> _RendererAPI;` | deleted; add `VulkanContext* _Context = nullptr;` and forward-declare `class VulkanContext;` |
| `application.cpp:9` | `#include "Renderer/IRendererAPI.h"` | `#include "Platform/Vulkan/VulkanContext.h"` |
| `application.cpp:27-28` | `_RendererAPI = IRendererAPI::Create(); _RendererAPI->Init();` | `_Context = VulkanContext::Get();` — non-null because `IWindow::createWindow` at `:20` already created it (`GLFWWindow.cpp:50`) |
| `application.cpp:53-58` | the `#ifdef BUILD_INSTALL` `_RendererAPI->Clear(...)` block | deleted; the clear colour is now `VkRenderingAttachmentInfo::clearValue`, supplied by Part 2's `beginSwapchainRendering` |
| `application.cpp:60-70` | `_LayerStack->onUpdate();` then `_Window->swapBuffers();` | the loop body below |
| `IWindow.h:35, 37` | `onUpdate()`, `swapBuffers()` | both deleted (`GLFWWindowIMPL::onUpdate` at `GLFWWindow.cpp:80-83` is unreferenced dead code; `swapBuffers` at `:89-91` is the GL verb) |
| `GLFWWindow.h:5, 53` | `#include "Renderer/IRenderingContext.h"`, `IRenderingContext* m_Context;` | `#include "Platform/Vulkan/VulkanContext.h"`, `std::unique_ptr<VulkanContext> m_Context;` — currently `new`'d at `GLFWWindow.cpp:50` and **never deleted** |
| `GLFWWindow.cpp:8` | `#include "Renderer/IRendererAPI.h"` | deleted (unguarded, unused; breaks when the header goes) |
| `GLFWWindow.cpp:107-110` | `setVSync` → `glfwSwapInterval(enabled)` (a no-op under Vulkan) | `m_VSync = enabled; m_Context->setVSync(enabled); m_Context->recreateSwapchain();` — this is the `RenderSettings::vSync` wiring |

```cpp
// Application::run
while (!_Window->shouldClose()) {
    Time::Update();
    auto t = _Profiler->globalTimer("GLOBAL");
    { auto t2 = _Profiler->timer("PollEvents"); _Window->pollEvents(); }

    const FrameContext* frame = _Context->beginFrame();
    if (!frame) continue;                        // swapchain was out of date

    { auto t2 = _Profiler->timer("LayerStack::onUpdate()"); _LayerStack->onUpdate(); }

    { auto t2 = _Profiler->timer("Present");
      _Context->endFrame();
      _Context->present(); }
}
vkDeviceWaitIdle(_Context->getDevice());
Shutdown();
```

### 3.7.8 `RuntimeLayer`

| File:line | Current | Becomes |
|---|---|---|
| `RuntimeLayer.h:10` | `#include "Platform/Vulkan/VulkanImage2D.h"` | `#include "Platform/Vulkan/VulkanImage.h"` |
| `RuntimeLayer.h:47` | `std::shared_ptr<IImage2D> m_CurrentFrame;` | `VulkanImage* m_CurrentFrame = nullptr; uint64_t m_CurrentFrameNumber = 0; VulkanContext* m_Ctx = nullptr;` |
| `RuntimeLayer.h:48` | `unsigned int m_Framebuffer = 0;` (a GL FBO name) | deleted with the OpenGL branch |
| `RuntimeLayer.cpp:176-193` | `dynamic_pointer_cast<VulkanImage2D>` at `:179`, `blitImageToSwapchain(vulkanImage->getImage(), VK_IMAGE_LAYOUT_GENERAL, …)` at `:183-190` | `if (VulkanImage* img = CurrentFrameImage()) { CalculateViewportCoordinates(); m_Ctx->blitImageToSwapchain(*img, m_ViewportCoords, m_WindowSize); }` |
| `RuntimeLayer.cpp:185` | hardcoded `VK_IMAGE_LAYOUT_GENERAL` | gone — the tracked layout comes from the image |
| `RuntimeLayer.cpp:199` | `m_CurrentFrame = …->frame;` | also store `m_CurrentFrameNumber = …->frameNumber;` |
| `RuntimeLayer.cpp:312` | `m_CurrentFrame->GetDimensions()` | `img->dimensions()` |

**`blitImageToSwapchain`'s signature — settled.** It becomes:

```cpp
void blitImageToSwapchain(VulkanImage& src, glm::ivec4 viewport, glm::ivec2 windowSize);
```

replacing `VulkanContext.h:80-82`. It reads `src.handle()`, `src.extent()` and `src.layout()` itself, and drives the source transitions through `src.transition(...)` so the tracked layout stays truthful — the current code at `VulkanContext.cpp:838-856` and `:935-953` transitions `GENERAL → TRANSFER_SRC_OPTIMAL → GENERAL` behind the object's back. The `FrameContext` is not a parameter; the function reads `m_Frame` internally, since it is already a `VulkanContext` member function and asserts `m_FrameActive`. **The Y-flip packing (`VulkanContext.cpp:905-906`) and `CalculateViewportCoordinates()` (`RuntimeLayer.cpp:306-376`) must not change.**

### 3.7.9 `ImGuiContext` and other files

| File:line | Change |
|---|---|
| `X3-Editor/src/ImGuiContext.cpp:20` | delete `#include "Renderer/IRendererAPI.h"` |
| `X3-Editor/src/ImGuiContext.cpp:278` | delete `vkContext->ensureFrameStarted();` — `Application::run` now guarantees the frame is open before any layer runs |
| `X3-Editor/src/ImGuiContext.cpp` | store `VulkanContext* m_Ctx` at `Init` and use it thereafter, instead of calling `VulkanContext::Get()` per frame (decision D2) |
| `X3/src/X3.h:16-21` | delete all six `Renderer/I*.h` includes |
| `X3/src/Project/ProjectManager.cpp:4, 113-119` | delete the include and the whole API-selection block |
| `X3/src/Renderer/RenderSettings.h:13-16, 28-32, 45, 61` | delete `enum class RendererAPI`, the member and its two serialization lines |
| `X3/src/Renderer/RenderSettings.h:24` | delete `useDoubleBuffering` |
| `X3-Editor/src/WindowTitleBar/WindowTitleBar.cpp:130-132, 138-140` | delete the two `useDoubleBuffering` assignments; keep the `UpdateRenderSettingsEvent` dispatches |
| `X3-Editor/src/EditorLayer.cpp:58-64` | `SET_VSYNC_EVENT` → `m_Window->setVSync(...)`, which now actually reaches `VulkanContext::setVSync` |

---

## 3.8 The migration bridge — the old `Bind()` calls must survive

This is the single most likely thing to be got wrong, because the bridge is invisible in the final code.

During the incremental steps in §3.9, the new `DescriptorWriter` and the old global registry coexist. For a binding whose resource has not yet been migrated, the writer takes the raw descriptor info straight out of `VulkanContext::getBoundStorageBuffers()` / `getBoundUniformBuffers()` / `getBoundStorageImages()` (`VulkanContext.h:49-52`) via `DescriptorWriter::raw(binding, type, info)`. So the set is always filled completely even when only some of its resources are new, and `flush()`'s completeness assert holds from the very first step.

**But the registry is populated only by `Bind()`.** Therefore:

> **The `Bind()` / `Unbind()` calls in `Renderer::SetupGPUResources` at `Renderer.cpp:219` and `:228` (SettingsUBO), `:231` and `:234` (CameraUBO), `:261` and `:263` (EntityLookup), `:273` and `:275` (Transform), `:285` and `:287` (Material), `:298` and `:300` (Lights), `:319` and `:321` (MeshBuffer), `:333` and `:335` (NodeBuffer), `:347` and `:349` (IndexBuffer), and the `ChangeImageUnit(0)` at `:209`, MUST REMAIN until the specific binding they feed has been migrated. Delete each `Bind()`/`Unbind()` pair only in the same commit that replaces that buffer with a `VulkanRingBuffer` or `VulkanBuffer`. Deleting them early leaves the corresponding registry entry stale (it holds the previous, destroyed handle) and `raw()` will happily write a dangling `VkBuffer` into a live descriptor.**

Two exceptions, both safe to delete in step 3a:

- `Renderer.cpp:26` and `:371`, `m_CurrentShader->Bind()`. `VulkanComputeShader::Bind` is empty (`VulkanComputeShader.cpp:89-92`) and registers nothing.
- Set 0 binding 1 (skybox) **cannot** be bridged through `raw()`, because as established in §3.1(c) `registerSampledImage` is never called by anything: `VulkanTexture2D`'s constructor (`VulkanTexture2D.cpp:8-17`) does not call it and `ChangeTextureUnit` (`:36-43`) has zero callers. From step 3a onward, binding 1 is written as `m_Ctx->dummyTexture()` until step 3f replaces it with a real `VulkanTexture`. This means the skybox renders black between 3a and 3f. **That is a visible, temporary, deliberate regression — record it in the 3a commit message so nobody bisects it.** It is also the point at which set 0 binding 1 becomes *defined* for the first time in the project's history.

`DescriptorWriter::raw()` is deleted in the final cleanup commit, when nothing calls it.

---

## 3.9 Incremental ordering — the engine runs after every step

Each numbered step is one commit. The interleave with Part 2 (dynamic rendering + frame lifecycle) is explicit.

**Step 0 — prerequisite.** OpenGL deletion has landed: `X3/src/Platform/OpenGL/` gone, every `X3_USE_OPENGL` branch gone, `X3_GRAPHICS_API` gone, `X3/CMakeLists.txt:56-61` filter removed. Nothing here is safe to start before that.

**Step 1 — add the layer, wire nothing.** Purely additive; the engine behaves identically.
 1.1 `VulkanCommon.h`, `FrameContext.h`. Replace `MAX_FRAMES_IN_FLIGHT` (`VulkanContext.h:146`) with `FRAMES_IN_FLIGHT` at `VulkanContext.cpp:293, 311, 325, 456, 758`; delete `getMaxFramesInFlight()` (`VulkanContext.h:88`).
 1.2 `VulkanContext`: deletion queue, `m_FrameNumber` / `m_CompletedFrame`, `drainDeletionQueue()` hooked into the **existing** `beginFrame()` right after the fence wait at `VulkanContext.cpp:346`, staging arena, sampler cache, cached `m_Limits`, dummy resources, and `m_Frame` / `currentFrame()` populated by the existing `beginFrame()` (which still returns `bool` at this point). Also gate the validation layers on build type: `VulkanContext.h:166` (`bool m_EnableValidationLayers = true; // Disable in release builds`) becomes `#ifdef NDEBUG false #else true #endif` — free, and it makes every later step verifiable.
 1.3 `VulkanBuffer.h/.cpp`.
 1.4 `VulkanImage.h/.cpp`, `VulkanTexture.h/.cpp`.
 1.5 `VulkanDescriptors.h/.cpp`.
 1.6 `VulkanComputePipeline.h/.cpp`.
 *Verify:* `cmake --build build/vulkan-debug -j 14` produces `Debug/X3Editor` and `Debug/runtime/X3Runtime` with zero `error:` in the log; the new symbols are unreferenced; no behaviour change.
 **The deletion queue MUST land in this step, before any step that removes a `vkQueueWaitIdle`.** `endSingleTimeCommands`' `vkQueueWaitIdle` (`VulkanContext.cpp:530`) is currently the only thing making image and texture recreation safe: `Renderer.cpp:202-203` and `:244` construct the replacement (which idles the queue) *before* the `shared_ptr` assignment destroys the old one, so the old `VkImage` is destroyed on an idle queue by accident. Removing the wait without the deletion queue converts a hidden inefficiency into an immediate use-after-free.

**Step 2 — the frame-lifecycle + dynamic-rendering commit. This is Part 2's commit.** `beginFrame()` changes its return type to `const FrameContext*`; `endFrame()` / `present()` split; `swapBuffers`, `ensureFrameStarted`, `m_FirstFrame`, both render passes, all framebuffers, `IRenderingContext`, `IRendererAPI`, `VulkanRendererAPI` are deleted; `Application::run` is restructured; `IWindow::swapBuffers`/`onUpdate` dropped; `GLFWWindowIMPL` takes `unique_ptr` ownership of the context; `setVSync` is wired to the present mode. `VulkanComputeShader`, the registry and the old buffer classes are untouched and still work, because they only ever needed a recording command buffer. Highest-risk step; isolated deliberately.
 *Verify:* editor and runtime render exactly as before; no black first frame; resize recreates the swapchain and re-inits ImGui.

**Step 3 — migrate the Renderer, one binding group at a time.** Verify after *each* sub-step: editor renders, runtime renders, resize is stable, validation layers clean.

- **3a.** `VulkanComputeShader` → `VulkanComputePipeline` + `VulkanDescriptorSetRing` + `DescriptorWriter`, with **every** binding except set 0 binding 1 still sourced from the old registry through `raw()`, and binding 1 sourced from `dummyTexture()`. `Renderer::Draw` gains `const FrameContext&`; the two `m_CurrentShader->Bind()` calls (`Renderer.cpp:26`, `:371`) go. **This alone fixes the per-frame-descriptor-set bug** — the single largest correctness defect — while every resource class is still the old one. All other `Bind()` calls stay (§3.8).
- **3b.** `m_CameraUBO`, `m_SettingsUBO` → `VulkanRingBuffer` + the two POD structs. Set 1 now writes from real objects; sets 0 and 2 still use `raw()`. Delete `Renderer.cpp:219, 228, 231, 234`. **Fixes the ring-buffer hazard for the UBOs.**
- **3c.** `m_EntityLookupSSBO`, `m_TransformSSBO`, `m_MaterialSSBO`, `m_LightSSBO` → `VulkanRingBuffer`, `ensureCapacity` replacing the `m_Cache.*Size` fields (`Renderer.h:35-38`), light-count early-out (`Renderer.cpp:292`) removed. Delete `Renderer.cpp:261, 263, 273, 275, 285, 287, 298, 300`. **Fixes the ring-buffer hazard for the dynamic SSBOs and closes the binding-6 hole.**
- **3d.** `m_MeshBufferSSBO`, `m_NodeBufferSSBO`, `m_IndexBufferSSBO` → `VulkanBuffer` with staged in-frame uploads; the three `static` version counters become members. Delete `Renderer.cpp:319, 321, 333, 335, 347, 349`. **Removes the per-upload `vkQueueWaitIdle`** for the large buffers.
- **3e.** `m_Frames` → `std::array<VulkanImage, FRAMES_IN_FLIGHT>`; delete `m_WriteFrameIndex`, `m_WasDoubleBuffering`, `useDoubleBuffering`, `Renderer.cpp:65-90` and `:209`; update `RenderEvents.h`, `RenderLayer`, `ViewportPanel` (§3.6.1 and §3.6.2 land together, here), `RuntimeLayer` and `blitImageToSwapchain`'s signature. Add the pre-dispatch RMW barrier for the accumulate path.
- **3f.** Skybox → `VulkanTexture`, with the `COMPUTE_SHADER | FRAGMENT_SHADER` destination-stage fix and the `dummyTexture()` fallback. **All three sets are now fully native; no `raw()` calls remain.**
- **3g.** Delete the registry (`VulkanContext.h:39-52`, `:175-178`, `VulkanContext.cpp:810-824`), `DescriptorWriter::raw`, and the thirteen file pairs in §3.2.9 (respecting the `IRendererAPI.h` → `IComputeShader.h` order), plus the two stray includes at `GLFWWindow.cpp:8` and `ImGuiContext.cpp:20`.

**Step 4 — cleanup.** `X3.h:16-21`; `ProjectManager.cpp:4, 113-119`; `RenderSettings::rendererAPI` and `useDoubleBuffering`; `WindowTitleBar.cpp:130-132, 138-140`; `Renderer::GetAPI/SetAPI`.

**Step 5 — Phase 2 hand-off (not this phase, recorded so the seam is owned).** `DescriptorWriter::sampledImageArray` (§3.4.6) already exists as of step 1.5 and is unit-testable without any shader change: add a temporary 4-element dummy array binding to `kSet0`, confirm `flush()` asserts on a short span and passes on a full one, then remove it. Phase 2 adds the real `{2, COMBINED_IMAGE_SAMPLER, 128, COMPUTE}` entry to `kSet0`, the device features, and the `COMBINED_IMAGE_SAMPLER` pool-size raise at `VulkanContext.cpp:465` — and nothing else in the descriptor layer.

---

## 3.10 Verification gate before declaring this part done

**Static gates** (all must hold after step 4):

1. `grep -rn 'VulkanContext::Get()' X3/src X3-Editor/src X3-Runtime/src` — every hit is inside a constructor, an `init()`, or an `onAttach()`. **Zero hits** in `X3/src/Platform/Vulkan/Vulkan{Buffer,Image,Texture,Descriptors,ComputePipeline}.{h,cpp}`.
2. `grep -rn 'MAX_FRAMES_IN_FLIGHT' X3/src X3-Editor/src X3-Runtime/src` — zero hits.
3. `grep -rn 'vkQueueWaitIdle\|vkDeviceWaitIdle' X3/src/Platform/Vulkan/` — exactly four sites: `endSingleTimeCommands` (`VulkanContext.cpp:530`), `recreateSwapchain` (`:665`), `cleanup` (`:743`), `drainDeletionQueueFully`. Any other occurrence is a regression.
4. `grep -rn 'Bind()\|Unbind()\|AddData(\|GetID()\|ChangeImageUnit\|ChangeTextureUnit\|registerStorageBuffer\|registerUniformBuffer\|registerStorageImage\|registerSampledImage' X3/src X3-Editor/src X3-Runtime/src` — zero hits.
5. Both build presets configure and build with zero `error:` in the log **and** produce `Debug/X3Editor` plus `Debug/runtime/X3Runtime`. (After the OpenGL deletion there is only one preset; until then, check both. Never judge either build by a piped exit code alone — grep the log and confirm the binaries exist and have a fresh mtime.)

**Runtime gate.** With validation layers on, including `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` (configure via `vkconfig` rather than environment variables, whose exact spelling varies by SDK), run the editor for several minutes exercising: camera motion, a resolution change, a shader-type switch, a skybox change, a scene with zero lights, an empty scene with zero mesh entities, accumulate on and off, project close and reopen, and a window resize including minimise/restore. Then the same in the runtime, plus a run with no project so `RenderLayer` produces no frame. Expect:

- zero `VUID-vkUpdateDescriptorSets-None-03047`
- zero `VUID-VkBufferCreateInfo-size-00912`, zero `VUID-VkDescriptorBufferInfo-range-00341`
- zero `VUID-vkDestroyBuffer-buffer-00922`, zero `VUID-vkDestroyImage-image-01000`, zero `VUID-vkFreeDescriptorSets-pDescriptorSets-00309`
- zero `SYNC-HAZARD-*` on the render-target image or the skybox
- with `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT`, the `vkQueueWaitIdle` pipeline-stall warnings that fire today on every resolution change and every skybox load are gone

**Measurement gate.** `ProfilerPanel` times `Renderer::SetupGPUResources()` — but only after the `auto t =` fix at `Renderer.cpp:198` (`Profiler::timer` returns `const std::shared_ptr<ScopeTimer>`, `Profiler.h:89`, and the current code discards it so the timer destructs immediately and the number is meaningless). Fix that first, then change the render resolution repeatedly and load a scene with several meshes: the per-upload spike should collapse from a full GPU drain to near zero. If the number does not move, the uploads are still going through `endSingleTimeCommands`.

**Visual gate.** Load a scene with a skybox and screenshot the first frame after load. A black or garbage skybox means the staging copy's barrier or its ordering relative to the dispatch is wrong — the copy is now in the *same* command buffer as the dispatch that samples it, so it must be correct, but this is the highest-risk part of the change and it fails silently.