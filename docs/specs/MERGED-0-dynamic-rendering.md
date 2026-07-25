## SECTION: DYNAMIC RENDERING MIGRATION

Everything below was verified against the working tree at `/home/sarah/Coding/Haptixxx/X3`. Verification method is stated per claim. `cmake --preset vulkan-debug && cmake --build build/vulkan-debug -j 14` was run: 0 occurrences of `error:` in the log, `build/vulkan-debug/Debug/X3Editor` (39 MB) produced.

---

### 0. Why this is not just a refactor: two bugs it deletes

**(a) `vkCmdDispatch` is currently recorded inside an active render pass.** `VulkanContext::beginFrame()` opens `m_RenderPass` at `X3/src/Platform/Vulkan/VulkanContext.cpp:392` and leaves it open for the whole frame (`swapBuffers` at `:639-661` ends the frame then immediately begins the next one, render pass and all). `VulkanComputeShader::Dispatch()` then records into `context->getCurrentCommandBuffer()` (`X3/src/Platform/Vulkan/VulkanComputeShader.cpp:106`) and issues `vkCmdDispatch` at `:137` plus a `COMPUTE_SHADER → FRAGMENT_SHADER` `vkCmdPipelineBarrier` at `:146-153`. Both are illegal inside a render pass instance (VUID-vkCmdDispatch-renderpass; a barrier inside a render pass requires a matching self-dependency, which `createRenderPass` does not declare — its only dependency is `EXTERNAL → 0` at `:196-202`). In the new shape `beginFrame()` opens **no** rendering block, so compute is recorded at top level and this dissolves.

**(b) The editor runs two render passes per frame to draw one ImGui layer.** `beginFrame` opens `m_RenderPass` (LOAD_OP_CLEAR, finalLayout `PRESENT_SRC_KHR`); `ImGuiContext::EndFrame` at `X3-Editor/src/ImGuiContext.cpp:283` calls `beginOverlayRenderPass()`, which *ends* that pass, barriers `PRESENT_SRC_KHR → COLOR_ATTACHMENT_OPTIMAL` (`VulkanContext.cpp:603-621`), and opens `m_OverlayRenderPass` (LOAD_OP_LOAD). Verified the editor never blits: `blitImageToSwapchain` has exactly one caller repo-wide, `X3-Runtime/src/RuntimeLayer.cpp:183`. The editor's compute output reaches the screen only as an ImGui-sampled texture (`X3-Editor/src/Panels/ViewportPanel/ViewportPanel.cpp:93-97`, `ImGui_ImplVulkan_AddTexture(..., VK_IMAGE_LAYOUT_GENERAL)`). One `vkCmdBeginRendering` with `LOAD_OP_CLEAR` replaces both passes.

---

### 1. Inventory of what dies

| Artefact | Location | Replacement |
|---|---|---|
| `VkRenderPass m_RenderPass` | `VulkanContext.h:132` | Deleted. No object. |
| `VkRenderPass m_OverlayRenderPass` | `VulkanContext.h:133` | Deleted. **The `m_RenderPass`-vs-`m_OverlayRenderPass` question is moot** — both go. `createFramebuffers` is deleted, not repointed. |
| `getRenderPass()` | `VulkanContext.h:32` | Deleted. Zero callers today (grep across `X3/src`, `X3-Editor/src`, `X3-Runtime/src`). |
| `getOverlayRenderPass()` | `VulkanContext.h:33` | Deleted. Callers: `ImGuiContext.cpp:171` and `:237` — both become `init_info.RenderPass = VK_NULL_HANDLE` + `UseDynamicRendering = true` (see §5). |
| `createRenderPass()` decl / def / call | `VulkanContext.h:101`, `.cpp:175-246`, called `.cpp:31` | Deleted entirely, including both `VkAttachmentDescription`s (`:177-185`, `:221-229`), the shared `VkSubpassDescription` (`:191-194`), the shared `VkSubpassDependency` (`:196-202`, reused at `:238`), and both `vkCreateRenderPass` calls (`:213`, `:240`). |
| `createFramebuffers()` decl / def / calls | `VulkanContext.h:102`, `.cpp:248-270`, called `.cpp:32` and `.cpp:682` | Deleted. `framebufferInfo.renderPass = m_RenderPass` at `:256` is the hard-coded coupling the critique flagged; it disappears with both. |
| `std::vector<VkFramebuffer> m_Framebuffers` | `VulkanContext.h:127` | Deleted. All uses die: `.cpp:249`, `:263`, `:269`, `:384`, `:568`, `:586-587`, `:628`, `:721-724`. Dynamic rendering consumes `m_SwapchainImageViews[m_ImageIndex]` directly. **Keep `m_SwapchainImageViews`** — it is now the only per-image object, and `cleanupSwapchain` (`.cpp:719-739`) keeps its view-destruction loop (`:727-730`) and loses its framebuffer loop (`:721-724`). |
| `beginRenderPass()` | `VulkanContext.h:72`, `.cpp:535-576` | Deleted. Zero callers (verified by grep; the critique's A10.2 said the same). |
| `beginOverlayRenderPass()` | `VulkanContext.h:73`, `.cpp:578-637` | Replaced by `beginSwapchainRendering()` / `endSwapchainRendering()` (§2a). Sole caller `ImGuiContext.cpp:283`. |
| `bool m_RenderPassActive` + `isRenderPassActive()` | `VulkanContext.h:167`, `:71` | Deleted. Uses: `.cpp:393`, `:400-403`, `:536`, `:575`, `:594-598`, `:635`, `:832-835`; and the assert at `ImGuiContext.cpp:290`. Replaced by `bool m_SwapchainImageWritten` (§3.4) which tracks *layout*, not pass state. |
| Render-pass begin block in `beginFrame` | `.cpp:380-393` (`VkRenderPassBeginInfo`, clear value, `vkCmdBeginRenderPass`) | Deleted outright. `beginFrame` ends after `vkBeginCommandBuffer` (`:375`). |
| Render-pass end block in `endFrame` | `.cpp:399-403` | Replaced by the "nothing was written" fallback (§3.4). |
| Render-pass end inside the blit | `.cpp:832-835` | Deleted. There is no pass to end. |
| Render-pass destruction | `.cpp:774-780` | Deleted. |
| `VulkanRendererAPI::Clear`'s comment "clearing is done at the start of the render pass" | `X3/src/Platform/Vulkan/VulkanRendererAPI.cpp:13-14` | Stale either way; the class is deleted by the resource-layer work. If it survives to this commit, the clear colour now feeds `VkRenderingAttachmentInfo::clearValue`. |
| `ensureFrameStarted()` / `m_FirstFrame` | `VulkanContext.h:64`, `:168`, `.cpp:492-500`, `:644-647`; caller `ImGuiContext.cpp:278` | Not strictly a render-pass artefact, but the `m_FirstFrame` hack exists only because `init()` couldn't open a render pass before ImGui's font upload (`.cpp:40-42`). With no render pass in `beginFrame` the hack has no purpose; delete it with the `beginFrame`/`endFrame`/`present` split. |

**Nothing else in the repo references a `VkRenderPass` or `VkFramebuffer`.** Verified: `grep -rn -e getRenderPass -e getOverlayRenderPass -e beginRenderPass -e beginOverlayRenderPass -e isRenderPassActive -e m_Framebuffers -e createFramebuffers -e createRenderPass --include='*.cpp' --include='*.h' X3/src X3-Editor/src X3-Runtime/src` returns only the sites tabulated above. `X3-Runtime` does not link ImGui at all (`X3-Runtime/CMakeLists.txt` has no imgui target and no source matches `ImGui`), so the runtime has no consumer of any render pass.

---

### 2. The new frame shape

`VulkanContext` gains exactly two new public methods and one private helper. Add to `VulkanContext.h` where `beginRenderPass`/`beginOverlayRenderPass` were (`:70-73`):

```cpp
// Dynamic rendering. Opens a single-color-attachment rendering block on the
// acquired swapchain image. Must be balanced by endSwapchainRendering().
// Records the UNDEFINED/PRESENT_SRC -> COLOR_ATTACHMENT_OPTIMAL barrier itself.
void beginSwapchainRendering(VkClearColorValue clear = {{0.0f, 0.0f, 0.0f, 1.0f}});
void endSwapchainRendering();

// Format of the swapchain images, for VkPipelineRenderingCreateInfo.
VkFormat getSwapchainImageFormat() const { return m_SwapchainImageFormat; }
```

and privately:

```cpp
void transitionSwapchainImage(VkCommandBuffer cmd,
                              VkImageLayout oldLayout, VkImageLayout newLayout,
                              VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                              VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);
VkImageLayout m_SwapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
bool          m_SwapchainImageWritten = false;
```

`m_SwapchainImageLayout` is reset to `VK_IMAGE_LAYOUT_UNDEFINED` and `m_SwapchainImageWritten` to `false` at the top of every successful `beginFrame()` (immediately after `vkResetCommandBuffer`, `.cpp:367`). Swapchain images come back from `vkAcquireNextImageKHR` with undefined contents; treating the layout as `UNDEFINED` each frame is correct and is what lets the first barrier discard.

#### (a) Editor — ImGui overlay onto the swapchain image

```cpp
void VulkanContext::beginSwapchainRendering(VkClearColorValue clear) {
    VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

    // No render pass => no automatic layout transition. This barrier is mandatory.
    // srcStage MUST be COLOR_ATTACHMENT_OUTPUT (not TOP_OF_PIPE): the acquire
    // semaphore is waited at that stage, and the layout transition must be
    // ordered after the wait. TOP_OF_PIPE as a source stage orders nothing.
    transitionSwapchainImage(cmd,
        m_SwapchainImageLayout,                          // UNDEFINED on first write this frame
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,   0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    m_SwapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkRenderingAttachmentInfo color{};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = m_SwapchainImageViews[m_ImageIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.resolveMode = VK_RESOLVE_MODE_NONE;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;   // see §3.3: CLEAR, never LOAD
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = clear;

    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.offset    = {0, 0};
    info.renderArea.extent    = m_SwapchainExtent;
    info.layerCount           = 1;
    info.viewMask             = 0;      // no multiview
    info.colorAttachmentCount = 1;
    info.pColorAttachments    = &color;
    info.pDepthAttachment     = nullptr;
    info.pStencilAttachment   = nullptr;
    info.flags                = 0;      // NOT suspending/resuming (see §6, MoltenVK)

    vkCmdBeginRendering(cmd, &info);
    m_SwapchainImageWritten = true;
}

void VulkanContext::endSwapchainRendering() {
    vkCmdEndRendering(m_CommandBuffers[m_CurrentFrame]);
}
```

`ImGuiContext::EndFrame` (`X3-Editor/src/ImGuiContext.cpp:271-306`) becomes:

```cpp
VulkanContext* vkContext = VulkanContext::Get();
VkCommandBuffer cmd = vkContext->getCurrentCommandBuffer();
assert(cmd != VK_NULL_HANDLE && "Command buffer is null");
vkContext->beginSwapchainRendering();                       // replaces beginOverlayRenderPass()
ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
vkContext->endSwapchainRendering();
```

The `ensureFrameStarted()` call at `:278` and the `isRenderPassActive()` assert at `:290` are removed.

#### (b) Runtime — compute blit path: **no rendering block at all**

`vkCmdBlitImage` and `vkCmdClearColorImage` are transfer commands and must be recorded *outside* any rendering block. The runtime never draws geometry and has no ImGui. So the correct answer is: `blitImageToSwapchain` opens nothing, and `RuntimeLayer::onRender` (`X3-Runtime/src/RuntimeLayer.cpp:174-194`) is unchanged apart from the barriers inside the context.

`blitImageToSwapchain` (`VulkanContext.cpp:826-954`) changes as follows and only as follows:

- **Delete `:832-835`** (the `if (m_RenderPassActive) vkCmdEndRenderPass(...)` block).
- **`:858-877`, the destination barrier**: keep `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED` (correct — contents are discarded), but change `srcStageMask` from `VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT` (`:875`) to `VK_PIPELINE_STAGE_TRANSFER_BIT`, for the same reason as above. Set `m_SwapchainImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; m_SwapchainImageWritten = true;` after it.
- **`:913-932`, the present barrier**: keep as-is (`TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR`, `TRANSFER`/`TRANSFER_WRITE` → `BOTTOM_OF_PIPE`/`0`). Set `m_SwapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;`. Under render passes this transition duplicated `finalLayout`; now it is the only thing producing `PRESENT_SRC_KHR`.
- Everything else — the source barriers at `:838-856` and `:935-953`, the letterbox clear at `:879-888`, the blit region and its Y-flip packing at `:891-911` (note `dstOffsets` at `:905-906` and `CalculateViewportCoordinates()` at `RuntimeLayer.cpp:306-376`) — is untouched. **Do not change the Y-flip.**

---

### 3. Image layout and barrier timeline

Without render passes there are no `initialLayout`/`finalLayout`, no implicit `EXTERNAL` subpass dependencies, and no automatic `PRESENT_SRC_KHR` transition. Every transition is now an explicit `VkImageMemoryBarrier`. Barriers are given in synchronization-1 form to match the existing `blitImageToSwapchain` code; see §4 for why `synchronization2` should be *enabled* but not *used* in this phase.

The shared helper:

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
```

#### 3.1 Editor path — full per-frame timeline for the swapchain image

| # | Point in frame | Layout before → after | srcStage | srcAccess | dstStage | dstAccess |
|---|---|---|---|---|---|---|
| 0 | `vkAcquireNextImageKHR` (`.cpp:351`) signals `imageAvailable` | — (contents undefined) | — | — | — | — |
| 1 | *(no barrier)* compute dispatches recorded at top level; they touch `VulkanImage2D`, never the swapchain | unchanged | — | — | — | — |
| 2 | `beginSwapchainRendering()`, before `vkCmdBeginRendering` | `UNDEFINED` → `COLOR_ATTACHMENT_OPTIMAL` | `COLOR_ATTACHMENT_OUTPUT` | `0` | `COLOR_ATTACHMENT_OUTPUT` | `COLOR_ATTACHMENT_WRITE` |
| 3 | rendering block: `loadOp = CLEAR`, ImGui draws, `storeOp = STORE` | `COLOR_ATTACHMENT_OPTIMAL` | — | — | — | — |
| 4 | `endFrame()`, after `vkCmdEndRendering` | `COLOR_ATTACHMENT_OPTIMAL` → `PRESENT_SRC_KHR` | `COLOR_ATTACHMENT_OUTPUT` | `COLOR_ATTACHMENT_WRITE` | `BOTTOM_OF_PIPE` | `0` |
| 5 | `vkQueueSubmit`: `pWaitDstStageMask` | — | — | — | `COLOR_ATTACHMENT_OUTPUT \| TRANSFER` | — |
| 6 | `vkQueuePresentKHR` waits `renderFinished` | `PRESENT_SRC_KHR` | — | — | — | — |

#### 3.2 Runtime path — full per-frame timeline for the swapchain image

| # | Point in frame | Layout before → after | srcStage | srcAccess | dstStage | dstAccess |
|---|---|---|---|---|---|---|
| 0 | `vkAcquireNextImageKHR` signals `imageAvailable` | — | — | — | — | — |
| 1 | *(no barrier)* compute dispatch writes `VulkanImage2D` in `GENERAL` | unchanged | — | — | — | — |
| 2 | `blitImageToSwapchain`, source barrier (`.cpp:838-856`, unchanged) | source image `GENERAL` → `TRANSFER_SRC_OPTIMAL` | `COMPUTE_SHADER` | `SHADER_WRITE` | `TRANSFER` | `TRANSFER_READ` |
| 3 | `blitImageToSwapchain`, dest barrier (`.cpp:858-877`, **srcStage changed**) | `UNDEFINED` → `TRANSFER_DST_OPTIMAL` | **`TRANSFER`** (was `TOP_OF_PIPE` at `:875`) | `0` | `TRANSFER` | `TRANSFER_WRITE` |
| 4 | `vkCmdClearColorImage` (`.cpp:887`), `vkCmdBlitImage` (`.cpp:908`) | `TRANSFER_DST_OPTIMAL` | — | — | — | — |
| 5 | present barrier (`.cpp:914-932`, unchanged) | `TRANSFER_DST_OPTIMAL` → `PRESENT_SRC_KHR` | `TRANSFER` | `TRANSFER_WRITE` | `BOTTOM_OF_PIPE` | `0` |
| 6 | source restore barrier (`.cpp:935-953`, unchanged) | source `TRANSFER_SRC_OPTIMAL` → `GENERAL` | `TRANSFER` | `TRANSFER_READ` | `COMPUTE_SHADER` | `SHADER_READ \| SHADER_WRITE` |
| 7 | `vkQueueSubmit`: `pWaitDstStageMask` | — | — | — | `COLOR_ATTACHMENT_OUTPUT \| TRANSFER` | — |

#### 3.3 Folding in the critique's A4, A7, A8

**A4 — acquire semaphore waited at the wrong stage.** Confirmed at `VulkanContext.cpp:417`: `VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };`, while the runtime's first write to that image is `vkCmdClearColorImage` at `TRANSFER`. The semaphore wait therefore does not cover the runtime's write, and — now that the layout transition is explicit rather than implicit-in-render-pass — it also does not cover the *transition*, which is worse. **Fix**, in `endFrame()`, replacing `.cpp:417`:

```cpp
VkPipelineStageFlags waitStages[] = {
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
};
```

One mask covering both paths, because `endFrame` is shared and cannot know which ran. This is why the barriers in §3.1 step 2 and §3.2 step 3 use `COLOR_ATTACHMENT_OUTPUT` and `TRANSFER` as **source** stages rather than `TOP_OF_PIPE`: a `TOP_OF_PIPE` source stage creates no ordering, so a layout transition specified with it can legally be scheduled before the semaphore wait retires. The rule to state and follow: *the barrier's srcStageMask must be a stage included in `pWaitDstStageMask`.*

**A7 — swapchain images lack `TRANSFER_DST`.** See §7. Under render passes this was already a validation error; under dynamic rendering it is unchanged in severity but now the *only* thing standing between the runtime and a bad swapchain, since there is no `VkFramebuffer` creation to independently validate the view's usage.

**A8 — overlay pass `LOAD_OP_LOAD` without `COLOR_ATTACHMENT_READ`.** Confirmed: `VulkanContext.cpp:224` sets `loadOp = VK_ATTACHMENT_LOAD_OP_LOAD`, and neither the subpass dependency (`:202`, `dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` only, shared by both passes via `:238`) nor the barrier at `:616` (`dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` only) grants read access. **A8 is eliminated by construction, not fixed**: in the new shape there is exactly one rendering block per frame in the editor and it uses `LOAD_OP_CLEAR`, and the runtime has none. Nothing loads. State the standing rule anyway, because someone will add a debug HUD to the runtime: *if a `VkRenderingAttachmentInfo` ever uses `VK_ATTACHMENT_LOAD_OP_LOAD`, the barrier preceding `vkCmdBeginRendering` must include `VK_ACCESS_COLOR_ATTACHMENT_READ_BIT` in `dstAccessMask`, and the src side must name the stage/access that produced the content being loaded (for a preceding blit: `srcStage = TRANSFER`, `srcAccess = TRANSFER_WRITE`, `oldLayout = TRANSFER_DST_OPTIMAL`) — `oldLayout` may not be `UNDEFINED`.*

#### 3.4 The "nothing was written" case

This is the gap the critique flagged as severity-1 ("what happens to the acquired swapchain image when neither the overlay pass nor the runtime blit touched it"). It is reachable in the runtime whenever `m_CurrentFrame` is null (`RuntimeLayer.cpp:176`), and in the editor if ImGui rendering is ever skipped. Presenting an image in `VK_IMAGE_LAYOUT_UNDEFINED` is invalid.

Resolve it in `endFrame()`, replacing the deleted `.cpp:399-403`:

```cpp
VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

if (!m_SwapchainImageWritten) {
    // Nothing drew this frame. Open and immediately close an empty rendering
    // block with LOAD_OP_CLEAR: it defines the layout, clears to black, and
    // does NOT require VK_IMAGE_USAGE_TRANSFER_DST_BIT (unlike a
    // vkCmdClearColorImage). Cheapest correct option.
    beginSwapchainRendering();
    endSwapchainRendering();
}

if (m_SwapchainImageLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
    // Editor path lands here (COLOR_ATTACHMENT_OPTIMAL). The runtime path
    // already transitioned inside blitImageToSwapchain.
    transitionSwapchainImage(cmd,
        m_SwapchainImageLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          0);
    m_SwapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

vkEndCommandBuffer(cmd);
```

**Choose the empty rendering block, not `vkCmdClearColorImage`.** The clear-image route would make `VK_IMAGE_USAGE_TRANSFER_DST_BIT` a hard requirement for the editor too; the rendering block only needs `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, which vk-bootstrap always requests.

**Interaction with the semaphore rework.** These barriers assume `renderFinished` is indexed by `m_ImageIndex` and `imageAvailable` by `m_CurrentFrame`, with `m_CurrentSemaphoreIndex` (`VulkanContext.h:154`, `.cpp:457`, `:711`) deleted. That change is specified elsewhere in the merged spec; nothing in this section depends on which indexing wins except that `pWaitDstStageMask` in §3.3 belongs to the *acquire* semaphore.

---

### 4. Enablement

**Current state, verified by reading both sides.** `createLogicalDevice` (`VulkanContext.cpp:95-104`) constructs `vkb::DeviceBuilder{m_VkbPhysicalDevice}` and calls `.build()` with no feature configuration; `pickPhysicalDevice` (`:74-93`) calls only `set_surface` and `set_minimum_version(1, 2)`. On the vk-bootstrap side, `fill_out_phys_dev_with_criteria` sets `phys_dev.features = criteria.required_features` (`X3/libs/vk-bootstrap/src/VkBootstrap.cpp:1268`) and `phys_dev.extended_features_chain = criteria.extended_features_chain` (`:1269`), and `DeviceBuilder::build` enables exactly those (`:1654-1670`). `criteria.required_features` is a value-initialized `VkPhysicalDeviceFeatures{}` (`VkBootstrap.h:722`) and the chain is empty. **Confirmed: the engine today enables zero device features.**

**Decision: Vulkan 1.3 core *and* the `VK_KHR_dynamic_rendering` extension. Both. Not one or the other.**

The extension is not redundant, and this is the single most likely thing to be got wrong. I proved it with a probe (`gcc -lvulkan`, run against the local NVIDIA 1.4.341 driver):

- Device created with `VkPhysicalDeviceVulkan13Features::dynamicRendering = VK_TRUE` and **no** extensions:
  `vkGetInstanceProcAddr(inst, "vkCmdBeginRenderingKHR")` → **non-NULL** (`0x…320`, a loader trampoline)
  `vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR")` → **NULL**
  `vkGetDeviceProcAddr(dev, "vkCmdBeginRendering")` → non-NULL
- Same device, plus `VK_KHR_dynamic_rendering` in `ppEnabledExtensionNames`:
  `vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR")` → **non-NULL**

The vendored ImGui resolves its function pointer via `vkGetInstanceProcAddr(info->Instance, "vkCmdBeginRenderingKHR")` (`X3-Editor/libs/imgui-docking/imgui_impl_vulkan.cpp:1098-1099`) and then asserts non-NULL at `:1101-1102`. With 1.3 core only, **that assert passes and the device dispatch entry behind the trampoline is empty** — the failure surfaces at the first `ImGui_ImplVulkan_RenderDrawData`, not at init. The header says so in as many words at `imgui_impl_vulkan.h:89`: *"Need to explicitly enable VK_KHR_dynamic_rendering extension to use this, even for Vulkan 1.3."* Believe it.

(The `IMGUI_IMPL_VULKAN_USE_LOADER` path at `:1078-1079` does not apply: it is gated on `VK_NO_PROTOTYPES` at `:112-113`, and this build uses `<vulkan/vulkan.h>` with prototypes.)

Local support confirmed: `VK_KHR_dynamic_rendering` revision 1 is advertised by both local GPUs (`vulkaninfo`), and `VkPhysicalDeviceDynamicRenderingFeatures::dynamicRendering == 1` on both.

**Exact vk-bootstrap calls.**

`createInstance` (`VulkanContext.cpp:49-52`) — bump the API version:
```cpp
builder.set_app_name("X3 Engine")
    .request_validation_layers(m_EnableValidationLayers)
    .use_default_debug_messenger()
    .require_api_version(1, 3, 0);      // was require_api_version(1, 2, 0) at :52
```

`pickPhysicalDevice` (`VulkanContext.cpp:76-78`) — replace the two-line chain:
```cpp
VkPhysicalDeviceVulkan13Features features13{};
features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
features13.dynamicRendering = VK_TRUE;
features13.synchronization2 = VK_TRUE;   // free here; see note below

vkb::PhysicalDeviceSelector selector{ m_VkbInstance };
selector.set_surface(m_Surface)
    .set_minimum_version(1, 3)                                              // was (1, 2) at :78
    .set_required_features_13(features13)                                   // VkBootstrap.h:679
    .add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);       // VkBootstrap.h:638
```

That is the whole change. `createLogicalDevice` (`:95-104`) needs **no** edit: `DeviceBuilder` reads the selected `PhysicalDevice`'s `extensions_to_enable` (`VkBootstrap.cpp:1630-1632`) and `extended_features_chain` (`:1654-1665`), both populated from the selector's criteria at `:1268-1281`. Do **not** hand-build a `VkPhysicalDeviceFeatures2` and pass it via `DeviceBuilder::add_pNext` — vk-bootstrap explicitly errors on that combination (`DeviceError::VkPhysicalDeviceFeatures2_in_pNext_chain_while_using_add_required_extension_features`, `VkBootstrap.h:215`, raised at `VkBootstrap.cpp:1649-1651`).

API-name corrections against guesses you may see elsewhere: it is `set_required_features_13` (**not** `set_required_features13`, not `require_features_13`), taking a `VkPhysicalDeviceVulkan13Features const&` and setting `sType` for you (`VkBootstrap.cpp:1427-1432`); and `add_required_extension(const char*)` (**not** `add_desired_extension`, which does not exist on `PhysicalDeviceSelector`). `VKB_VK_API_VERSION_1_3` is defined because the installed `vulkan_core.h` defines `VK_VERSION_1_3` (`/usr/include/vulkan/vulkan_core.h:7062`; header version 350, `VK_HEADER_VERSION_COMPLETE = 1.4.350`), so `set_required_features_13` is compiled in (`VkBootstrap.h:676-680`).

**Do not also pass `VkPhysicalDeviceDynamicRenderingFeatures` via `add_required_extension_features`.** `VkPhysicalDeviceVulkan13Features` and `VkPhysicalDeviceDynamicRenderingFeatures` are mutually exclusive in the same `pNext` chain (VUID-VkDeviceCreateInfo-pNext-06532). Pick the 1.3 struct; it is a superset.

**Two knock-on edits:**
1. `createAllocator` (`VulkanContext.cpp:139`): `allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;` → `VK_API_VERSION_1_3`. Not a correctness fix, but VMA asserts this does not exceed the instance version and it should track it.
2. `synchronization2` is enabled but **not used** in this phase. Rationale: `blitImageToSwapchain` is written in synchronization-1 form and the barriers in §3 match it; converting to `VkImageMemoryBarrier2`/`vkCmdPipelineBarrier2` is a mechanical follow-up with no behavioural change, and mixing forms in one commit makes the diff unreadable. Enabling the bit now costs nothing and makes the conversion a pure edit later.

**One risk worth a sentence.** `PhysicalDeviceSelector::populate_device_details` queries `VkPhysicalDeviceVulkan13Features` on *every* enumerated physical device (`VkBootstrap.cpp:1101-1113`) *before* `is_device_suitable` filters on version (`:1220`, `:1130`). On a machine with a mixed set of GPUs where one only reports 1.2, that query is technically out of contract. Drivers ignore unrecognised `pNext` structs in practice, and vk-bootstrap ships this way, but if device selection behaves oddly on a heterogeneous machine, this is the first place to look.

---

### 5. ImGui — verified against the vendored copy

**The vendored ImGui supports dynamic rendering. This is not a blocker.**

`X3-Editor/libs/imgui-docking/imgui.h:30` → `IMGUI_VERSION "1.90.9 WIP"`. Field names confirmed by reading `X3-Editor/libs/imgui-docking/imgui_impl_vulkan.h`:

- `bool UseDynamicRendering;` — `imgui_impl_vulkan.h:90`
- `VkPipelineRenderingCreateInfoKHR PipelineRenderingCreateInfo;` — `imgui_impl_vulkan.h:92`, guarded by `#ifdef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING` (`:91`), which is defined at `:62-64` when the Vulkan headers define `VK_VERSION_1_3` or `VK_KHR_dynamic_rendering` — both are present locally.
- `VkRenderPass RenderPass;` at `:79` is documented "Ignored if using dynamic rendering", and the assert at `imgui_impl_vulkan.cpp:1125-1126` only fires when `UseDynamicRendering == false`.

Two hard asserts you must satisfy, at `imgui_impl_vulkan.cpp:973-974`:
- `PipelineRenderingCreateInfo.sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR`
- `PipelineRenderingCreateInfo.pNext == nullptr`

And one trap the asserts do **not** catch: `ImGui_ImplVulkan_Init` copies the init info by value (`bd->VulkanInitInfo = *info;`, `imgui_impl_vulkan.cpp:1128`) and reuses it as the pipeline `pNext` at `:975` whenever the pipeline is (re)created. `pColorAttachmentFormats` is a pointer; the array it points at **must outlive the ImGui backend**. A stack local will dangle. Use a file-scope `static`.

**`X3-Editor/src/ImGuiContext.cpp:157-201` becomes:**

```cpp
#ifdef X3_USE_VULKAN
    ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(m_Window->getNativeWindow()), true);

    VulkanContext* vkContext = VulkanContext::Get();

    // MUST have static storage duration: ImGui_ImplVulkan_Init copies the
    // InitInfo by value and keeps the pColorAttachmentFormats POINTER, then
    // dereferences it on every pipeline (re)creation.
    static VkFormat s_ColorAttachmentFormat = VK_FORMAT_UNDEFINED;
    s_ColorAttachmentFormat = vkContext->getSwapchainImageFormat();

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance        = vkContext->getInstance();
    init_info.PhysicalDevice  = vkContext->getPhysicalDevice();
    init_info.Device          = vkContext->getDevice();
    init_info.QueueFamily     = vkContext->getGraphicsQueueFamily();
    init_info.Queue           = vkContext->getGraphicsQueue();
    init_info.PipelineCache   = VK_NULL_HANDLE;
    init_info.DescriptorPool  = vkContext->getDescriptorPool();

    // Dynamic rendering: RenderPass is ignored, Subpass is unused.
    init_info.RenderPass          = VK_NULL_HANDLE;   // was getOverlayRenderPass() at :171
    init_info.Subpass             = 0;
    init_info.UseDynamicRendering = true;
    init_info.PipelineRenderingCreateInfo = {};
    init_info.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;   // asserted, imgui_impl_vulkan.cpp:973
    init_info.PipelineRenderingCreateInfo.pNext = nullptr;      // asserted, imgui_impl_vulkan.cpp:974
    init_info.PipelineRenderingCreateInfo.viewMask = 0;
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_ColorAttachmentFormat;
    init_info.PipelineRenderingCreateInfo.depthAttachmentFormat   = VK_FORMAT_UNDEFINED;
    init_info.PipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    uint32_t swapchainImageCount = vkContext->getSwapchainImageCount();
    init_info.ImageCount    = swapchainImageCount;
    init_info.MinImageCount = swapchainImageCount;
    init_info.MSAASamples   = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS) { LOG_ENGINE_ERROR("ImGui Vulkan error: {}", static_cast<int>(err)); }
    };

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        LOG_ENGINE_CRITICAL("Failed to initialize ImGui Vulkan backend!");
    }
    if (!ImGui_ImplVulkan_CreateFontsTexture()) {
        LOG_ENGINE_CRITICAL("Failed to create ImGui font texture!");
    }
    vkDeviceWaitIdle(vkContext->getDevice());
#else
```

Notes on the diff:
- `init_info.MinImageCount = vkContext->getMinImageCount();` at `:173` was dead — overwritten two lines later at `:178`. Drop it, and delete `getMinImageCount()` (`VulkanContext.h:87`) since that was its only caller. The asserts at `imgui_impl_vulkan.cpp:1123-1124` (`MinImageCount >= 2`, `ImageCount >= MinImageCount`) are satisfied by `swapchainImageCount`.
- **The swapchain-recreation re-init block at `ImGuiContext.cpp:217-261` must receive the identical treatment**, with the same `static VkFormat` refreshed from the new swapchain — the surface format can in principle change across recreation. Replace `init_info.RenderPass = vkContext->getOverlayRenderPass();` at `:237` with the same seven `UseDynamicRendering` / `PipelineRenderingCreateInfo` lines. Extract a small `static ImGui_ImplVulkan_InitInfo MakeInitInfo(VulkanContext*)` helper rather than duplicating the block a third time.
- `ImGui_ImplVulkanH_Window::UseDynamicRendering` (`imgui_impl_vulkan.h:177`) is for the multi-viewport helper path, which is disabled under Vulkan (`ImGuiContext.cpp:129-131`). Ignore it.

---

### 6. MoltenVK

**Available.** `VK_KHR_dynamic_rendering` has been implemented in MoltenVK since the 1.2.x series, and MoltenVK has advertised Vulkan 1.3 since 1.2.5 (Vulkan SDK 1.3.268). Any SDK current in 2026 is far past both. I could **not** verify this on this machine — there is no macOS build here and no vendored MoltenVK anywhere in the tree (`grep -rni moltenvk` over the non-`libs` build files returns nothing; there is no `if(APPLE)` in `CMakePresets.json` or any `CMakeLists.txt`). Treat the version floor as a claim to confirm on the target Mac with one command before relying on it:

```
vulkaninfo | grep -E 'apiVersion|dynamicRendering|VK_KHR_dynamic_rendering'
```

Expect `dynamicRendering = true` and `VK_KHR_dynamic_rendering` in the device extension list. If either is absent, the SDK is too old; there is no workaround short of keeping render passes.

**Caveats to design around now:**

1. **Do not use `VK_RENDERING_SUSPENDING_BIT` / `VK_RENDERING_RESUMING_BIT`.** MoltenVK maps each `vkCmdBeginRendering` onto a fresh `MTLRenderCommandEncoder`; suspend/resume is the weakest-supported corner of the feature there. The design in §2 sets `VkRenderingInfo::flags = 0` and opens at most one block per frame, so this costs nothing — but it is a constraint on Phase 5's render graph, which must not split a logical pass across command buffers via suspend/resume.
2. **Keep the number of rendering blocks per frame small.** On Metal each begin/end is an encoder boundary, which is meaningfully more expensive than on desktop drivers. One block per frame is the target; the editor and runtime shapes in §2 both hit it.
3. **`dynamicRenderingLocalRead` is not available on MoltenVK** (it is a Vulkan 1.4 feature; the local GPUs here report it `true`, which will mislead you if you develop against them). Do not design input-attachment-style reads within a rendering block. Relevant to Phase 7, not Phase 1.
4. **Portability is already handled.** vk-bootstrap sets `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` and adds `VK_KHR_portability_enumeration` when available (`X3/libs/vk-bootstrap/src/VkBootstrap.cpp:742-749`, `:843-844`), and adds `VK_KHR_portability_subset` to the device automatically (`:1272-1280`). No code change needed for macOS in this section.
5. **Swapchain usage on Metal.** §7's `TRANSFER_DST` request depends on `CAMetalLayer.framebufferOnly` being disabled, which MoltenVK does in response to non-attachment usage requests. If it fails, vk-bootstrap returns `SwapchainError::required_usage_not_supported` (`VkBootstrap.h:224`, raised at `VkBootstrap.cpp:1946`) — a hard, loud failure rather than silent corruption, which is the right failure mode. Do not paper over it with `add_image_usage_flags` being made conditional.

---

### 7. Swapchain usage flags

**Confirmed the bug.** `VulkanContext::createSwapchain` (`VulkanContext.cpp:149-173`) builds with only `.set_old_swapchain(m_Swapchain).build()` (`:153-155`) — it never touches image usage. vk-bootstrap's default is `VkImageUsageFlags image_usage_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;` (`X3/libs/vk-bootstrap/src/VkBootstrap.h:1002`, and reset to the same at `VkBootstrap.cpp:2136`). Meanwhile `blitImageToSwapchain` issues `vkCmdClearColorImage` into the swapchain image at `.cpp:887` and `vkCmdBlitImage` into it at `.cpp:908`, both of which require `VK_IMAGE_USAGE_TRANSFER_DST_BIT`.

**Fix** — replace `VulkanContext.cpp:153-155`:

```cpp
auto swap_ret = swapchain_builder
    .set_old_swapchain(m_Swapchain)
    .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)   // VkBootstrap.h:932
    .build();
```

`add_image_usage_flags` ORs into the existing mask, preserving `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` (which the editor's rendering block needs). **Do not use `set_image_usage_flags` (`VkBootstrap.h:930`)** — it *replaces* the mask and would strip `COLOR_ATTACHMENT`, breaking the editor.

Locally verified supported: `vulkaninfo` reports `supportedUsageFlags` for this surface as `TRANSFER_SRC | TRANSFER_DST | SAMPLED | STORAGE | COLOR_ATTACHMENT | INPUT_ATTACHMENT`. If a platform does not support it, `build()` returns `SwapchainError::required_usage_not_supported` (`VkBootstrap.cpp:1946`) and the existing error path at `VulkanContext.cpp:157-160` throws with the message — no silent degradation.

Note that this is the same function that must gain `RenderSettings::vSync → set_desired_present_mode` wiring, so batch the two edits.

---

### 8. Ordering within the merged spec

This section's edits must land **in the same commit** as the frame-lifecycle restructure (`beginFrame`/`endFrame`/`present`, deletion of `swapBuffers`/`ensureFrameStarted`/`m_FirstFrame`). They cannot be split: deleting `beginFrame`'s render pass without adding `beginSwapchainRendering` leaves ImGui with no attachment, and adding `UseDynamicRendering` to ImGui before the device enables the extension trips the null-dispatch described in §4. Within the commit:

1. `createInstance` API version → 1.3; `pickPhysicalDevice` features + extension; `createAllocator` version bump. *(Nothing else works without this.)*
2. `createSwapchain` `add_image_usage_flags(TRANSFER_DST)`.
3. Delete `createRenderPass`, `createFramebuffers`, `m_RenderPass`, `m_OverlayRenderPass`, `m_Framebuffers`, `beginRenderPass`, `beginOverlayRenderPass`, `m_RenderPassActive`, `isRenderPassActive`, `getRenderPass`, `getOverlayRenderPass`, `getMinImageCount`; strip `.cpp:31-32`, `:380-393`, `:399-403`, `:682`, `:721-724`, `:774-780`, `:832-835`.
4. Add `transitionSwapchainImage`, `beginSwapchainRendering`, `endSwapchainRendering`, `getSwapchainImageFormat`, `m_SwapchainImageLayout`, `m_SwapchainImageWritten`.
5. `endFrame`: the fallback + present transition (§3.4) and the `pWaitDstStageMask` fix (§3.3, A4).
6. `blitImageToSwapchain`: the two edits in §2b.
7. `ImGuiContext.cpp`: `Init` (§5) and the swapchain-recreation block at `:217-261`; `EndFrame` at `:271-306`.

**Verification gate:** run the editor with `VK_LAYER_KHRONOS_validation` including synchronization validation. Expect zero `VUID-vkCmdDispatch-renderpass`, zero `VUID-VkPresentInfoKHR-pImageIndices` layout complaints, zero `SYNC-HAZARD-*` on the swapchain image. Then run the runtime with a project open and with no frame produced (to exercise §3.4), and resize both repeatedly to exercise swapchain recreation and the ImGui re-init path.