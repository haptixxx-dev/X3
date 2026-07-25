# Vulkan validation baseline

The state of the render path on `vulkan-migration`, measured rather than assumed.
Every line below came out of a real run of `X3Editor` with the committed test
fixture open and the Khronos validation layer loaded. Phase 1 is done when this
file is empty.

Re-measure it after any change to `VulkanContext`, `VulkanComputeShader` or the
resource layer, and update this file in the same commit.

---

## How this was captured

```bash
cd /home/sarah/Coding/Haptixxx/X3
cmake --preset vulkan-debug && cmake --build build/vulkan-debug -j"$(nproc)"

X3_OPEN_PROJECT=$PWD/TestProject/TestProject.lrproj \
  timeout 22 stdbuf -oL -eL ./build/vulkan-debug/Debug/X3Editor 2>&1 | tee run.log

grep -oE 'VUID-[A-Za-z0-9-]+$' run.log | sort | uniq -c
```

Two things about that command line are not optional:

* **`stdbuf -oL -eL`.** The validation messages arrive on `stdout` through
  vk-bootstrap's default debug messenger. Redirected to a file, `stdout` is
  fully buffered, and a crash (`abort()`) discards the buffer. The first capture
  of this baseline looked like "no validation output at all" for exactly that
  reason, while spdlog's own lines survived because spdlog flushes itself.
  Anything that runs the editor non-interactively and greps for `VUID-` must
  line-buffer it or it is grepping a lie.
* **`X3_OPEN_PROJECT`.** Without an open project `RenderLayer::onUpdate` returns
  immediately, no compute is dispatched, and the run exercises Vulkan
  initialisation only. A bare launch reports nothing and proves nothing. That is
  why the fixture exists.

By default the layer stops reporting a VUID after 10 occurrences, which is why
the counts below cap at 10. To count per-frame instead:

```bash
echo 'khronos_validation.duplicate_message_limit = 0' > /tmp/vk_layer_settings.txt
VK_LAYER_SETTINGS_PATH=/tmp/vk_layer_settings.txt X3_OPEN_PROJECT=... ./X3Editor
```

**Environment.** NVIDIA proprietary driver 610.43.03, `VK_LAYER_KHRONOS_validation`
1.4.350, Vulkan instance 1.4.350, X11. VUID *numbers* move between layer
releases: `VUID-vkCmdDispatch-None-10672` is what older layers reported as
`VUID-vkCmdDispatch-renderpass`. Match on the message text, not only the number.

**Fixture.** `TestProject/TestProject.lrproj` — one scene (`SmokeScene`) with a
main camera, the Stanford bunny (5696 triangles), a primitive ground plane, an
emissive sphere, a directional light, a point light, and a 4k HDR skybox.

---

## Current baseline — 4 distinct VUIDs

Measured over a 20 s run: **1074 frames**, each producing the first three errors
below, plus 10 `vkUpdateDescriptorSets` errors per frame (one per descriptor
written). The editor stays up and the profiler reports a live frame graph; the
viewport itself is black (see "Not validation errors" below).

| VUID | Occurrences | Root cause | Fixed in Phase 1? |
|---|---|---|---|
| `VUID-vkCmdDispatch-None-10672` | 1 per frame | dispatch inside an active render pass | Yes — Part 0 (dynamic rendering) |
| `VUID-vkCmdPipelineBarrier-None-07889` | 1 per frame | barrier inside a subpass with no self-dependency | Yes — Part 0 (dynamic rendering) |
| `VUID-vkCmdDispatch-None-08114` | 1 per frame | `skyboxTexture` descriptor never written | Yes — Part 3 (resource layer), but see note |
| `VUID-vkUpdateDescriptorSets-None-03047` | 10 per frame | descriptor set rewritten while still in use by a pending command buffer | Yes — Part 3 (`VulkanDescriptorSetRing`) |

### `VUID-vkCmdDispatch-None-10672` — dispatch inside a render pass

> vkCmdDispatch(): It is invalid to issue this call inside an active VkRenderPass.
> The Vulkan spec states: If the per-tile execution model is not enabled, this
> command must be called outside of a render pass instance.

**This is THE bug the migration is about.** `VulkanContext::beginFrame` opens
`m_RenderPass` (`VulkanContext.cpp:392`) and leaves it open for the entire frame;
`VulkanComputeShader::Dispatch` then records `vkCmdDispatch`
(`VulkanComputeShader.cpp:149`) inside it. Compute is not a graphics command and
cannot execute in a render pass instance.

**Phase 1:** dissolved rather than patched. Under dynamic rendering (MERGED-0)
`beginFrame` opens no rendering block at all — there is no `VkRenderPass` and no
`VkFramebuffer` — so the dispatch is outside one by construction. Owned by the
dynamic-rendering part; the lifecycle part (2) must not re-open a rendering scope
around the layer stack.

### `VUID-vkCmdPipelineBarrier-None-07889` — barrier inside a subpass

> vkCmdPipelineBarrier(): Barriers cannot be set during subpass 0 of VkRenderPass
> with no self-dependency specified.

Same root cause, second symptom. The post-dispatch compute→fragment barrier at
`VulkanComputeShader.cpp:158` is recorded inside the render pass opened by
`beginFrame`. Inside a render pass a barrier is only legal if the pass declares a
matching subpass self-dependency, which this one does not.

**Phase 1:** fixed by the same change. With no render pass open the barrier is a
plain pipeline barrier and is legal where it stands. The barrier itself still
needs review under the new sync model (the image transition to
`SHADER_READ_ONLY_OPTIMAL` for the ImGui sample belongs in the image layout
tracking of Part 3), but it will no longer be *invalid*.

### `VUID-vkCmdDispatch-None-08114` — skybox descriptor never written

> the descriptor [Set 0, Binding 1, variable "skyboxTexture"] is being used in
> dispatch but has never been updated via vkUpdateDescriptorSets().

Not a synchronisation bug — a plain wiring bug the fixture exposed. `VulkanContext`
only learns about a sampled image through `registerSampledImage`, and the only
caller is `VulkanTexture2D::ChangeTextureUnit` (`VulkanTexture2D.cpp:41`). The
constructor (`VulkanTexture2D.cpp:8`) takes a texture unit and never registers it.
`Renderer::SetupGPUResources` creates the skybox with
`ITexture2D::Create(data, w, h, SKYBOX_TEXTURE_UNIT)` (`Renderer.cpp:244`) and
never calls `ChangeTextureUnit`, so binding 1 of set 0 stays unwritten forever
and the shader samples an undefined descriptor.

**Phase 1:** expected to disappear, but *only* if the port is done deliberately.
The descriptor-writing path is replaced by `DescriptorWriter` +
`VulkanDescriptorSetRing`, whose `flush()` validates completeness against the
layout, so an unwritten binding becomes a caught error rather than silent
garbage. Whoever ports `Renderer::SetupGPUResources` must write the skybox
binding explicitly. If this VUID survives Phase 1, the port copied the bug.

### `VUID-vkUpdateDescriptorSets-None-03047` — set updated while in use

> pDescriptorWrites[N].dstBinding was created with VkDescriptorBindingFlags(0),
> but VkDescriptorSet ... is in use by VkCommandBuffer ...

`VulkanComputeShader` allocates exactly one descriptor set per set index
(`allocateDescriptorSets`, once) and rewrites all of them every frame from
`Dispatch` (`updateDescriptorSets` at `VulkanComputeShader.cpp:130`, flushing at
`:294`). With `FRAMES_IN_FLIGHT = 2` the command buffer from the previous frame
is still pending on the GPU while frame N overwrites the very sets it references.
This is the classic "one descriptor set, two frames in flight" hazard.

**Phase 1:** this is precisely what `VulkanDescriptorSetRing` in
`VulkanDescriptors.h` exists to fix — `FRAMES_IN_FLIGHT` sets per (pipeline, set
index), indexed by `frame.index()`, written only for the slot whose fence has
been waited on. The hazard is documented in that header's contract. Owned by
Part 3.

---

## Not validation errors, but observed in the same run

* **The viewport is black.** Frames are produced and presented, but the compute
  output is not visibly correct. Consistent with the undefined `skyboxTexture`
  descriptor and with the branch's own history (`a2f652f "render is absolutely
  cooked"`). Not a Phase 1 exit criterion by itself, but the fixture is what
  makes it checkable: once Phase 1 lands, this scene should show a bunny.
* **No `VK_ERROR_*` and no lost device** over 20 s / ~1100 frames.
* **No swapchain-recreation errors** at the default window size. Resizing was not
  exercised; that path is still unmeasured.

---

## Fixed while capturing this baseline — the frame-1 abort

Recorded because it is the first thing the fixture found, and because it will
come back the moment someone reorders the layer stack.

With a project open, `RenderLayer` records the compute dispatch **before**
anything begins the frame's command buffer: `VulkanContext::init` deliberately
does not call `beginFrame`, `swapBuffers` only begins the *next* frame, and the
lazy `ensureFrameStarted()` was called solely from `ImGuiContext::EndFrame` —
which runs after `RenderLayer` in the layer stack. Until the fixture existed the
editor rendered nothing until a project was open, so ImGui always got there
first and the ordering bug was invisible.

The result was four more VUIDs, once each, followed by a hard abort inside the
NVIDIA driver:

```
VUID-vkCmdBindPipeline-commandBuffer-recording       vkCmdBindPipeline(): was called before vkBeginCommandBuffer().
VUID-vkCmdBindDescriptorSets-commandBuffer-recording vkCmdBindDescriptorSets(): was called before vkBeginCommandBuffer().
VUID-vkCmdDispatch-commandBuffer-recording           vkCmdDispatch(): was called before vkBeginCommandBuffer().
VUID-vkCmdPipelineBarrier-commandBuffer-recording    vkCmdPipelineBarrier(): was called before vkBeginCommandBuffer().
free(): invalid pointer                              (SIGABRT in vkBeginCommandBuffer, one frame later)
```

Fix: `VulkanComputeShader::Dispatch` now calls `context->ensureFrameStarted()`
before it touches the command buffer — the same idiom `ImGuiContext::EndFrame`
already used. Phase 1 Part 2 deletes `ensureFrameStarted()`/`m_FirstFrame`
outright in favour of an explicit `beginFrame()`/`endFrame()` around the whole
frame; when it does, this ordering must be guaranteed structurally instead.

Without this fix the editor aborts roughly one second after the project opens and
none of the four baseline VUIDs above are reachable.
