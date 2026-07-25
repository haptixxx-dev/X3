# Adjudication — binding decisions for the merged Phase 1/2 spec

All three verifiers returned `readyToImplement: false`. Their objections are
overwhelmingly one problem wearing different hats: the four merged parts were
written **in parallel against a shared API surface that did not exist yet**, so
each part invented its own names and signatures.

That is an orchestration error, not an authoring one. Parallel agents can
safely split work that touches disjoint files; they cannot safely split work
that defines a common interface. The fix for future phases is in
`ORCHESTRATION.md`: **one agent authors the API surface first, then parts are
written against it.**

Below, every conflict is decided. These are binding. Where the choice is
arbitrary, it is still made, because an arbitrary decision that is written
down beats a defensible one that is re-argued.

## API surface — canonical

**`DescriptorWriter`.** Take Part 3's form.

```cpp
DescriptorWriter(VulkanContext& ctx,
                 const VulkanDescriptorSetLayout& layout,
                 VkDescriptorSet dst);
void flush();   // no arguments
```

Capacity is derived from the layout (`imageDescriptorCount()`,
`bufferDescriptorCount()`), not passed as `maxWrites`/`maxImageInfos`. A
writer that knows its layout cannot be given a count that disagrees with it,
which removes a whole class of caller error. Part 4's four-argument
constructor and one-argument `flush()` are void.

**`sampledImageArray`.** Owned by Part 3, the resource layer. Built in step
1.5. Part 4 *consumes* it and must not define it. This was the critique's
explicit instruction and Part 4 reversed it.

**Frame accessors.** Take Part 3's naming: `frameNumber()`,
`completedFrame()`. Part 2's `getFrameNumber()`/`getCompletedFrame()` are
void. Rationale: the new layer drops `get` prefixes throughout, and
consistency inside the new code matters more than consistency with the old.

**Frames in flight.** `FRAMES_IN_FLIGHT` only. Part 3 deletes
`MAX_FRAMES_IN_FLIGHT` (`VulkanContext.h:146`) and `getMaxFramesInFlight()`
(`VulkanContext.h:88`) in step 1.1, so every later part must use the new
name. Part 2's body needs a sweep.

**`blitImageToSwapchain`.** Take the explicit-frame form:

```cpp
void blitImageToSwapchain(const FrameContext& frame, VulkanImage& src,
                          glm::ivec4 viewport, glm::ivec2 windowSize);
```

Passing the frame explicitly beats reaching for ambient current-frame state,
which is the habit the whole resource layer exists to break. The Y-flip
packing at `VulkanContext.cpp:905-906` and `CalculateViewportCoordinates()`
in `RuntimeLayer.cpp:306-376` must behave identically before and after.

**Uploads.** Take Part 3's `ctx.stage()` / `StagingAlloc`. Part 2's
`getUploadCommandBuffer()`, `endUploadRecording()`, `flushUploadsBlocking()`,
`m_UploadPool`, `m_UploadCmd`, and `m_UploadCmdRecording` are void, and Part
2's `beginFrame`/`endFrame` bodies must be rewritten to not reference them —
verifier three correctly notes Part 2's Commit 6 cannot compile as written.
Rationale: staging belongs to the resource layer that owns the allocations,
not to the context.

**`VulkanBuffer` constructor.** Take Part 4's five-argument form; the extra
`VkBufferUsageFlags extraUsage = 0` is needed for vertex and index buffers
and defaults harmlessly. `ensureCapacity` carries it through.

**`TextureDesc` / `SamplerDesc`.** Part 3 already declares
`TextureDesc::mipLevels` and `SamplerDesc::mipmapMode`; Part 4's claim that
these are missing is wrong. Add only `SamplerDesc::maxLod`, which genuinely
does not exist.

## Ownership

**Deleting `IRendererAPI`, `IRenderingContext`, `VulkanRendererAPI`.** Owned
by Part 1, the OpenGL deletion. Not Part 2, not Part 3. Part 1 already
removes the factory layer; these go with it, and the later parts should
assume they are gone.

**Wait-idle post-condition.** *Corrected — the first version of this ruling
was wrong and the header verifiers caught it.*

I originally wrote that `endSingleTimeCommands` is deleted outright and no
upload path performs a queue wait. That is not implementable: ImGui's font
upload and the dummy resources are created before any frame exists, so
something must upload outside a frame, and `VulkanTexture`'s blocking
constructor would have had no legal implementation.

The correct rule distinguishes *when*, not *whether*:

- **In-frame uploads never wait.** They go through `ctx.stage()` into the
  frame's command buffer and are retired by the frame fence. The per-call
  `vkQueueWaitIdle` in the old `endSingleTimeCommands` is what Phase 1
  removes from this path.
- **Out-of-frame uploads may wait**, and are confined to initialization and
  teardown: ImGui fonts, dummy resources, and nothing else. One blocking
  submit-and-wait helper survives for this.

Final permitted wait-idle set: `recreateSwapchain`, `cleanup`, resource-layer
teardown, and the out-of-frame upload helper. A gate that greps for zero
wait-idle sites is wrong; the gate should assert that none of them are
reachable from a frame.

Consequently `MERGED-2-lifecycle-sync.md:53` ("Batched uploads; delete
beginSingleTimeCommands/endSingleTimeCommands") is **void**. `MERGED-3` is
already consistent with the corrected rule.

## Corrections

**Part 1 Commit 0 is stale.** Phase 0 is committed (`8cdcec6`). The working
tree is clean. Delete that commit from the plan; do not re-apply its changes.

**Part 4's shader edit is wrong about two of three files.** The real
signatures differ and cannot be edited identically:

```
PathTracing.comp:341  IsInShadow(vec3 origin, vec3 dirToLight, float distToLight)
Phong.comp:251        IsInShadow(vec3 origin, vec3 lightDir,   float maxDist)
PBR.comp:251          IsInShadow(vec3 origin, vec3 normal, vec3 lightDir, float maxDist)
```

`PBR.comp` takes an extra `normal` parameter. Any instruction to mirror one
signature into all three is void; each file needs its own edit.

**Part 4's "unused" claims are grep-only.** `TextureMetadata::texStartIdx`
and `useDoubleBuffering` must be re-verified by compiling with the symbol
removed, not by grepping. The `DXC_COMMAND` regression this session came from
exactly that shortcut: a grep-verified "dead" setting that was load-bearing
in a configuration the agent could not build.

## Newly found, and it outranks everything else in Phase 1

`beginFrame` opens `m_RenderPass` at `VulkanContext.cpp:392` and leaves it
open for the whole frame. `VulkanComputeShader::Dispatch` then records
`vkCmdDispatch` (`VulkanComputeShader.cpp:137`) and a `vkCmdPipelineBarrier`
(`:146-153`) into that same command buffer while the pass is still open.

`vkCmdDispatch` inside a render pass instance is invalid —
VUID-vkCmdDispatch-renderpass. The barrier is invalid too: a barrier inside a
render pass requires a matching subpass self-dependency, and
`createRenderPass` declares only an `EXTERNAL → 0` dependency
(`VulkanContext.cpp:196-202`).

This is the engine's entire rendering path and has presumably been wrong for
its whole life. The dynamic rendering migration dissolves it, because
`beginFrame` will open no rendering block at all and compute is recorded at
top level. Verify it with validation layers **and a project open** — a bare
launch renders nothing and reports nothing, which is how this stayed hidden.
