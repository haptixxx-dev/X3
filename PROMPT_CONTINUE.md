# Continue Here

Handoff for resuming the X3 engine migration.

**Status: Phases 0-6 COMPLETE (2026-07-29). Phase 7 (Forward+) is next.**

Phase 6's energy gate is CLOSED -- the BSDF energy LUT is baked and
`bsdf-furnace` is green (white-furnace mean 0.9991, was 0.829 uncompensated).
All 10 render-test scenarios pass.

**Phase 2's visual exit criterion is MET** (verified by screenshot
2026-07-29): the fixture renders the Stanford bunny smooth-shaded with its
embedded base-colour and normal maps applied, on a ground plane with a cast
shadow under a correctly-oriented skybox, in both Phong and the path tracer.

Looking at it is what found the last real bug of the phase -- see the
"inherit a model's textures" commit. Four textures were decoding into the
pool and not one was ever bound, because every pre-Phase-2 scene carries a
MaterialComponent override that names no maps and the override won outright.
Validation was clean and verify.sh passed the whole time.

**To screenshot the editor on this machine:** `import -window root` grabs the
desktop, not the GLFW surface. Diff `xprop -root _NET_CLIENT_LIST` across the
launch to get the window id, then `import -window <id>`. The editor camera
starts at the origin looking at sky; flip `EditorState::temp::useEditorCamera`
to false to use the scene camera, which the fixture frames on the model.

One gate is still NOT met, though it is now RUNNABLE:

* **Phase 3's pixel-identity gate.** The plan asks for the post-Slang image to
  be diffed against the pre-Slang one. That was impossible when Phase 3 landed
  (no frame readback existed) and is merely unfinished now. `X3RenderTest`
  exists, and the engine-side pieces it needs are pure additions between the
  Phase 2 and Phase 5 commits, so they backport cleanly to either side of the
  migration. The experiment to run: build `2d6c7f9` (last GLSL) and `5691aa1`
  (first Slang) each with the harness backported, render both, and diff. A
  `git worktree` does NOT get the submodules -- run
  `git submodule update --init --recursive` inside it or configure fails on
  ImGuizmo and assimp. That is where the attempt stopped.

Start a new session with:

> Read PROMPT_CONTINUE.md and continue from where it says work stopped.

---

## 1. Read these first, in this order

| File | What it is |
|---|---|
| `ENGINE_PLAN.md` | The plan. Thirteen phases, twelve locked decisions. **Authoritative.** |
| `docs/specs/ADJUDICATION.md` | Binding API decisions. **Overrides every spec.** Its wait-idle section was corrected once — read the correction. |
| `X3/src/Platform/Vulkan/Vulkan*.h` | The canonical resource-layer API. These headers are the contract, not a suggestion. |
| `docs/VALIDATION-BASELINE.md` | **The regression oracle, and it is now EMPTY.** Every VUID Phase 1 cleared, with the guard that keeps each one fixed. A VUID appearing here again is a regression, not a known issue. |
| `ORCHESTRATION.md` | How to run subagents on this repo. Read "Rules that keep this from going wrong" — each rule cost real time to learn. |
| `docs/RENDER-TESTS.md` | The golden-image tests. **The only gate that looks at the image** -- `verify.sh` never has. |
| `docs/DEPENDENCIES.md` | What to install and when. |

`docs/specs/MERGED-*.md` are detailed but stale wherever `ADJUDICATION.md` or
the headers disagree. Treat them as reference, not instruction.

---

## 2. Where work stopped

**Phase 0: complete.** Repo and build hygiene. The build was completely broken
on a clean Linux checkout and now works.

**Phase 1: complete.** `scripts/verify.sh` reports `ALL CHECKS PASSED` on both
`debug` and `release`: both binaries build, both run 20 s with the committed
fixture open, and **no validation message of any kind is emitted**. All four
baseline VUIDs are cleared and `docs/VALIDATION-BASELINE.md` is empty.

Confirm before trusting this:

```bash
git log --oneline | head -10
bash scripts/verify.sh
```

### 2a-bis. What Phases 2-5 left behind

**Phase 2 -- mesh attributes.** `Triangle` is gone. Three buffers now:
`TriPositionBuffer` (de-referenced positions, BVH-only, byte-identical to the
old layout), `TriRefBuffer` (three GLOBAL vertex indices plus a material slot,
16 B) and `VertexBuffer` (position/normal/UV/tangent, 48 B, UVs in the `.w`
lanes). Every GPU-mirrored struct lives in `X3/src/Renderer/GpuTypes.h` with
`sizeof`/`offsetof` asserts. Materials are per-submesh:
`Gpu::MeshEntityHandle` carries `materialBase` + `materialSlotCount`, and
`MaterialComponent` is a `std::vector<MaterialDesc>`. Textures go through a
fixed 128-entry combined-image-sampler array at set 0 binding 2, every element
always written. `stbi_set_flip_vertically_on_load` is gone and the skybox `v`
was corrected in the same change.

Two latent bugs fixed because this rewrote the code containing them: the
importer never walked the node graph (any mesh under a transformed node
imported at the wrong placement), and embedded `.glb` textures were
unreachable.

**Phase 3 -- Slang.** `res/shaders/*.comp` are gone. `GpuTypes.slang`,
`Bindings.slang` and `Trace.slang` are modules; `PathTracing`, `PBR` and
`Phong` are entry points that contain only their own BRDF. The descriptor
table is GENERATED from reflection into
`X3/src/Renderer/Generated/DescriptorTables.h` -- do not edit it, edit
`Bindings.slang`. `scripts/fetch-slang.sh` downloads a pinned v2026.14 into
the gitignored `X3/libs/slang`; **run it on a fresh checkout or configure
fails**. `-matrix-layout-column-major` is load-bearing.

Slang's uninitialised-variable analysis found a real bug the GLSL hid: PBR
and Phong shaded with an uninitialised light direction for any light type
outside 0-2.

**Phase 4 -- job system.** enkiTS, behind `X3/src/Core/JobSystem.h`, started
in `Application`'s constructor. Asset loading is decode-parallel,
merge-serial: `AssetManager::DecodeMesh` (assimp, attributes, BVH) touches
nothing shared and runs on the pool; `MergeMesh` is the only writer of the
AssetPool and runs on the main thread IN DECLARATION ORDER, so the buffer
layout does not depend on which decode finished first. The audit found one
real race -- `stbi_set_flip_vertically_on_load` writes a process-wide global
-- now the `_thread` variant everywhere in the decode path. `Profiler` is
still not thread-safe; its header says why and what to do instead.

**Phase 5 -- render graph.** `X3/src/Renderer/RenderGraph.h`.
`Renderer::Draw` runs on it: one `Trace` pass and one `Present` pass whose
only job is to declare the two consumers. Barriers are COALESCED PER
RESOURCE PER PASS, which is load-bearing -- see the commit. No reordering,
no async compute in v1. Toggle "Dump Render Graph" in the render settings
panel to see the derived lifetimes.

### 2a. What Phase 1 actually left behind

**Part 2 — the frame lifecycle.** It lives in `Application::run`:

```
beginFrame()  ->  (null? skip the whole iteration)  ->  LayerStack::onUpdate()
              ->  endFrame()  ->  present()
```

`IWindow::swapBuffers()` is gone and the window does not touch the context after
construction. `ensureFrameStarted()` / `m_FirstFrame` are gone; the ordering they
papered over is structural now. `ImGuiContext` runs on dynamic rendering
(`UseDynamicRendering` + `PipelineRenderingCreateInfo`) and owns the single
rendering block per editor frame, opened in `EndFrame` and closed before it
returns. That is what killed `VUID-vkCmdDispatch-None-10672` and
`VUID-vkCmdPipelineBarrier-None-07889`.

**Part 3 — the resource layer.** `Renderer` runs on `VulkanComputePipeline`,
`VulkanDescriptorSetRing`, `VulkanRingBuffer`, `VulkanBuffer`, `VulkanImage` and
`VulkanTexture`. `VulkanComputeShader`, `VulkanImage2D`, `VulkanTexture2D`,
`VulkanUniformBuffer`, `VulkanShaderStorageBuffer` and `VulkanContextInterface.h`
are deleted, along with the context's bound-resource registries.
`blitImageToSwapchain` is in its adjudicated `(const FrameContext&,
VulkanImage&, ...)` form. Per-frame descriptor sets killed
`VUID-vkUpdateDescriptorSets-None-03047`; the always-write rule plus the dummy
resources killed `VUID-vkCmdDispatch-None-08114`.

**Five asserts hold the result in place. Do not weaken them.**

| Assert | What it prevents |
|---|---|
| `endFrame()`: no rendering block left open | the render-pass VUIDs returning |
| `beginSwapchainRendering()`: none already open | two blocks in one frame |
| `VulkanComputePipeline::dispatch()`: `!renderingBlockOpen()` | `vkCmdDispatch` inside a rendering block |
| `DescriptorWriter::flush()`: every binding written exactly once | an unwritten descriptor read by a dispatch |
| `~DescriptorWriter`: `flush()` was called | a set silently keeping two-frame-old resources |

And one more, newly armed: `beginSingleTimeCommands()` now asserts
`!frameActive()`. That assert — not a grep for wait-idle sites — is
ADJUDICATION.md's gate on the corrected wait-idle rule. In-frame uploads go
through `ctx.stage()` + `frame.cmd()`; blocking uploads are confined to
initialisation and teardown.

### 2b. What Phase 1 did NOT do, and what to check first

**The viewport renders.** The original baseline recorded "the viewport is black"
as a separate observation from the VUIDs, attributed to the undefined
`skyboxTexture` descriptor and to the branch's own history (`a2f652f "render is
absolutely cooked"`). Checked by eye after Part 3: the fixture shows the HDR
skybox, the ground plane with a cast shadow, and the Stanford bunny, at ~409 FPS
with the profiler live. That is the first end-to-end confirmation that the
render path produces a picture and not merely valid Vulkan.

Three behaviour changes to weigh if the picture ever looks wrong:

* **The double-buffer swap is gone.** `Renderer::Render` returns the image it
  just wrote. The old code returned the OTHER slot, which the current frame's
  command buffer had recorded no barrier for.
  `RenderSettings::useDoubleBuffering` is now unused.
* **Accumulation pins the write slot to 0** (`Renderer::writeSlot`) because
  `PathTracing.comp` does `imageLoad` then `imageStore` on the same image;
  alternating slots would give each half the samples.
* **The skybox is uploaded in-frame** through the staging arena instead of a
  blocking `vkQueueWaitIdle`, so a skybox change no longer stalls.

### 2c. Still open, not blocking

- **Frame readback: DONE.** `VulkanContext::readbackImage` plus the
  `X3RenderTest` harness -- see `docs/RENDER-TESTS.md`. Runs are bit-exact
  including the path tracer. Phase 7 should lean on this hard: its per-pass
  gate is "the path-traced reference agrees", which is now a `--filter` away
  rather than a manual comparison.
- The harness needs a DISPLAY. The window is unmapped, not surfaceless, so it
  will not run over bare SSH or in CI. Making `beginFrame`/`endFrame` support
  an offscreen path is the remaining piece if that is ever wanted.
- `RenderSettings::useDoubleBuffering` is dead. Remove it from the settings UI
  and serialization, or repurpose it.
- Texture mip generation is NOT implemented. `TextureDesc::mipLevels` must
  still be 1 and both `VulkanTexture` constructors assert it. Phase 2's spec
  called for a `vkCmdBlitImage` chain; it was deferred, so minified surfaces
  alias. Phase 7 wants mips regardless.
- The inspector shows material texture GUIDs read-only. A texture picker is
  Phase 13's material editor.
- `docs/specs/MERGED-*.md` still describe pre-migration code in places.
- macOS/MoltenVK is unmeasured; the dynamic-rendering path and the descriptor
  indexing Phase 2 now requires (`shaderSampledImageArrayNonUniformIndexing`)
  in particular.

## 3. Operational rules

These are not style preferences. Every one of them cost hours in the last
session.

**A build is not verified by an exit code.** A piped build reports the status
of the last stage in the pipe. One reported success while producing no
binaries. Check for artefacts and grep the log for `error:`. `scripts/verify.sh`
does both — use it.

**Grep cannot prove something is unused.** A CMake setting was deleted on
thorough grep evidence and correct reasoning, and was load-bearing in a
configuration the agent could not build. If a deletion rests on a deadness
claim, remove it and build every configuration.

**A compile-fail probe needs a control that compiles.** Three probes "passed"
by failing to find a header. Without a control, a broken harness is
indistinguishable from a real rejection.

**The renderer needs an open project.** The editor boots to a launcher and
renders nothing until a project is open. A bare launch exercises Vulkan init
only and reports no validation errors — which looks like success and is not.
`TestProject/` and `X3_OPEN_PROJECT` exist for this. `scripts/verify.sh` uses
them.

**Never author a shared interface in parallel.** Four agents wrote four parts
of one spec concurrently against an API that did not exist, and each invented
its own: two incompatible constructors, two "final" signatures for the same
function, two upload mechanisms. A full cycle to detect, another to fix.
Parallel agents split disjoint *files*. One agent defines the *interface*,
it gets verified, then the rest fan out against it.

**Documented is not enforced.** Two verifiers rejected an API whose invariants
lived in comments. Deleting `FrameContext`'s copy operations was not enough —
the default constructor was still public, so `struct Evil { FrameContext f; };`
compiled. If a guarantee matters, make violating it a compile error and prove
it with a probe.

**Clean up scratch files.** Agent probe files left in the tree pollute the
editor's diagnostics for everyone afterwards.

---

## 4. Model and orchestration

Haiku for single-file mechanical edits where the instruction fully determines
the output. Sonnet for multi-file refactors, inventories, and verification —
most implementation work. Opus for design, synchronization correctness, and
adversarial verification. Effort matters as much as tier: Sonnet at high effort
often beats Opus at low effort.

The shape that works here:

```
one agent authors the interface  →  verify it  →  fan out implementations
  →  adversarially verify (2-3 skeptics, distinct lenses)  →  verify.sh gate
```

Scale verification to consequence. A doc edit needs one skeptic; a descriptor
lifetime change needs three.

---

## 5. Remaining phases

Each block below is a ready-to-use prompt. Prefix with: *"Read
PROMPT_CONTINUE.md, then:"*

*Phases 2-5 are done; their prompts have been removed. See §2a-bis for what
they left behind.*

### Phase 6 — DONE

`res/shaders/Bsdf.slang`: an `IBsdf` interface with `DiffuseBsdf`,
`MetalRoughBsdf` and `CoatedBsdf`; GGX with VNDF importance sampling; shading
loops generic over it, specialised per tier at link time. `Gpu::Material` carries
a feature bitfield (80 B) and the optional lobes live in `Gpu::MaterialExt`
(64 B), allocated only when used. **Both renderers evaluate this one module**,
and since the `lights` fixture caught them diverging, so is the direct-lighting
loop -- it lives in `Trace.slang`. Do not copy either.

`BsdfLutBake.slang` bakes the split-sum `(A, B)` table using the SAME sampling
and visibility functions `Bsdf.slang` shades with, so it is exact for this lobe
rather than a fit. Every analytic shortcut tried before it failed in a different
direction (2.13, then 1.89, then 1.09); do not reach for another one.

The lobes are authorable in the inspector and round-trip through `.lrscn`.

### Phase 6 — original plan text · ~4-6 weeks · Opus for the BSDF

> Build the runtime material representation and the shared BSDF library. Must
> precede Phase 7 — raster shaders cannot be written first.
>
> Per locked decision 3, OpenPBR is authoring-side only. Target a 32-48 byte
> runtime struct: base colour, metallic, roughness, normal scale, occlusion,
> emission, a feature bitfield, texture indices. Optional lobes (clearcoat,
> sheen, anisotropy, thin-film) as a second tier that only materials using them
> pay for.
>
> Write the BSDF library **once in Slang**, imported by both the Forward+
> shader and the path tracer's hit shader, so the raster path and the reference
> path agree by construction rather than by discipline. This is the payoff for
> doing Slang first.
>
> Validate numerically against `github.com/adobe/openpbr-bsdf` — white furnace
> and energy conservation tests, not screenshots. Shading math fails by looking
> plausible.

### Phase 7 — Clustered Forward+ · ~6-10 weeks · the big one

> Build the rasterizer. The engine has no graphics pipeline at all today: no
> vertex input state, no depth prepass, no framebuffer management, no mesh draw
> submission. All of it is new.
>
> Passes: depth prepass, cluster assignment and light culling (compute — the
> part that reuses existing skills), opaque forward, transparent forward,
> velocity. Add velocity now; retrofitting it for TAA is painful.
>
> Decompose by pass, three agents each (spec, implement, verify), pipelined so
> the depth prepass verifies while light culling is still being written.
>
> Gate per pass: renders correctly **and** the path-traced reference agrees on
> direct lighting. This is what the reference renderer is for.
>
> Accepted weaknesses: decals and screen-space subsurface scattering are both
> awkward without a G-buffer, and dense alpha-tested foliage suffers overdraw.

### Phase 8 — Shadows · ~3-4 weeks · Sonnet

> Cascaded shadow maps for the primary directional light — four cascades,
> stabilized against shimmer, normal-offset bias. Then BVH-traced shadow rays
> for soft shadows and secondary lights; a shadow ray is a single occlusion
> query against the intersector you already have. Budget per light with a
> shadow-map fallback. Skip virtual shadow maps — they solve Nanite-scale
> density you do not have.

### Phase 9 — Asset cook pipeline · ~4-6 weeks · Sonnet

> Add the cook step. Must precede Phase 10 — baked lightmaps need somewhere to
> live. Editor keeps the source workflow; export cooks.
>
> Produce BC7 textures, prebuilt serialized BVH, meshoptimizer-optimized meshes,
> and a binary scene format. Today assets are re-imported from original source
> paths on every project open, synchronously, and moving a source file silently
> breaks a project.
>
> Fan out by asset type. Each needs a round-trip test: cook, load, compare.
> Add a "load cooked" mode to the editor so cook-only bugs are reproducible
> without exporting.

### Phase 10 — Global illumination · ~6-10 weeks · Opus for DDGI

> Where the path tracer investment pays off.
>
> **Lightmap baking:** retarget the existing tracer from a camera to lightmap
> UV space. The tracing code does not change — only ray generation. New work is
> UV unwrapping (integrate xatlas), atlas packing, and a dilate/seam pass.
> Parallelize across the Phase 4 job system or it is unusable.
>
> **DDGI:** the algorithm is tracer-agnostic — it needs any ray/scene
> intersection oracle plus a probe update pass, so it runs directly on the
> existing software BVH with no RT hardware. Cascaded probe volumes, octahedral
> irradiance and depth maps, temporal blending, Chebyshev visibility weighting
> for leak mitigation.
>
> Build a light-leak test scene early — leaking is the classic DDGI failure and
> will not be obvious from one screenshot.

### Phase 11 — Post stack · ~3-4 weeks · Sonnet

> Best fidelity-per-effort ratio in the plan. In order: TAA (about a week given
> velocity buffers already exist, and the single largest image-quality win),
> AgX tonemapping via OpenColorIO (a few dozen lines, enormous gain over naive
> clamping), progressive downsample/upsample bloom, then XeSS as the
> vendor-neutral upscaler — its DP4a path runs on any SM6.4 GPU. Then depth of
> field and motion blur. Defer volumetrics.
>
> One Opus agent on TAA history rejection; that is where temporal artifacts
> actually come from.

### Phase 12 — OpenPBR importer · ~3-5 weeks · Sonnet

> Offline importer reading MaterialX documents with OpenPBR Surface nodes,
> baking them to the Phase 6 runtime struct. Authoring-side, no runtime risk.
>
> You need document parsing only, not ShaderGen — MaterialX's hardware
> generators are vertex+fragment only and cannot emit compute anyway. A targeted
> `.mtlx` parser is likely cheaper than the full library, whose OIIO and OCIO
> options pull heavy dependency trees.
>
> There is no blessed real-time subset of OpenPBR. Document your reduction
> explicitly — which lobes map where, which are approximated, which are dropped
> — and validate against the Adobe reference so you know the error you accept.

### Phase 13 — Editor catch-up · interleaved · Sonnet/Haiku

> ImGui multi-viewport under Vulkan (needs per-viewport swapchains), render
> settings UI for the new pipeline, lightmap bake UI with progress and preview,
> material editor with live preview. Existing TODOs: asset deletion in
> `AssetsPanel`, child entity hierarchy in `SceneHierarchyPanel`, physics
> gravity wiring in `PhysicsSettingsPanel`.

---

## 6. Known traps

**Component maintenance.** Adding a component means editing `Components.h`,
`Scene::Copy`, `Scene::DuplicateEntity`, and both serialization paths. Some are
*already* inconsistent — `LightComponent` and `FirstPersonCameraComponent` are
serialized but not fully handled in duplicate. The renderer phases add several
components. Consider macro or reflection-based registration before that, not
after.

**macOS/MoltenVK.** Test regularly, not at phase end. It is a translation layer
with real gaps, and discovering at Phase 10 that a Phase 5 design does not work
there is expensive.

**clangd diagnostics.** `compile_commands.json` is symlinked to the build dir.
After a preset rename or a fresh build directory the symlink goes stale and the
editor fills with phantom errors. Re-point it:
`ln -sf build/debug/compile_commands.json compile_commands.json`

**Three hard dependencies**, everything else is negotiable: Phase 1 blocks all
rendering work; Phase 6 must precede Phase 7; Phase 9 must precede Phase 10.

**Two remaining structural holes** in the resource layer, documented rather
than hidden: a cached `const FrameContext*` does not dangle — it silently
re-reads a member the context mutates in place, which is worse. And
`VkDescriptorSet` is a raw copyable handle that can outlive its frame, which is
the exact VUID Phase 1 exists to fix. Both need the implementation's asserts to
catch them.
