# PART 2 — Frame lifecycle, dynamic rendering, and synchronization correctness

**Scope:** `X3/src/Platform/Vulkan/VulkanContext.{h,cpp}`, `X3/src/Core/application.{h,cpp}`, `X3/src/Core/IWindow.h`, `X3/src/Platform/Windows/GLFWWindow.{h,cpp}`, `X3/src/Renderer/IRenderingContext.h`, `X3/src/Renderer/RenderSettings.h`, `X3/CMakeLists.txt`, `X3-Editor/src/ImGuiContext.cpp`, `X3/src/Renderer/Renderer.cpp:198`.

**Preconditions.**
1. Phase 1a is done: `X3/src/Platform/OpenGL/` deleted, every `#ifdef X3_USE_OPENGL` branch removed, `X3_GRAPHICS_API` and the four graphics-API presets replaced by plain `debug`/`release`. Nothing here is safe to start before that.
2. The additive step of the resource-layer work has landed: `X3/src/Platform/Vulkan/VulkanCommon.h` (with `inline constexpr uint32_t FRAMES_IN_FLIGHT = 2;` and the `VK_CHECK` macro) and `X3/src/Platform/Vulkan/FrameContext.h` exist. This part depends on `FrameContext` and nothing else from that step. Its exact shape, restated so you do not need to look it up:

```cpp
// X3/src/Platform/Vulkan/FrameContext.h
class FrameContext {
public:
    uint32_t        index()  const { return m_Index; }   // 0..FRAMES_IN_FLIGHT-1
    VkCommandBuffer cmd()    const { return m_Cmd; }     // already in the recording state
    uint64_t        number() const { return m_Number; }  // monotonic, never wraps
    VulkanContext&  context() const { return *m_Context; }
private:
    friend class VulkanContext;
    VulkanContext*  m_Context = nullptr;
    VkCommandBuffer m_Cmd     = VK_NULL_HANDLE;
    uint32_t        m_Index   = 0;
    uint64_t        m_Number  = 0;
};
```
It is a non-owning view, valid only between `beginFrame()` and `endFrame()`. It is **never** stored by a resource or cached across frames.

**Verification status of this document.** Every `file:line` below was read from the working tree on the date of writing, not from the earlier specs (whose `X3/CMakeLists.txt` citations are stale — that file is now **141 lines**, and the numbers here are fresh). Both builds were run to completion:

```
cmake --build build/vulkan-debug -j 14   → 0 occurrences of 'error:', build/vulkan-debug/Debug/X3Editor produced (39 MB)
cmake --build build/opengl-debug -j 14   → 0 occurrences of 'error:', build/opengl-debug/Debug/X3Editor produced
```

Two corrections to earlier specs that a build/read caught and grep alone would not have:

- **`add_desired_present_mode` does not exist.** `vkb::SwapchainBuilder` has `set_desired_present_mode` (`X3/libs/vk-bootstrap/src/VkBootstrap.h:921`) and `add_fallback_present_mode` (`:923`). The earlier vSync snippet would not have compiled. Worse, `set_desired_present_mode` **inserts at the front** (`VkBootstrap.cpp:2110-2113`), so calling it twice reverses your intended order. See §4.
- **The `DXC_COMMAND` regression is now closed at the source.** `X3/CMakeLists.txt:39-43` explicitly sets `JPH_USE_VK/DX12/MTL` to `OFF`, so Jolt no longer auto-enables its Vulkan backend when `find_package(Vulkan)` succeeds and no longer needs `dxc`. This was confirmed by a full Vulkan build with no `DXC_COMMAND` anywhere in the tree. Do not re-add it.

---

## Implementation order

Seven commits. The order is not negotiable at the marked points.

| # | Commit | Constraint |
|---|---|---|
| 1 | Validation gating, custom debug callback, syncval + best-practices | **First.** It is the instrument that verifies every commit after it. |
| 2 | Frame identity (`m_FrameNumber`/`m_CompletedFrame`) + deferred-destruction queue | **Must precede commit 7.** See §2.4. |
| 3 | Instance/device: Vulkan 1.3, `dynamicRendering` + `synchronization2`, `VK_KHR_dynamic_rendering`, VMA version | **Must precede commit 6.** ImGui's `UseDynamicRendering` trips a null device dispatch without it. |
| 4 | `createSwapchain`: `TRANSFER_DST` usage, present mode / vSync, old-swapchain reuse, cleanup ordering | Independent of 5 and 6; do it here because 6 assumes `TRANSFER_DST`. |
| 5 | Semaphore re-indexing and lifetime, fence-leak fix | Independent of 6; small and separately verifiable. |
| 6 | **The frame commit**: lifecycle restructure + dynamic rendering + `endFrame` fallback + `blitImageToSwapchain` body + ImGui + `Application::run` | **Cannot be split.** See §6.0. Highest-risk change in Phase 1. |
| 7 | Batched uploads; delete `beginSingleTimeCommands`/`endSingleTimeCommands` | **Requires 2.** Removing the wait without the deletion queue converts a hidden stall into a use-after-free. |

---

## 1. Commit 1 — Validation layer gating, and making "validation clean" mean something

### 1.1 Current behaviour

`X3/src/Platform/Vulkan/VulkanContext.h:166`:

```cpp
	bool m_EnableValidationLayers = true; // Disable in release builds
```

Consumed at `X3/src/Platform/Vulkan/VulkanContext.cpp:49-52`:

```cpp
	builder.set_app_name("X3 Engine")
		.request_validation_layers(m_EnableValidationLayers)
		.use_default_debug_messenger()
		.require_api_version(1, 2, 0);  // Vulkan 1.2 minimum
```

Nothing acts on the comment. `X3/CMakeLists.txt` has no build-type-derived define at all: the only `target_compile_definitions` blocks are `:121-123` (`BUILD_INSTALL`), `:125-130` (the graphics-API define, deleted by Phase 1a), `:131-134` (resource paths) and `:135-137` (`LR_PLATFORM_WINDOWS`). `CMakePresets.json` sets `CMAKE_BUILD_TYPE` per preset; no source file reads it.

### 1.2 Why it is wrong

Not a spec violation — a shipping defect with a correctness consequence.

`request_validation_layers` is the non-fatal variant (`X3/libs/vk-bootstrap/src/VkBootstrap.h:411`): it silently proceeds when `VK_LAYER_KHRONOS_validation` is absent. So a shipped binary's behaviour depends on whether the end user happens to have the Vulkan SDK installed, and validation costs 2-10× CPU time in the driver plus heavy allocation. `use_default_debug_messenger()` prints to stdout, which for a GUI application means the messages are usually lost entirely — which is how a repo can accumulate the per-frame `VUID-vkCmdDispatch-renderpass` error documented in §6.0 without anyone noticing.

More importantly: **standard validation alone does not find most of what the rest of this document fixes.** The hazards in §5 and §6 are cross-submission (frame *N* versus frame *N-1*, different `vkQueueSubmit` calls). Only synchronization validation with submit-time validation enabled sees them.

### 1.3 Fix — CMake

`X3/CMakeLists.txt`, immediately after the `BUILD_INSTALL` definitions block that ends at **line 123**:

```cmake
# Vulkan validation layers: on in every configuration except Release.
# Force with -DX3_VULKAN_VALIDATION=ON/OFF.
if(NOT DEFINED X3_VULKAN_VALIDATION)
    target_compile_definitions(X3Engine PUBLIC
        $<$<NOT:$<CONFIG:Release>>:X3_VULKAN_VALIDATION>)
else()
    target_compile_definitions(X3Engine PUBLIC
        $<$<BOOL:${X3_VULKAN_VALIDATION}>:X3_VULKAN_VALIDATION>)
endif()
```

Use the generator expression, **not** `option(... $<IF:$<CONFIG:Release>,OFF,ON>)` as an earlier spec proposed — `option()` does not evaluate generator expressions and would store the literal string. `$<CONFIG:...>` resolves correctly under both single-config generators (from `CMAKE_BUILD_TYPE`, which is what the presets set) and multi-config ones.

### 1.4 Fix — header

`VulkanContext.h:166` becomes:

```cpp
#ifdef X3_VULKAN_VALIDATION
	bool m_EnableValidationLayers = true;
#else
	bool m_EnableValidationLayers = false;
#endif
```

### 1.5 Fix — `createInstance()`

Replace `VulkanContext.cpp:45-54` with:

```cpp
static VKAPI_ATTR VkBool32 VKAPI_CALL X3DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT /*types*/,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOG_ENGINE_ERROR("[vulkan] {}", data->pMessage);
#ifdef X3_VULKAN_VALIDATION
    #if defined(_MSC_VER)
        __debugbreak();
    #elif defined(__GNUC__) || defined(__clang__)
        __builtin_trap();
    #endif
#endif
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_ENGINE_WARN("[vulkan] {}", data->pMessage);
    } else {
        LOG_ENGINE_INFO("[vulkan] {}", data->pMessage);
    }
    return VK_FALSE;   // MUST be VK_FALSE: VK_TRUE aborts the offending call.
}

void VulkanContext::createInstance() {
    if (const char* env = std::getenv("X3_VULKAN_VALIDATION"))
        m_EnableValidationLayers = (env[0] == '1');

    vkb::InstanceBuilder builder;
    builder.set_app_name("X3 Engine")
        .request_validation_layers(m_EnableValidationLayers)
        .set_debug_callback(&X3DebugCallback)                       // VkBootstrap.h:416
        .set_debug_messenger_severity(                              // VkBootstrap.h:420
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        .require_api_version(1, 3, 0);                              // was (1,2,0); see §3

    if (m_EnableValidationLayers) {
        // VkBootstrap.h:434. Enum values: /usr/include/vulkan/vulkan_core.h:18470, :18472.
        builder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        builder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
    }
    // ... existing build()/error handling at :54-64, unchanged
}
```

Notes:
- The `__builtin_trap()` on error is what turns "there was a VUID somewhere in the log" into "the debugger is stopped at the offending call". It fires only in validation-enabled builds.
- `set_debug_messenger_severity` replaces `use_default_debug_messenger()`'s default (which includes `VERBOSE`/`INFO`) and keeps the log readable. Add `VERBOSE`/`INFO` behind a second env var if you ever need them.

### 1.6 Enabling extra validation at run time — use `vkconfig`, not environment variables

The `add_validation_feature_enable` calls above are the *default-on* path for developer builds. For anything beyond that — turning on GPU-assisted validation, shader `printf`, message filtering, or **submit-time synchronization validation, which is not exposed through `VkValidationFeatureEnableEXT` at all** — the primary path is the **Vulkan Configurator (`vkconfig`)**, which ships with the SDK.

Reasons this is the recommendation rather than environment variables:
- The layer's settings interface changed around SDK 1.3.275 and the environment-variable names differ across SDK versions. Any list written down here will rot. `vkconfig`'s UI names are stable and self-documenting.
- `vkconfig`'s layer-override mode writes a persistent settings file that takes precedence over what the application requested, so it works without a rebuild and without touching the shell each run.
- It exposes the object tracker's leak report, which is one of the pass criteria in §8.

Procedure: launch `vkconfig`, select the **Synchronization** preset for `VK_LAYER_KHRONOS_validation`, additionally enable *Submit-time validation* under Synchronization and *Best Practices*, and apply the override. If you need a non-interactive equivalent for CI, use a `vk_layer_settings.txt` in the working directory — again, generate it from `vkconfig` rather than hand-writing key names.

**Submit-time validation is not optional here.** Command-buffer-time syncval alone cannot see any of the cross-submission hazards in §5 or §6.

### 1.7 Verification

- Configure Release; confirm `X3_VULKAN_VALIDATION` is absent from `build/release/compile_commands.json`; run; confirm the log has no `[vulkan]` lines.
- Configure Debug; run; deliberately break something (e.g. temporarily set `range = 0` on a `VkDescriptorBufferInfo`) and confirm the message arrives through `LOG_ENGINE_ERROR` and the debugger stops.
- `X3_VULKAN_VALIDATION=0 ./X3Editor` in a Debug build must start with layers off.
- With the layers on, **record the baseline now**: `VUID-vkCmdDispatch-renderpass` will be firing every frame (see §6.0(a)). That message disappearing is the acceptance test for commit 6.

---

## 2. Commit 2 — Frame identity and the deferred-destruction queue

### 2.1 Current behaviour

`VulkanContext` has `m_CurrentFrame` (`VulkanContext.h:153`) and `MAX_FRAMES_IN_FLIGHT = 2` (`VulkanContext.h:146`). There is no monotonic frame counter and no deletion queue. Every Vulkan resource destructor in `X3/src/Platform/Vulkan/` destroys inline, and `Renderer::SetupGPUResources` reassigns `shared_ptr`s to buffers and images every time a size changes (`Renderer.cpp:202-203`, `:244`, `:257-259`, and the sibling blocks at `:269-271`, `:281-283`, `:294-296`, `:314-322`, `:328-336`, `:342-350`).

### 2.2 Why it is wrong

`VUID-vkDestroyBuffer-buffer-00922`: *"All submitted commands that refer to buffer … must have completed execution."* With `MAX_FRAMES_IN_FLIGHT = 2`, `beginFrame`'s fence wait (`VulkanContext.cpp:346`) proves only that frame *N-2* completed; frame *N-1*'s command buffer is pending and its descriptor set holds the exact `VkBuffer` that is being freed. Freeing it returns the pages to VMA, which hands them to the next allocation.

The image/texture paths are safe **only by accident**: the replacement's constructor calls `endSingleTimeCommands` → `vkQueueWaitIdle` (`VulkanContext.cpp:530`) *before* the `shared_ptr` assignment drops the old object. Commit 7 destroys that accident, which is exactly why this commit must precede it.

### 2.3 Fix — frame identity

`VulkanContext.h`, private section next to `m_CurrentFrame` (`:153`):

```cpp
	uint64_t m_FrameNumber   = 0;  // number of the frame about to be recorded
	uint64_t m_CompletedFrame = 0; // highest frame number known to have completed on the GPU
```

Public, next to `getCurrentFrame()` (`:58`):

```cpp
	uint64_t getFrameNumber()    const { return m_FrameNumber; }
	uint64_t getCompletedFrame() const { return m_CompletedFrame; }
```

The arithmetic, placed in `beginFrame()` immediately after the fence wait at `VulkanContext.cpp:346` succeeds:

```cpp
	// The fence for slot m_CurrentFrame was signalled by the submit of frame
	// (m_FrameNumber - MAX_FRAMES_IN_FLIGHT). Guard the subtraction: m_FrameNumber
	// is unsigned and is 0 and 1 for the first two frames.
	if (m_FrameNumber >= MAX_FRAMES_IN_FLIGHT)
		m_CompletedFrame = m_FrameNumber - MAX_FRAMES_IN_FLIGHT;
	drainDeletionQueue();
```

`m_FrameNumber` is incremented in `present()` (§6.3), not in `beginFrame()`. Take this formulation rather than the equivalent-looking `m_FrameNumber - retireFrame >= FRAMES_IN_FLIGHT`: that form underflows for the first two frames and destroys resources one frame early.

**The invariant, which must be written into the header as a comment:** *every CPU write to a resource slot indexed by `frame.index()` happens after `beginFrame()` has waited `m_InFlightFences[m_CurrentFrame]` and before `endFrame()` submits.* After commit 6 this holds structurally, because `Application::run` calls `beginFrame()` before `LayerStack::onUpdate()` and `endFrame()` after it, in the same iteration. It is the entire correctness argument for the per-frame buffer rings that the resource-layer work adds later, and it cannot be verified by any tool — synchronization validation does not model host writes to persistently-mapped memory. Re-check it by hand whenever `Application::run` is touched, including in Phase 4 when the job system starts moving work off the main thread.

### 2.4 Fix — deferred-destruction queue

`VulkanContext.h` public:

```cpp
	// Freed once the frame in which they were retired has completed on the GPU,
	// i.e. FRAMES_IN_FLIGHT frames later. Drained at the top of beginFrame().
	void deferDestroy(VkBuffer buffer, VmaAllocation allocation);
	void deferDestroy(VkImage image, VmaAllocation allocation, VkImageView view);
	void deferFreeDescriptorSets(std::span<const VkDescriptorSet> sets);
```

private:

```cpp
	struct PendingDelete {
		uint64_t                     retireFrame = 0;
		VkBuffer                     buffer      = VK_NULL_HANDLE;
		VkImage                      image       = VK_NULL_HANDLE;
		VkImageView                  view        = VK_NULL_HANDLE;
		VmaAllocation                allocation  = VK_NULL_HANDLE;
		std::vector<VkDescriptorSet> sets;
	};
	std::vector<PendingDelete> m_DeletionQueue;
	void drainDeletionQueue();       // destroys entries with retireFrame <= m_CompletedFrame
	void drainDeletionQueueFully();  // vkDeviceWaitIdle + destroy everything
```

Semantics:
- Each `deferDestroy`/`deferFreeDescriptorSets` pushes with `retireFrame = m_FrameNumber`.
- `drainDeletionQueue()` destroys and erases every entry with `retireFrame <= m_CompletedFrame`. Within one entry the order is: **view, then image/buffer** (`vkDestroyImageView` before `vmaDestroyImage`); `vkFreeDescriptorSets` uses `m_DescriptorPool`, which was created with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` (`VulkanContext.cpp:479`) — that flag is what makes `deferFreeDescriptorSets` legal, so do not remove it.
- `drainDeletionQueueFully()` is called at the top of `cleanup()` (`VulkanContext.cpp:741`), right after the existing `vkDeviceWaitIdle` at `:743`.

**Placement matters.** `drainDeletionQueue()` runs after the fence wait and **before** the acquire, so that an `OUT_OF_DATE` early return (which happens on every frame of a click-and-drag resize) does not starve the queue.

Edge cases, all correct under this arithmetic and worth stating so nobody "optimizes" them:
- `deferDestroy` before the first frame (`m_FrameNumber == 0`) → destroyed at the start of frame 2.
- `deferDestroy` between `endFrame()` and `present()` (`m_FrameNumber` is still *N*, and frame *N* was submitted) → retired correctly.
- `deferDestroy` from an event handler after `present()` (`m_FrameNumber == N+1`) → destroyed at the start of frame *N+3*. Conservative, still correct.

### 2.5 Verification

`VUID-vkDestroyBuffer-buffer-00922` / `VUID-vkDestroyImage-image-01000` / `VUID-vkDestroyImageView-imageView-01026` must be absent while adding and deleting mesh entities at run time and while changing render resolution repeatedly. The object tracker (via `vkconfig`) must report zero leaked objects at `vkDestroyInstance`.

---

## 3. Commit 3 — Device and instance enablement for dynamic rendering

### 3.1 Current behaviour

`createLogicalDevice` (`VulkanContext.cpp:95-104`) is:

```cpp
	vkb::DeviceBuilder device_builder{ m_VkbPhysicalDevice };
	auto dev_ret = device_builder.build();
```

with no feature configuration whatsoever, and `pickPhysicalDevice` (`:74-93`) requests only:

```cpp
	selector.set_surface(m_Surface)
		.set_minimum_version(1, 2);  // Require Vulkan 1.2
```

On the vk-bootstrap side, `fill_out_phys_dev_with_criteria` copies `criteria.required_features` and `criteria.extended_features_chain` into the selected device (`X3/libs/vk-bootstrap/src/VkBootstrap.cpp:1268-1269`) and `DeviceBuilder::build` enables exactly those (`:1654-1670`). `criteria.required_features` is a value-initialized `VkPhysicalDeviceFeatures{}` and the chain is empty. **The engine today enables zero device features.**

### 3.2 Decision: Vulkan 1.3 core **and** the `VK_KHR_dynamic_rendering` extension. Both.

This is the single most likely thing to get wrong, so the reasoning is recorded. With `VkPhysicalDeviceVulkan13Features::dynamicRendering = VK_TRUE` and **no** extension enabled:

- `vkGetInstanceProcAddr(inst, "vkCmdBeginRenderingKHR")` returns **non-NULL** — a loader trampoline.
- `vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR")` returns **NULL** — the device dispatch entry behind the trampoline is empty.
- `vkGetDeviceProcAddr(dev, "vkCmdBeginRendering")` (no suffix) returns non-NULL.

The vendored ImGui resolves its pointer via `vkGetInstanceProcAddr(info->Instance, "vkCmdBeginRenderingKHR")` (`X3-Editor/libs/imgui-docking/imgui_impl_vulkan.cpp:1098-1099`) and asserts non-NULL at `:1101-1102`. **That assert passes**, and the failure surfaces at the first `ImGui_ImplVulkan_RenderDrawData` instead of at init. The header says so verbatim at `imgui_impl_vulkan.h:89`: *"Need to explicitly enable VK_KHR_dynamic_rendering extension to use this, even for Vulkan 1.3."*

(The `IMGUI_IMPL_VULKAN_USE_LOADER` path at `imgui_impl_vulkan.cpp:1078-1079` does not apply: it is gated on `VK_NO_PROTOTYPES` at `:112-113`, and this build uses `<vulkan/vulkan.h>` with prototypes.)

### 3.3 Fix — exact calls

`createInstance` (already edited in commit 1): `.require_api_version(1, 3, 0)` replacing `.require_api_version(1, 2, 0)` at `VulkanContext.cpp:52`.

`pickPhysicalDevice`, replacing `VulkanContext.cpp:76-78`:

```cpp
	VkPhysicalDeviceVulkan13Features features13{};
	features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features13.dynamicRendering = VK_TRUE;
	features13.synchronization2 = VK_TRUE;   // enabled, deliberately unused this phase

	vkb::PhysicalDeviceSelector selector{ m_VkbInstance };
	selector.set_surface(m_Surface)
		.set_minimum_version(1, 3)                                        // was (1, 2)
		.set_required_features_13(features13)                             // VkBootstrap.h:679
		.add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME); // VkBootstrap.h:638
```

`createLogicalDevice` (`:95-104`) needs **no** edit: `DeviceBuilder` reads the selected `PhysicalDevice`'s `extensions_to_enable` (`VkBootstrap.cpp:1630-1632`) and `extended_features_chain` (`:1654-1665`), both populated by the selector at `:1268-1281`.

API-name corrections against plausible guesses: it is `set_required_features_13` (**not** `set_required_features13`, not `require_features_13`), taking `VkPhysicalDeviceVulkan13Features const&` and setting `sType` for you (`VkBootstrap.cpp:1427-1432`); and `add_required_extension(const char*)` (`VkBootstrap.h:638`).

**Do not** hand-build a `VkPhysicalDeviceFeatures2` and pass it via `DeviceBuilder::add_pNext` — vk-bootstrap errors on that combination (`DeviceError::VkPhysicalDeviceFeatures2_in_pNext_chain_while_using_add_required_extension_features`, `VkBootstrap.h:215`, raised at `VkBootstrap.cpp:1649-1651`).

**Do not** also pass a `VkPhysicalDeviceDynamicRenderingFeatures`. `VUID-VkDeviceCreateInfo-pNext-06532` forbids a `VkPhysicalDeviceVulkan13Features` and the individual feature structs it subsumes in the same `pNext` chain. The 1.3 aggregate is a superset; use it alone.

### 3.4 Knock-on edit

`createAllocator` (`VulkanContext.cpp:139`): `allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;` → `VK_API_VERSION_1_3`. VMA asserts this does not exceed the instance version and it should track it.

### 3.5 `synchronization2` is enabled but not used

Every barrier in this document is written in synchronization-1 form (`VkImageMemoryBarrier` / `vkCmdPipelineBarrier`) to match the existing `blitImageToSwapchain` body. Converting to `VkImageMemoryBarrier2` / `vkCmdPipelineBarrier2` is a mechanical follow-up with no behavioural change, and mixing both forms in one commit makes the diff unreadable. Enabling the feature bit now costs nothing and makes the later conversion a pure edit.

### 3.6 Descriptor indexing — deliberately **not** here

The mesh-attribute work (Phase 2) will need a sampled-image array (`uniform sampler2D u_MaterialTextures[128]`), which requires `VkPhysicalDeviceVulkan12Features::descriptorIndexing`, `runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingPartiallyBound` and `descriptorBindingVariableDescriptorCount`. When that lands, add a second `set_required_features_12(features12)` call (`VkBootstrap.h:674`) **alongside** the `_13` call — `VkPhysicalDeviceVulkan12Features` and `VkPhysicalDeviceVulkan13Features` are both versioned aggregates and may coexist in one `pNext` chain; `VUID-VkDeviceCreateInfo-pNext-06532` prohibits mixing an aggregate with the *individual* structs it subsumes, not 1.2 with 1.3. Do not add them speculatively now: each required feature is a hard device-selection filter, and `descriptorBindingUniformBufferUpdateAfterBind` in particular is `VK_FALSE` on a meaningful share of drivers and on MoltenVK. Adding an unused required feature makes the engine refuse to start on hardware that would otherwise work.

### 3.7 A risk worth one sentence

`PhysicalDeviceSelector::populate_device_details` queries `VkPhysicalDeviceVulkan13Features` on *every* enumerated physical device (`VkBootstrap.cpp:1101-1113`) *before* `is_device_suitable` filters on version (`:1220`, `:1130`). On a machine with a mixed GPU set where one device reports only 1.2, that query is technically out of contract. Drivers ignore unrecognised `pNext` structs in practice and vk-bootstrap ships this way — but if device selection misbehaves on a heterogeneous machine, look here first.

### 3.8 MoltenVK

`VK_KHR_dynamic_rendering` has been in MoltenVK since the 1.2.x series and MoltenVK has advertised Vulkan 1.3 since 1.2.5 (SDK 1.3.268); any SDK current in 2026 is far past both. This could **not** be verified on this machine — there is no macOS build and no vendored MoltenVK anywhere in the tree (`grep -rni moltenvk` over the non-`libs` build files returns nothing; there is no `if(APPLE)` in `CMakePresets.json` or any `CMakeLists.txt`). Confirm on the target Mac with one command before relying on it:

```
vulkaninfo | grep -E 'apiVersion|dynamicRendering|VK_KHR_dynamic_rendering'
```

Expect `dynamicRendering = true` and `VK_KHR_dynamic_rendering` in the device extension list. If either is absent the SDK is too old, and there is no workaround short of keeping render passes.

Portability enumeration is already handled: vk-bootstrap sets `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` and adds `VK_KHR_portability_enumeration` when available (`VkBootstrap.cpp:742-749`, `:843-844`), and adds `VK_KHR_portability_subset` to the device automatically (`:1272-1280`). No macOS-specific code is needed.

---

## 4. Commit 4 — Swapchain creation: usage flags, present mode, old-swapchain reuse

### 4.1 Current behaviour

`VulkanContext::createSwapchain()` (`VulkanContext.cpp:149-173`):

```cpp
	vkb::SwapchainBuilder swapchain_builder{ m_VkbDevice };

	auto swap_ret = swapchain_builder
		.set_old_swapchain(m_Swapchain)
		.build();
```

Three defects in five lines.

**(a) No `TRANSFER_DST` image usage.** vk-bootstrap's default is `VkImageUsageFlags image_usage_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;` (`X3/libs/vk-bootstrap/src/VkBootstrap.h:1002`, reset to the same at `VkBootstrap.cpp:2136`), copied verbatim into `VkSwapchainCreateInfoKHR::imageUsage`. But `blitImageToSwapchain` does `vkCmdClearColorImage` into the swapchain image at `VulkanContext.cpp:887` and `vkCmdBlitImage` into it at `:908`, and transitions it to `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` at `:862`.

Three hard VUIDs, violated on every runtime frame:
- `VUID-vkCmdBlitImage-dstImage-00224`: `dstImage` must have been created with `VK_IMAGE_USAGE_TRANSFER_DST_BIT`.
- `VUID-vkCmdClearColorImage-image-00002`: same requirement.
- `VUID-VkImageMemoryBarrier-oldLayout-01213` family: `TRANSFER_DST_OPTIMAL` requires that usage flag.

Under render passes this was already an error. Under dynamic rendering it is unchanged in severity but is now the *only* thing standing between the runtime and a bad swapchain, since there is no `VkFramebuffer` creation left to independently validate the view's usage.

**(b) `RenderSettings::vSync` is dead.** `X3/src/Renderer/RenderSettings.h:38`:

```cpp
        bool vSync = true;
```

serialized at `:51`, deserialized at `:70`. `grep -rn -i "vsync|present_mode|PRESENT_MODE" X3/src/Platform/Vulkan/` returns **nothing** — verified, zero matches. The only implementation is `GLFWWindowIMPL::setVSync` (`X3/src/Platform/Windows/GLFWWindow.cpp:107-110`):

```cpp
	void GLFWWindowIMPL::setVSync(bool enabled) {
		m_VSync = enabled;
		glfwSwapInterval(enabled);
	}
```

`glfwSwapInterval` requires a current OpenGL/GLES context and is a **no-op** when the window was created with `GLFW_CLIENT_API == GLFW_NO_API`, which is exactly what `VulkanContext::setWindowHints()` sets (`VulkanContext.cpp:9-12`). So the checkbox at `X3-Editor/src/Panels/RenderSettingsPanel/RenderSettingsPanel.cpp:216`, the `SetVSyncEvent` handler at `X3-Editor/src/EditorLayer.cpp:58-62`, and `ExportSettings::vSync` → `X3-Runtime/src/RuntimeLayer.cpp:75` all terminate in a no-op.

What actually happens: with no present mode requested, `build()` falls back to vk-bootstrap's default preference list `{ MAILBOX, FIFO }` (`VkBootstrap.cpp:2172-2175`, applied at `:1899`). **The engine therefore behaves as though vSync is permanently off**, regardless of the setting.

**(c) `set_old_swapchain(m_Swapchain)` is always `VK_NULL_HANDLE`.** `recreateSwapchain()` calls `cleanupSwapchain()` first (`VulkanContext.cpp:678`), which destroys the swapchain and nulls the handle (`:733-736`), *then* calls `createSwapchain()` at `:681`. The old-swapchain retirement path never engages; every resize is a full teardown with a visible black flash.

### 4.2 Fix — signature change

`createSwapchain` takes the old handle explicitly instead of reading a member that has already been cleared. `VulkanContext.h:99`:

```cpp
	void createSwapchain(VkSwapchainKHR oldSwapchain);
```

`init()` (`VulkanContext.cpp:30`) calls `createSwapchain(VK_NULL_HANDLE);`.

### 4.3 Fix — body

Replace `VulkanContext.cpp:151-172`:

```cpp
	vkb::SwapchainBuilder swapchain_builder{ m_VkbDevice };
	swapchain_builder.set_old_swapchain(oldSwapchain);

	if (m_VSync) {
		swapchain_builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
	} else {
		// set_desired_present_mode INSERTS AT THE FRONT (VkBootstrap.cpp:2110-2113);
		// add_fallback_present_mode APPENDS (VkBootstrap.h:923). Calling
		// set_desired_present_mode twice would reverse the intended order.
		swapchain_builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR);
		swapchain_builder.add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR);
		swapchain_builder.add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR);
	}

	// The runtime path clears and blits into the swapchain image.
	// add_ (VkBootstrap.h:932) ORs; set_ (VkBootstrap.h:930) REPLACES and would
	// strip COLOR_ATTACHMENT, which the editor's rendering block needs.
	swapchain_builder.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);

	auto swap_ret = swapchain_builder.build();
	if (!swap_ret) {
		LOG_ENGINE_CRITICAL("Failed to create Vulkan swapchain: {}", swap_ret.error().message());
		throw std::runtime_error("Failed to create Vulkan swapchain");
	}

	m_VkbSwapchain         = swap_ret.value();
	m_Swapchain            = m_VkbSwapchain.swapchain;
	m_SwapchainImageFormat = static_cast<VkFormat>(m_VkbSwapchain.image_format);
	m_SwapchainExtent      = m_VkbSwapchain.extent;
	m_SwapchainImages      = m_VkbSwapchain.get_images().value();
	m_SwapchainImageViews  = m_VkbSwapchain.get_image_views().value();

	// Report what was actually granted, so "vsync doesn't work" is diagnosable.
	LOG_ENGINE_INFO("Swapchain: {} images, {}x{}, format {}, present mode {}, usage 0x{:x}",
		m_SwapchainImages.size(), m_SwapchainExtent.width, m_SwapchainExtent.height,
		int(m_SwapchainImageFormat), int(m_VkbSwapchain.present_mode),
		uint32_t(m_VkbSwapchain.image_usage_flags));
```

`m_VkbSwapchain.present_mode` and `.image_usage_flags` are `VkBootstrap.h:850` and `:846` respectively — the values *actually used*, not the requested ones.

### 4.4 Present-mode availability — no manual query needed

Do **not** hand-roll `vkGetPhysicalDeviceSurfacePresentModesKHR`. `detail::find_present_mode` (`VkBootstrap.cpp:1811-1821`) walks the desired list in order, picks the first that is available, and falls back to `VK_PRESENT_MODE_FIFO_KHR` unconditionally when nothing matches. FIFO is the one mode the spec requires every `VK_KHR_swapchain` implementation to support, so the fallback can never fail.

Mapping, and the reasoning behind it:

| `vSync` | Preference order | Rationale |
|---|---|---|
| `true` | `FIFO_KHR` only | Guaranteed present, hard-throttles to refresh, no tearing. That is what a user ticking "VSync" means. Do **not** silently prefer `FIFO_RELAXED_KHR` — it tears on late frames, which is precisely what the user asked not to happen. |
| `false` | `MAILBOX_KHR`, then `IMMEDIATE_KHR`, then `FIFO_KHR` | MAILBOX: unthrottled, tear-free, cheapest to the compositor; needs ≥3 images, which the `minImageCount + 1` default supplies (`VkBootstrap.cpp:1911-1913`). IMMEDIATE: genuinely lowest latency but tears; the honest fallback when MAILBOX is absent, which is common on some Wayland/X11 driver paths. FIFO last so `build()` cannot fail. |

Do not add `FIFO_RELAXED_KHR` to either list — it is a third behaviour that neither checkbox state describes.

### 4.5 Fix — the usage-flag failure mode

If a platform does not support `TRANSFER_DST` on the surface, `build()` returns `SwapchainError::required_usage_not_supported` (`VkBootstrap.h:224`, raised at `VkBootstrap.cpp:1946`) and the existing error path throws with that message. That is the right failure mode — a hard, loud failure rather than a validation-error-per-frame that happens to work on your driver. **Do not** paper over it by making `add_image_usage_flags` conditional. On macOS this depends on MoltenVK disabling `CAMetalLayer.framebufferOnly` in response to the non-attachment usage request, which it does; if it ever fails, the correct response is to replace the blit with a fullscreen-triangle draw, not to weaken the request.

Locally verified supported: `vulkaninfo` reports this surface's `supportedUsageFlags` as `TRANSFER_SRC | TRANSFER_DST | SAMPLED | STORAGE | COLOR_ATTACHMENT | INPUT_ATTACHMENT`.

Note that vk-bootstrap only performs the usage check for unextended present modes (`VkBootstrap.cpp:1938-1947`) — `IMMEDIATE`, `MAILBOX`, `FIFO`, `FIFO_RELAXED`. All four of ours qualify.

### 4.6 Fix — `recreateSwapchain` ordering

Replace `VulkanContext.cpp:677-682`:

```cpp
	// Keep the old swapchain alive across creation so the driver can retire it
	// incrementally instead of tearing everything down (removes the black flash).
	VkSwapchainKHR           oldSwapchain = m_Swapchain;
	std::vector<VkImageView> oldViews     = std::move(m_SwapchainImageViews);
	m_SwapchainImageViews.clear();

	createSwapchain(oldSwapchain);

	for (auto view : oldViews)
		vkDestroyImageView(m_Device, view, nullptr);
	if (oldSwapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(m_Device, oldSwapchain, nullptr);
```

The `vkDeviceWaitIdle` at `:665` and the minimize spin at `:668-673` are unchanged and must stay. `createFramebuffers()` at `:682` is deleted by commit 6 (there are no framebuffers). `cleanupSwapchain()` survives but is now called only from `cleanup()`.

### 4.7 Fix — `cleanup()` ordering

`cleanup()` (`VulkanContext.cpp:741-808`) currently destroys the semaphores at `:747-756` and only reaches `cleanupSwapchain()` at `:783`. Reorder: **`cleanupSwapchain()` first, then the semaphores.** `vkDeviceWaitIdle` waits for *queue* operations; `vkQueuePresentKHR` hands its semaphore wait to the presentation engine, which is not a queue operation and is explicitly not covered by device-idle. Destroying the swapchain first is what closes that window at shutdown. See §5.3 for the same problem at resize.

### 4.8 Fix — vSync plumbing

`VulkanContext.h`:

```cpp
	VulkanContext(GLFWwindow* window, bool vsync);
	void setVSync(bool enabled);              // recreates the swapchain if the value changed
	bool getVSync() const { return m_VSync; }
private:
	bool m_VSync = true;
```

```cpp
void VulkanContext::setVSync(bool enabled) {
	if (enabled == m_VSync) return;
	m_VSync = enabled;
	recreateSwapchain();
}
```

`X3/src/Platform/Windows/GLFWWindow.cpp:50` currently constructs the context and `:56` calls `setVSync(windowProps.VSync)` — *after* `m_Context->init()`, i.e. after the swapchain already exists. Pass the flag in so the first swapchain is correct and no immediate recreation happens:

```cpp
	m_Context = new VulkanContext(m_NativeWindow, windowProps.VSync);
```

Replace `GLFWWindow.cpp:107-110`:

```cpp
	void GLFWWindowIMPL::setVSync(bool enabled) {
		m_VSync = enabled;
		if (auto* ctx = VulkanContext::Get()) ctx->setVSync(enabled);
	}
```

Do **not** add a virtual `setVSync` to `IRenderingContext` — that interface is being deleted.

Single source of truth: `WindowProps::VSync` defaults to `false` (`X3/src/Core/IWindow.h:25`) while `RenderSettings::vSync` defaults to `true` (`RenderSettings.h:38`) and `EditorState::temp.vSync` defaults to `false` (`X3-Editor/src/EditorState.h:21`). Change `IWindow.h:25`'s default to `true` to match the persisted project setting, or document why they differ. `ExportSettings::vSync` (`X3/src/Export/ExportSettings.h:29` → `RuntimeLayer.cpp:75`) then works unchanged.

### 4.9 Verification

- Start the editor; the new log line must read present mode `2` (`VK_PRESENT_MODE_FIFO_KHR`) with the setting on and `1` (`MAILBOX`) or `0` (`IMMEDIATE`) with it off, and `usage` must include bit `0x2` (`TRANSFER_DST`) alongside `0x10` (`COLOR_ATTACHMENT`).
- Toggle the checkbox at run time: the log must show a swapchain recreation and the new mode. `ProfilerPanel`'s frame rate must clamp to the display refresh with vSync on and exceed it with it off (lower `resolution` enough that the compute dispatch is not the bottleneck).
- Run the **runtime** executable with a project open: the three `TRANSFER_DST` VUIDs above must be gone.
- Resize by dragging: the black flash on every resize must be gone.
- Best-practices layer must not warn about MAILBOX with fewer than three images.
- macOS/MoltenVK: verify the reported mode and that the fallback chain does not land somewhere unexpected. Test this now, not at Phase 10.

---

## 5. Commit 5 — Semaphore indexing, semaphore lifetime, and the fence leak

### 5.1 Current behaviour

Semaphores are allocated per swapchain image (`VulkanContext.cpp:313-315`, `:332-339`) and indexed by a free-running counter, `m_CurrentSemaphoreIndex` (`VulkanContext.h:154`), advanced modulo the image count at `:457`:

```cpp
	m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	m_CurrentSemaphoreIndex = (m_CurrentSemaphoreIndex + 1) % m_SwapchainImages.size();
```

It is used at the acquire (`:352`), the submit wait (`:416`), the submit signal (`:425`), and is reset in `recreateSwapchain` (`:711`). Those six sites are the complete set (verified by grep across `X3/src`, `X3-Editor/src`, `X3-Runtime/src`).

### 5.2 Why the pairing is unsound

The rule that matters: a `VkSemaphore` may be signalled again only once every prior wait on it has been retired.

For `m_RenderFinishedSemaphores`, the waiter is `vkQueuePresentKHR` (`:437-438`). A present's semaphore wait is retired by the presentation engine, and the only signal the application gets that this has happened is that `vkAcquireNextImageKHR` hands the corresponding **image** back. So the semaphore's reuse must be gated on the *image index*, not on a frame counter.

Today it is gated on `m_CurrentSemaphoreIndex`, which advances one step per frame regardless of which image `vkAcquireNextImageKHR` actually returned at `:351-352`. **`vkAcquireNextImageKHR` is not required to round-robin.** If acquire returns image 0 repeatedly — legal, and *routinely* what MAILBOX does, which is the mode the engine currently gets by default (§4.1(b)) — then the presents of images 1 and 2 are never retired and their `renderFinished` semaphores stay pending, yet the free-running counter reuses them after `imageCount` frames. That is a signal-on-a-semaphore-with-an-unretired-wait, `VUID-vkQueueSubmit-pSignalSemaphores-00067`. This is not theoretical for this engine; it is the live default configuration.

`m_ImageAvailableSemaphores` is, by luck, currently safe: it is reused every `imageCount` frames (≥ 2), and the fence wait at `:346` proves frame *N-2* completed. But it is safe for the wrong reason and it breaks the moment anyone changes `MAX_FRAMES_IN_FLIGHT`.

### 5.3 Why the recreation path is also wrong

`recreateSwapchain` destroys both semaphore arrays at `VulkanContext.cpp:686-691` having done nothing but `vkDeviceWaitIdle` at `:665`:

```cpp
	for (auto semaphore : m_ImageAvailableSemaphores) {
		vkDestroySemaphore(m_Device, semaphore, nullptr);
	}
	for (auto semaphore : m_RenderFinishedSemaphores) {
		vkDestroySemaphore(m_Device, semaphore, nullptr);
	}
```

`VUID-vkDestroySemaphore-semaphore-01137`: *"All submitted batches that refer to semaphore must have completed execution."* `vkDeviceWaitIdle` waits for **queue** operations. `vkQueuePresentKHR` (`:446`) hands the wait on `m_RenderFinishedSemaphores[...]` to the presentation engine, which is not a queue operation. And `recreateSwapchain` is reached from `endFrame` at `:450` *immediately after* the present that returned `SUBOPTIMAL`/`OUT_OF_DATE` — the highest-probability moment for the violation. This is the classic "validation error on window resize", and it is present here.

### 5.4 Fix — indexing

- `m_ImageAvailableSemaphores` → sized **`MAX_FRAMES_IN_FLIGHT`**, indexed by **`m_CurrentFrame`**. Safe because `beginFrame` waited `m_InFlightFences[m_CurrentFrame]`, which proves the submit that waited on this semaphore has completed, so the semaphore is unsignalled.
- `m_RenderFinishedSemaphores` → sized **`m_SwapchainImages.size()`**, indexed by **`m_ImageIndex`** at both the signal (`:425`) and the present wait (`:438`). Safe because semaphore *k* is only reused when image *k* is re-acquired, and image *k* is only re-acquired after its previous present consumed it.
- **Delete `m_CurrentSemaphoreIndex`** — `VulkanContext.h:154`, and all five uses (`.cpp:352`, `:416`, `:425`, `:457`, `:711`).

Update `createSyncObjects` (`:309-342`) accordingly: `m_InFlightFences` and `m_ImageAvailableSemaphores` both `resize(MAX_FRAMES_IN_FLIGHT)`; `m_RenderFinishedSemaphores` `resize(m_SwapchainImages.size())`.

Honest caveat to record in a comment: the `renderFinished`-by-image-index argument is the canonical pattern and is what every mainstream engine ships, but it is an inference from acquire semantics rather than an explicit spec guarantee. The airtight tool is `VK_EXT_swapchain_maintenance1`'s `VkSwapchainPresentFenceInfoEXT`, which is not universally available and is **not** on MoltenVK. Do not depend on it.

### 5.5 Fix — lifetime across recreation

Delete `VulkanContext.cpp:684-711` entirely (the destroy/clear/resize/recreate/reset block) and replace with a grow-only helper:

```cpp
// Semaphores are NEVER destroyed at swapchain recreation: vkDeviceWaitIdle does
// not retire an outstanding vkQueuePresentKHR wait (VUID-vkDestroySemaphore-
// semaphore-01137). m_ImageAvailableSemaphores does not depend on the image
// count at all. m_RenderFinishedSemaphores only ever grows.
void VulkanContext::ensureRenderFinishedSemaphores() {
	VkSemaphoreCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	while (m_RenderFinishedSemaphores.size() < m_SwapchainImages.size()) {
		VkSemaphore s = VK_NULL_HANDLE;
		VK_CHECK(vkCreateSemaphore(m_Device, &info, nullptr, &s));
		m_RenderFinishedSemaphores.push_back(s);
	}
}
```

Call it in `recreateSwapchain` after the swapchain rebuild from §4.6. The array may end up longer than the image count after a shrink; that is harmless — indexing by `m_ImageIndex` never reads past the current count. All semaphores are destroyed exactly once, in `cleanup()`, **after** `cleanupSwapchain()` per §4.7.

### 5.6 Fix — the fence leak

`beginFrame` resets the fence at `VulkanContext.cpp:364`:

```cpp
	// Only reset the fence if we're submitting work
	vkResetFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame]);
```

but can still return `false` at `:377` when `vkBeginCommandBuffer` fails. The fence is then unsignalled with nothing pending, and the next `vkWaitForFences` at `:346` blocks forever on `UINT64_MAX`. The comment at `:363` states an intent the code does not implement.

Fix: **delete `:363-364` and move `vkResetFences` into `endFrame`, immediately before `vkQueueSubmit`, with nothing between them that can fail.** Exact placement is in §6.2. Add the failure-path re-signal shown there so a failed submit cannot strand the fence either.

The resulting invariant, worth a header comment: *`m_InFlightFences[i]` is signalled if and only if there is no pending submit for slot `i`.*

### 5.7 Verification

Grab the window edge and resize continuously for ~10 seconds; minimize and restore (exercises the `width == 0` spin at `:668-673`); move the window between monitors of different refresh rates. Expect zero `VUID-vkDestroySemaphore-semaphore-01137`, zero `VUID-vkQueueSubmit-pSignalSemaphores-*`, zero `VUID-VkPresentInfoKHR-pWaitSemaphores-*`. Memory must be flat across a 2-hour resize soak — a leaked semaphore per resize is otherwise visible in the object tracker.

---

## 6. Commit 6 — The frame commit

### 6.0 Why this cannot be split, and the two bugs it deletes

`beginFrame` opening a render pass, `swapBuffers` inverting the frame boundary, and ImGui's render-pass handle are one knot. Deleting `beginFrame`'s render pass without adding `beginSwapchainRendering` leaves ImGui with no attachment; adding `UseDynamicRendering` to ImGui before the device enables the extension trips the null device dispatch of §3.2. One commit.

**(a) `vkCmdDispatch` is currently recorded inside an active render pass instance — every frame.**

`beginFrame()` opens `m_RenderPass` at `VulkanContext.cpp:392` and sets `m_RenderPassActive = true` at `:393`, and leaves it open for the whole frame: `swapBuffers` (`:639-661`) calls `endFrame()` then immediately `beginFrame()`, so the render pass for iteration *N+1* is opened at the end of iteration *N*. `VulkanComputeShader::Dispatch()` then records into `context->getCurrentCommandBuffer()` (`X3/src/Platform/Vulkan/VulkanComputeShader.cpp:106`) and issues `vkCmdDispatch` at `:137` plus a `COMPUTE_SHADER → FRAGMENT_SHADER` `vkCmdPipelineBarrier` at `:146-153`.

- `VUID-vkCmdDispatch-renderpass`: *"This command must only be called outside of a render pass instance."*
- `VUID-vkCmdPipelineBarrier-pDependencies-02285`: a barrier inside a render pass instance requires a subpass self-dependency (`srcSubpass == dstSubpass == current subpass`). `createRenderPass()` declares exactly one dependency, `VK_SUBPASS_EXTERNAL → 0` (`:196-202`, reused for the overlay pass at `:238`). There is none.

Its practical consequence is driver-dependent: some tolerate it, some fault, and tile-based GPUs — which is what MoltenVK sits on top of — are exactly the class most likely to misbehave, because a render pass instance is a tiling job with no compute capability inside it. In the new shape `beginFrame()` opens **no** rendering block, so compute is recorded at top level and this dissolves.

There is a second defect in the same knot. `init()` deliberately does not begin a frame (`VulkanContext.cpp:40-42`):

```cpp
	// Don't call beginFrame() here - let the first swapBuffers() call handle it
	// This avoids conflicts with ImGui font upload which creates its own command buffer
	m_FirstFrame = true;
```

so on the very first main-loop iteration `Dispatch()` records into `m_CommandBuffers[0]`, which has never been `vkBeginCommandBuffer`'d — the **initial** state, violating `VUID-vkCmdDispatch-commandBuffer-recording`. `ImGuiContext::EndFrame` then calls `ensureFrameStarted()` (`X3-Editor/src/ImGuiContext.cpp:278`) → `beginFrame()` → `vkResetCommandBuffer` at `:367`, silently discarding that first dispatch. With no render pass in `beginFrame`, the `m_FirstFrame` hack has no purpose.

**(b) The editor runs two render passes per frame to draw one ImGui layer.**

`beginFrame` opens `m_RenderPass` (`LOAD_OP_CLEAR` at `:180`, `finalLayout = PRESENT_SRC_KHR`); `ImGuiContext::EndFrame` at `ImGuiContext.cpp:283` calls `beginOverlayRenderPass()`, which *ends* that pass (`:594-598`), barriers `PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL` (`:603-621`), and opens `m_OverlayRenderPass` (`LOAD_OP_LOAD` at `:224`). The editor never blits — `blitImageToSwapchain` has exactly one caller repo-wide, `X3-Runtime/src/RuntimeLayer.cpp:183` (verified by grep). The editor's compute output reaches the screen only as an ImGui-sampled texture (`X3-Editor/src/Panels/ViewportPanel/ViewportPanel.cpp:93-97`, `ImGui_ImplVulkan_AddTexture(..., VK_IMAGE_LAYOUT_GENERAL)`). One `vkCmdBeginRendering` with `LOAD_OP_CLEAR` replaces both passes.

### 6.1 Inventory of what dies

| Artefact | Location | Replacement |
|---|---|---|
| `VkRenderPass m_RenderPass` | `VulkanContext.h:132` | Deleted. |
| `VkRenderPass m_OverlayRenderPass` | `VulkanContext.h:133` | Deleted. **The `m_RenderPass`-vs-`m_OverlayRenderPass` question is moot** — both go, and `createFramebuffers` is deleted rather than repointed. |
| `getRenderPass()` | `VulkanContext.h:32` | Deleted. Zero callers (grep across `X3/src`, `X3-Editor/src`, `X3-Runtime/src`). |
| `getOverlayRenderPass()` | `VulkanContext.h:33` | Deleted. Callers `ImGuiContext.cpp:171` and `:237` become `RenderPass = VK_NULL_HANDLE` + `UseDynamicRendering = true` (§6.5). |
| `createRenderPass()` decl/def/call | `VulkanContext.h:101`, `.cpp:175-246`, called `.cpp:31` | Deleted entirely: both `VkAttachmentDescription`s (`:177-185`, `:221-229`), the shared `VkSubpassDescription` (`:191-194`), the shared `VkSubpassDependency` (`:196-202`, reused at `:238`), both `vkCreateRenderPass` calls (`:213`, `:240`). |
| `createFramebuffers()` decl/def/calls | `VulkanContext.h:102`, `.cpp:248-270`, called `.cpp:32` and `.cpp:682` | Deleted. `framebufferInfo.renderPass = m_RenderPass` at `:256` is the hard-coded coupling; it disappears with both. |
| `std::vector<VkFramebuffer> m_Framebuffers` | `VulkanContext.h:127` | Deleted. All uses die: `.cpp:249`, `:263`, `:269`, `:384`, `:586-587`, `:628`, `:721-724`. Dynamic rendering consumes `m_SwapchainImageViews[m_ImageIndex]` directly. **Keep `m_SwapchainImageViews`** — it is now the only per-image object; `cleanupSwapchain` (`.cpp:719-739`) keeps its view loop (`:727-730`) and loses its framebuffer loop (`:721-724`). |
| `beginRenderPass()` | `VulkanContext.h:72`, `.cpp:535-576` | Deleted. Zero callers. |
| `beginOverlayRenderPass()` | `VulkanContext.h:73`, `.cpp:578-637` | Replaced by `beginSwapchainRendering()`/`endSwapchainRendering()`. Sole caller `ImGuiContext.cpp:283`. |
| `bool m_RenderPassActive` + `isRenderPassActive()` | `VulkanContext.h:167`, `:71` | Deleted. Uses: `.cpp:393`, `:400-403`, `:536`, `:575`, `:594-598`, `:635`, `:832-835`; and the assert at `ImGuiContext.cpp:290`. Replaced by `m_SwapchainImageWritten`, which tracks *layout state*, not pass state. |
| Render-pass begin block in `beginFrame` | `.cpp:380-393` | Deleted outright. |
| Render-pass end block in `endFrame` | `.cpp:399-403` | Replaced by the nothing-written fallback (§6.4). |
| Render-pass end inside the blit | `.cpp:832-835` | Deleted. There is no pass to end. |
| Render-pass destruction | `.cpp:774-780` | Deleted. |
| `swapBuffers()` | `VulkanContext.h:21`, `.cpp:639-661`; `IRenderingContext.h:9`; `IWindow.h:37`; `GLFWWindow.h:19`, `.cpp:89-91` | Deleted. Including the retry loop at `.cpp:651-660`, which on the `OUT_OF_DATE` path (`:354-357`) recreates the swapchain twice. |
| `ensureFrameStarted()` / `m_FirstFrame` | `VulkanContext.h:64`, `:168`, `.cpp:42`, `:492-500`, `:644-647`; caller `ImGuiContext.cpp:278` | Deleted. |
| `GLFWWindowIMPL::onUpdate()` + `IWindow::onUpdate()` | `GLFWWindow.cpp:80-83`, `IWindow.h:35` | Dead code (`onUpdate` calls `glfwPollEvents` then `m_Context->swapBuffers()`; nothing calls it). Delete both. `GLFWWindowIMPL` is the only `IWindow` implementation (verified: `grep -rn "public IWindow"` → `GLFWWindow.h:12` only). |
| `getMinImageCount()` | `VulkanContext.h:87` | Deleted. Hardcodes `2`; its only consumer, `ImGuiContext.cpp:173`, is overwritten two lines later at `:178`. |
| `_RendererAPI->Clear(...)` | `application.cpp:53-58` | Deleted. The clear colour now feeds `VkRenderingAttachmentInfo::clearValue` (§6.2). `IRendererAPI` itself is deleted by the resource-layer work, not here. |

**Nothing else in the repo references a `VkRenderPass` or `VkFramebuffer`.** Verified: `grep -rn -e getRenderPass -e getOverlayRenderPass -e beginRenderPass -e beginOverlayRenderPass -e isRenderPassActive -e m_Framebuffers -e createFramebuffers -e createRenderPass X3/src X3-Editor/src X3-Runtime/src` returns only the sites tabulated above. `X3-Runtime` does not link ImGui at all, so the runtime has no consumer of any render pass.

### 6.2 The new `VulkanContext` frame API

Add to `VulkanContext.h`, where `beginRenderPass`/`beginOverlayRenderPass` were (`:70-73`):

```cpp
	// ---- Frame lifecycle. Replaces swapBuffers(). ----------------------------
	// Waits the fence for slot m_CurrentFrame, publishes m_CompletedFrame, drains
	// the deletion queue, acquires a swapchain image, resets and begins
	// m_CommandBuffers[m_CurrentFrame]. Does NOT open a rendering block.
	// Returns nullptr if the swapchain was out of date (already recreated); the
	// caller must skip the whole frame. No retry loop.
	const FrameContext* beginFrame();

	// Guarantees the acquired swapchain image ends in PRESENT_SRC_KHR, ends the
	// command buffer, resets the frame fence and submits. Precondition: a
	// successful beginFrame().
	void endFrame();

	// vkQueuePresentKHR; advances m_CurrentFrame and m_FrameNumber; on
	// OUT_OF_DATE/SUBOPTIMAL calls recreateSwapchain().
	void present();

	// Non-null only between beginFrame() and endFrame().
	const FrameContext* currentFrame() const { return m_FrameActive ? &m_Frame : nullptr; }

	// ---- Dynamic rendering --------------------------------------------------
	// Opens a single-colour-attachment rendering block on the acquired swapchain
	// image. Must be balanced by endSwapchainRendering(). Records the
	// UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL barrier itself.
	void beginSwapchainRendering(VkClearColorValue clear = kDefaultClearColor);
	void endSwapchainRendering();

	// For VkPipelineRenderingCreateInfo (ImGui, and every future pipeline).
	VkFormat getSwapchainImageFormat() const { return m_SwapchainImageFormat; }
```

private:

```cpp
	void transitionSwapchainImage(VkCommandBuffer cmd,
	                              VkImageLayout oldLayout, VkImageLayout newLayout,
	                              VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
	                              VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);

	FrameContext  m_Frame;
	bool          m_FrameActive           = false;
	VkImageLayout m_SwapchainImageLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	bool          m_SwapchainImageWritten = false;
```

and, in `VulkanContext.cpp`, preserving the existing debug affordance from `application.cpp:54-58`:

```cpp
#ifdef BUILD_INSTALL
	static constexpr VkClearColorValue kDefaultClearColor = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
#else
	static constexpr VkClearColorValue kDefaultClearColor = {{ 0.98f, 0.24f, 0.97f, 1.0f }};
#endif
```

(Declare it in the header as an `inline constexpr` so the default argument can see it.)

**`beginFrame()`** — replaces `VulkanContext.cpp:344-396` entirely:

```cpp
const FrameContext* VulkanContext::beginFrame() {
	assert(!m_FrameActive && "beginFrame() called twice without endFrame()");
	assert(!m_UploadCmdRecording && "upload buffer left recording across frames"); // commit 7

	// 1. Wait for the GPU to finish with this frame slot.
	VK_CHECK(vkWaitForFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame], VK_TRUE, UINT64_MAX));

	// 2. Publish completion and reclaim. Must run even if the acquire below
	//    fails, or a click-and-drag resize starves the deletion queue.
	if (m_FrameNumber >= MAX_FRAMES_IN_FLIGHT)
		m_CompletedFrame = m_FrameNumber - MAX_FRAMES_IN_FLIGHT;
	drainDeletionQueue();

	// 3. Acquire. imageAvailable is indexed by frame slot (see §5.4).
	VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
		m_ImageAvailableSemaphores[m_CurrentFrame], VK_NULL_HANDLE, &m_ImageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		// OUT_OF_DATE does NOT signal the semaphore, so nothing is stranded.
		recreateSwapchain();
		return nullptr;      // no counter advances; the same slot is retried
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		LOG_ENGINE_ERROR("vkAcquireNextImageKHR failed ({})", static_cast<int>(result));
		return nullptr;
	}
	// SUBOPTIMAL_KHR DOES signal the semaphore; proceed and let present() handle it.

	// 4. Per-frame swapchain state. Acquired contents are undefined, which is
	//    what lets the first barrier of the frame discard rather than preserve.
	m_SwapchainImageLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	m_SwapchainImageWritten = false;

	// 5. Open the command buffer. NO rendering block is opened here.
	vkResetCommandBuffer(m_CommandBuffers[m_CurrentFrame], 0);
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if (vkBeginCommandBuffer(m_CommandBuffers[m_CurrentFrame], &beginInfo) != VK_SUCCESS) {
		// The acquire semaphore is already signalled and nothing will wait on it;
		// there is no legal way to un-signal it. This is OOM or DEVICE_LOST.
		LOG_ENGINE_CRITICAL("vkBeginCommandBuffer failed; acquire semaphore stranded");
		throw std::runtime_error("Failed to begin recording command buffer");
	}

	m_Frame.m_Context = this;
	m_Frame.m_Cmd     = m_CommandBuffers[m_CurrentFrame];
	m_Frame.m_Index   = m_CurrentFrame;
	m_Frame.m_Number  = m_FrameNumber;
	m_FrameActive     = true;
	return &m_Frame;
}
```

The throw on `vkBeginCommandBuffer` failure is deliberate and is a change from the old `return false`. Returning at that point leaves a signalled semaphore with no waiter, which poisons the next acquire; there is no recovery, and the only causes are `VK_ERROR_OUT_OF_DEVICE_MEMORY` and `VK_ERROR_DEVICE_LOST`.

**`endFrame()`** — replaces `VulkanContext.cpp:398-433` (the present half moves to `present()`):

```cpp
void VulkanContext::endFrame() {
	assert(m_FrameActive);
	VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

	// --- Nothing-was-written fallback. See §6.4. ---
	if (!m_SwapchainImageWritten) {
		beginSwapchainRendering();
		endSwapchainRendering();
	}

	// --- Present transition. The runtime path already did this inside the blit. ---
	if (m_SwapchainImageLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
		transitionSwapchainImage(cmd,
			m_SwapchainImageLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          0);
		m_SwapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("vkEndCommandBuffer failed");
		throw std::runtime_error("Failed to record command buffer");
	}

	// Command buffers within ONE VkSubmitInfo execute in submission order
	// (spec §7.2), so the upload buffer's trailing barrier orders correctly
	// against the frame buffer with no extra semaphore. Commit 7.
	VkCommandBuffer cmds[2];
	uint32_t        cmdCount = 0;
	if (m_UploadCmdRecording) {
		endUploadRecording();
		m_UploadCmdRecording = false;
		cmds[cmdCount++] = m_UploadCmd;   // MUST be first
	}
	cmds[cmdCount++] = cmd;

	VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[m_CurrentFrame] };
	// One mask covering both paths: endFrame is shared and cannot know whether
	// the editor (COLOR_ATTACHMENT_OUTPUT) or the runtime (TRANSFER) ran. See §6.3.
	VkPipelineStageFlags waitStages[] = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
	};
	VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[m_ImageIndex] };

	VkSubmitInfo submitInfo{};
	submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount   = 1;
	submitInfo.pWaitSemaphores      = waitSemaphores;
	submitInfo.pWaitDstStageMask    = waitStages;
	submitInfo.commandBufferCount   = cmdCount;
	submitInfo.pCommandBuffers      = cmds;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores    = signalSemaphores;

	// FENCE-LEAK FIX (§5.6): reset immediately before the submit, with nothing
	// between that can fail. Invariant: the fence is signalled iff no submit is
	// pending for this slot.
	vkResetFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame]);
	if (vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, m_InFlightFences[m_CurrentFrame]) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("vkQueueSubmit failed");
		// The fence was reset with nothing pending; re-signal it so the next
		// beginFrame does not block forever.
		vkQueueSubmit(m_GraphicsQueue, 0, nullptr, m_InFlightFences[m_CurrentFrame]);
	}
	m_FrameActive = false;
}
```

**`present()`** — new, taking over `VulkanContext.cpp:434-458`:

```cpp
void VulkanContext::present() {
	VkSemaphore  waitSemaphores[] = { m_RenderFinishedSemaphores[m_ImageIndex] };
	VkSwapchainKHR swapchains[]   = { m_Swapchain };

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores    = waitSemaphores;
	presentInfo.swapchainCount     = 1;
	presentInfo.pSwapchains        = swapchains;
	presentInfo.pImageIndices      = &m_ImageIndex;

	VkResult result = vkQueuePresentKHR(m_PresentQueue, &presentInfo);

	// Advance BEFORE any recreation, so the counters are consistent either way.
	m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	++m_FrameNumber;

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
		recreateSwapchain();   // does vkDeviceWaitIdle; all fences end up signalled
	} else if (result != VK_SUCCESS) {
		LOG_ENGINE_ERROR("vkQueuePresentKHR failed ({})", static_cast<int>(result));
	}
}
```

**`Application::run`** — this is the *single* place the loop is restructured; do not also do it as part of the resource-layer work. `application.cpp:41-74` becomes:

```cpp
	void Application::run() {
		while (!_Window->shouldClose()) {
			Time::Update();
			auto tGlobal = _Profiler->globalTimer("GLOBAL");

			{ auto t = _Profiler->timer("PollEvents"); _Window->pollEvents(); }

			const FrameContext* frame = _Context->beginFrame();
			if (!frame) continue;   // swapchain out of date; already recreated

			{ auto t = _Profiler->timer("LayerStack::onUpdate()"); _LayerStack->onUpdate(); }

			{ auto t = _Profiler->timer("Present");
			  _Context->endFrame();
			  _Context->present(); }
		}

		vkDeviceWaitIdle(_Context->getDevice());   // layer/renderer dtors must be safe
		Shutdown();
	}
```

One frame per iteration, no cross-iteration straddle. `continue` on a minimized window is safe: `recreateSwapchain` blocks in `glfwWaitEvents` (`VulkanContext.cpp:670-673`) until the framebuffer is non-zero.

Accompanying edits:

| File:line | Current | Becomes |
|---|---|---|
| `application.h:12, 28` | `class IRendererAPI;` / `_RendererAPI` | keep both for now (the resource layer deletes them); **add** `VulkanContext* _Context = nullptr;` |
| `application.cpp:27-28` | `_RendererAPI = IRendererAPI::Create(); _RendererAPI->Init();` | keep, and add `_Context = VulkanContext::Get();` — the context is created by `GLFWWindow.cpp:50`, which runs inside `IWindow::createWindow` at `application.cpp:20`, before this line |
| `application.cpp:53-58` | the two `_RendererAPI->Clear({...})` calls | **deleted** — the colour now lives in `kDefaultClearColor` |
| `application.cpp:9` | `#include "Renderer/IRendererAPI.h"` | add `#include "Platform/Vulkan/VulkanContext.h"` |
| `IRenderingContext.h:9` | `virtual void swapBuffers() = 0;` | deleted |
| `VulkanContext.h:21` | `void swapBuffers() override;` | deleted |
| `IWindow.h:35, 37` | `virtual void onUpdate() = 0;` / `virtual void swapBuffers() = 0;` | both deleted |
| `GLFWWindow.h:19` / `.cpp:80-83, 89-91` | `onUpdate` / `swapBuffers` | deleted |

`LayerStack::onUpdate()` keeps its signature. Layers that need the frame call `VulkanContext::Get()->currentFrame()` — exactly two do: `ImGuiContext::EndFrame` and `RuntimeLayer::onUpdate`. Threading `const FrameContext&` down through `Renderer::Render`/`SetupGPUResources`/`Draw` belongs to the resource-layer work, not here.

### 6.3 Dynamic rendering: the barrier helper and the rendering block

```cpp
void VulkanContext::transitionSwapchainImage(VkCommandBuffer cmd,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
        VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
	VkImageMemoryBarrier b{};
	b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout           = oldLayout;
	b.newLayout           = newLayout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image               = m_SwapchainImages[m_ImageIndex];
	b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	b.srcAccessMask       = srcAccess;
	b.dstAccessMask       = dstAccess;
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanContext::beginSwapchainRendering(VkClearColorValue clear) {
	VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

	// No render pass => no automatic layout transition. This barrier is mandatory.
	// srcStage MUST be COLOR_ATTACHMENT_OUTPUT, not TOP_OF_PIPE: the acquire
	// semaphore is waited at that stage, and the transition must be ordered
	// after the wait. A TOP_OF_PIPE source scope is empty and orders nothing.
	transitionSwapchainImage(cmd,
		m_SwapchainImageLayout,                          // UNDEFINED on first write this frame
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,   0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	m_SwapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkRenderingAttachmentInfo color{};
	color.sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	color.imageView        = m_SwapchainImageViews[m_ImageIndex];
	color.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.resolveMode      = VK_RESOLVE_MODE_NONE;
	color.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;   // CLEAR, never LOAD; see below
	color.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
	color.clearValue.color = clear;

	VkRenderingInfo info{};
	info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
	info.renderArea.offset    = { 0, 0 };
	info.renderArea.extent    = m_SwapchainExtent;
	info.layerCount           = 1;
	info.viewMask             = 0;   // no multiview
	info.colorAttachmentCount = 1;
	info.pColorAttachments    = &color;
	info.pDepthAttachment     = nullptr;
	info.pStencilAttachment   = nullptr;
	info.flags                = 0;   // NOT suspending/resuming; see §6.6

	vkCmdBeginRendering(cmd, &info);
	m_SwapchainImageWritten = true;
}

void VulkanContext::endSwapchainRendering() {
	vkCmdEndRendering(m_CommandBuffers[m_CurrentFrame]);
}
```

**The acquire-wait stage fix.** `VulkanContext.cpp:417` currently reads:

```cpp
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
```

while the runtime's first touch of the acquired image is `vkCmdClearColorImage` at `TRANSFER`. `pWaitDstStageMask` defines the second synchronization scope of the semaphore wait: only the listed stages, and stages logically later, are blocked until the semaphore signals. The transfer commands are therefore free to execute before `vkAcquireNextImageKHR` has made the image available — i.e. while the presentation engine may still be reading it. Under render passes this at least covered the implicit layout transition; with the transition now explicit, it covers nothing. The fix is the combined mask shown in `endFrame` above.

**The rule to follow, stated once:** *the `srcStageMask` of any barrier that transitions the acquired swapchain image must be a stage included in `pWaitDstStageMask`.* That is why §6.3's barrier uses `COLOR_ATTACHMENT_OUTPUT` and §6.5's blit barrier uses `TRANSFER` as **source** stages rather than `TOP_OF_PIPE`.

**Why `LOAD_OP_CLEAR`, never `LOAD_OP_LOAD`.** The old overlay pass used `VK_ATTACHMENT_LOAD_OP_LOAD` (`VulkanContext.cpp:224`), which performs a *read* of the attachment with `VK_ACCESS_COLOR_ATTACHMENT_READ_BIT` at `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT` (spec §8.4). Neither the shared subpass dependency (`:202`, `dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` only) nor the preceding barrier (`:616`, same) granted read access — a read-after-write hazard on every editor frame. In the new shape this is **eliminated by construction**, not fixed: there is exactly one rendering block per frame in the editor and it clears; the runtime has none. Nothing loads.

State the standing rule anyway, because someone will add a debug HUD to the runtime: *if a `VkRenderingAttachmentInfo` ever uses `VK_ATTACHMENT_LOAD_OP_LOAD`, the barrier preceding `vkCmdBeginRendering` must include `VK_ACCESS_COLOR_ATTACHMENT_READ_BIT` in `dstAccessMask`, its src side must name the stage/access that produced the loaded content (for a preceding blit: `srcStage = TRANSFER`, `srcAccess = TRANSFER_WRITE`, `oldLayout = TRANSFER_DST_OPTIMAL`), and `oldLayout` may not be `UNDEFINED`.*

#### Editor per-frame timeline for the swapchain image

| # | Point in frame | Layout before → after | srcStage | srcAccess | dstStage | dstAccess |
|---|---|---|---|---|---|---|
| 0 | `vkAcquireNextImageKHR` signals `imageAvailable[m_CurrentFrame]` | — (contents undefined) | — | — | — | — |
| 1 | *(no barrier)* compute dispatches recorded at top level; they touch the render target image, never the swapchain | unchanged | — | — | — | — |
| 2 | `beginSwapchainRendering()`, before `vkCmdBeginRendering` | `UNDEFINED` → `COLOR_ATTACHMENT_OPTIMAL` | `COLOR_ATTACHMENT_OUTPUT` | `0` | `COLOR_ATTACHMENT_OUTPUT` | `COLOR_ATTACHMENT_WRITE` |
| 3 | rendering block: `loadOp = CLEAR`, ImGui draws, `storeOp = STORE` | `COLOR_ATTACHMENT_OPTIMAL` | — | — | — | — |
| 4 | `endFrame()`, after `vkCmdEndRendering` | `COLOR_ATTACHMENT_OPTIMAL` → `PRESENT_SRC_KHR` | `COLOR_ATTACHMENT_OUTPUT` | `COLOR_ATTACHMENT_WRITE` | `BOTTOM_OF_PIPE` | `0` |
| 5 | `vkQueueSubmit` `pWaitDstStageMask` | — | — | — | `COLOR_ATTACHMENT_OUTPUT \| TRANSFER` | — |
| 6 | `vkQueuePresentKHR` waits `renderFinished[m_ImageIndex]` | `PRESENT_SRC_KHR` | — | — | — | — |

#### Runtime per-frame timeline for the swapchain image

| # | Point in frame | Layout before → after | srcStage | srcAccess | dstStage | dstAccess |
|---|---|---|---|---|---|---|
| 0 | acquire signals `imageAvailable[m_CurrentFrame]` | — | — | — | — | — |
| 1 | *(no barrier)* compute dispatch writes the render target in `GENERAL` | unchanged | — | — | — | — |
| 2 | blit source barrier (`.cpp:838-856`, **unchanged**) | source `GENERAL` → `TRANSFER_SRC_OPTIMAL` | `COMPUTE_SHADER` | `SHADER_WRITE` | `TRANSFER` | `TRANSFER_READ` |
| 3 | blit dest barrier (`.cpp:858-877`, **srcStage changed**) | `UNDEFINED` → `TRANSFER_DST_OPTIMAL` | **`TRANSFER`** (was `TOP_OF_PIPE` at `:875`) | `0` | `TRANSFER` | `TRANSFER_WRITE` |
| 4 | `vkCmdClearColorImage` (`:887`), `vkCmdBlitImage` (`:908`) | `TRANSFER_DST_OPTIMAL` | — | — | — | — |
| 5 | present barrier (`.cpp:914-932`, **unchanged**) | `TRANSFER_DST_OPTIMAL` → `PRESENT_SRC_KHR` | `TRANSFER` | `TRANSFER_WRITE` | `BOTTOM_OF_PIPE` | `0` |
| 6 | source restore barrier (`.cpp:935-953`, **unchanged**) | source `TRANSFER_SRC_OPTIMAL` → `GENERAL` | `TRANSFER` | `TRANSFER_READ` | `COMPUTE_SHADER` | `SHADER_READ \| SHADER_WRITE` |
| 7 | `vkQueueSubmit` `pWaitDstStageMask` | — | — | — | `COLOR_ATTACHMENT_OUTPUT \| TRANSFER` | — |

Under render passes, step 5 duplicated the render pass's `finalLayout`. It is now the only thing producing `PRESENT_SRC_KHR` on the runtime path, and `endFrame`'s transition correctly skips because the tracked layout already matches.

### 6.4 Resolution: what happens when neither path touched the image

This is the gap the critique flagged as severity-1, and it is reachable. In the runtime, `RuntimeLayer::onUpdate` guards the whole blit on `if (m_CurrentFrame)` (`X3-Runtime/src/RuntimeLayer.cpp:176`), and `m_CurrentFrame` is null whenever `Renderer::Render` returned `nullptr` (`Renderer.cpp:59-61`, "scene missing camera") or before the first `NewFrameRenderedEvent`. In the editor it is reachable if ImGui rendering is ever skipped. Presenting an image in `VK_IMAGE_LAYOUT_UNDEFINED` is invalid.

**Resolution, definitively: open and immediately close an empty rendering block with `LOAD_OP_CLEAR`.** That is the `if (!m_SwapchainImageWritten)` branch already shown in `endFrame` (§6.2). It defines the layout, clears the image, and — decisively — requires only `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`.

**Do not use `vkCmdClearColorImage` for this.** It would make `VK_IMAGE_USAGE_TRANSFER_DST_BIT` a hard requirement for the *editor* as well as the runtime, coupling the editor to a swapchain capability it otherwise does not need, on the exact platform (MoltenVK) where that capability is most likely to be refused. The rendering block needs only `COLOR_ATTACHMENT`, which vk-bootstrap always requests.

`m_SwapchainImageWritten` is set to `true` by `beginSwapchainRendering()` and by `blitImageToSwapchain`; `m_SwapchainImageLayout` is written by both and read by `endFrame`. Together they replace `m_RenderPassActive` entirely, and they track *layout state* rather than *pass state*, which is what the reversal note in `ENGINE_PLAN.md` §0 calls "resource state tracking".

### 6.5 `blitImageToSwapchain` — resolution of the signature question, definitively

**In this commit the signature does not change.** It stays exactly as `VulkanContext.h:80-82`:

```cpp
	void blitImageToSwapchain(VkImage sourceImage, VkImageLayout currentLayout,
	                          uint32_t srcWidth, uint32_t srcHeight,
	                          glm::ivec4 viewport, glm::ivec2 windowSize);
```

Rationale: the `VulkanImage&` form requires `VulkanImage`, which does not exist until the resource-layer migration replaces `VulkanImage2D`. Changing the signature here would force `RuntimeLayer.cpp:176-193` into this already-large, already-highest-risk commit for no correctness gain. Only the **body** changes, and only in two places:

1. **Delete `VulkanContext.cpp:832-835**:
   ```cpp
   	// End the render pass if it's active (we need to be outside render pass for blitting)
   	if (m_RenderPassActive) {
   		vkCmdEndRenderPass(cmd);
   		m_RenderPassActive = false;
   	}
   ```
   There is no pass to end. `vkCmdBlitImage` and `vkCmdClearColorImage` are transfer commands and must be recorded outside any rendering block; since `beginFrame` opens none and the runtime never opens one, they already are.

2. **`:874-877`, the destination barrier's source stage**: change `VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT` to `VK_PIPELINE_STAGE_TRANSFER_BIT`, per the rule in §6.3. `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED` at `:861` is **correct and stays** — discarding is what we want. Then record the state:
   ```cpp
   	m_SwapchainImageLayout  = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   	m_SwapchainImageWritten = true;
   ```
   and after the present barrier at `:929-932`:
   ```cpp
   	m_SwapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   ```

Everything else is untouched: the source barriers at `:838-856` and `:935-953`, the letterbox clear at `:879-888`, the blit region at `:891-911`.

> **Do not change the Y-flip.** `VulkanContext.cpp:905-906`:
> ```cpp
> 	blitRegion.dstOffsets[0] = {viewport.x, windowSize.y - viewport.w, 0}; // Flip Y
> 	blitRegion.dstOffsets[1] = {viewport.z, windowSize.y - viewport.y, 1}; // Flip Y
> ```
> These lines and `RuntimeLayer::CalculateViewportCoordinates()` (`X3-Runtime/src/RuntimeLayer.cpp:306-376`) are a matched pair: the viewport is packed in the OpenGL `glBlitFramebuffer` convention `(x, y, x+width, y+height)` with a bottom-left origin, and the subtraction converts it to Vulkan's top-left origin. Changing either half in isolation flips the runtime image. Neither is touched by this commit, or by any commit in Phase 1. The `VK_FILTER_LINEAR` at `:911` also stays.

**The final form, for the resource-layer commit that follows this one**, recorded so nobody re-litigates it:

```cpp
	void blitImageToSwapchain(const FrameContext& frame, VulkanImage& src,
	                          glm::ivec4 viewport, glm::ivec2 windowSize);
```

It obtains extent from `src.extent()` and the current layout from `src.layout()` instead of the hardcoded `VK_IMAGE_LAYOUT_GENERAL` at `RuntimeLayer.cpp:185`, and calls `src.transition(...)` twice so the tracked layout stays accurate across the `GENERAL → TRANSFER_SRC_OPTIMAL → GENERAL` round trip. The Y-flip packing and `CalculateViewportCoordinates()` are carried over byte-for-byte.

### 6.6 MoltenVK constraints to design around now

1. **Do not use `VK_RENDERING_SUSPENDING_BIT` / `VK_RENDERING_RESUMING_BIT`.** MoltenVK maps each `vkCmdBeginRendering` onto a fresh `MTLRenderCommandEncoder`; suspend/resume is the weakest-supported corner of the feature there. The design above sets `flags = 0` and opens at most one block per frame, so this costs nothing — but it is a binding constraint on Phase 5's render graph, which must not split a logical pass across command buffers via suspend/resume.
2. **Keep the number of rendering blocks per frame small.** On Metal each begin/end is an encoder boundary, meaningfully more expensive than on desktop drivers. One block per frame is the target; both shapes above hit it.
3. **`dynamicRenderingLocalRead` is not available on MoltenVK** (a Vulkan 1.4 feature; the local GPUs report it `true`, which will mislead you if you only develop against them). Do not design input-attachment-style reads within a rendering block. Relevant to Phase 7, not here.

### 6.7 ImGui wiring

The vendored ImGui supports dynamic rendering; this is not a blocker. Verified in the tree: `X3-Editor/libs/imgui-docking/imgui.h:30` → `IMGUI_VERSION "1.90.9 WIP"`; `imgui_impl_vulkan.h:90` → `bool UseDynamicRendering;`; `imgui_impl_vulkan.h:92` → `VkPipelineRenderingCreateInfoKHR PipelineRenderingCreateInfo;`, guarded by `IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING` which is defined at `imgui_impl_vulkan.h:63`. `VkRenderPass RenderPass;` at `:79` is documented "Ignored if using dynamic rendering", and the assert at `imgui_impl_vulkan.cpp:1125-1126` only fires when `UseDynamicRendering == false`.

Two hard asserts you must satisfy (`imgui_impl_vulkan.cpp:973-974`):
- `PipelineRenderingCreateInfo.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR`
- `PipelineRenderingCreateInfo.pNext == nullptr`

And one trap the asserts do **not** catch: `ImGui_ImplVulkan_Init` copies the init info by value (`bd->VulkanInitInfo = *info;`, `imgui_impl_vulkan.cpp:1128`) and reuses it as the pipeline `pNext` at `:975` whenever the pipeline is (re)created. `pColorAttachmentFormats` is a **pointer**; the array it points at must outlive the ImGui backend. A stack local will dangle.

Because `ImGuiContext.cpp:157-201` (init) and `:217-261` (swapchain-recreation re-init) build the same struct twice and would now build it three times' worth of duplicated lines, **extract a helper** rather than duplicating:

```cpp
// X3-Editor/src/ImGuiContext.cpp, file scope
namespace {
// MUST have static storage duration: ImGui_ImplVulkan_Init copies InitInfo by
// value and keeps the pColorAttachmentFormats POINTER, dereferencing it on every
// pipeline (re)creation. Refreshed on every call because the surface format can
// in principle change across swapchain recreation.
VkFormat s_ColorAttachmentFormat = VK_FORMAT_UNDEFINED;

ImGui_ImplVulkan_InitInfo MakeImGuiInitInfo(X3::VulkanContext* ctx) {
    s_ColorAttachmentFormat = ctx->getSwapchainImageFormat();

    ImGui_ImplVulkan_InitInfo info = {};
    info.Instance       = ctx->getInstance();
    info.PhysicalDevice = ctx->getPhysicalDevice();
    info.Device         = ctx->getDevice();
    info.QueueFamily    = ctx->getGraphicsQueueFamily();
    info.Queue          = ctx->getGraphicsQueue();
    info.PipelineCache  = VK_NULL_HANDLE;
    info.DescriptorPool = ctx->getDescriptorPool();

    // Dynamic rendering: RenderPass is ignored, Subpass is unused.
    info.RenderPass          = VK_NULL_HANDLE;   // was getOverlayRenderPass()
    info.Subpass             = 0;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {};
    info.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;   // asserted, imgui_impl_vulkan.cpp:973
    info.PipelineRenderingCreateInfo.pNext   = nullptr;         // asserted, imgui_impl_vulkan.cpp:974
    info.PipelineRenderingCreateInfo.viewMask = 0;
    info.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_ColorAttachmentFormat;
    info.PipelineRenderingCreateInfo.depthAttachmentFormat   = VK_FORMAT_UNDEFINED;
    info.PipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    const uint32_t imageCount = ctx->getSwapchainImageCount();
    info.ImageCount    = imageCount;
    info.MinImageCount = imageCount;   // asserts at imgui_impl_vulkan.cpp:1123-1124 need >= 2
    info.MSAASamples   = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS) { LOG_ENGINE_ERROR("ImGui Vulkan error: {}", static_cast<int>(err)); }
    };
    return info;
}
} // namespace
```

Then:
- `ImGuiContext.cpp:162-185` collapses to `ImGui_ImplVulkan_InitInfo init_info = MakeImGuiInitInfo(vkContext);`. Note `init_info.MinImageCount = vkContext->getMinImageCount();` at `:173` was dead — overwritten at `:178`. Its removal is what allows `getMinImageCount()` (`VulkanContext.h:87`) to be deleted; that was its only caller.
- `ImGuiContext.cpp:229-247` (the swapchain-recreation re-init) collapses to the same one line. **This block must receive identical treatment** — the assert at `imgui_impl_vulkan.cpp:1125-1126` fires immediately if it is missed.
- `ImGui_ImplVulkanH_Window::UseDynamicRendering` (`imgui_impl_vulkan.h:177`) is for the multi-viewport helper path, which is disabled under Vulkan (`ImGuiContext.cpp:129-133`). Ignore it.

`ImGuiContext::EndFrame` (`ImGuiContext.cpp:271-297`) becomes:

```cpp
	VulkanContext*  vkContext = VulkanContext::Get();
	VkCommandBuffer cmd       = vkContext->getCurrentCommandBuffer();
	assert(cmd != VK_NULL_HANDLE && "Command buffer is null");

	vkContext->beginSwapchainRendering();                      // replaces beginOverlayRenderPass()
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	vkContext->endSwapchainRendering();
```

Deleted from that function: `vkContext->ensureFrameStarted();` (`:278`) — `Application::run` guarantees the frame is open before any layer runs — and `assert(vkContext->isRenderPassActive() && ...)` (`:290`).

### 6.8 Verification for commit 6

- **`VUID-vkCmdDispatch-renderpass` must go from firing every frame to zero.** This is the acceptance test; it is the loudest message in the pre-fix log, so the delta is unmissable. `VUID-vkCmdPipelineBarrier-pDependencies-02285` and `VUID-vkCmdDispatch-commandBuffer-recording` likewise.
- The first rendered frame must contain content instead of being blank (the `m_FirstFrame` discard, §6.0(a), is gone).
- RenderDoc: the compute dispatch must appear as a top-level command, not nested under a `vkCmdBeginRenderPass` marker; there must be exactly one `vkCmdBeginRendering`/`vkCmdEndRendering` pair per editor frame and **zero** per runtime frame.
- Zero `SYNC-HAZARD-*` on the swapchain image with submit-time syncval on.
- Zero `VUID-VkPresentInfoKHR-*` layout complaints — exercise §6.4 by running the runtime with a project whose scene has no camera (`Renderer.cpp:59-61` returns `nullptr`, `RuntimeLayer.cpp:176` skips the blit), which must present a cleared image rather than an undefined one.
- Resize both executables repeatedly to exercise swapchain recreation and the ImGui re-init path at `ImGuiContext.cpp:217-261`.

---

## 7. Commit 7 — Batched resource uploads

### 7.1 Current behaviour

`VulkanContext.cpp:502-533`:

```cpp
VkCommandBuffer VulkanContext::beginSingleTimeCommands() {
	... allocInfo.commandPool = m_CommandPool; ...
	vkAllocateCommandBuffers(m_Device, &allocInfo, &cmd);
	... vkBeginCommandBuffer(cmd, &beginInfo);
	return cmd;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer cmd) {
	vkEndCommandBuffer(cmd);
	...
	vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(m_GraphicsQueue);

	vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &cmd);
}
```

The complete caller set (verified by grep across `X3/src`, `X3-Editor/src`, `X3-Runtime/src`):

- `X3/src/Platform/Vulkan/VulkanImage2D.cpp:27` / `:29` — the layout transition to `GENERAL` in the constructor. Hit on every resolution change, because `Renderer.cpp:202-203` constructs two `IImage2D`s.
- `X3/src/Platform/Vulkan/VulkanImage2D.cpp:124` / `:148` — the staging upload path.
- `X3/src/Platform/Vulkan/VulkanTexture2D.cpp:105` / `:128` — the skybox upload.

`m_CommandPool` is the *same* pool the per-frame command buffers come from (`VulkanContext.cpp:284`, `:297`).

### 7.2 Why it is wrong

`vkQueueWaitIdle` is defined (spec §5.5) as equivalent to submitting a fence and waiting on it with an infinite timeout: it blocks the calling thread until **every** submission on that queue completes, including the previous frame's rendering. Every texture, every image and every resolution change costs a full GPU drain plus a round trip; loading a scene with *N* meshes and *M* textures serializes into *N + M* drains.

It is also on the *graphics* queue, mid-frame: `Renderer::SetupGPUResources` runs inside `LayerStack::onUpdate()` while `m_CommandBuffers[m_CurrentFrame]` is in the recording state. A resolution change therefore stalls the pipeline at the worst possible moment.

> **Read this before deleting the wait.** That call is currently the only thing making image and texture recreation safe: `Renderer.cpp:202-203` and `:244` construct the replacement (which idles the queue) *before* the `shared_ptr` assignment destroys the old one, so the old `VkImage` is destroyed on an idle queue **by accident**. Removing the wait without commit 2's deletion queue in place converts a hidden inefficiency into an immediate use-after-free.

### 7.3 Fix

`VulkanContext.h`:

```cpp
	// Records into a batched upload command buffer submitted as the FIRST command
	// buffer of the next frame submission. Never blocks.
	VkCommandBuffer getUploadCommandBuffer();

	// Submits pending uploads and blocks. Only for paths that run outside the
	// frame loop (the ImGui font upload).
	void flushUploadsBlocking();

private:
	VkCommandPool   m_UploadPool         = VK_NULL_HANDLE; // TRANSIENT | RESET_COMMAND_BUFFER
	VkCommandBuffer m_UploadCmd          = VK_NULL_HANDLE;
	bool            m_UploadCmdRecording = false;
	void createUploadResources();
	void endUploadRecording();   // trailing barrier + vkEndCommandBuffer; no submit
```

**`createUploadResources()`** — called from `init()` (`VulkanContext.cpp:24-43`) right after `createCommandPool()` at `:33`. A **separate** pool with `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` and one primary command buffer. The separate pool matters: command pools are externally synchronized, and allocating from `m_CommandPool` while one of its buffers is recording becomes fragile the moment Phase 4's job system lands.

**`getUploadCommandBuffer()`**:

```cpp
VkCommandBuffer VulkanContext::getUploadCommandBuffer() {
	if (!m_UploadCmdRecording) {
		vkResetCommandBuffer(m_UploadCmd, 0);
		VkCommandBufferBeginInfo bi{};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(m_UploadCmd, &bi);
		m_UploadCmdRecording = true;
	}
	return m_UploadCmd;
}
```

**`endUploadRecording()`** appends one global barrier so every upload's writes are visible to every possible consumer without each call site reasoning about it:

```cpp
	VkMemoryBarrier b{};
	b.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT
	                | VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(m_UploadCmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			| VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &b, 0, nullptr, 0, nullptr);
	vkEndCommandBuffer(m_UploadCmd);
```

A global memory barrier does not transition layouts, so the per-image barriers stay at the call sites — but **their stage masks are wrong and must be fixed in this same commit**, because the `vkQueueWaitIdle` was the only thing hiding it:

- `X3/src/Platform/Vulkan/VulkanTexture2D.cpp:207`: `destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;` → `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`. `VulkanTexture2D` is used for exactly one thing, the skybox (`Renderer.cpp:244`), and it is sampled from a **compute** shader (`X3/res/shaders/PathTracing.comp:85`, sampled at `:208`). A read issued from `COMPUTE_SHADER` is not in the visibility scope of a barrier whose `dstStageMask` names only `FRAGMENT_SHADER` — a read-after-write hazard, not a layout error.
- `X3/src/Platform/Vulkan/VulkanImage2D.cpp:209` and `:230`: same substitution.

**Submission** — already shown in `endFrame` (§6.2). Two separate `vkQueueSubmit` calls to the same queue are *not* automatically ordered in execution; command buffers within a **single** `VkSubmitInfo::pCommandBuffers` array **are** in submission order (spec §7.2), so the trailing barrier correctly synchronizes against everything in the frame buffer. Zero extra semaphores, zero extra submits. Because the upload buffer is submitted with the frame's fence and reset only at the *next* `getUploadCommandBuffer()`, the assert in `beginFrame` (§6.2) that `m_UploadCmdRecording == false` is sound.

**Staging buffer lifetime.** `VulkanTexture2D.cpp:131` and `VulkanImage2D.cpp:151` destroy the staging buffer immediately, relying on the wait. Change both to `context->deferDestroy(stagingBuffer, stagingAllocation)` and delete the now-false comments at `VulkanTexture2D.cpp:130` and `VulkanImage2D.cpp:150`.

**`flushUploadsBlocking()`** — for the one path that genuinely runs outside a frame: the ImGui font upload at `X3-Editor/src/ImGuiContext.cpp:192-197`, which already does its own `vkDeviceWaitIdle` at `:197`. (An earlier spec also listed "`VulkanContext::init()` teardown"; `init()` at `VulkanContext.cpp:24-43` has no teardown — the ImGui path is the only one.) It does `endUploadRecording()`, `vkQueueSubmit` with a dedicated fence, `vkWaitForFences`, reset the fence, then `drainDeletionQueueFully()`.

**Call-site updates** — replace `beginSingleTimeCommands()`/`endSingleTimeCommands()` with `getUploadCommandBuffer()` and *no* end call:
- `VulkanImage2D.cpp:27-29` → `transitionToGeneral(context->getUploadCommandBuffer());`
- `VulkanImage2D.cpp:124` → `VkCommandBuffer cmd = context->getUploadCommandBuffer();`, delete `:148`
- `VulkanTexture2D.cpp:105` → same, delete `:128`

Then delete `beginSingleTimeCommands`/`endSingleTimeCommands` (`VulkanContext.h:67-68`, `.cpp:502-533`).

### 7.4 The profiler bug that makes this unverifiable — fix it first

`X3/src/Renderer/Renderer.cpp:198`:

```cpp
		m_Profiler->timer("Renderer::SetupGPUResources()");
```

`Profiler::timer` returns `const std::shared_ptr<ScopeTimer>` (`X3/src/Core/Profiler.h:89`). The return value is discarded, so the `ScopeTimer` destructs on the same line and the recorded interval is ~0. Compare the three correct sites in the same file: `Renderer.cpp:56` (`auto t = m_Profiler->timer("Renderer::Render()")`), `:100`, and `:357`. **Any verification that reads the `Renderer::SetupGPUResources()` number in `ProfilerPanel` is worthless until this is fixed.** The fix is one token:

```cpp
		auto t = m_Profiler->timer("Renderer::SetupGPUResources()");
```

Land it at the top of this commit, before measuring anything.

### 7.5 Verification

- **Static:** `grep -rn "vkQueueWaitIdle\|vkDeviceWaitIdle" X3/src/Platform/Vulkan/` must return exactly: `recreateSwapchain` (`VulkanContext.cpp:665`), `cleanup` (`:743`), `~VulkanComputeShader` (`VulkanComputeShader.cpp:58`), and `flushUploadsBlocking`. Anything else is a regression.
- **Profiler:** with §7.4 fixed, change the render resolution repeatedly and load a scene with several meshes. The `Renderer::SetupGPUResources()` spike must collapse from a full frame-time drain to near-zero.
- **Correctness of the batch:** load a scene with a skybox and screenshot frame 1. A black or garbage skybox means the trailing barrier or the submission order is wrong. This is the highest-risk part of the commit — the batched upload is now in the *same submit* as the frame that samples it, so it must be correct, but verify.
- **Validation:** `VUID-vkDestroyBuffer-buffer-00922` / `VUID-vkDestroyImage-image-01000` must not appear; if they do, the deletion queue is not covering a path. With syncval on, loading a skybox must not produce `SYNC-HAZARD-READ-AFTER-WRITE` at the dispatch citing the skybox image and a prior `vkCmdCopyBufferToImage` — that is the §7.3 stage-mask fix being missed.
- **RenderDoc:** one submit per frame containing two command buffers, upload first.
- Best-practices `BestPractices-vkQueueWaitIdle`-class stall warnings should disappear.

---

## 8. Phase-2 exit gate

Run **both** `X3Editor` and `X3Runtime` with `vkconfig`'s Synchronization preset active, submit-time validation on, and best practices on. In each:

1. Cold start; capture the first 5 seconds of log.
2. Open a project with meshes, lights and a skybox; open one with none of those; open one whose scene has **no camera** (exercises §6.4's nothing-written path).
3. Add and delete mesh entities at run time.
4. Change render resolution repeatedly.
5. Switch shader type PathTracing ↔ PBR ↔ Phong.
6. Toggle vSync; confirm the logged present mode changes and the frame rate clamps.
7. Continuous window resize for 10 s; minimize and restore; move between monitors of different refresh rates.
8. Enter and leave play mode; close cleanly.

**Pass criteria:**

- Zero messages containing `VUID-` in the log. In particular, `VUID-vkCmdDispatch-renderpass` (was firing every frame), `VUID-vkCmdBlitImage-dstImage-00224`, `VUID-vkCmdClearColorImage-image-00002`, `VUID-vkDestroySemaphore-semaphore-01137`, `VUID-vkDestroyBuffer-buffer-00922`.
- Zero messages containing `SYNC-HAZARD-`.
- Best-practices warnings reviewed and either fixed or explicitly waived in a code comment.
- Object tracker reports zero leaked objects at `vkDestroyInstance`.
- Memory footprint flat across the 10-second resize soak.
- No black flash on resize.
- RenderDoc: compute dispatch at top level; exactly one `vkCmdBeginRendering` per editor frame and zero per runtime frame; one submit per frame with the upload buffer first; swapchain image usage includes `TRANSFER_DST`.

**What cannot be verified by tooling** and must be argued from the code: the §2.3 frame-slot invariant. Synchronization validation does not model host writes to persistently-mapped memory. Assert it, comment it, and re-check it whenever `Application::run` is touched.