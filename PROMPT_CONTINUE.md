# Continue Here

Handoff for resuming the X3 engine migration. Written 2026-07-26 at the end of
the Phase 0 / Phase 1 session.

Start a new session with:

> Read PROMPT_CONTINUE.md and continue from where it says work stopped.

---

## 1. Read these first, in this order

| File | What it is |
|---|---|
| `ENGINE_PLAN.md` | The plan. Thirteen phases, twelve locked decisions. **Authoritative.** |
| `docs/specs/ADJUDICATION.md` | Binding API decisions. **Overrides every spec.** Its wait-idle section was corrected once — read the correction. |
| `X3/src/Platform/Vulkan/Vulkan*.h` | The canonical resource-layer API. These headers are the contract, not a suggestion. |
| `docs/VALIDATION-BASELINE.md` | Every VUID the engine emitted before Phase 1, with who fixes each. **This is the regression oracle.** |
| `ORCHESTRATION.md` | How to run subagents on this repo. Read "Rules that keep this from going wrong" — each rule cost real time to learn. |
| `docs/DEPENDENCIES.md` | What to install and when. |

`docs/specs/MERGED-*.md` are detailed but stale wherever `ADJUDICATION.md` or
the headers disagree. Treat them as reference, not instruction.

---

## 2. Where work stopped

**Phase 0: complete.** Repo and build hygiene. The build was completely broken
on a clean Linux checkout and now works.

**Phase 1: in progress.** A workflow was running when the session ended. Check
what actually landed before assuming anything:

```bash
git log --oneline | head -20
bash scripts/verify.sh debug
```

At handoff time these had landed: the committed test fixture and validation
baseline, and the complete OpenGL deletion (backend, factory layer, interface
layer, preprocessor guards, `RendererAPI` project setting, GL splash screen,
Vulkan-only build).

Still outstanding in Phase 1:

- Implementations for the remaining resource-layer headers
  (`VulkanDescriptors.cpp`, `VulkanComputePipeline.cpp`) — the headers are
  committed, the `.cpp` files do not exist yet. `VulkanBuffer.cpp`,
  `VulkanImage.cpp` and `VulkanStaging.cpp` are written and linking.
- Migrating `Renderer` onto per-frame rings and per-frame descriptor sets, and
  deleting `VulkanComputeShader`, `VulkanImage2D`, `VulkanTexture2D`,
  `VulkanUniformBuffer`, `VulkanShaderStorageBuffer`.
- Deleting `VulkanContextInterface.h` once `VulkanContext` genuinely matches it.

**Phase 1 is done when** `scripts/verify.sh debug` passes with zero VUIDs. Not
when it compiles.

### 2a. Part 2 (frame lifecycle) is DONE — what landed and what it proved

The broken `a615c08 wip: dynamic rendering context rewrite [DOES NOT BUILD]`
has been finished. Both presets build and both smoke-test clean; the frame
lifecycle now lives in `Application::run`:

```
beginFrame()  ->  (null? skip the whole iteration)  ->  LayerStack::onUpdate()
              ->  endFrame()  ->  present()
```

`IWindow::swapBuffers()` is gone and the window no longer touches the context
after construction. `ensureFrameStarted()` / `m_FirstFrame` are gone; the
ordering they papered over is now structural. `ImGuiContext` was ported to
dynamic rendering (`UseDynamicRendering` + `PipelineRenderingCreateInfo`) and
owns the single rendering block per editor frame, opened in `EndFrame` and
closed before it returns.

**The payoff, measured:** `VUID-vkCmdDispatch-None-10672`
(`VUID-vkCmdDispatch-renderpass` on older layers) and
`VUID-vkCmdPipelineBarrier-None-07889` are **gone**. Two of the four baseline
VUIDs cleared. `docs/VALIDATION-BASELINE.md` has been re-measured and now lists
two, both owned by Part 3:

```
VUID-vkCmdDispatch-None-08114            skyboxTexture descriptor never written
VUID-vkUpdateDescriptorSets-None-03047   set rewritten while a frame still uses it
```

Three asserts hold the result in place; do not weaken them when porting Part 3.
`endFrame()` asserts no rendering block was left open, `beginSwapchainRendering()`
asserts one is not already open, and `VulkanComputeShader::Dispatch` asserts
`!renderingBlockOpen()`. That last one is the standing guard against the
render-pass VUIDs returning the moment something starts drawing geometry.

One thing to know before touching `VulkanImage.h`: `VulkanImage::allocate`,
`VulkanTexture::create` and `VulkanTexture::recordUpload` are private helpers
that the committed `.cpp` already used but the header had never declared. They
are declared now. The public contract is unchanged.

### 2b. General triage — if the tree is dirty or does not build

A workflow was running when the session ended, and **you cannot resume it.**
Workflow `resumeFromRunId` is same-session only; run `wf_c12981e2-52c` is gone.
Whatever it had committed is durable, whatever it had not is not.

So expect one of three states. Find out which before doing anything:

```bash
git status --short          # dirty?
bash scripts/verify.sh debug   # green?
```

**State A — clean tree, verify green.** Ideal. Pick up from the outstanding
list above.

**State B — clean tree, verify red on VUIDs only (build fine).** Also fine and
expected mid-Phase-1. Compare against `docs/VALIDATION-BASELINE.md`; a VUID
listed there is known work, one that is not is a regression you just inherited.

**State C — dirty tree and/or the build is broken.** This is the likely one,
and it is *normal*: an agent was interrupted mid-edit. At the time of writing,
`VulkanContext.h` had been rewritten to the new interface while
`VulkanContext.cpp` still had the old bodies, giving ~30 errors like
`no declaration matches 'VulkanContext::VulkanContext(GLFWwindow*)'` and
`'VulkanContext' has no member named 'swapBuffers'`. That is a half-applied
rewrite, not a mystery.

Two ways forward. Decide deliberately rather than drifting into one:

*Finish it.* Read the partial diff (`git diff`) and the new header, and write
the matching `.cpp`. The header is the contract and it is closer to the target
than the old code, so this is usually the better option — the work is most of
the way to somewhere good.

*Reset to the last green commit.* Cheaper if the partial work looks confused.

```bash
git stash -u                       # park it, do not delete it
git log --oneline                  # find the last commit claiming a passing gate
bash scripts/verify.sh debug       # confirm HEAD is actually green
```

Every commit from that workflow states its verification gate in the message,
so the log tells you where the last known-good point is. Reset there and re-run
the remaining Phase 1 steps as a fresh workflow. Do not `git stash drop` until
you are sure you do not want the partial work.

**The dead agents' reasoning is still on disk.** Even an agent that never
returned wrote a full transcript:

```
~/.claude/projects/-home-sarah-Coding-Haptixxx/*/subagents/workflows/wf_c12981e2-52c/
```

`journal.jsonl` has one line per *completed* agent with its return value.
`agent-*.jsonl` files hold the full working transcript of every agent including
the ones that were interrupted. If you are unsure what a half-finished edit was
trying to do, read the transcript rather than guessing from the diff.

---

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

### Phase 2 — Mesh format and vertex attributes · ~2-3 weeks · Sonnet-heavy

> Implement Phase 2 per `ENGINE_PLAN.md` and `docs/specs/MERGED-4-mesh.md`,
> with `ADJUDICATION.md` overriding both. The engine's `Triangle` is three
> positions and nothing else, so everything is flat-shaded and untextured, and
> normal mapping is unrepresentable. Add indexed vertices with normals, UV0 and
> tangents; assimp already generates them under the preset being passed and the
> importer simply does not read them. Keep BVH traversal on positions only.
> Move materials from per-entity to per-submesh. Add texture sampling to the
> path tracer — that is the visible payoff. Add `static_assert`s on `sizeof`
> and `offsetof` for every struct mirrored between C++ and GLSL until Phase 3's
> reflection replaces them.
>
> Fan out by subsystem (assimp import, AssetTypes/BVH, Renderer upload, shader)
> with worktree isolation — these agents share files. Then one Opus agent
> verifies struct layout consistency end to end.
>
> Gate: `verify.sh` clean, and the fixture renders smooth-shaded textured
> geometry with normal mapping.

### Phase 3 — Slang migration · ~2-3 weeks · Opus for the port

> Migrate the three compute shaders to Slang. Do it now while the shader set is
> three files and ~1400 lines; after Phase 7 it is ten times the work.
>
> `shader-slang` is NOT in the Arch repos, and the installed `extra/slang` is
> S-Lang, an unrelated interpreted language. Get it from GitHub releases or
> vcpkg; consider vendoring a pinned version given the weekly cadence.
>
> Value is modules (~250 lines are currently copy-pasted three ways), generics
> and interfaces (an `IBRDF` conformance is what makes Phase 6's material model
> affordable), `ParameterBlock<T>` mapping one-to-one onto descriptor sets, and
> reflection to generate the C++ descriptor tables that are currently
> hand-synced. Budget time to write that codegen — no off-the-shelf tool exists.
>
> Traps: matrix convention differences bite silently, `mix→lerp` /
> `fract→frac` renames produce no compile error when missed, and layout must be
> pinned explicitly (`Std140DataLayout` / `Std430DataLayout`) so C++ structs
> still match byte-for-byte.
>
> Gate: rendered output pixel-identical to pre-migration. Diff the images.

### Phase 4 — Job system · ~1-2 weeks · Sonnet

> Integrate enkiTS. Do not reuse Jolt's `JobSystemThreadPool` — it is tied to
> the physics simulation lifecycle and shaped around physics.
>
> Parallelize in value order: BVH builds (currently synchronous and blocking in
> `LoadMesh`), asset loading, then command buffer recording. Lightmap baking in
> Phase 10 is the thing that genuinely requires this.
>
> Run the thread-safety audit as parallel per-subsystem agents over
> `AssetManager`, `Log` and `Profiler`, each asked to *find races* rather than
> confirm safety.

### Phase 5 — Render graph · ~3-4 weeks · Opus for design

> Build the render graph before the rasterizer. Forward+ with depth prepass,
> cluster culling, shadow cascades, velocity, DDGI update, GI apply,
> transparency, TAA, bloom and tonemap is fifteen-plus passes, well past where
> hand-written barriers stay tractable.
>
> Keep v1 minimal: passes declare reads and writes, barriers are derived,
> transient lifetimes and aliasing are automatic, execution order is linear.
> **No pass reordering and no async compute in v1** — both are where render
> graph complexity explodes. Add a graph dump; you will need it constantly.
>
> Use a judge panel: three independent Opus designs from the same brief, three
> scorers, synthesize from the winner. This is the phase most prone to an agent
> inventing something baroque.

### Phase 6 — Material model and BSDF library · ~4-6 weeks · Opus for the BSDF

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
