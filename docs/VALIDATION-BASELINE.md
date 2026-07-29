# Vulkan validation baseline

The state of the render path on `main`, measured rather than assumed.
Every line below came out of a real run of `X3Editor` with the committed test
fixture open and the Khronos validation layer loaded.

**The baseline is empty. Phase 1's exit criterion is met.**

Re-measure it after any change to `VulkanContext`, the resource layer or
`Renderer`, and update this file in the same commit. A VUID that appears here
again is a regression, not a known issue.

---

## How this was captured

`scripts/verify.sh debug` does all of this and prints the distinct VUIDs. By
hand, with the current presets:

```bash
cd /home/sarah/Coding/Haptixxx/X3
cmake --preset debug && cmake --build build/debug -j"$(nproc)"

X3_OPEN_PROJECT=$PWD/TestProject/TestProject.lrproj \
  timeout 22 stdbuf -oL -eL ./build/debug/Debug/X3Editor 2>&1 | tee run.log

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

By default the layer stops reporting a VUID after 10 occurrences. To count per
frame instead:

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

## Current baseline — none

`scripts/verify.sh` reports `ALL CHECKS PASSED` on both `debug` and `release`:
both binaries build, both run 20 s with the fixture open, the renderer produces
a frame every iteration, and no validation message of any kind is emitted.

Checked by eye as well, because a clean validation log is not a picture: the
fixture renders the HDR skybox, the ground plane with a cast shadow and the
Stanford bunny, at ~409 FPS. The "viewport is black" note that sat below the
original baseline is resolved.

---

## Cleared during Phase 1 — the four original VUIDs

Kept because each one records why the design is shaped the way it is, and
because the guard that keeps it fixed is worth naming.

### `VUID-vkCmdDispatch-None-10672` — dispatch inside a render pass

> vkCmdDispatch(): It is invalid to issue this call inside an active VkRenderPass.

**This was THE bug the migration was about.** `VulkanContext::beginFrame` opened
`m_RenderPass` and left it open for the entire frame; `VulkanComputeShader::Dispatch`
then recorded `vkCmdDispatch` inside it.

**Dissolved, not patched.** Under dynamic rendering there is no `VkRenderPass`
and no `VkFramebuffer` anywhere in the engine, and `beginFrame()` opens no
rendering block, so the dispatch is at top level by construction. The single
rendering block per editor frame is opened by `ImGuiContext::EndFrame` and closed
before it returns.

**The guard:** `VulkanComputePipeline::dispatch()` asserts
`!frame.context().renderingBlockOpen()`, and `endFrame()` asserts no block was
left open. A future raster pass cannot reintroduce this quietly.

### `VUID-vkCmdPipelineBarrier-None-07889` — barrier inside a subpass

Same root cause, second symptom: the post-dispatch barrier was recorded inside
the render pass, which needs a matching subpass self-dependency the pass did not
declare. Fixed by the same change — and the unconditional barrier itself is gone.
`dispatch()` inserts no barriers at all; `Renderer::Draw` records exactly the
ones its consumers need.

### `VUID-vkCmdDispatch-None-08114` — skybox descriptor never written

> the descriptor [Set 0, Binding 1, "skyboxTexture"] is being used in dispatch
> but has never been updated via vkUpdateDescriptorSets().

A wiring bug the fixture exposed: `Renderer::SetupGPUResources` created the
skybox but never registered it, so binding 1 of set 0 stayed unwritten forever
and the shader sampled an undefined descriptor.

**Fixed by the always-write rule.** Every binding the layout declares is written
every frame; an absent skybox writes `ctx.dummyTexture()` instead of nothing.

**The guard:** `DescriptorWriter::flush()` asserts in debug that every binding in
the layout was written exactly once with the declared type and count, and
`~DescriptorWriter` asserts that `flush()` was called at all. An unwritten
binding is now a caught error rather than silent garbage.

### `VUID-vkUpdateDescriptorSets-None-03047` — set updated while in use

`VulkanComputeShader` allocated one descriptor set per set index and rewrote all
of them every frame, while the previous frame's command buffer was still pending
with those sets bound. That also fed frame N-1's dispatch frame N's data, because
descriptors are consumed at execution time.

**Fixed by `VulkanDescriptorSetRing`:** `FRAMES_IN_FLIGHT` sets per (pipeline, set
index), and `ring.get(frame)` is the only way to name one. A set belonging to a
frame other than `frame.index()` is unnameable, so the hazard is unrepresentable
rather than merely avoided.

---

## Fixed during the Part 3 migration, in the same style

These three appeared once, in a single verify run, and are recorded because each
is a trap the next person can walk into.

* **`VUID-VkWriteDescriptorSet-descriptorType-00330` / `-00327`.** The camera and
  settings rings were left to be allocated by their first `ensureCapacity()`, and
  a default-constructed `VulkanRingBuffer` is `BufferKind::Storage`. `BufferKind`
  selects both the usage flag and the offset alignment the slot stride is rounded
  to, so the UBOs got storage usage and storage alignment. **A ring's kind is
  fixed at construction; construct rings explicitly, do not let a default one
  drift into use.**
* **`VUID-vkFreeDescriptorSets-pDescriptorSets-00309`.** The editor cached ONE
  ImGui descriptor for the viewport image. Once the Renderer began alternating
  image slots per frame, that cache missed every frame and re-registered, and
  `ImGui_ImplVulkan_RemoveTexture` frees the set immediately — on a set the
  previous frame's command buffer was still using. **The cache is now a map keyed
  on `VulkanImage::id()`**, which is what bounds it at `FRAMES_IN_FLIGHT` entries
  forever. A genuine generation change (a resolution change) still frees
  immediately and waits idle first, because ImGui exposes no deferred-free path.

---

## Fixed while capturing the original baseline — the frame-1 abort

Recorded because it is the first thing the fixture found.

With a project open, `RenderLayer` recorded the compute dispatch **before**
anything began the frame's command buffer: `VulkanContext::init` deliberately did
not call `beginFrame`, `swapBuffers` only began the *next* frame, and the lazy
`ensureFrameStarted()` was called solely from `ImGuiContext::EndFrame` — which
runs after `RenderLayer` in the layer stack. Until the fixture existed the editor
rendered nothing until a project was open, so ImGui always got there first and
the ordering bug was invisible. The result was four `-commandBuffer-recording`
VUIDs and a hard abort inside the NVIDIA driver one frame later.

The whole mechanism is gone. `Application::run` calls `beginFrame()` before
`LayerStack::onUpdate()` and `endFrame()` + `present()` after it, so every
recorder in the stack runs inside an open command buffer regardless of layer
order. The ordering is structural, not lazy.
