# EXECUTION SPEC — Phase 1c: Vulkan Synchronization and Correctness Fixes

Repo root: `/home/sarah/Coding/Haptixxx/X3` (engine sources live under `/home/sarah/Coding/Haptixxx/X3/X3/`). Branch `vulkan-migration`.

**Preconditions.** This spec assumes Phase 1a (OpenGL deleted) and 1b (GL-shaped interfaces replaced) are done or in flight. Where a fix interacts with 1b, that is called out. Nothing on this machine compiles (`vulkan-headers` absent), so every claim below is from reading; §9 gives the verification procedure to run once a build exists.

**Standing constraint (from ENGINE_PLAN.md §0 "Reversal on decision 8").** This is a Vulkan-native resource layer. No virtual dispatch, no `I*` factories, no second backend. `VkStructs` in interfaces are fine. Do not add abstraction to "keep options open".

---

## 0. Shared infrastructure that must land first

Four of the six listed issues plus five of the additional findings all need the same two primitives. Build them first, in this order, or the individual fixes will each grow a private half-version of them.

### 0.1 Frame identity and the frame-slot invariant

`VulkanContext` already has `m_CurrentFrame` (`VulkanContext.h:153`) and `MAX_FRAMES_IN_FLIGHT = 2` (`VulkanContext.h:146`). Add a monotonic counter alongside it.

In `/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanContext.h`, private section near line 153:

```cpp
uint64_t m_FrameNumber   = 0;  // monotonically increasing, never wraps in practice
uint64_t m_CompletedFrame = 0; // highest frame number whose submission is known complete
```

Public getters next to `getCurrentFrame()` (`VulkanContext.h:58`):

```cpp
uint64_t getFrameNumber()    const { return m_FrameNumber; }
uint64_t getCompletedFrame() const { return m_CompletedFrame; }
```

In `VulkanContext::beginFrame()` (`VulkanContext.cpp:344`), immediately after the fence wait at `VulkanContext.cpp:346` succeeds:

```cpp
// The fence for slot m_CurrentFrame was signalled by frame (m_FrameNumber - MAX_FRAMES_IN_FLIGHT).
if (m_FrameNumber >= MAX_FRAMES_IN_FLIGHT)
    m_CompletedFrame = m_FrameNumber - MAX_FRAMES_IN_FLIGHT;
drainDeletionQueue();   // §0.2
```

and increment `m_FrameNumber` in `endFrame()` next to the existing index advance at `VulkanContext.cpp:456`.

**Invariant to document in the header and assert on:** *all* CPU writes to frame-slot `m_CurrentFrame` resources happen after `beginFrame()` has waited `m_InFlightFences[m_CurrentFrame]` and before `endFrame()` submits. This holds today by call order — `Application::run()` (`/home/sarah/Coding/Haptixxx/X3/X3/src/Core/application.cpp:41-71`) runs `LayerStack::onUpdate()` at line 63 and `_Window->swapBuffers()` at line 69, and `VulkanContext::swapBuffers()` (`VulkanContext.cpp:639`) calls `endFrame()` then `beginFrame()`. So by the time `RenderLayer::onUpdate` runs, `beginFrame()` for the current slot has already completed. **This invariant is the entire correctness argument for fix 2. Write it down.**

### 0.2 Frame-indexed deferred deletion queue

Add to `VulkanContext.h` public section:

```cpp
void deferDestroyBuffer(VkBuffer buffer, VmaAllocation allocation);
void deferDestroyImage(VkImage image, VmaAllocation allocation);
void deferDestroyImageView(VkImageView view);
void deferDestroySampler(VkSampler sampler);
```

private:

```cpp
struct PendingDelete {
    uint64_t      retireFrame;
    VkBuffer      buffer     = VK_NULL_HANDLE;
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VkSampler     sampler    = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};
std::vector<PendingDelete> m_DeletionQueue;
void drainDeletionQueue();       // destroys entries where retireFrame <= m_CompletedFrame
void drainDeletionQueueFully();  // vkDeviceWaitIdle + destroy everything; call from cleanup()
```

`defer*` pushes with `retireFrame = m_FrameNumber`. `drainDeletionQueue()` destroys anything with `retireFrame <= m_CompletedFrame`. Call `drainDeletionQueueFully()` at the top of `VulkanContext::cleanup()` (`VulkanContext.cpp:741`), after the existing `vkDeviceWaitIdle`.

Every Vulkan resource destructor in `Platform/Vulkan/` then routes through this instead of destroying inline:

| File:line | Current | Change to |
|---|---|---|
| `VulkanUniformBuffer.cpp:45` | `vmaDestroyBuffer(...)` | `context->deferDestroyBuffer(m_Buffer, m_Allocation)` |
| `VulkanShaderStorageBuffer.cpp:46` | `vmaDestroyBuffer(...)` | `context->deferDestroyBuffer(...)` |
| `VulkanImage2D.cpp:42,45` | `vkDestroyImageView` / `vmaDestroyImage` | `deferDestroyImageView` / `deferDestroyImage` |
| `VulkanTexture2D.cpp:26,29,32` | sampler / view / image | `deferDestroySampler` / `deferDestroyImageView` / `deferDestroyImage` |

**This is a hard prerequisite for fix 6.** See A1.

---

## 1. Per-frame descriptor sets

### (a) Current behaviour

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanComputeShader.cpp:112-118`:

```cpp
	// Allocate descriptor sets if not done yet
	if (!m_DescriptorSetsAllocated) {
		allocateDescriptorSets();
	}

	// Update descriptor sets with current bound resources
	updateDescriptorSets();
```

`allocateDescriptorSets()` (`VulkanComputeShader.cpp:157-180`) allocates exactly `m_DescriptorSetLayouts.size()` sets — three, one per set index — once, guarded by `m_DescriptorSetsAllocated` (`VulkanComputeShader.cpp:178`, member declared `VulkanComputeShader.h:75`).

`updateDescriptorSets()` (`VulkanComputeShader.cpp:182-284`) rebuilds `m_WriteDescriptorSets` from the `VulkanContext` binding registries and calls `vkUpdateDescriptorSets` at `VulkanComputeShader.cpp:282` — **every single `Dispatch()`**, against those same three sets.

Those sets are then bound at `VulkanComputeShader.cpp:125-134` into `context->getCurrentCommandBuffer()` (`VulkanComputeShader.cpp:106`), which is `m_CommandBuffers[m_CurrentFrame]` (`VulkanContext.h:59`). With `MAX_FRAMES_IN_FLIGHT = 2` (`VulkanContext.h:146`), `endFrame()` submits with fence `m_InFlightFences[m_CurrentFrame]` (`VulkanContext.cpp:429`) and only frame slot `N-2` is fence-waited before recording frame `N` (`VulkanContext.cpp:346`). So when frame `N` calls `vkUpdateDescriptorSets`, frame `N-1`'s command buffer is in the **pending** state and holds a binding to the identical `VkDescriptorSet` handles.

### (b) Why it is wrong, in spec terms

Two distinct violations:

1. **Illegal API call.** Vulkan spec, *Descriptor Set Updates* (§14.2.6 in the 1.3 spec): the contents of a descriptor set must not be updated while that set is *in use* by a command buffer in the pending state, unless the binding was created with `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT` or `VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT`. The layouts here are created with no `VkDescriptorSetLayoutBindingFlagsCreateInfo` at all (`VulkanComputeShader.cpp:381-384` — `layoutInfo` has no `pNext`), so both flags are absent. The validation layers report this as **VUID-vkUpdateDescriptorSets-None-03047**.

2. **Wrong data even if the call were legal.** Descriptors are consumed at *execution* time, not at record time. Frame `N-1`'s pending `vkCmdDispatch` will read whatever the descriptor slot contains when the GPU actually executes it. After fix 2 gives each buffer a per-frame ring slot, frame `N-1`'s dispatch would read frame `N`'s camera matrix and SSBO offsets. This is the failure mode that produces "the viewport is one frame stale, sometimes two, and it flickers under load".

### (c) Fix

Allocate `MAX_FRAMES_IN_FLIGHT × setCount` sets up front, from a pool owned by the shader, and index by frame.

**`VulkanComputeShader.h`** — replace lines 74-75 and 84-85:

```cpp
    // Flat array: index [frame * m_SetCount + set]. The MAX_FRAMES_IN_FLIGHT
    // slices are contiguous, so &m_DescriptorSets[frame * m_SetCount] can be
    // handed straight to vkCmdBindDescriptorSets.
    std::vector<VkDescriptorSet> m_DescriptorSets;
    VkDescriptorPool             m_DescriptorPool = VK_NULL_HANDLE;
    uint32_t                     m_SetCount = 0;

    void createDescriptorPool();
    void allocateDescriptorSets();                 // called once, from the ctor
    void updateDescriptorSets(uint32_t frameIndex);
```

Also delete `bool m_DescriptorSetsAllocated` (`VulkanComputeShader.h:75`) and make the info caches frame-indexed so the `VkDescriptorBufferInfo`/`VkDescriptorImageInfo` backing storage a `VkWriteDescriptorSet` points at stays alive until `vkUpdateDescriptorSets` returns (it already does within one call — but with per-frame updates it is clearer to key them by frame too):

```cpp
    std::unordered_map<uint64_t, VkDescriptorBufferInfo> m_BufferInfos; // key: (frame<<32)|(set<<16)|binding
    std::unordered_map<uint64_t, VkDescriptorImageInfo>  m_ImageInfos;
```

**`createDescriptorPool()`** — new, called from the constructor between `createDescriptorSetLayouts()` and `createPipeline()` (`VulkanComputeShader.cpp:44-45`). A dedicated pool, not the shared one at `VulkanContext.cpp:460-490`, so descriptor budgeting is not entangled with ImGui:

```cpp
void VulkanComputeShader::createDescriptorPool() {
    auto context = VulkanContext::Get();
    const uint32_t frames = context->getMaxFramesInFlight();

    // Count declared bindings per type across all sets in m_DescriptorSetInfos,
    // multiply each by `frames`, emit one VkDescriptorPoolSize per non-zero type.
    std::unordered_map<VkDescriptorType, uint32_t> counts;
    for (const auto& [setNum, info] : m_DescriptorSetInfos)
        for (const auto& b : info.bindings)
            counts[b.type] += b.count * frames;

    std::vector<VkDescriptorPoolSize> sizes;
    for (auto [type, n] : counts) sizes.push_back({ type, n });

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = 0;   // no FREE_DESCRIPTOR_SET: sets live as long as the shader
    poolInfo.maxSets       = static_cast<uint32_t>(m_DescriptorSetLayouts.size()) * frames;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes    = sizes.data();

    if (vkCreateDescriptorPool(context->getDevice(), &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS)
        throw std::runtime_error("Failed to create compute descriptor pool");
}
```

Destroy it in `~VulkanComputeShader` (`VulkanComputeShader.cpp:51-87`) — after the existing `vkDeviceWaitIdle` at line 58, before the layouts are destroyed at lines 73-78. `vkDestroyDescriptorPool` frees all sets allocated from it; do not free sets individually.

**`allocateDescriptorSets()`** — replace `VulkanComputeShader.cpp:157-180` entirely:

```cpp
void VulkanComputeShader::allocateDescriptorSets() {
    auto context = VulkanContext::Get();
    VkDevice device = context->getDevice();
    const uint32_t frames = context->getMaxFramesInFlight();

    m_SetCount = static_cast<uint32_t>(m_DescriptorSetLayouts.size());

    // createDescriptorSetLayouts() resizes with VK_NULL_HANDLE fill (VulkanComputeShader.cpp:363),
    // so a gap in declared set indices would produce a null layout here. Allocation with a null
    // layout is invalid; fail loudly rather than at vkAllocateDescriptorSets.
    for (uint32_t s = 0; s < m_SetCount; ++s) {
        if (m_DescriptorSetLayouts[s] == VK_NULL_HANDLE) {
            LOG_ENGINE_CRITICAL("Descriptor set index {} declared no bindings; set indices must be contiguous", s);
            throw std::runtime_error("Non-contiguous descriptor set indices");
        }
    }

    std::vector<VkDescriptorSetLayout> layouts;
    layouts.reserve(m_SetCount * frames);
    for (uint32_t f = 0; f < frames; ++f)
        layouts.insert(layouts.end(), m_DescriptorSetLayouts.begin(), m_DescriptorSetLayouts.end());

    m_DescriptorSets.resize(layouts.size());

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_DescriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts        = layouts.data();

    if (vkAllocateDescriptorSets(device, &allocInfo, m_DescriptorSets.data()) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate descriptor sets");
}
```

Call it from the constructor, immediately after `createPipeline()` (`VulkanComputeShader.cpp:45`).

**`updateDescriptorSets(uint32_t frameIndex)`** — the body of `VulkanComputeShader.cpp:182-284` stays structurally identical; change three things:

- line 197 `VkDescriptorSet descriptorSet = m_DescriptorSets[setNum];` → `m_DescriptorSets[frameIndex * m_SetCount + setNum]`
- the `m_ImageInfos[setNum][binding.binding]` / `m_BufferInfos[setNum][binding.binding]` accesses at lines 220-221, 234-235, 248-249, 262-263 → the flat frame-keyed map
- line 195 `if (setNum >= m_DescriptorSets.size()) continue;` → `if (setNum >= m_SetCount) continue;`

**`Dispatch()`** — replace `VulkanComputeShader.cpp:112-134`:

```cpp
    const uint32_t frame = context->getCurrentFrame();
    updateDescriptorSets(frame);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);

    if (m_SetCount > 0) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout,
            0, m_SetCount, &m_DescriptorSets[frame * m_SetCount], 0, nullptr);
    }
```

### Would `UPDATE_AFTER_BIND` work instead? No. Three reasons, in increasing order of decisiveness.

1. **Cost.** It requires: enabling `VkPhysicalDeviceVulkan12Features::descriptorBindingUniformBufferUpdateAfterBind`, `...StorageBufferUpdateAfterBind`, `...StorageImageUpdateAfterBind` and `...SampledImageUpdateAfterBind` at device creation — `VulkanContext::createLogicalDevice()` (`VulkanContext.cpp:95-132`) currently enables *no* features at all (`device_builder.build()` at line 99 with nothing requested); adding `VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT` plus a per-binding `VkDescriptorSetLayoutBindingFlagsCreateInfo` at `VulkanComputeShader.cpp:381-384`; and `VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT` on the pool (`VulkanContext.cpp:479` has only `FREE_DESCRIPTOR_SET_BIT`).

2. **Portability.** `descriptorBindingUniformBufferUpdateAfterBind` is the least-supported member of that family and is reported `VK_FALSE` on a meaningful share of drivers and on MoltenVK. Decision 7 keeps macOS on MoltenVK. Making the renderer refuse to start on hardware where a compute path is otherwise fine is a poor trade for zero benefit.

3. **It does not fix the bug.** Read the flag's definition precisely: `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT` says that updates made *between when the set is bound in a command buffer and when that command buffer is submitted* take effect for that submission and do not invalidate the command buffer. It does **not** permit updating a descriptor that a *pending* (already submitted, not yet complete) command buffer will access — that remains undefined. The hazard here is exactly the pending case: frame `N`'s update races frame `N-1`'s in-flight dispatch. `UPDATE_AFTER_BIND` is the bindless-array tool (update slot 4000 while a pending draw touches slot 12); it is not a substitute for per-frame sets.

Per-frame sets cost `3 sets × 2 frames × 3 shaders = 18` descriptor sets total, need no device features, and work identically on MoltenVK. Take them.

### (d) Verification

- **Static:** grep that `vkUpdateDescriptorSets` appears exactly once in `Platform/Vulkan/` and that its `dstSet`s are all derived from `frame * m_SetCount + …`. Assert `m_DescriptorSets.size() == m_SetCount * getMaxFramesInFlight()` in `Dispatch()` under `#ifndef NDEBUG`.
- **Runtime, standard validation:** any occurrence of `VUID-vkUpdateDescriptorSets-None-03047` in the log is a regression. Zero before fix is impossible; the message is emitted from frame 2 onward, so it appears within a second of startup.
- **Runtime, RenderDoc:** capture two consecutive frames, open the compute dispatch's descriptor sets in the Pipeline State viewer, confirm the `VkDescriptorSet` handles differ between the two captures. Same handle in both = fix not applied.
- **Behavioural:** with fix 2 also in, set `raysPerPixel` high enough that a dispatch takes >2 frames of wall time and orbit the editor camera. Pre-fix the image lags and snaps; post-fix it lags by a constant one frame.

---

## 2. Per-frame buffer rings

### (a) Current behaviour

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanUniformBuffer.cpp:17-38` creates one buffer of exactly `size` bytes, `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT`, and keeps `m_MappedData = allocationInfo.pMappedData` (line 36). `AddData` is `VulkanUniformBuffer.cpp:68-74`:

```cpp
void VulkanUniformBuffer::AddData(uint32_t offset, uint32_t dataSize, const void* data) {
	if (m_MappedData) {
		memcpy(static_cast<char*>(m_MappedData) + offset, data, dataSize);
	}
```

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanShaderStorageBuffer.cpp:17-40` is the same with `VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT`; `AddData` at lines 69-75 is the same raw `memcpy`.

Callers: `Renderer::SetupGPUResources` (`/home/sarah/Coding/Haptixxx/X3/X3/src/Renderer/Renderer.cpp:197-354`) writes the settings UBO at lines 219-228, the camera UBO at lines 231-234, and four SSBOs at lines 253-302 — **every frame**, unconditionally.

`VulkanContext::beginFrame()` waits only `m_InFlightFences[m_CurrentFrame]` (`VulkanContext.cpp:346`). With two frame slots, that fence guarantees frame `N-2` completed. Frame `N-1` is still running.

There is **no** `vmaFlushAllocation` call anywhere in `Platform/Vulkan/`.

### (b) Why it is wrong, in spec terms

**Hazard 1 — host-write/device-read data race.** Vulkan's host-write ordering guarantee (spec §7.9, *Host Write Ordering Guarantees*) is that host writes to mapped memory performed *before* a `vkQueueSubmit` are made available to that submission. It says nothing about writes performed *during* a pending submission. Frame `N`'s `memcpy` at `VulkanUniformBuffer.cpp:70` writes the same bytes that frame `N-1`'s in-flight `vkCmdDispatch` is reading through descriptor `set 1 binding 0`. That is an unsynchronized concurrent access to the same memory location by two agents — undefined behaviour, no VUID, no diagnostic. The observable symptom is torn matrices: half of frame `N-1`'s view matrix, half of frame `N`'s, which renders as a one-frame camera "snap" under load.

Note that this is *not* fixed by descriptor-set fix 1 alone, and fix 1 is not fixed by this one alone. They are the same bug seen from the descriptor side and the memory side; both must land.

**Hazard 2 — missing flush on non-coherent memory.** `VMA_MEMORY_USAGE_AUTO` + `HOST_ACCESS_SEQUENTIAL_WRITE` (`VulkanUniformBuffer.cpp:24-26`) selects a host-visible type; VMA does *not* guarantee `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`. On a memory type without it, spec §11.2.7 requires `vkFlushMappedMemoryRanges` before the writes are visible to the device. Nothing calls it. On desktop NVIDIA/AMD the selected type is normally coherent, so this is latent — but it is exactly the kind of thing that surfaces first on MoltenVK or on a memory-pressure fallback heap, and `vmaFlushAllocation` compiles to nothing on coherent memory, so there is no reason not to call it.

### (c) Fix

Give each buffer `MAX_FRAMES_IN_FLIGHT` slots inside one allocation and offset the descriptor write.

**Expose the alignment limits.** In `VulkanContext::pickPhysicalDevice()` (`VulkanContext.cpp:74-93`), the code already fetches `VkPhysicalDeviceProperties properties` at line 91. Store them:

```cpp
// VulkanContext.h private
VkPhysicalDeviceProperties m_DeviceProperties{};
// VulkanContext.h public
const VkPhysicalDeviceLimits& getLimits() const { return m_DeviceProperties.limits; }
```

**Extend the binding registry** so it carries an offset. `VulkanContext.h:39-47`:

```cpp
struct BoundStorageBuffer { VkBuffer buffer; VkDeviceSize offset; uint32_t size; };
struct BoundUniformBuffer { VkBuffer buffer; VkDeviceSize offset; uint32_t size; };

void registerStorageBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset, uint32_t size);
void registerUniformBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset, uint32_t size);
```

Bodies at `VulkanContext.cpp:810-816` become `m_BoundStorageBuffers[binding] = { buffer, offset, size };` etc. Consumers at `VulkanComputeShader.cpp:245-246` and `259-260` set `bufferInfo.offset = boundBuffers.at(binding.binding).offset;` instead of the hardcoded `0`.

**`VulkanUniformBuffer`** — new member layout in `VulkanUniformBuffer.h`:

```cpp
    uint32_t     m_Size        = 0;   // logical size, one frame's worth
    VkDeviceSize m_SliceStride = 0;   // m_Size rounded up to the required alignment
    uint32_t     m_FrameCount  = 0;
```

Constructor (`VulkanUniformBuffer.cpp:17-38`):

```cpp
    const VkDeviceSize align = std::max<VkDeviceSize>(
        context->getLimits().minUniformBufferOffsetAlignment, 1);
    m_FrameCount  = context->getMaxFramesInFlight();
    m_SliceStride = (size + align - 1) / align * align;

    bufferInfo.size = m_SliceStride * m_FrameCount;
```

(For `VulkanShaderStorageBuffer.cpp:17-40`, the same with `minStorageBufferOffsetAlignment`. `VkBufferCreateInfo::size` must be > 0 — see A6.)

`AddData` (`VulkanUniformBuffer.cpp:68-74` and `VulkanShaderStorageBuffer.cpp:69-75`) becomes:

```cpp
void VulkanUniformBuffer::AddData(uint32_t offset, uint32_t dataSize, const void* data) {
    auto context = VulkanContext::Get();
    if (!m_MappedData || !data || !context) { /* warn, return */ }
    assert(offset + dataSize <= m_Size && "AddData writes past one frame slice");

    const VkDeviceSize base = currentFrameOffset();  // context->getCurrentFrame() * m_SliceStride
    memcpy(static_cast<char*>(m_MappedData) + base + offset, data, dataSize);

    // No-op when the memory type is HOST_COHERENT; VMA rounds the range to
    // nonCoherentAtomSize internally.
    vmaFlushAllocation(context->getAllocator(), m_Allocation, base + offset, dataSize);
}
```

`Bind()` (`VulkanUniformBuffer.cpp:51-57`, `VulkanShaderStorageBuffer.cpp:52-58`) registers the current frame's offset:

```cpp
    context->registerUniformBuffer(m_BindingPoint, m_Buffer, currentFrameOffset(), m_Size);
```

This is correct *because* of the §0.1 invariant: `Bind()` is called from `SetupGPUResources` during the same frame whose descriptor set will be updated, so the offset registered is the one frame `N`'s set needs.

**Do not** use `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` here. It would let one descriptor set serve all frames, but fix 1 already gives per-frame sets for the storage-image binding, so the dynamic-offset path adds a second mechanism for the same problem plus per-dispatch `pDynamicOffsets` bookkeeping in `vkCmdBindDescriptorSets` (`VulkanComputeShader.cpp:132`), for nothing.

### (d) Verification

This one is the hardest to verify, and you should know that up front: **synchronization validation does not track host writes to persistently-mapped memory.** Syncval will not flag the pre-fix code and will not confirm the post-fix code. Verify by:

- **Static, mechanical:** grep `Platform/Vulkan/` for `memcpy` — every remaining one must be preceded by a `currentFrameOffset()` base, and every one must be followed by a `vmaFlushAllocation`. Grep for `bufferInfo.offset = 0` — there must be none left in `VulkanComputeShader.cpp`.
- **Static, arithmetic:** assert at buffer creation that `m_SliceStride % align == 0` and at descriptor-write time that `bufferInfo.offset % align == 0`. Standard validation *will* catch a misaligned offset (`VUID-VkWriteDescriptorSet-descriptorType-00327` / `-00328`), which is a useful proxy check that the ring maths is being exercised.
- **Behavioural stress:** temporarily raise `MAX_FRAMES_IN_FLIGHT` (`VulkanContext.h:146`) to 3, raise the ring to match, set `raysPerPixel` to something that makes a dispatch take 50-100 ms, and sweep the editor camera. Pre-fix: visible per-frame jitter/tearing in the projection as slices of two different camera matrices land in one dispatch. Post-fix: smooth, uniformly one frame behind.
- **Non-coherent path:** to exercise the flush, run once under a driver/config where the chosen heap is not `HOST_COHERENT`, or temporarily force it by passing `VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT` off and `requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT` with `preferredFlags = 0`. Without the flush the image is stale garbage; with it, correct.

---

## 3. `ReadData`

### (a) Current behaviour

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanShaderStorageBuffer.cpp:77-93`:

```cpp
void* VulkanShaderStorageBuffer::ReadData(uint32_t offset, uint32_t dataSize) {
	if (!m_MappedData) { ... return nullptr; }
	if (m_ReadBuffer.size() < dataSize) { m_ReadBuffer.resize(dataSize); }

	// Copy from mapped memory to our read buffer
	// Note: For GPU-written data, a memory barrier should be issued before this
	memcpy(m_ReadBuffer.data(), static_cast<char*>(m_MappedData) + offset, dataSize);

	return m_ReadBuffer.data();
}
```

### (b) Who calls it

**Nothing.** An exhaustive grep across `X3/`, `X3-Editor/` and `X3-Runtime/` (excluding `libs/`) finds only declarations and definitions:

- `/home/sarah/Coding/Haptixxx/X3/X3/src/Renderer/IShaderStorageBuffer.h:32` — pure virtual declaration
- `/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanShaderStorageBuffer.h:19` and `.cpp:77` — this implementation
- `/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/OpenGL/OpenGLShaderStorageBuffer.h:20` and `.cpp:41` — deleted in Phase 1a

There are zero call sites. Every SSBO in the engine is `readonly` on the GPU side — see `/home/sarah/Coding/Haptixxx/X3/X3/res/shaders/PathTracing.comp:103-127`, where all seven storage buffers are declared `readonly buffer`. Nothing in the engine ever writes an SSBO from a shader, so there is nothing to read back.

Were it called, the bug would be a host read of device-written memory with no execution dependency (no fence wait on the submission that wrote it) and no `vkInvalidateMappedMemoryRanges` — spec §11.2.7 requires the invalidate on non-coherent memory, and the fence wait is required for the write to have happened at all.

### (c) Fix — delete it

1. Delete `/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanShaderStorageBuffer.cpp:77-93`.
2. Delete `/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanShaderStorageBuffer.h:19` and the `std::vector<char> m_ReadBuffer;` member at line 36 (and its comment at line 35).
3. Delete `/home/sarah/Coding/Haptixxx/X3/X3/src/Renderer/IShaderStorageBuffer.h:32`.
4. `VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT` at `VulkanShaderStorageBuffer.cpp:26` exists only to serve readback (see the comment at line 25: *"Use RANDOM access bit for both read and write operations"*). With `ReadData` gone, change it to `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` to match `VulkanUniformBuffer.cpp:25`. **This matters:** `HOST_ACCESS_RANDOM` steers VMA away from write-combined / uncached-write memory, which is the memory you actually want for a write-only upload ring. Reading from write-combined memory is catastrophically slow; writing to it is not. With no reader, the sequential-write hint is strictly better.

**If readback is ever needed** (it will be, for GPU-driven culling or a picking buffer in Phase 7), do not resurrect this shape. The correct design, recorded here so the next person does not reinvent it wrong: a *separate* `VK_BUFFER_USAGE_TRANSFER_DST_BIT` buffer on `HOST_VISIBLE | HOST_CACHED` memory, `MAX_FRAMES_IN_FLIGHT` slices; a `vkCmdCopyBuffer` from the device-local SSBO into slice `N` recorded at the end of frame `N`'s command buffer, preceded by a `VkBufferMemoryBarrier` (`srcStageMask = COMPUTE_SHADER`, `srcAccessMask = SHADER_WRITE` → `dstStageMask = TRANSFER`, `dstAccessMask = TRANSFER_READ`); the host reads slice `N` in frame `N + MAX_FRAMES_IN_FLIGHT`, after `beginFrame()`'s fence wait, calling `vmaInvalidateAllocation` first. The API is inherently latent by `MAX_FRAMES_IN_FLIGHT` frames — expose that in the signature rather than pretending it is synchronous.

### (d) Verification

Compile. If it links, nothing called it. Confirm with `grep -rn "ReadData" X3 X3-Editor X3-Runtime --include='*.cpp' --include='*.h'` returning nothing.

---

## 4. Gate validation layers on build type

### (a) Current behaviour

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanContext.h:166`:

```cpp
	bool m_EnableValidationLayers = true; // Disable in release builds
```

Consumed at `VulkanContext.cpp:50` (`.request_validation_layers(m_EnableValidationLayers)`). Nothing acts on the comment. `X3/CMakeLists.txt:116-128` defines `BUILD_INSTALL`, `X3_USE_VULKAN`/`X3_USE_OPENGL` and resource paths; there is no build-type-derived define at all. `CMakePresets.json` sets `CMAKE_BUILD_TYPE` per preset but nothing reads it in source.

### (b) Why it is wrong

Not a spec violation — a shipping defect. Validation layers cost 2-10× CPU time in the driver, allocate heavily, and (with `use_default_debug_messenger()` at `VulkanContext.cpp:51`) print to stdout. A shipped build that silently requires `VK_LAYER_KHRONOS_validation` to be installed will also behave differently on machines that lack it, since `request_validation_layers` is the non-fatal variant (`VkBootstrap.h:411`) and silently proceeds without them — so the shipped product's behaviour depends on whether the end user happens to have the Vulkan SDK.

### (c) Fix

**CMake.** In `/home/sarah/Coding/Haptixxx/X3/X3/CMakeLists.txt`, alongside the existing `target_compile_definitions` block at line 116:

```cmake
option(X3_VULKAN_VALIDATION "Enable Vulkan validation layers" $<IF:$<CONFIG:Release>,OFF,ON>)
# For single-config generators, resolve at configure time instead:
if(NOT DEFINED X3_VULKAN_VALIDATION)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(X3_VULKAN_VALIDATION OFF)
    else()
        set(X3_VULKAN_VALIDATION ON)
    endif()
endif()
target_compile_definitions(X3Engine PUBLIC
    $<$<BOOL:${X3_VULKAN_VALIDATION}>:X3_VULKAN_VALIDATION>)
```

Note Phase 1a replaces the four graphics-API presets with plain Debug/Release; wire this to those.

**Header.** `VulkanContext.h:166`:

```cpp
#ifdef X3_VULKAN_VALIDATION
	bool m_EnableValidationLayers = true;
#else
	bool m_EnableValidationLayers = false;
#endif
```

Additionally allow a runtime opt-in in non-Release so a QA build can be told to turn syncval on without a rebuild: in `VulkanContext::createInstance()` (`VulkanContext.cpp:45`), before line 50, honour an env var:

```cpp
    if (const char* env = std::getenv("X3_VULKAN_VALIDATION"))
        m_EnableValidationLayers = (env[0] == '1');
```

**While you are in `createInstance()`, do these three related things** (they are what makes the Exit Criteria "validation layers clean under load" actually checkable):

1. Replace `.use_default_debug_messenger()` (`VulkanContext.cpp:51`) with `.set_debug_callback(...)` routing to `LOG_ENGINE_ERROR` / `LOG_ENGINE_WARN` / `LOG_ENGINE_INFO` by severity, so messages land in the engine log rather than stdout. In a debug build, `__builtin_trap()` / `__debugbreak()` on `VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT` so a VUID stops the debugger at the offending call.
2. Enable the extra validation features (vk-bootstrap exposes `add_validation_feature_enable`, `VkBootstrap.h:434`):
   ```cpp
   if (m_EnableValidationLayers) {
       builder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
       builder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
   }
   ```
   Synchronization validation is the layer that finds the hazards in §7 (A2, A3, A4, A5). Without it, "validation layers clean" is a much weaker claim than the exit criteria intends.
3. `.set_debug_messenger_severity(...)` to exclude `VERBOSE` and `INFO` unless an env var asks for them, so the log stays readable.

### (d) Verification

- Configure with `-DCMAKE_BUILD_TYPE=Release`, confirm `X3_VULKAN_VALIDATION` is absent from `build/*/compile_commands.json`, run, and confirm the log contains no `VK_LAYER_KHRONOS_validation` init lines and `vkEnumerateInstanceLayerProperties` is not being relied on.
- Configure Debug, run, and deliberately trigger a known VUID (e.g. temporarily pass `range = 0` to a `VkDescriptorBufferInfo`) — confirm the message arrives via `LOG_ENGINE_ERROR` and the debugger breaks.
- `X3_VULKAN_VALIDATION=0 ./X3Editor` in a Debug build must start with layers off.

---

## 5. Wire `RenderSettings::vSync` to swapchain present mode

### (a) Current behaviour

`/home/sarah/Coding/Haptixxx/X3/X3/src/Renderer/RenderSettings.h:38`:

```cpp
        bool vSync = true;
```

Serialized at line 51, deserialized at line 70. **Zero references in `Platform/Vulkan/`.** An exhaustive grep shows the only things that touch vsync at all are:

- `/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Windows/GLFWWindow.cpp:107-110`:
  ```cpp
  	void GLFWWindowIMPL::setVSync(bool enabled) {
  		m_VSync = enabled;
  		glfwSwapInterval(enabled);
  	}
  ```
  `glfwSwapInterval` is **a no-op when the window was created with `GLFW_CLIENT_API == GLFW_NO_API`**, which is exactly what `VulkanContext::setWindowHints()` does (`VulkanContext.cpp:9-12`). GLFW's own docs state the call requires a current OpenGL/GLES context. So this does nothing.
- Call sites: `GLFWWindow.cpp:56` (from `WindowProps::VSync`), `EditorLayer.cpp:61` (from `SetVSyncEvent`, dispatched by `RenderSettingsPanel.cpp:217`), `RuntimeLayer.cpp:75` (from `ExportSettings::vSync`).

So the checkbox in `RenderSettingsPanel.cpp:216` and the export setting both terminate in a no-op.

**What the swapchain actually does today.** `VulkanContext::createSwapchain()` (`VulkanContext.cpp:149-173`) is:

```cpp
	auto swap_ret = swapchain_builder
		.set_old_swapchain(m_Swapchain)
		.build();
```

No present mode is requested. vk-bootstrap's default list is `{ MAILBOX, FIFO }` (`X3/libs/vk-bootstrap/src/VkBootstrap.cpp:2172-2175`), applied at `VkBootstrap.cpp:1898-1899`, selected by `detail::find_present_mode` which falls back to `VK_PRESENT_MODE_FIFO_KHR` when nothing matches (`VkBootstrap.cpp:1812-1821`). **Net effect: MAILBOX wherever available, FIFO otherwise — i.e. the engine currently behaves as if vSync is permanently *off*** (MAILBOX does not tear but does not throttle the CPU either), regardless of the setting.

Also note the extent is never requested — `find_extent` uses surface `currentExtent` (`VkBootstrap.cpp:1925`), which is correct. And `min_image_count` is 0 → `capabilities.minImageCount + 1` (`VkBootstrap.cpp:1911-1913`), typically 3 or 4.

### (b) Why it is wrong

Not a hazard; a dead setting that misreports. Two secondary problems worth fixing while here:

- Three independent copies of the flag exist and can disagree: `WindowProps::VSync` (`/home/sarah/Coding/Haptixxx/X3/X3/src/Core/IWindow.h:19`, defaulting to `false` at line 25), `RenderSettings::vSync` (defaulting to `true`), and `EditorState::temp.vSync` (`/home/sarah/Coding/Haptixxx/X3/X3-Editor/src/EditorState.h:21`, defaulting to `false`).
- Present mode is fixed at swapchain creation. Changing it at runtime requires swapchain recreation, which the code has (`VulkanContext.cpp:663-717`) but never invokes for this reason.

### (c) Fix

**Mapping.** Use vk-bootstrap's existing preference-list-with-fallback rather than hand-rolling `vkGetPhysicalDeviceSurfacePresentModesKHR` — the availability check is already correct at `VkBootstrap.cpp:1812-1821`, and `VK_PRESENT_MODE_FIFO_KHR` is the one mode the spec requires every implementation supporting `VK_KHR_swapchain` to support, so the fallback can never fail.

| `vSync` | Preference order | Rationale |
|---|---|---|
| `true` | `FIFO_KHR` only | Guaranteed present. Hard-throttles to refresh, no tearing. This is what a user ticking "VSync" means. Do **not** silently prefer `FIFO_RELAXED_KHR` — it tears on late frames, which is precisely what the user asked not to happen. |
| `false` | `MAILBOX_KHR`, then `IMMEDIATE_KHR`, then `FIFO_KHR` | MAILBOX first: unthrottled, tear-free, lowest cost to the compositor; it needs ≥3 swapchain images, which the `minImageCount + 1` default already supplies. IMMEDIATE second: genuinely lowest latency but tears; it is the honest fallback when MAILBOX is absent (common on some Wayland/X11 driver paths). FIFO last so `build()` cannot fail. |

Do not add `FIFO_RELAXED_KHR` to either list. It is a third behaviour that neither checkbox state describes.

**Implementation.**

`VulkanContext.h` — constructor and new members:

```cpp
	VulkanContext(GLFWwindow* window, bool vsync);
	void setVSync(bool enabled);       // recreates the swapchain if the value changed
	bool getVSync() const { return m_VSync; }
private:
	bool m_VSync = true;
```

`VulkanContext::createSwapchain()` (`VulkanContext.cpp:149`), replacing lines 153-155:

```cpp
	vkb::SwapchainBuilder swapchain_builder{ m_VkbDevice };
	swapchain_builder.set_old_swapchain(m_Swapchain);

	if (m_VSync) {
		swapchain_builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
	} else {
		// set_desired_present_mode inserts at the front; add_desired_present_mode appends.
		swapchain_builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR);
		swapchain_builder.add_desired_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR);
		swapchain_builder.add_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
	}
	// See A7: the blit path requires TRANSFER_DST on swapchain images.
	swapchain_builder.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);

	auto swap_ret = swapchain_builder.build();
```

After `build()` succeeds, log the mode actually selected (`m_VkbSwapchain.present_mode`) next to the existing log at `VulkanContext.cpp:171-172`, so a user reporting "vsync doesn't work" produces a diagnosable line.

`setVSync`:

```cpp
void VulkanContext::setVSync(bool enabled) {
    if (enabled == m_VSync) return;
    m_VSync = enabled;
    recreateSwapchain();
}
```

`recreateSwapchain()` already does `vkDeviceWaitIdle`, teardown and rebuild (`VulkanContext.cpp:663-717`) and sets `m_SwapchainRecreated` (line 714) so `ImGuiContext::BeginFrame` re-inits the ImGui backend (`/home/sarah/Coding/Haptixxx/X3/X3-Editor/src/ImGuiContext.cpp:221-261`). Fix A9 before relying on it.

**Plumbing, initial value.** `GLFWWindowIMPL`'s constructor creates the context at `/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Windows/GLFWWindow.cpp:49-54` and calls `setVSync(windowProps.VSync)` at line 56 — *after* `m_Context->init()`, i.e. after the swapchain already exists. Change line 50 to pass the flag in so the first swapchain is correct and no immediate recreation happens:

```cpp
	m_Context = new VulkanContext(m_NativeWindow, windowProps.VSync);
```

**Plumbing, runtime toggle.** Replace `GLFWWindow.cpp:107-110`:

```cpp
	void GLFWWindowIMPL::setVSync(bool enabled) {
		m_VSync = enabled;
		if (auto* ctx = VulkanContext::Get()) ctx->setVSync(enabled);
	}
```

(Phase 1b collapses `IRenderingContext` into `VulkanContext`; until then, either downcast `m_Context` or call the singleton. Do not add a virtual `setVSync` to `IRenderingContext` — decision 8 says that interface is going away.)

**Plumbing, single source of truth.** `RenderSettings::vSync` is the persisted project setting. Make `RenderSettingsPanel.cpp:216` write `RenderSettings::vSync` and go through the existing `UpdateRenderSettingsEvent` path (which `RenderLayer::onEvent` already handles, `/home/sarah/Coding/Haptixxx/X3/X3/src/Core/Layers/RenderLayer.cpp:50-53`) rather than the separate `SetVSyncEvent`; have whoever consumes it call `IWindow::setVSync`. Change `WindowProps::VSync`'s default at `IWindow.h:25` from `false` to `true` to match `RenderSettings::vSync`, or explicitly document why they differ. `ExportSettings::vSync` (`/home/sarah/Coding/Haptixxx/X3/X3/src/Export/ExportSettings.h:29` → `RuntimeLayer.cpp:75`) then works unchanged.

### (d) Verification

- Start the editor; the log line added after `VulkanContext.cpp:171` must read `FIFO` with the setting on and `MAILBOX` (or `IMMEDIATE`) with it off.
- Toggle the checkbox at runtime: the log must show a swapchain recreation and the new mode. Frame rate as reported by `ProfilerPanel` must clamp to the display refresh with vSync on and exceed it with vSync off (raise `resolution` low enough that the compute dispatch is not the bottleneck).
- With `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT` on (§4), the best-practices layer warns if MAILBOX is requested with fewer than three swapchain images — confirm no such warning.
- macOS/MoltenVK: verify the selected mode is reported and that the fallback chain does not land on something unexpected. Per the cross-cutting note in ENGINE_PLAN.md, test this now, not at Phase 10.

---

## 6. Batch resource uploads

### (a) Current behaviour

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanContext.cpp:502-533`:

```cpp
VkCommandBuffer VulkanContext::beginSingleTimeCommands() {
	VkCommandBufferAllocateInfo allocInfo{};
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

Callers:
- `VulkanImage2D.cpp:27-29` — the layout transition to `GENERAL` in the constructor. Hit whenever the render resolution changes (`Renderer.cpp:201-205` creates two `IImage2D`s).
- `VulkanImage2D.cpp:124` / `:148` — the staging upload path.
- `VulkanTexture2D.cpp:105` / `:128` — the skybox upload.

`m_CommandPool` is the *same* pool the per-frame command buffers come from (`VulkanContext.cpp:284`, `:297`).

### (b) Why it is wrong

`vkQueueWaitIdle` is defined (spec §5.5) as equivalent to submitting a fence to the queue and waiting on it with an infinite timeout — it blocks the calling thread until **every** submission on that queue has completed, including the previous frame's rendering. Every texture, every image, and every resolution change therefore costs a full GPU drain plus a round trip. Loading a scene with `N` meshes and `M` textures serializes into `N + M` drains.

It is also on the *graphics* queue, mid-frame: `Renderer::SetupGPUResources` runs inside `LayerStack::onUpdate()` while `m_CommandBuffers[m_CurrentFrame]` is in the recording state with a render pass open. So a resolution change stalls the pipeline at the worst possible moment.

### (c) Fix — and a warning about ordering

> **Read this before deleting `vkQueueWaitIdle`.** That call is currently the *only* thing making image and texture recreation safe. See A1: `Renderer.cpp:202-203` and `Renderer.cpp:244` construct the replacement resource (which idles the queue) *before* the `shared_ptr` assignment destroys the old one, so the old `VkImage` is destroyed on an idle queue by accident. **Removing the wait without the §0.2 deletion queue in place converts a hidden inefficiency into an immediate use-after-free.** Land §0.2 first.

Build an upload context on the existing infrastructure.

`VulkanContext.h` — add:

```cpp
	// Records into a batched upload command buffer that is submitted as the first
	// command buffer of the next frame submission. Never blocks.
	VkCommandBuffer getUploadCommandBuffer();

	// Submits pending uploads and blocks. Only for init/teardown paths that run
	// outside the frame loop (e.g. ImGui font upload).
	void flushUploadsBlocking();

private:
	VkCommandPool   m_UploadPool          = VK_NULL_HANDLE; // TRANSIENT | RESET_COMMAND_BUFFER
	VkCommandBuffer m_UploadCmd           = VK_NULL_HANDLE;
	bool            m_UploadCmdRecording  = false;
	void createUploadResources();
	void endUploadRecording();  // vkEndCommandBuffer + trailing barrier; no submit
```

**`createUploadResources()`** — called from `init()` (`VulkanContext.cpp:24-43`) right after `createCommandPool()` at line 33. A separate pool with `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`, one primary command buffer. A separate pool matters: command pools are externally synchronized, and allocating from `m_CommandPool` while one of its buffers is recording is fragile the moment Phase 4's job system lands.

**`getUploadCommandBuffer()`**:

```cpp
VkCommandBuffer VulkanContext::getUploadCommandBuffer() {
    if (!m_UploadCmdRecording) {
        vkResetCommandBuffer(m_UploadCmd, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_UploadCmd, &bi);
        m_UploadCmdRecording = true;
    }
    return m_UploadCmd;
}
```

**`endUploadRecording()`** appends one global barrier before ending, so every upload's writes are visible to every possible consumer without each call site reasoning about it:

```cpp
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
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

(Image *layout* transitions still belong at the call sites — a global memory barrier does not transition layouts. Keep the per-image barriers in `VulkanImage2D::transitionImageLayout` / `VulkanTexture2D::transitionImageLayout`, but fix their stage masks per A5.)

**Submission — this is the part that makes it correct without extra semaphores.** In `VulkanContext::endFrame()` (`VulkanContext.cpp:398`), replace the single-command-buffer submit at lines 421-422:

```cpp
	VkCommandBuffer cmds[2];
	uint32_t cmdCount = 0;
	if (m_UploadCmdRecording) {
		endUploadRecording();
		m_UploadCmdRecording = false;
		cmds[cmdCount++] = m_UploadCmd;   // must be FIRST: submission order matters
	}
	cmds[cmdCount++] = m_CommandBuffers[m_CurrentFrame];

	submitInfo.commandBufferCount = cmdCount;
	submitInfo.pCommandBuffers    = cmds;
```

Two separate `vkQueueSubmit` calls to the same queue are *not* automatically ordered in execution — you would need a semaphore. Command buffers within a *single* `VkSubmitInfo::pCommandBuffers` array **are** in submission order (spec §7.2), so the trailing barrier in the upload buffer correctly synchronizes against everything in the frame buffer. This costs zero semaphores and zero extra submits.

Because the upload command buffer is reset at the start of the *next* `getUploadCommandBuffer()` call, and it is submitted with the frame's fence, it is safe to reset once that fence has been waited — which `beginFrame()` does. Add an assert that `m_UploadCmdRecording == false` on entry to `beginFrame()`.

**Staging buffer lifetime.** `VulkanTexture2D.cpp:131` and `VulkanImage2D.cpp:151` currently destroy the staging buffer immediately, relying on the wait. Change both to `context->deferDestroyBuffer(stagingBuffer, stagingAllocation)` (§0.2) and delete the misleading comments at `VulkanTexture2D.cpp:130` and `VulkanImage2D.cpp:150`.

**`flushUploadsBlocking()`** — for the two paths that genuinely run outside a frame: `VulkanContext::init()` teardown, and the ImGui font upload at `/home/sarah/Coding/Haptixxx/X3/X3-Editor/src/ImGuiContext.cpp:192-197` (which already does its own `vkDeviceWaitIdle` at line 197). It does `endUploadRecording()`, `vkQueueSubmit` with a dedicated fence, `vkWaitForFences`, reset fence, then `drainDeletionQueueFully()`.

**Update the call sites** — replace `beginSingleTimeCommands()`/`endSingleTimeCommands()` with `getUploadCommandBuffer()` and *no* end call:

- `VulkanImage2D.cpp:27-29` → `transitionToGeneral(context->getUploadCommandBuffer());`
- `VulkanImage2D.cpp:124` → `VkCommandBuffer cmd = context->getUploadCommandBuffer();`, delete line 148
- `VulkanTexture2D.cpp:105` → same, delete line 128

Delete `beginSingleTimeCommands` / `endSingleTimeCommands` (`VulkanContext.cpp:502-533`, `VulkanContext.h:67-68`) once nothing calls them.

### (d) Verification

- **Static:** `grep -rn "vkQueueWaitIdle\|vkDeviceWaitIdle" X3/src/Platform/Vulkan/` must return exactly: `recreateSwapchain` (`VulkanContext.cpp:665`), `cleanup` (`VulkanContext.cpp:743`), `~VulkanComputeShader` (`VulkanComputeShader.cpp:58`), and `flushUploadsBlocking`. Any other occurrence is a regression.
- **Runtime, validation:** with `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT` on, the best-practices layer emits `BestPractices-vkQueueWaitIdle`-class warnings for pipeline stalls — these should disappear. `VUID-vkDestroyBuffer-buffer-00922` / `-vkDestroyImage-image-01000` must not appear; if they do, the deletion queue is not covering a path.
- **Runtime, profiler:** `ProfilerPanel` already times `Renderer::SetupGPUResources()`. Change the render resolution repeatedly and load a scene with several meshes; the per-upload spike should collapse from a full frame-time drain to near-zero.
- **Correctness of the batch:** load a scene with a skybox and immediately screenshot frame 1. If the skybox is black/garbage, the upload barrier or the submission order is wrong (the batched upload is now *in the same submit* as the frame that samples it, so it must be correct — but verify, because this is the highest-risk part of the change).
- **RenderDoc:** the capture should show one submit per frame containing two command buffers, upload first.

---

# 7. ADDITIONAL FINDINGS

These were not on the list. They are ordered by severity. **A2, A3 and A7 are, in my assessment, more severe than several of the six numbered items** — A2 and A7 are outright undefined behaviour executed on every frame.

---

## A1 — Resources destroyed while referenced by in-flight command buffers (SSBOs: real; images/textures: safe only by accident)

**(a) Current behaviour.** `/home/sarah/Coding/Haptixxx/X3/X3/src/Renderer/Renderer.cpp:253-264`:

```cpp
			uint32_t count = pScene->MeshEntityLookupTable.size();
			uint32_t sizeBytes = sizeof(MeshEntityHandle) * count;
			if (count != m_Cache.entityLookupSize || !m_MeshEntityLookupSSBO) {
				m_MeshEntityLookupSSBO = IShaderStorageBuffer::Create(sizeBytes, 0, BufferUsageType::DYNAMIC_DRAW);
				m_Cache.entityLookupSize = count;
			}
```

Identical patterns at `Renderer.cpp:269-271` (transforms), `:281-283` (materials), `:294-296` (lights), `:314-322` (mesh buffer), `:328-336` (nodes), `:342-350` (indices). The `shared_ptr` assignment drops the last reference to the old `VulkanShaderStorageBuffer`, whose destructor calls `vmaDestroyBuffer` immediately (`VulkanShaderStorageBuffer.cpp:46`). `VulkanShaderStorageBuffer`'s constructor performs no queue synchronization of any kind.

**(b) Why it is wrong.** `VUID-vkDestroyBuffer-buffer-00922`: *"All submitted commands that refer to buffer, either directly or via a VkBufferView, must have completed execution."* Frame `N-1`'s command buffer is pending and its descriptor set at `set 2 binding 0` holds this exact `VkBuffer`. Destroying it frees the underlying `VkDeviceMemory` back to VMA, which will hand the same pages to the *next* allocation — so the in-flight dispatch reads whatever now lives there. Add one entity to a scene and this fires.

The image and texture paths (`Renderer.cpp:202-203`, `:244`) have the same shape, and are safe *only* because the replacement's constructor calls `endSingleTimeCommands` → `vkQueueWaitIdle` (`VulkanImage2D.cpp:29`, `VulkanTexture2D.cpp:128`) *before* the `shared_ptr` assignment releases the old object. This is accidental. **Fix 6 destroys this accident.**

There is a second failure mode in the same area. `VulkanContext`'s binding registries (`VulkanContext.h:175-178`) store raw `VkBuffer`/`VkImageView`/`VkSampler` handles and are **never cleared** — `registerStorageBuffer` etc. (`VulkanContext.cpp:810-824`) only overwrite. When a resource is destroyed and its replacement fails to register (see A6) or is never registered (see A3), the map retains a dangling handle which `VulkanComputeShader::updateDescriptorSets` then writes into a live descriptor (`VulkanComputeShader.cpp:244`, `:258`).

**(c) Fix.** §0.2's deletion queue. Route every destructor in `Platform/Vulkan/` through it (table in §0.2). Additionally, add unregistration so the registry cannot outlive its target:

```cpp
	void unregisterStorageBuffer(uint32_t binding, VkBuffer buffer); // erases only if handle matches
	void unregisterUniformBuffer(uint32_t binding, VkBuffer buffer);
	void unregisterStorageImage(uint32_t unit, VkImageView view);
	void unregisterSampledImage(uint32_t unit, VkImageView view);
```

called from each resource destructor before the deferred destroy. The "only if the handle matches" guard matters: the new resource has usually already registered itself over the old entry by the time the old destructor runs.

Phase 1b removes this registry entirely in favour of explicit descriptor-set membership. Until then, this guard is what keeps it from being a use-after-free machine.

**(d) Verification.** Standard validation catches this directly: `VUID-vkDestroyBuffer-buffer-00922` / `VUID-vkDestroyImage-image-01000` / `VUID-vkDestroyImageView-imageView-01026`. Reproduce pre-fix by adding a mesh entity to a scene at runtime; post-fix, do the same and confirm silence. Also run with `VK_LAYER_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES` and check for `BestPractices-Threading-*`. A RenderDoc capture taken across the frame in which the entity count changes will show the descriptor pointing at a valid, correctly-sized buffer.

---

## A2 — `vkCmdDispatch` and `vkCmdPipelineBarrier` are recorded **inside an active render pass instance**

**(a) Current behaviour.** `VulkanContext::beginFrame()` opens the main render pass at `VulkanContext.cpp:392` and sets `m_RenderPassActive = true` at line 393. `beginFrame()` runs from `swapBuffers()` (`VulkanContext.cpp:651`) at the *end* of the previous main-loop iteration (`application.cpp:69`). The next iteration then runs `LayerStack::onUpdate()` (`application.cpp:63`), whose first layer is `RenderLayer` (pushed first at `application.cpp:33`, before `PhysicsLayer` at line 34 and before `EditorLayer`/`RuntimeLayer` at `EditorMain.cpp:17` / `RuntimeMain.cpp:15`). `RenderLayer::onUpdate` → `Renderer::Render` → `Renderer::Draw` (`Renderer.cpp:63`, `:356-378`) → `IComputeShader::Dispatch()` at `Renderer.cpp:377`.

So at `VulkanComputeShader.cpp:121`, `:125`, `:137` and `:146`, `m_RenderPassActive == true` and the main render pass instance begun at `VulkanContext.cpp:392` is open:

```cpp
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
	...
	vkCmdDispatch(cmd, m_WorkGroupSizes.x, m_WorkGroupSizes.y, m_WorkGroupSizes.z);
	...
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
```

**(b) Why it is wrong.**

- `VUID-vkCmdDispatch-renderpass`: *"This command must only be called outside of a render pass instance."* Dispatch is not a render-pass command. Full stop.
- `VUID-vkCmdPipelineBarrier-pDependencies-02285`: if `vkCmdPipelineBarrier` is called within a render pass instance, the render pass must have been created with a `VkSubpassDependency` whose `srcSubpass` and `dstSubpass` both equal the current subpass index (a self-dependency), and the barrier must be a memory or image barrier honouring `VK_DEPENDENCY_BY_REGION_BIT`. `createRenderPass()` (`VulkanContext.cpp:196-211`) declares exactly one dependency, `VK_SUBPASS_EXTERNAL → 0`. There is no self-dependency. Both the barrier here and the layout barriers elsewhere would be illegal inside a pass.

This is not a subtle hazard — it is a hard validation error on every frame, and its practical consequence is driver-dependent: some drivers tolerate it, some fault, and tile-based GPUs (which is what MoltenVK sits on top of, per decision 7) are exactly the class most likely to misbehave, because a render pass instance corresponds to a tiling job with no compute capability inside it.

There is a second, separate defect in the same code path. `VulkanContext::init()` deliberately does not begin a frame (`VulkanContext.cpp:40-42`):

```cpp
	// Don't call beginFrame() here - let the first swapBuffers() call handle it
	m_FirstFrame = true;
```

so on the very first main-loop iteration, `Dispatch()` records into `m_CommandBuffers[0]`, which has never been `vkBeginCommandBuffer`'d — it is in the **initial** state, and recording into it violates `VUID-vkCmdDispatch-commandBuffer-recording`. Then `ImGuiContext::EndFrame` calls `ensureFrameStarted()` (`ImGuiContext.cpp:278`) → `beginFrame()` → `vkResetCommandBuffer` at `VulkanContext.cpp:367`, silently discarding that first dispatch.

**(c) Fix.** The structural cure is Phase 1b's item: *"`IRenderingContext::swapBuffers()` conflates submit, present and acquire into one GL-shaped call, requiring an `m_FirstFrame` special case to work at all → replace with explicit `beginFrame()` / `endFrame()` / `present()`."* Do it here, because it is a correctness fix and not merely an interface cleanup.

Concretely:

1. **Do not begin a render pass in `beginFrame()`.** Delete `VulkanContext.cpp:380-393`. `beginFrame()` becomes: wait fence, drain deletion queue, acquire, reset fence, reset command buffer, `vkBeginCommandBuffer`. The command buffer is open, no render pass is active, and compute can be recorded legally.
2. **Restructure the loop** in `application.cpp:41-71` to `beginFrame()` → `LayerStack::onUpdate()` → `endFrame()` — one frame per iteration, no cross-iteration straddle. `m_FirstFrame` (`VulkanContext.h:168`), `ensureFrameStarted()` (`VulkanContext.cpp:492-500`, `VulkanContext.h:64`) and the `ImGuiContext.cpp:278` call all disappear.
3. **Render passes start where they are needed.** The editor path already funnels through `beginOverlayRenderPass()` (`ImGuiContext.cpp:283`); that becomes the *only* pass in the editor frame, and it needs `loadOp` handling per A4. The runtime path uses only the blit + present and needs no pass at all.
4. The main render pass (`m_RenderPass`, `VulkanContext.cpp:175-218`) then has no user — the editor uses `m_OverlayRenderPass`, the runtime uses the blit. **Delete `m_RenderPass`, `createFramebuffers`' dependence on it, and the dead `beginRenderPass()` (`VulkanContext.cpp:535-576`, which has zero callers).** Keep one render pass whose `loadOp` is `LOAD` for the editor overlay, with the swapchain cleared by `vkCmdClearColorImage` or by the blit's existing clear at `VulkanContext.cpp:887-888`.
5. Move the barrier currently at `VulkanComputeShader.cpp:141-154` so it is recorded after the dispatch and outside any pass (it will be, after step 1), and widen its `dstStageMask` — see A5.

**(d) Verification.** Standard validation, no syncval needed. Pre-fix, `VUID-vkCmdDispatch-renderpass` fires on literally every frame from frame 2 onward; it is the single loudest message in the log. Post-fix it must be absent, along with `VUID-vkCmdPipelineBarrier-pDependencies-02285` and `VUID-vkCmdDispatch-commandBuffer-recording`. A RenderDoc capture must show the dispatch as a top-level command, not nested under a `vkCmdBeginRenderPass` marker. Visually, the first rendered frame should now contain content instead of being blank.

---

## A3 — The skybox descriptor is **never written**, and the light SSBO descriptor is not written when a scene has no lights

**(a) Current behaviour.** `VulkanComputeShader`'s set 0 declares two bindings (`VulkanComputeShader.cpp:19-23`):

```cpp
		{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},           // rayTracingTexture
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT}   // skyboxTexture
```

matching `/home/sarah/Coding/Haptixxx/X3/X3/res/shaders/PathTracing.comp:83-85`:

```glsl
layout (rgba32f, SET(0) binding = 0) uniform image2D rayTracingTexture;
layout (SET(0) binding = 1) uniform sampler2D skyboxTexture;
```

`skyboxTexture` is statically sampled (ENGINE_PLAN.md §1 records it at `PathTracing.comp:208`, with identical lines in `PBR.comp`/`Phong.comp`).

`updateDescriptorSets` writes binding 1 only if the sampled-image registry has an entry (`VulkanComputeShader.cpp:227-238`). That registry is populated exclusively by `VulkanContext::registerSampledImage`, called exclusively from `VulkanTexture2D::ChangeTextureUnit` (`VulkanTexture2D.cpp:36-43`).

**`ChangeTextureUnit` has no callers.** An exhaustive grep across `X3/`, `X3-Editor/`, `X3-Runtime/` finds only the interface declaration (`/home/sarah/Coding/Haptixxx/X3/X3/src/Renderer/ITexture2D.h:12`) and the two implementations. `Renderer.cpp:244` passes `SKYBOX_TEXTURE_UNIT` as a *constructor argument*:

```cpp
					m_SkyboxTexture = ITexture2D::Create(data, metadata->width, metadata->height, SKYBOX_TEXTURE_UNIT);
```

and `VulkanTexture2D`'s constructor (`VulkanTexture2D.cpp:8-17`) stores `m_TextureUnit` but never registers. Contrast `VulkanImage2D`, which works only because `Renderer.cpp:209` explicitly calls `ChangeImageUnit(0)` each frame.

Same class of bug for lights: `Renderer.cpp:289-302` guards the entire light SSBO block with `if (count > 0)`, so on a scene with no lights, `set 2 binding 6` (`VulkanComputeShader.cpp:40`, `PathTracing.comp:127`) is never registered and never written.

**(b) Why it is wrong.** `VUID-vkCmdDispatch-None-08114` (and the older `-02697`/`-02699` family): descriptors that are statically used by the bound pipeline's shaders must have been written before the command executes. Reading an unwritten descriptor is undefined behaviour — in practice, a sample from an arbitrary image handle, a device fault, or a GPU hang. The validation message is *"VkDescriptorSet … encountered the following validation error at vkCmdDispatch time: Descriptor in binding #1 index 0 is being used in draw but has never been updated."*

**(c) Fix.**

1. **Immediate:** in `Renderer::SetupGPUResources`, after `Renderer.cpp:244`, call `m_SkyboxTexture->ChangeTextureUnit(SKYBOX_TEXTURE_UNIT);` — mirroring what line 209 does for the image. Do this even though 1b deletes `ChangeTextureUnit`, so that the descriptor is correct in the interim.
2. **Both cases, robustly:** create a 1×1 default white texture and a 16-byte dummy SSBO in `Renderer::Init()` (`Renderer.cpp:15-27`) and bind them whenever the real resource is absent, so no declared binding is ever unwritten. This is cheaper and far more debuggable than making the shader branch on a "has skybox" uniform, and it is the pattern Phase 1b's explicit descriptor-set membership should formalize.
3. **Structural (Phase 1b):** `VulkanComputeShader::updateDescriptorSets`'s `else` branch at `VulkanComputeShader.cpp:275-277` currently silently skips (the warn is commented out). Make it a hard error in debug builds: a declared binding with no resource is a bug, not a condition.

**(d) Verification.** Standard validation, immediate. Open a project with no skybox assigned and one with a skybox; both must be silent. Delete all lights from a scene; must be silent. Pre-fix, all three cases produce "never been updated" errors at the first dispatch.

---

## A4 — The acquire semaphore is waited at `COLOR_ATTACHMENT_OUTPUT`, but the swapchain image is written at the `TRANSFER` stage

**(a) Current behaviour.** `VulkanContext::endFrame()` (`VulkanContext.cpp:416-420`):

```cpp
	VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[m_CurrentSemaphoreIndex] };
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;
```

But in the runtime path, the first thing done to the acquired swapchain image is `vkCmdClearColorImage` (`VulkanContext.cpp:887-888`) followed by `vkCmdBlitImage` (`VulkanContext.cpp:908-911`) — both `VK_PIPELINE_STAGE_TRANSFER_BIT` operations. The barrier that precedes them (`VulkanContext.cpp:859-877`) uses `srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT` and `srcAccessMask = 0`.

**(b) Why it is wrong.** `pWaitDstStageMask` defines the *second* synchronization scope of the semaphore wait: only the listed stages, and stages later in the pipeline, are blocked until the semaphore signals. `VK_PIPELINE_STAGE_TRANSFER_BIT` is **earlier** than `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT` in the logical pipeline order (spec §7.1, the graphics pipeline stage order places `TRANSFER` outside the graphics ordering but the rule is that only the specified stages and *logically later* ones wait). The transfer commands are therefore free to execute before `vkAcquireNextImageKHR` has actually made the image available — i.e. while the presentation engine may still be reading it.

Compounding it: the barrier at `VulkanContext.cpp:874-877` uses `srcStageMask = TOP_OF_PIPE`, which forms **no** source dependency at all (a `TOP_OF_PIPE` source scope is empty by definition), and `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED` (line 861) which discards contents. So the render pass's `loadOp = CLEAR` colour-attachment write from `beginFrame` (`VulkanContext.cpp:180`, executed at `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT`) has no ordering against the subsequent `vkCmdClearColorImage` / `vkCmdBlitImage`. That is a write-after-write hazard. The single subpass dependency created at `VulkanContext.cpp:196-202` is `EXTERNAL → 0` only; the *implicit* `0 → EXTERNAL` dependency the spec inserts has `dstStageMask = BOTTOM_OF_PIPE` and `dstAccessMask = 0`, which makes writes available but not visible and does not chain to a `TOP_OF_PIPE`-sourced barrier.

**(c) Fix.**

1. `VulkanContext.cpp:417` → `VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };`
   (Both, because the editor path writes the image as a colour attachment and the runtime path as a transfer destination. `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` also works and is what most engines use; the explicit pair documents intent better.)
2. `VulkanContext.cpp:874-877` → `srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT`, and `dstBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` (line 871). Keep `oldLayout = UNDEFINED` — discarding is correct and desirable here — but the execution dependency must be real.
3. A2's fix (stop opening the main render pass in `beginFrame`) removes the WAW source entirely and is the better long-term answer; item 2 above is the belt-and-braces version for whichever lands first.

**(d) Verification.** Synchronization validation, submit-time enabled (see §9). Pre-fix, expect `SYNC-HAZARD-WRITE-AFTER-WRITE` on the swapchain image at `vkCmdClearColorImage`, and — with submit-time validation — a `SYNC-HAZARD-WRITE-AFTER-PRESENT`-class report on the acquire. Post-fix, silence. Also run the runtime executable and resize aggressively; pre-fix this is where tearing/corruption on the first frame after a resize comes from.

---

## A5 — Barrier `dstStageMask`s target `FRAGMENT_SHADER` for resources consumed by the **compute** stage

**(a) Current behaviour.** Three places.

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanTexture2D.cpp:203-207`:

```cpp
	} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
```

`VulkanTexture2D` is used for exactly one thing — the skybox (`Renderer.cpp:244`) — and it is sampled by `skyboxTexture` in the **compute** shader (`PathTracing.comp:85`, sampled at line 208). `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` does not cover compute-stage reads.

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanImage2D.cpp:207-210` and `:228-230`: `transitionToGeneral` treats `SHADER_READ_ONLY_OPTIMAL` as a fragment-stage source, and `transitionToShaderRead` targets `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`. `transitionToShaderRead` currently has no callers, but the layout it produces would be wrong for the compute consumer.

`/home/sarah/Coding/Haptixxx/X3/X3/src/Platform/Vulkan/VulkanComputeShader.cpp:146-154`: the post-dispatch barrier's `dstStageMask` is `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` only, with the comment *"dst: fragment shader will read"*. In the editor that is right (ImGui samples the image, `ViewportPanel.cpp:93-97`), but in the runtime the next consumer is `vkCmdBlitImage` at `VK_PIPELINE_STAGE_TRANSFER_BIT`. That case happens to be re-covered by the separate barrier at `VulkanContext.cpp:850-856`, so it is not currently a live bug — but it is one barrier away from becoming one.

**(b) Why it is wrong.** A memory dependency's *visibility* operation makes writes visible only to the access types in `dstAccessMask` at the stages in `dstStageMask`. A read issued from `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` is not in that scope, so the transfer write is not guaranteed visible to it. This is a read-after-write hazard, not a layout error — the layout transition itself is fine.

For `VulkanTexture2D` there is a mitigating factor today: `endSingleTimeCommands` does `vkQueueWaitIdle` (`VulkanContext.cpp:530`), and the queue idling drains the write. **Fix 6 removes that**, at which point this becomes a live hazard on every skybox load. Land A5 in the same commit as fix 6.

**(c) Fix.**

- `VulkanTexture2D.cpp:207` → `destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;`
- `VulkanImage2D.cpp:209` → `srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;`
- `VulkanImage2D.cpp:230` → `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`
- `VulkanComputeShader.cpp:149` → `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT`, and add `VK_ACCESS_TRANSFER_READ_BIT` to `memoryBarrier.dstAccessMask` at line 144.
- The generic barrier in the upload path (§6's `endUploadRecording`) already uses the wide mask; keep it.

Longer term, Phase 5's render graph derives these from declared usage. Until then, prefer erring wide: the cost of an over-broad `dstStageMask` on a once-per-load barrier is nil.

**(d) Verification.** Synchronization validation. Pre-fix (and after fix 6 lands), loading a scene with a skybox produces `SYNC-HAZARD-READ-AFTER-WRITE` at the dispatch, citing the skybox image and a prior `vkCmdCopyBufferToImage`. Post-fix, silence. Cross-check with a RenderDoc capture that the skybox actually appears in the traced image rather than showing as black — a visible-but-wrong result is the tell that the layout was right and only visibility was missing.

---

## A6 — Zero-sized buffer creation when a scene has no meshes/entities, and the failure leaves a dangling descriptor

**(a) Current behaviour.** `Renderer.cpp:255-262`:

```cpp
			uint32_t count = pScene->MeshEntityLookupTable.size();
			uint32_t sizeBytes = sizeof(MeshEntityHandle) * count;
			if (count != m_Cache.entityLookupSize || !m_MeshEntityLookupSSBO) {
				m_MeshEntityLookupSSBO = IShaderStorageBuffer::Create(sizeBytes, 0, BufferUsageType::DYNAMIC_DRAW);
```

With an empty scene, `count == 0` and `sizeBytes == 0`. `VulkanShaderStorageBuffer`'s constructor passes it straight to `VkBufferCreateInfo::size` (`VulkanShaderStorageBuffer.cpp:19`). Same at `Renderer.cpp:268`, `:280`, `:317`, `:331`, `:345`.

On failure, `VulkanShaderStorageBuffer.cpp:32-33` logs and returns with `m_Buffer == VK_NULL_HANDLE`. `Bind()` then guards on that (`VulkanShaderStorageBuffer.cpp:55`) and skips registration — **so the registry keeps the previous, now-destroyed buffer's handle**, which `updateDescriptorSets` writes into the live descriptor (`VulkanComputeShader.cpp:258`).

**(b) Why it is wrong.** `VUID-VkBufferCreateInfo-size-00912`: *"size must be greater than 0."* And the silent-failure path produces the dangling-handle case described in A1.

**(c) Fix.**

1. In `VulkanShaderStorageBuffer`'s and `VulkanUniformBuffer`'s constructors, clamp: `bufferInfo.size = std::max<VkDeviceSize>(m_SliceStride * m_FrameCount, 16);` and record `m_Size = std::max(size, 16u)` so the descriptor `range` is also legal (`VUID-VkDescriptorBufferInfo-range-00341` requires `range > 0`).
2. Make construction failure fatal rather than silent — throw, as `VulkanComputeShader` does (`VulkanComputeShader.cpp:175`), instead of logging and returning a half-constructed object.
3. Add the unregister-on-destroy from A1.

**(d) Verification.** Open a project with an empty scene, or delete every mesh entity at runtime. Pre-fix: `VUID-VkBufferCreateInfo-size-00912` followed by descriptor errors. Post-fix: silence, and the viewport shows the skybox with no geometry rather than corrupting.

---

## A7 — Swapchain images lack `VK_IMAGE_USAGE_TRANSFER_DST_BIT`, but the runtime path clears and blits into them

**(a) Current behaviour.** `VulkanContext::createSwapchain()` (`VulkanContext.cpp:149-173`) never calls `set_image_usage_flags` or `add_image_usage_flags`. vk-bootstrap's default is `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` alone — `/home/sarah/Coding/Haptixxx/X3/X3/libs/vk-bootstrap/src/VkBootstrap.h:1002`:

```cpp
        VkImageUsageFlags image_usage_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
```

and that value is copied verbatim into `VkSwapchainCreateInfoKHR::imageUsage` at `VkBootstrap.cpp:1966`.

`VulkanContext::blitImageToSwapchain` — the runtime's only presentation path, called from `/home/sarah/Coding/Haptixxx/X3/X3-Runtime/src/RuntimeLayer.cpp:183-190` — does:

```cpp
	vkCmdClearColorImage(cmd, m_SwapchainImages[m_ImageIndex],
	                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);   // :887
	...
	vkCmdBlitImage(cmd, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		m_SwapchainImages[m_ImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blitRegion, VK_FILTER_LINEAR);                                                      // :908
```

and transitions the image to `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` at `VulkanContext.cpp:862`.

**(b) Why it is wrong.** Three hard VUIDs, all violated on every runtime frame:

- `VUID-vkCmdBlitImage-dstImage-00224`: *"dstImage must have been created with `VK_IMAGE_USAGE_TRANSFER_DST_BIT` usage flag."*
- `VUID-vkCmdClearColorImage-image-00002`: same requirement for the clear.
- `VUID-VkImageMemoryBarrier-oldLayout-01213` family: `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` requires the image to have been created with `TRANSFER_DST` usage.

Additionally, `VkSurfaceCapabilitiesKHR::supportedUsageFlags` must advertise `TRANSFER_DST` for the surface — vk-bootstrap already checks the requested flags against it and returns `SwapchainError::required_usage_not_supported` (`VkBootstrap.cpp:1946-1948`), so requesting it is self-validating. It is universally supported on desktop; MoltenVK supports it.

There is also a missing format check: `vkCmdBlitImage` requires `VK_FORMAT_FEATURE_BLIT_SRC_BIT` on the source format (`VK_FORMAT_R32G32B32A32_SFLOAT`, `VulkanImage2D.cpp:74`) and `VK_FORMAT_FEATURE_BLIT_DST_BIT` plus `..._SAMPLED_IMAGE_FILTER_LINEAR_BIT` (because `VK_FILTER_LINEAR` is used, `VulkanContext.cpp:911`) on the swapchain format. Nothing queries `vkGetPhysicalDeviceFormatProperties`.

**(c) Fix.**

1. Add `swapchain_builder.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);` in `createSwapchain()` — already folded into the fix-5 snippet in §5(c).
2. At device selection time (`VulkanContext::pickPhysicalDevice`, `VulkanContext.cpp:74-93`), after the swapchain format is known, query `vkGetPhysicalDeviceFormatProperties` for both `VK_FORMAT_R32G32B32A32_SFLOAT` and `m_SwapchainImageFormat` and log a critical error if `BLIT_SRC` / `BLIT_DST` / `SAMPLED_IMAGE_FILTER_LINEAR` are missing. Fall back to `VK_FILTER_NEAREST` if linear filtering is unsupported for the destination.
3. If `build()` now fails with `required_usage_not_supported`, that is a genuine platform limitation and the blit path must be replaced with a fullscreen-triangle draw. Log it as such rather than crashing opaquely.

**(d) Verification.** Standard validation, running the **runtime** executable (not the editor — the editor never calls `blitImageToSwapchain`). Pre-fix the three VUIDs fire every frame. Post-fix, silence, plus a startup log line reporting the swapchain image usage flags actually granted. Also confirm the runtime still presents an image — a swapchain that fails to build now fails loudly instead of producing a validation-error-per-frame that happens to work on your driver.

---

## A8 — The overlay render pass's `loadOp = LOAD` read is never made visible

**(a) Current behaviour.** `m_OverlayRenderPass` (`VulkanContext.cpp:220-245`) uses:

```cpp
	overlayAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // Preserve existing content   // :224
	overlayAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;            // :228
	...
	overlayRenderPassInfo.pDependencies = &dependency;  // Same dependency                 // :238
```

reusing the `dependency` declared at `VulkanContext.cpp:196-202`, whose `dstAccessMask` is `VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` only.

The barrier that precedes it in `beginOverlayRenderPass` (`VulkanContext.cpp:603-621`) has `srcStageMask = COLOR_ATTACHMENT_OUTPUT`, `srcAccessMask = COLOR_ATTACHMENT_WRITE`, `dstAccessMask = COLOR_ATTACHMENT_WRITE`.

**(b) Why it is wrong — two separate defects.**

1. **Missing read access.** `VK_ATTACHMENT_LOAD_OP_LOAD` performs a *read* of the attachment, with `VK_ACCESS_COLOR_ATTACHMENT_READ_BIT` at `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT` (spec §8.4, *Load Operations*). Neither the subpass dependency's `dstAccessMask` (`VulkanContext.cpp:202`) nor the barrier's `dstAccessMask` (`VulkanContext.cpp:616`) includes it, so the prior write is never made visible to the load. Read-after-write hazard on every editor frame.

2. **Wrong source scope in the runtime-adjacent path.** The barrier at `VulkanContext.cpp:615,619` declares `srcAccessMask = COLOR_ATTACHMENT_WRITE` / `srcStageMask = COLOR_ATTACHMENT_OUTPUT`, and `oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` (line 605). Whenever `blitImageToSwapchain` ran first, the actual producer was a `vkCmdBlitImage` at `VK_PIPELINE_STAGE_TRANSFER_BIT` with `VK_ACCESS_TRANSFER_WRITE_BIT` (`VulkanContext.cpp:908-911`, left in `PRESENT_SRC_KHR` by the barrier at `:914-932`). The declared source scope therefore contains none of the actual writes; the barrier forms no dependency with them. (The comment at `VulkanContext.cpp:601` — *"After main render pass ends, image is in PRESENT_SRC_KHR"* — only describes the editor case.)

**(c) Fix.**

1. Give the overlay pass its own dependency rather than sharing `dependency` at `VulkanContext.cpp:238`:
   ```cpp
   VkSubpassDependency overlayDependency{};
   overlayDependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
   overlayDependency.dstSubpass    = 0;
   overlayDependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                   | VK_PIPELINE_STAGE_TRANSFER_BIT;
   overlayDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                   | VK_ACCESS_TRANSFER_WRITE_BIT;
   overlayDependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   overlayDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                   | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
   ```
2. `VulkanContext.cpp:615-620` → `srcAccessMask = COLOR_ATTACHMENT_WRITE | TRANSFER_WRITE`, `dstAccessMask = COLOR_ATTACHMENT_WRITE | COLOR_ATTACHMENT_READ`, `srcStageMask = COLOR_ATTACHMENT_OUTPUT | TRANSFER`.
3. Better still: after A2's restructure, track the swapchain image's current layout and last-writer stage/access in a small `struct SwapchainImageState { VkImageLayout layout; VkPipelineStageFlags lastWriteStage; VkAccessFlags lastWriteAccess; }` per image, updated by whoever writes it, and have `beginOverlayRenderPass` derive the barrier from it rather than guessing. This is the "resource state tracking" the reversal note in ENGINE_PLAN.md §0 explicitly lists as belonging in this layer.

Also fix the identical missing-read-access in the dead `beginRenderPass()` (`VulkanContext.cpp:556-557`) if that function survives; it should be deleted per A2(c)(4).

**(d) Verification.** Synchronization validation. Pre-fix, expect `SYNC-HAZARD-READ-AFTER-WRITE` reported against the swapchain image at `vkCmdBeginRenderPass` (syncval attributes load-op hazards to the `vkCmdBeginRenderPass` call). Post-fix, silence. Visually, the pre-fix symptom is intermittent flicker of the viewport content behind the ImGui chrome under GPU load — hard to reproduce on demand, which is exactly why syncval is the verification of record here.

---

## A9 — Swapchain recreation: semaphores destroyed while presents may still reference them; `renderFinished` indexed by the wrong counter; format changes ignored

**(a) Current behaviour.** `VulkanContext::recreateSwapchain()` (`VulkanContext.cpp:663-717`):

```cpp
	vkDeviceWaitIdle(m_Device);                                        // :665
	...
	cleanupSwapchain();                                                // :678
	createSwapchain();                                                 // :681
	createFramebuffers();                                              // :682
	for (auto semaphore : m_ImageAvailableSemaphores) {
		vkDestroySemaphore(m_Device, semaphore, nullptr);              // :687
	}
	for (auto semaphore : m_RenderFinishedSemaphores) {
		vkDestroySemaphore(m_Device, semaphore, nullptr);              // :690
	}
```

Semaphores are indexed by `m_CurrentSemaphoreIndex` (`VulkanContext.h:154`), a free-running counter advanced modulo the swapchain image count at `VulkanContext.cpp:457`, decoupled from `m_ImageIndex`.

**(b) Why it is wrong — three distinct defects.**

1. **Semaphore destroyed with a present outstanding.** `VUID-vkDestroySemaphore-semaphore-01137`: *"All submitted batches that refer to semaphore must have completed execution."* `vkDeviceWaitIdle` waits for **queue** operations. `vkQueuePresentKHR` (`VulkanContext.cpp:446`) hands the wait on `m_RenderFinishedSemaphores[...]` to the presentation engine, which is not a queue operation and is explicitly not covered by device-idle. Because `endFrame` calls `recreateSwapchain` at line 450 *immediately after* the present that returned `SUBOPTIMAL`/`OUT_OF_DATE`, this is the highest-probability moment for the violation. This is the classic "validation error on window resize" and it is present here.

2. **`renderFinished` should be indexed by acquired image index, not a free-running counter.** The canonical safe pairing is: `imageAvailable` indexed by frame-in-flight slot, `renderFinished` indexed by `m_ImageIndex`. Here both use `m_CurrentSemaphoreIndex`, which advances independently of which image `vkAcquireNextImageKHR` actually returned (`VulkanContext.cpp:352`). Acquire is not required to return images in round-robin order. When it doesn't, two outstanding presents can end up waiting on the same `VkSemaphore`, which is a signal/wait pairing violation.

3. **Format and image-count changes are not propagated.** `createSwapchain()` re-derives `m_SwapchainImageFormat` from the new swapchain (`VulkanContext.cpp:164`), but `m_RenderPass` and `m_OverlayRenderPass` — created once in `init()` (`VulkanContext.cpp:31`) with `colorAttachment.format = m_SwapchainImageFormat` (`VulkanContext.cpp:178`, `:222`) — are never recreated. `createFramebuffers()` at line 682 then builds framebuffers whose image-view format may not match the render pass attachment format (`VUID-VkFramebufferCreateInfo-pAttachments-00880`). Rare in practice (format changes on HDR toggle or monitor switch), but it is a silent corruption path when it happens.

Two smaller notes in the same function: `m_CurrentSemaphoreIndex = 0` at line 711 is immediately overwritten to 1 by line 457 when the recreate is reached from `endFrame`, so index 0 is skipped for one cycle (cosmetic). And `createSwapchain()`'s `.set_old_swapchain(m_Swapchain)` at line 154 is always passed `VK_NULL_HANDLE`, because `cleanupSwapchain()` (line 678) already destroyed the swapchain and nulled the handle at `VulkanContext.cpp:735` — so the old-swapchain retirement path never engages, costing a full teardown and a visible black flash on every resize.

**(c) Fix.**

1. Resize semaphore arrays and re-index:
   - `m_ImageAvailableSemaphores` → size `MAX_FRAMES_IN_FLIGHT`, indexed by `m_CurrentFrame`. Delete `m_CurrentSemaphoreIndex` (`VulkanContext.h:154`, `VulkanContext.cpp:457`, `:711`).
   - `m_RenderFinishedSemaphores` → size `m_SwapchainImages.size()`, indexed by `m_ImageIndex` at both the signal (`VulkanContext.cpp:425`) and the present wait (`VulkanContext.cpp:438`).
2. Before destroying any semaphore in `recreateSwapchain`, ensure outstanding presents are retired. The pragmatic, portable approach: keep the old semaphores alive by pushing them onto the §0.2 deletion queue instead of destroying them inline, and only recycle them once `MAX_FRAMES_IN_FLIGHT` further frames have completed. (`VK_EXT_swapchain_maintenance1`'s `VkSwapchainPresentFenceInfoEXT` is the exact tool for this, but it is not universally available and definitely not on MoltenVK; do not depend on it.)
3. Reorder so the old swapchain is retired properly: call `createSwapchain()` *before* `cleanupSwapchain()`, passing the live old handle to `set_old_swapchain`, then destroy the old one. This removes the black flash.
4. After `createSwapchain()`, compare the new `m_SwapchainImageFormat` against the format the render passes were built with; if it differs, destroy and recreate both render passes before `createFramebuffers()`.
5. Delete the `swapBuffers` retry loop (`VulkanContext.cpp:651-660`) as part of A2's restructure — a failed `beginFrame` should return to the caller, not recurse.
6. Fix the fence-leak deadlock: `beginFrame` resets the fence at `VulkanContext.cpp:364` but can still return `false` at line 377 if `vkBeginCommandBuffer` fails, leaving `m_InFlightFences[m_CurrentFrame]` unsignalled with nothing pending — the next `vkWaitForFences` at line 346 then blocks forever with `UINT64_MAX`. Either move `vkResetFences` to immediately before `vkQueueSubmit` in `endFrame`, or re-signal on the failure path.

**(d) Verification.** Standard validation. Grab the window edge and resize continuously for ~10 seconds, then minimize and restore (which exercises the `width == 0` spin at `VulkanContext.cpp:670-673`), then move the window between monitors of different refresh rates. Expect zero `VUID-vkDestroySemaphore-semaphore-01137`, zero `VUID-VkPresentInfoKHR-pWaitSemaphores-*`, zero `VUID-VkFramebufferCreateInfo-pAttachments-00880`. Visually, the black flash on resize should be gone after (3). Under a 2-hour soak with continuous resizing, memory should be flat — a leaked semaphore per resize is otherwise visible in `vkconfig`'s object tracker.

---

## A10 — Minor, worth fixing while adjacent

| # | Finding | Location | Action |
|---|---|---|---|
| A10.1 | `VulkanRendererAPI::SetViewportSize` records `vkCmdSetViewport`/`vkCmdSetScissor` into `getCurrentCommandBuffer()`, which may not be recording, and no pipeline in the engine has dynamic viewport state. It has **zero callers** (only the `IRendererAPI` declaration at `/home/sarah/Coding/Haptixxx/X3/X3/src/Renderer/IRendererAPI.h:20`). | `VulkanRendererAPI.cpp:19-50` | Delete. Reintroduce in Phase 7 when a graphics pipeline exists. |
| A10.2 | `VulkanContext::beginRenderPass()` has zero callers and contains the same missing-`COLOR_ATTACHMENT_READ` bug as A8, plus a `TOP_OF_PIPE` source scope at line 560 that forms no dependency. | `VulkanContext.cpp:535-576`, `VulkanContext.h:72` | Delete. |
| A10.3 | `getMinImageCount()` hardcodes `2` with the comment *"Minimum for double buffering"*, while the actual swapchain gets `capabilities.minImageCount + 1` (typically 3-4). Its one consumer overwrites it two lines later. | `VulkanContext.h:87`; consumed at `/home/sarah/Coding/Haptixxx/X3/X3-Editor/src/ImGuiContext.cpp:173` then overwritten at `:178` | Delete the getter; have callers use `getSwapchainImageCount()`. |
| A10.4 | `VulkanImage2D::GetID()` returns `int` from a `static int s_NextID` counter, not a Vulkan handle — but `ViewportPanel::GetImGuiTextureID` uses it as an identity key for descriptor caching (`ViewportPanel.cpp:49-50`). Since `Renderer::SetupGPUResources` recreates both `m_Frames` on every resolution change (`Renderer.cpp:202-203`), IDs are monotonically increasing and the cache invalidates correctly — but the double-buffer swap at `Renderer.cpp:84-88` alternates between two *different* IDs every frame, forcing an `ImGui_ImplVulkan_RemoveTexture` + `AddTexture` pair per frame (`ViewportPanel.cpp:64-67`, `:93-97`) whenever `useDoubleBuffering` is on. | `VulkanImage2D.h:16`, `ViewportPanel.cpp:45-102` | Cache one ImGui descriptor per image rather than one globally. Note ENGINE_PLAN.md 1b already flags `GetID() → int` for removal (`VulkanTexture2D.h:16` truncating a 64-bit handle). |
| A10.5 | `VulkanImage2D::createImage` computes `size_t dataSize = m_Width * m_Height * 4;` at line 93 and never uses it; `m_Width * m_Height * 4` at line 119 is `int` arithmetic that overflows above ~23k×23k. | `VulkanImage2D.cpp:93`, `:119` | Delete the dead variable; use `size_t`. |
| A10.6 | The compute output image is written by the dispatch of frame `N` and sampled by ImGui in frame `N`, but frame `N+1`'s dispatch writes the *same* image (`useDoubleBuffering` defaults `false`, `RenderSettings.h:24`) with only the frame fence between them — and that fence gates frame `N-1`, not `N`. Write-after-read across submissions. This is what `useDoubleBuffering` exists to paper over, and it is off by default. | `Renderer.cpp:67-90`, `Renderer.cpp:201-209` | Make the frame images per-frame-in-flight (`MAX_FRAMES_IN_FLIGHT` of them, indexed by `getCurrentFrame()`), which subsumes `useDoubleBuffering` and lets that setting be deleted. Verify with syncval submit-time validation: pre-fix expect `SYNC-HAZARD-WRITE-AFTER-READ` on the storage image between submissions. |

---

# 8. Implementation order

Dependencies are real; this order is not negotiable at the marked points.

1. **§0.1** frame counter + invariant documented and asserted.
2. **§0.2** deferred deletion queue, all destructors routed through it. — *Must precede step 8.*
3. **A1** unregister-on-destroy for the binding registry.
4. **A6** zero-size clamp + fail-loud construction.
5. **§4** validation gating, custom debug callback, syncval + best-practices features enabled. — *Do this early: it is how you verify everything after it.*
6. **A2** restructure the frame loop; remove the main render pass; move compute outside any render pass; delete `m_FirstFrame` / `ensureFrameStarted` / `beginRenderPass`. — *Largest single change; do it before the barrier work so barriers are written once against the final structure.*
7. **A4, A5, A8** stage/access mask corrections and the acquire-wait stage.
8. **§6** batched uploads; delete `beginSingleTimeCommands`/`endSingleTimeCommands`. — *Requires 2 and 7.*
9. **§1** per-frame descriptor sets + dedicated pool.
10. **§2** per-frame buffer rings + `vmaFlushAllocation` + registry offsets. — *Requires 9 (the offsets are per-frame-set).*
11. **A3** skybox/light descriptor writes + default fallback resources.
12. **§3** delete `ReadData`.
13. **A7 + §5** swapchain usage flags and present-mode plumbing (one change to `createSwapchain`).
14. **A9** semaphore re-indexing, recreation ordering, format check, fence-leak fix.
15. **A10.6** per-frame compute output images; delete `useDoubleBuffering`.
16. **A10.1-A10.5** cleanup.

Steps 1-7 are one logical commit series ("make the frame legal"); 8-12 are "make the frame's data correct"; 13-16 are "make presentation and lifetimes correct".

---

# 9. Verification procedure (run once the build works)

**Enabling synchronization validation.** Programmatic (preferred, since §4 already wires `add_validation_feature_enable`) plus environment overrides. Confirm the exact spelling against the installed SDK version, as the layer's settings interface changed around SDK 1.3.275:

```bash
# Modern layer settings (SDK >= 1.3.275)
export VK_LAYER_KHRONOS_VALIDATION_VALIDATE_SYNC=1
export VK_LAYER_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION=1   # REQUIRED for cross-submission hazards
export VK_LAYER_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES=1
export VK_LAYER_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn,perf

# Older mechanism, still accepted
export VK_LAYER_KHRONOS_VALIDATION_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT
```

or configure it once through `vkconfig` (Vulkan Configurator) with the "Synchronization" preset, which survives across runs and does not need env vars.

**Submit-time validation is not optional here.** Most of the hazards in this document (§1, §2, A1, A4, A10.6) are *cross-submission* — they involve frame `N` versus frame `N-1`, which are different `vkQueueSubmit` calls. Command-buffer-time syncval alone will not see them.

**The exercise script.** Run both `X3Editor` and `X3Runtime`. In each:

1. Cold start; capture the first 5 seconds of log.
2. Open a project with meshes, lights and a skybox; open one with none of those.
3. Add and delete mesh entities at runtime (exercises A1, A6).
4. Change render resolution repeatedly (exercises §6, A1, A10.6).
5. Switch shader type PathTracing ↔ PBR ↔ Phong (exercises §1 across three shader objects).
6. Toggle `accumulate` and `useDoubleBuffering`.
7. Toggle vSync; confirm the logged present mode changes and the frame rate clamps (§5).
8. Continuous window resize for 10 s; minimize and restore; move between monitors (A9).
9. Enter and leave play mode.
10. Close cleanly.

**Pass criteria** (this is the concrete form of ENGINE_PLAN.md's "validation layers clean under load, no synchronization warnings, resize and swapchain recreation stable"):

- Zero messages with `VUID-` in the log.
- Zero messages with `SYNC-HAZARD-` in the log.
- Best-practices warnings reviewed and either fixed or explicitly waived in a comment.
- The object tracker reports zero leaked objects at `vkDestroyInstance`.
- Memory footprint flat across step 8's 10-second resize soak.

**RenderDoc spot checks** (validation cannot confirm these):

- One submit per frame, containing the upload command buffer first and the frame command buffer second (§6).
- The compute dispatch appears at the top level, not nested inside a render pass (A2).
- Descriptor set handles for the dispatch differ between two consecutive captures (§1).
- The buffer descriptors' `offset` fields alternate between 0 and `m_SliceStride` across consecutive captures (§2).
- Swapchain image usage flags include `TRANSFER_DST` (A7).

**What cannot be verified by tooling** and must be argued from the code: §2's host-write race (syncval does not model host writes to mapped memory). Its correctness rests entirely on the §0.1 invariant. Assert the invariant, comment it, and re-check it whenever the main loop in `application.cpp:41-71` is touched — including in Phase 4, when the job system starts moving `Renderer::SetupGPUResources` off the main thread.