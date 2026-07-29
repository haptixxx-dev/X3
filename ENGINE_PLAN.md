# X3 Engine Plan — Vulkan, Slang, and a High-Fidelity Real-Time Renderer

Written 2026-07-25. Supersedes `TODO_VULKAN.md` (see Phase 0).

---

## Status — 2026-07-30

**Every phase has been implemented. Five carry named exceptions; nothing is silently incomplete.**

Gates, all green and deterministic across consecutive runs:

| Gate | Result | Needs |
|---|---|---|
| `scripts/render-test.sh` | **31 passed, 0 failed** | a display |
| `scripts/verify.sh` | ALL CHECKS PASSED, both configs | a display |
| `X3MathTest` | 47 checks | nothing |
| `X3MtlxTest` | 102 checks | nothing |
| `X3LightmapTest` | 176 checks | nothing |
| `X3AssetCook --self-test` | 66 checks | nothing |

**What is genuinely NOT built**, gathered here so it cannot be missed:

- **No lightmap bake pass.** UV unwrapping, packing and dilation exist; nothing bakes into them. The bake UI is disabled and labelled as such.
- **No BC7 texture compression, no meshoptimizer, no binary scene format.** `ProjectExporter` does not invoke the cook step.
- **No XeSS.** It requires an external SDK that cannot be fetched in this environment.
- **No ImGui multi-viewport.**
- **The OpenPBR reduction is unvalidated** against `adobe/openpbr-bsdf`.
- **`AmbientIBL` samples the skybox with no occlusion**, so a sealed interior is lit wherever DDGI is off. This is the remaining source of the 1.77/255 residual in `ddgi-leaktest`.

### Three failure patterns that recurred, each costing time more than once

These are worth reading before touching the renderer.

1. **Order-dependent per-frame state.** Three times. Accumulation twice, then DDGI's ray-rotation counter and probe atlases. Each made a golden depend on what ran *before* it — `lights-pathtracing` once differed by 20 levels between running alone and running in the suite. **Anything integrated over frames must reset on a scene change AND on a settings change.** If a golden moves for no reason you can name, check this before touching a threshold.

2. **Unordered read/write of one storage image in a single dispatch.** Twice in TAA, narrowly avoided in bloom. It does not crash and does not trip validation; it makes output differ between runs of the same build. Ping-pong, or split into two passes.

3. **Descriptor rings shared across PASSES rather than frames.** Three times. A ring hands out one set per frame slot, so two passes sharing it rewrite the same set while the first dispatch is still recorded against it — reported as "descriptor set destroyed or updated without UPDATE_AFTER_BIND". Every pass needs its own rings.

### What the gates are actually for

Every raster pass is gated against the **reference renderer**, not against a golden of itself. That asymmetry is deliberate and is the single most valuable decision in this plan: when cascaded shadows replaced traced ones, or AgX replaced Reinhard, the question "did this break shading" had an answer that did not depend on anyone's judgement.

Effects that the reference does not have — bloom, TAA, DDGI, DoF, motion blur — are **off in every scenario but their own**. Leaving any of them enabled elsewhere would make every forward-vs-reference comparison differ by that term and quietly weaken the gate.

Two gates are assertions rather than pictures: `cluster-correctness` paints red wherever a light that reaches a point is absent from its cluster, and `X3MathTest` proves properties that hold for *any* camera and light. Both caught real bugs that no golden image could have.

---

## 0. Decisions locked

| # | Decision | Choice |
|---|---|---|
| 1 | OpenGL backend | **Delete it.** Vulkan-only. |
| 2 | Primary visibility | **Clustered Forward+.** |
| 3 | Material model | **OpenPBR as authoring interchange**, baked offline to a compact runtime parameter set. |
| 4 | Global illumination | **Baked lightmaps** (path tracer as baker) **+ DDGI** on the existing software BVH. |
| 5 | Shader language | **Migrate to Slang** after the Vulkan correctness fixes, before the rasterizer. |
| 6 | Path tracer's future | **Offline reference + lightmap/probe baker.** Its BVH stays live at runtime for DDGI and shadow rays. |
| 7 | Platforms | Windows, Linux, macOS. **Consoles dropped.** |
| 8 | Graphics abstraction | See the reversal note below. |
| 9 | Threading | **Adopt a job system library early** (recommend enkiTS). |
| 10 | Frame structure | **Build a render graph up front.** |
| 11 | Asset pipeline | **Add a cook step.** Editor keeps the source workflow; export bakes. |
| 12 | Render passes | **Migrate to dynamic rendering.** No `VkRenderPass`, no `VkFramebuffer`. |

### Reversal on decision 8

The RHI question was asked while consoles were on the list. With consoles dropped, the answer changes and I'm flagging it rather than quietly keeping the old one.

macOS runs Vulkan through MoltenVK, so it is still *Vulkan* — a portability abstraction buys nothing there. There is no second API left to abstract over. So the plan is **not** a multi-backend RHI. It is a thin, Vulkan-native resource-management layer: per-frame descriptor sets, per-frame buffer rings, resource state tracking, command list helpers. No virtual dispatch, no `I*` factories, no pretending a second backend might appear. `VkStructs` may appear in the interface, because there is nothing to hide them from.

This is less work than an RHI and produces faster, more debuggable code. If a console target ever comes back, the layer is already shaped around explicit-API concepts, which is the part that would actually port. Deleting the abstraction entirely would be the wrong call — the current code needs *a* layer, just not a portable one.

### Decision 12 — dynamic rendering

Added after the Phase 1 specs were written, and it supersedes every render-pass discussion in them. `VK_KHR_dynamic_rendering` is core in Vulkan 1.3 and lets attachments be declared inline at draw time, so both `VkRenderPass` objects and all `VkFramebuffer` objects disappear — `createFramebuffers` is deleted rather than repointed, and the question of whether `m_RenderPass` survives alongside `m_OverlayRenderPass` is moot.

Three reasons this is worth doing now rather than at Phase 7. It removes a whole category of correctness reasoning (render pass compatibility, subpass dependencies) that the spec critique had already found underspecified in two places. Phase 5's render graph gets simpler, because there are no framebuffer objects to cache and invalidate per pass. And Phase 7's Forward+ is many passes, each of which would otherwise need its own render pass and framebuffer boilerplate.

The cost is that transitions become explicit: without render passes there are no automatic layout changes, so every swapchain image transition is a barrier you write yourself. That is more code, but it is code whose correctness you can see, and it is where the acquire-semaphore stage bug and the missing `TRANSFER_DST` usage flag both live anyway.

MoltenVK supports dynamic rendering, so macOS is unaffected. ImGui's Vulkan backend supports it through `UseDynamicRendering` plus a pipeline rendering create info instead of a render pass handle.

---

## 1. The finding that reframes this whole plan

Before any of the phases below, understand what the engine's mesh format actually contains today:

```cpp
// X3/src/Project/Assets/AssetTypes.h:12
struct Triangle {
    glm::vec4 v0 = {}, v1 = {}, v2 = {};
};
```

Three positions. That is the entire per-triangle payload.

- **No vertex normals.** `PathTracing.comp:227` computes `vec3 Ng = cross(E1, E2)` — the geometric face normal. Every surface in the engine is flat-shaded. There is no smooth shading anywhere.
- **No texture coordinates.** None. Anywhere.
- **No tangents**, so normal mapping is not merely unimplemented, it is unrepresentable.
- **No per-vertex data of any kind** — the assimp import at `AssetManager.cpp:214` reads `mVertices` and nothing else, despite requesting `aiProcessPreset_TargetRealtime_MaxQuality` which already generates normals and tangents for free.
- **The only texture sampled in any shader is the skybox** (`PathTracing.comp:208`, and the identical line in PBR/Phong). `AssetPool::TextureBuffer` exists but `Renderer.cpp:243` uses it solely for the skybox.
- Materials are **per-entity**, twelve floats total: emission, colour, and a packed `(metallic, roughness, ao)`.

So the honest statement of where the engine is: it renders flat-shaded, untextured, single-material objects lit by a path tracer. It is a competent renderer of *geometry*, and it has essentially no *surface* representation at all.

This matters more than every item in `TODO_VULKAN.md` combined. No amount of Vulkan correctness, GI quality, or material-model sophistication produces a high-fidelity image on untextured flat-shaded geometry. Albedo maps, normal maps, and roughness maps are the single largest perceived-fidelity jump available to this engine, and they are currently impossible.

I've therefore placed the mesh format rework at Phase 2, immediately after Vulkan correctness and before everything else. Treat it as foundational, not as a renderer feature.

---

## 2. What the engine becomes

Today X3 is a path tracer with an editor bolted on. The end state is a conventional real-time engine that happens to own an unusually good offline renderer, used as a baking and validation tool.

The path tracer stops being the product and becomes infrastructure. Three things it already does turn out to be exactly what the new renderer needs:

1. **The BVH and its compute traversal kernel** become the ray oracle for DDGI probe updates and soft shadow rays. This is the highest-value reuse in the plan — DDGI is tracer-agnostic, it needs any ray/scene intersection function plus a probe update pass, and you already have the hard half.
2. **The path tracer itself**, retargeted from a camera to lightmap UV space, is a lightmap baker. Almost no new tracing code.
3. **The path traced image** becomes ground truth. Every raster feature can be diffed against a converged reference render of the same scene. Very few small engines have this, and it is worth a great deal during the raster bring-up.

---

## Phase 0 — Repo and build hygiene

*Days. No dependencies. Do it first because it's cheap and it removes noise from every later phase.*

**Repository**

- `X3/libs/MaterialX` is a tracked gitlink with **no `.gitmodules` entry**. The directory is empty and cannot be initialized by anyone who clones. Either add the submodule entry properly or remove the gitlink. Given decision 3 defers MaterialX to Phase 12, remove it for now.
- Delete the SDL submodules (`SDL`, `SDL_image`, `SDL_mixer`, `SDL_ttf`). They are never added to any build, never included by any source file, and never linked. The windowing layer is GLFW throughout. They are dead weight in a 3 GB checkout and actively misleading about which windowing library is in use.
- Delete `/comp.spv` at the repo root. Referenced by nothing, predates the three-shader split.
- Add `*.spv` to `.gitignore` and untrack the three `res/shaders/*.spv` files. They are build outputs; committing them means binary diffs in every shader commit.
- Delete the dead empty `#ifdef X3_USE_OPENGL` block at `X3-Runtime/src/RuntimeLayer.cpp:211-214`.

**Build**

- Reconcile the C++ standard. The root sets C++23, `X3Engine` and `X3Runtime` set C++20. Pick one (C++23 is fine) and set it once.
- Remove the hardcoded `set(DXC_COMMAND "/usr/local/bin/dxc" ...)` at `X3/CMakeLists.txt:33`. It won't survive a different SDK layout or a Windows machine.
- Break the `X3-Editor` → `X3Runtime` hard dependency, or document why it exists. Building the runtime for an editor-only workflow is wasteful.

**Documentation**

- `TODO_VULKAN.md` has one item that is already done and doesn't know it. It asks to "Add `-DVULKAN` define when compiling shaders." **This is unnecessary** — `glslc` predefines `VULKAN=100` automatically when targeting Vulkan. I verified this two ways: a probe shader with `#ifndef VULKAN → #error` compiles clean, and disassembling the committed `PathTracing.comp.spv` shows correct `DescriptorSet 0/1/2` decorations matching the C++ tables. I also rebuilt all three shaders and they are byte-identical to the committed artifacts, so those aren't stale either. Tick it off and replace the file with a pointer to this document.

---

## Phase 1 — Vulkan-only, and correct

*Roughly 3-4 weeks. Blocks everything. The engine is currently shipping code with real synchronization hazards.*

**1a. Delete the OpenGL backend first.** — **DONE.**

Do this before the correctness work, not after. The unsafe Vulkan implementations exist *because* the interfaces are GL-shaped — fixing them while still serving OpenGL means designing a compromise you'll throw away.

Delete `X3/src/Platform/OpenGL/` entirely, drop the `X3_GRAPHICS_API` option and all four presets in favour of plain Debug/Release, remove the `#ifdef X3_USE_OPENGL` branches across `IRendererAPI.cpp`, `IComputeShader.cpp`, `ITexture2D.cpp`, `IImage2D.cpp`, `IUniformBuffer.cpp`, `IShaderStorageBuffer.cpp`, `GLFWWindow.cpp`, `ImGuiContext.cpp`, and `RuntimeLayer.cpp`. Drop the GLEW and OpenGL `find_package` calls.

Also resolve the API-selection trap while you're here: `ProjectManager.cpp:114-119` reads a `RendererAPI` enum from the project file and calls `IRendererAPI::SetAPI()`, but every factory resolves by `#ifdef`. A project marked "Vulkan" opened in an OpenGL binary reported Vulkan and silently used OpenGL objects. With OpenGL gone, delete the enum and the setting.

**Landed.** `X3/src/Platform/OpenGL/` is gone, `X3_GRAPHICS_API` and the four presets are gone (`debug`/`release` remain), no `X3_USE_OPENGL`/`X3_USE_VULKAN` remains anywhere, and GLEW/OpenGL are unlinked. Per ADJUDICATION "Ownership", Phase 1a also deleted the factory layer: `IRendererAPI`, `IRenderingContext`, `VulkanRendererAPI` and the five resource interfaces (`IImage2D`, `ITexture2D`, `IComputeShader`, `IUniformBuffer`, `IShaderStorageBuffer`) with their `Create()` factories. The Vulkan classes are now standalone and callers hold them directly; Phase 1b replaces those classes themselves. `VulkanContext::swapBuffers()` survives until Phase 1c's `beginFrame`/`endFrame`/`present` exists to replace it. Removing the `rendererAPI` YAML key breaks no existing `.lrproj`: the loader is a sequence of independent optional key lookups, and the committed fixture still carries the key and still opens.

**1b. Replace the GL-shaped interfaces with a Vulkan-native resource layer.**

The specific things to remove and what replaces them:

| Remove | Why | Replace with |
|---|---|---|
| `Bind()` / `Unbind()` on every resource | Pure GL vestige. Every Vulkan implementation is an explicit no-op with a comment saying so. | Nothing. Resources are referenced by descriptor writes, not bound globally. |
| `GetID() → int` | `VulkanTexture2D.h:16` truncates a 64-bit `VkImage` handle into 32 bits. Unsafe and non-portable. | Return the native handle typed correctly, or a strongly-typed opaque handle. |
| `ChangeImageUnit()` / `ChangeTextureUnit()` | Mirrors `glBindImageTexture` / `glActiveTexture`. The Vulkan side reinterprets "unit" as a descriptor binding via a global registry keyed only on binding number, not `(set, binding)` — which works today by coincidence, not design. | Explicit descriptor set membership. |
| `AddData(offset, size, data)` | `glBufferSubData` semantics. This API is *why* the Vulkan buffers are single unsynchronized allocations. | Per-frame write into a ring slot. |
| `Image2DType` (LR_READ / LR_WRITE / LR_READ_WRITE) | GL access qualifier concept. `VulkanImage2D` never branches on it; all images get identical usage flags. | Actual `VkImageUsageFlags`. |
| `IRenderingContext::swapBuffers()` | Conflates submit, present, and acquire into one GL-shaped call, requiring an `m_FirstFrame` special case to work at all. | Explicit `beginFrame()` / `endFrame()` / `present()`. |

**1c. Fix the correctness bugs.** In rough priority order:

1. **Per-frame descriptor sets.** `VulkanComputeShader.cpp:113-118` allocates one descriptor set and rewrites it on every `Dispatch()`, while up to `MAX_FRAMES_IN_FLIGHT = 2` command buffers may still be executing. Allocate `MAX_FRAMES_IN_FLIGHT` sets and index by current frame.
2. **Per-frame buffer rings.** `VulkanUniformBuffer` and `VulkanShaderStorageBuffer` are single persistently-mapped allocations. The CPU overwrites frame N+1's data while the GPU is still reading frame N. Same fix: N copies, indexed by frame.
3. **`ReadData` synchronization.** `VulkanShaderStorageBuffer.cpp:89-90` has a comment admitting a barrier is needed, immediately followed by an unsynchronized `memcpy`. Fence and invalidate before the host read.
4. **Gate validation layers on build type.** `VulkanContext.h:166` hardcodes them on with a comment saying "Disable in release builds" that nothing acts on.
5. **Wire `RenderSettings::vSync` to swapchain present mode.** It currently has zero references anywhere in `Platform/Vulkan/`, so the toggle does nothing.
6. **Batch resource uploads.** `VulkanContext.cpp:502-533` does a full `vkQueueWaitIdle` per single-time command, fully serializing every texture and buffer upload.

**1d. Close the two Vulkan feature gaps.**

- The runtime splash screen is raw OpenGL with no `#else` (`RuntimeLayer.cpp:36-70, 218-308`). Either implement a Vulkan path or drop the feature deliberately. Don't leave it silently absent.
  - **DECIDED, Phase 1a: DROPPED.** Not re-implemented on Vulkan and not deferred — the feature is gone. `X3/res/made_with_X3.png` is retained (and still installed by `X3/CMakeLists.txt`) so Phase 13 can revive it cheaply. The runtime now presents black until the first compute frame arrives. This obligation is discharged.
- ImGui multi-viewport is disabled under Vulkan (`ImGuiContext.cpp:129-131`) pending per-viewport swapchains. This is an editor UX regression, not a blocker. Schedule it in Phase 13.

**Worth knowing:** the editor's Vulkan path is in better shape than `TODO_VULKAN.md` suggests. `ImGui_ImplVulkan_Init` is properly wired with a dedicated overlay render pass, and `ViewportPanel.cpp:45-102` registers the compute output through `ImGui_ImplVulkan_AddTexture` with a cached descriptor and correct teardown. That work is done.

**Exit criteria:** validation layers clean under load, no synchronization warnings, resize and swapchain recreation stable, both editor and runtime render correctly.

---

## Phase 2 — Mesh format and vertex attributes — **DONE (2026-07-29)**

*Foundational. See section 1.*

**Extend the vertex representation.** Move from three bare positions per triangle to indexed vertices carrying position, normal, UV0, and tangent. Assimp already generates normals and tangents under the preset you're passing — you simply aren't reading them.

Design considerations:

- **Keep the BVH consuming positions.** Traversal doesn't need attributes; only the hit shader does. Store positions in a traversal-friendly layout and attributes in a parallel buffer indexed by the same triangle index. This keeps BVH traversal cache behaviour unchanged and gives the rasterizer proper vertex buffers later.
- **You will need real vertex and index buffers regardless.** Forward+ rasterizes actual geometry through a graphics pipeline. The engine has none today. Build the buffers now in a form both consumers can use.
- **Per-triangle material index.** Materials are currently per-entity via `EntityHandle::materialIdx`. Multi-material meshes are extremely common and this needs to move to per-submesh at minimum.
- **Update the CPU/GLSL struct sync.** `Triangle`, `EntityHandle`, `Material`, and `BVHNode` are hand-mirrored between GLSL, `AssetTypes.h`, `BVHAccel.h`, and `Renderer.h`, with comments as the only guard. Phase 3 replaces this with generated code — until then, be careful, and consider adding `static_assert`s on `sizeof` and `offsetof` as a stopgap.

**Add texture sampling to the path tracer.** Once UVs exist, the path tracer should sample albedo, normal, and roughness maps. This is a prerequisite for the lightmap baker producing anything worth baking, and it's the visible payoff that makes this phase feel worthwhile.

**Texture management.** `AssetPool::TextureBuffer` is a flat `std::vector<unsigned char>` used only for the skybox. Real material textures need an array or bindless approach, mip generation, and eventually compression (Phase 9).

**Exit criteria:** smooth-shaded, textured, normal-mapped geometry in the path traced view.

---

## Phase 3 — Slang migration and reflection-driven codegen — **DONE (2026-07-29)**

*Done while the shader set is three files and ~1400 lines — after the rasterizer it would be ten times the work.*

**Toolchain.** Slang moved to Khronos open governance in November 2024 and is Apache 2.0. Releases are roughly weekly; v2026.14 shipped 2026-07-24. Its SPIR-V backend is production-grade and emits directly rather than routing through glslang. There's a vcpkg port (`shader-slang`) and prebuilt binaries. `slangc` drops into the existing `add_custom_command` pattern exactly like `glslc` did.

One relevant note given decision 1: Slang's **GLSL text output is officially "limited support."** That would have been the shakiest part of the plan if you'd kept OpenGL. You didn't, so it's moot — but don't rely on that target if a GL fallback ever comes back.

**What the migration buys, concretely:**

1. **Modules kill the triplication.** `PBR.comp` and `Phong.comp` differ by 16 lines across their first 260. Roughly 250 lines of BVH traversal, ray-AABB slab tests, and Möller–Trumbore are copy-pasted three ways, so every BVH fix must be made three times. Slang modules are real separate compilation with an IR and link step, not textual include.

2. **Generics and interfaces replace `#ifdef` permutation soup.** This matters enormously for Phase 6 — an `IBRDF` interface with conforming implementations lets one shading loop be written once and specialized per material type at link time. This is the mechanism that makes a layered material model affordable, and it's the main reason to do Slang before the material system rather than after.

3. **`ParameterBlock<T>` maps one-to-one onto a Vulkan descriptor set**, with bindings inside starting at 0. This directly replaces the hardcoded set 0/1/2 scheme.

4. **Reflection kills the hand-synced descriptor tables.** `VulkanComputeShader.cpp:19-42` declares every binding by hand, matched to the shader source by comment. Any new shader with a different layout silently breaks. Slang's reflection API exposes resolved set/binding assignments and struct layouts/offsets, with a JSON dump option — enough to generate the C++ descriptor tables *and* the struct definitions at build time.

   **Budget time to write this codegen yourself.** I searched specifically and found no established off-the-shelf tool that generates C++ descriptor layouts from Slang reflection JSON. The pattern is well supported and Slang's own docs describe it, but you're writing the script.

**Migration risks, honestly:**

- **Matrix conventions.** Slang follows HLSL-style semantics; GLSL is column-major with `mat * vec`. Multiplication order differences bite hard and silently. Verify every matrix multiply rather than transliterating.
- **Function renames.** `mix→lerp`, `fract→frac`, `inversesqrt→rsqrt`. Mechanical, but a wrong-function-picked bug produces no compile error.
- **Layout pinning.** Slang exposes `Std140DataLayout`, `Std430DataLayout`, `ScalarDataLayout` as explicit generic arguments plus global flags. Pin these deliberately so the C++ structs still match byte-for-byte; don't trust defaults.
- **Tooling maturity.** NVIDIA Nsight Graphics 2025.4 added beta Slang source stepping, and RenderDoc handles SPIR-V source debugging with `-emit-spirv-directly -g2`. Both are recent. Expect rough edges relative to glslang.

**Type mapping:** `layout(local_size_x=...)` → `[numthreads(x,y,z)]`, `gl_GlobalInvocationID` → `SV_DispatchThreadID`, `image2D` → `RWTexture2D<T>`, SSBOs → `StructuredBuffer<T>` / `RWStructuredBuffer<T>`, UBOs → `ConstantBuffer<T>`.

**Exit criteria:** three shaders become one shared module plus three thin entry points; descriptor tables generated at build time; path traced output pixel-identical to before.

---

## Phase 4 — Job system — **DONE (2026-07-29)**

*Independent of Phases 1-3.*

Recommend **enkiTS** — small, permissively licensed, proven, and deliberately unopinionated about what a task is. Taskflow is the alternative if you want a richer dependency-graph API.

Do **not** reuse Jolt's `JobSystemThreadPool` despite it already shipping. It's created and destroyed with the physics simulation lifecycle (`PhysicsLayer.cpp:19-21` deliberately avoids spinning it up when not simulating), and its barrier model is shaped around physics. Decoupling it would be more work than adopting a library, and you'd inherit an awkward API.

**What to parallelize first**, in value order:

1. **BVH builds** — currently synchronous and blocking inside `LoadMesh` (`AssetManager.cpp:232`). Per-mesh builds are embarrassingly parallel.
2. **Asset loading** — assimp import and stb_image decode, both currently main-thread and blocking.
3. **Lightmap baking** (Phase 10) — this is the one that genuinely *requires* a job system to be usable. Single-threaded baking of a real scene is a coffee break per iteration.
4. **Command buffer recording** (Phase 5) — parallel recording across render graph passes.

**Thread-safety audit required** on: `AssetManager` (the `AssetPool` buffers and version counters are written from load), `Log` (spdlog is thread-safe with the right sink, verify the configuration), and `Profiler` (`ScrollingBuffer` is not synchronized).

---

## Phase 5 — Render graph — **DONE (2026-07-29)**

*Built up front; here's what that means and the one risk to manage.*

The case for up-front is strong given where you're heading. Forward+ with a depth prepass, cluster culling, shadow cascades, velocity, DDGI probe update, GI apply, transparency, TAA, bloom, and tonemap is fifteen-plus passes. That is well past the point where hand-written barriers stop being tractable, and barrier bugs are the single most common source of subtle, hardware-dependent Vulkan failures — the kind that work on your GPU and corrupt on someone else's.

**The risk:** render graphs are hard to design well without knowing your passes, and you don't yet. Mitigate by keeping the first version deliberately minimal.

**Minimum viable render graph:**

- Passes declare resource reads and writes with intended usage. Nothing else.
- Automatic barrier insertion derived from those declarations.
- Automatic transient resource lifetime and aliasing.
- Linear execution order. **No pass reordering, no async compute in v1.** Both are real wins and both are where render graph complexity explodes. Add them once passes are stable.
- Debug visualization: dump the graph. You will need this constantly.

Explicitly out of scope for v1: multi-queue scheduling, cross-frame resource persistence beyond a simple history mechanism for TAA, and automatic memory budgeting.

---

## Phase 6 — Material model and shading library — **DONE (2026-07-29)**

*Roughly 4-6 weeks. Must precede the rasterizer — the raster shaders can't be written until the material representation is settled.*

Per decision 3: **OpenPBR is an authoring-side interchange format, not a runtime evaluator.** This phase builds the runtime side. The importer comes in Phase 12.

**Context on why this split is right.** OpenPBR is an ASWF specification (v1.1.1, 2026-04-17, Apache 2.0) that unified Autodesk Standard Surface and Adobe Standard Material into a layered slab model — coat, fuzz, thin-film, metal, specular, diffuse, subsurface, transmission with dispersion. It carries roughly 40-50 channels versus about 5 for glTF metallic-roughness. **No game engine ships it as a runtime model.** UE5 imports MaterialX/OpenPBR documents and translates them into native Substrate materials at authoring time — an asset conversion, not a live evaluator. That's precisely the pattern decision 3 chose, and it's the one with a shipping precedent.

**Design the runtime material struct.** Target something like 32-48 bytes: base colour, metallic, roughness, normal scale, occlusion, emission, plus a small feature bitfield and texture indices. Add optional lobes (clearcoat weight and roughness, sheen colour and roughness, anisotropy, thin-film thickness and IOR) as a second tier that only materials using them pay for.

**Build the BSDF library in Slang, once, shared by both renderers.** This is the payoff from doing Slang first. Define an `IBRDF`-style interface with conforming implementations, and write the shading loop generically over it. The same module is imported by the Forward+ fragment shader and the path tracer's hit shader, which means **the raster path and the reference path stay in agreement by construction** rather than by discipline. That property is worth a lot during bring-up.

**Permutation strategy.** Slang generics specialize at link time, so each material feature combination compiles to a tight shader with unused lobes eliminated. Maintain a small fixed set of tiers rather than a combinatorial explosion — something like: base (metallic-roughness only), extended (+ clearcoat and sheen), and hero (+ anisotropy and thin-film). Precompute expensive layer-interaction terms — multi-scatter energy compensation, Fresnel of coat over substrate — into LUTs baked once rather than derived per pixel.

**Adobe's reference BSDF is the implementation reference.** `github.com/adobe/openpbr-bsdf` is a standalone reference written in a macro-portable GLSL dialect targeting C++, GLSL, CUDA, MSL, and **Slang**, with specialization constants (`EnableSheenAndCoat`, `EnableDispersion`, `EnableTranslucency`, `EnableMetallic`) for compiling lobes out. It's a far better fit here than MaterialX ShaderGen — use it to validate your reduced model against spec behaviour, even though you're shipping the reduction rather than the full model.

**Why not MaterialX ShaderGen at runtime:** all its hardware generators derive from `HwShaderGenerator`, which is built around a vertex-stage plus pixel-stage pair with a `VERTEX_DATA` interconnect block. That's baked into the class hierarchy. **There is no compute-shader emission mode.** For an engine whose reference renderer is a single compute dispatch, ShaderGen output cannot be consumed wholesale. (A MaterialX → Slang codegen backend does exist as of MaterialX 1.39.5, May 2026 — but it inherits the same vertex/fragment constraint.)

---

## Phase 7 — Clustered Forward+ rasterizer — **DONE (2026-07-29)**

> **Outcome.** `DepthPrepass -> ClusterBuild -> LightCull -> Velocity -> SkyboxFill -> ForwardOpaque -> ForwardTransparent -> Tonemap`, selected by `ShaderType::FORWARD`. Every pass gated against the reference renderer rather than against a golden of itself: `forward-lights` vs `lights-pbr` agrees to a mean of **0.59 levels of 255**, and the residual is silhouettes — the rasterizer's coverage rule against the tracer's ray through the pixel centre, which is a sampling difference and not a shading one.
>
> **Four camera conventions, each of which failed silently.** The pass runs, the draws are issued, validation stays clean, and the buffer is empty or subtly wrong:
> - **+Z is forward**, so the projection is `perspectiveLH_ZO`. `MakeCameraRay` builds `float3(x, y, focalLength)`; a right-handed projection gives `w = -viewZ`, so everything actually visible is clipped and only geometry *behind* the camera survives.
> - **No `proj[1][1] *= -1`** — going left-handed already inverts Y, and flipping again renders a perfect, vertically mirrored image.
> - **`focalLength` is HORIZONTAL.** `CameraComponent` says so ("half of the screen width is 1") and `MakeCameraRay` divides both ray axes by `dims.x`. glm takes a vertical fov. Getting it wrong does not look broken, it looks like different framing.
> - **`VK_FRONT_FACE_COUNTER_CLOCKWISE`**, measured not reasoned: CCW is bit-identical to `CULL_MODE_NONE`, CW drops coverage 66% → 14% by culling the ground plane.
>
> **`SV_VertexID` is already the fetched index.** `vkCmdDrawIndexed` binds `MeshIndexBuffer`, so `gl_VertexIndex` is the value read *from* it. Indexing it again does not blank the frame — the scrambled vertices are still real vertices of the same mesh — which is exactly how it survived a session of debugging that had "concluded" the fetch path was fine.
>
> **Raster writes LINEAR radiance; Tonemap runs once at the end.** Required, not tidy: alpha compositing is only correct in linear light, and a forward pass that tonemapped per fragment produced visibly washed-out transparency. For opaque geometry the two are provably identical, which is what let every opaque golden survive the change bit-exact.
>
> **Draws are per SUBMESH**, which keeps the pass off `SV_PrimitiveID` — Vulkan gates `gl_PrimitiveID` in a fragment shader behind the `geometryShader` feature, which MoltenVK does not have. The geometric normal comes from `cross(ddx(P), ddy(P))` for the same reason.
>
> Accepted weaknesses, all deliberate: transparent sorting is **per object**, so interpenetrating or concave transparent meshes composite wrongly; there is no order-independent transparency.


*Roughly 6-10 weeks. The largest single phase. The engine has no graphics pipeline at all today.*

**What has to be built from nothing:** graphics pipeline creation and pipeline cache, vertex input state, depth prepass, framebuffer and attachment management, and a mesh draw submission path. None of this exists — the engine has only ever dispatched compute.

**Pass structure:**

1. **Depth prepass.** Also mitigates Forward+'s main weakness, overdraw.
2. **Cluster assignment and light culling** — a compute pass building the froxel grid and binning lights per cluster. This is the one part that reuses skills you already have.
3. **Opaque forward pass** — shade with the Phase 6 BSDF library, reading the cluster light list.
4. **Transparent forward pass** — same shading, back-to-front sorted, depth-test without depth-write. Forward+'s genuine advantage: transparents light identically to opaques with no separate code path.
5. **Velocity pass** — needed for TAA in Phase 11. Cheap to add now, annoying to retrofit.

**Known weaknesses you're accepting**, from the matrix you reviewed: decals are awkward without a G-buffer, screen-space subsurface scattering is awkward for the same reason, and dense alpha-tested foliage suffers from overdraw. The depth prepass helps the third. For decals, plan on forward decals — meaning decal projection happens in the material shader, which is a constraint worth knowing before you design the material system's texture indexing in Phase 6.

**Validate against the path tracer constantly.** Render the same scene both ways, diff the images. Direct lighting should converge closely. This is the workflow advantage of keeping the reference renderer, and it will catch shading bugs far faster than eyeballing.

**The tooling for this now exists** — `X3RenderTest` / `docs/RENDER-TESTS.md`, built between Phases 5 and 6. Add a scenario per pass and diff it against the path-traced reference; runs are bit-exact, so a real difference is unambiguous.

---

## Phase 8 — Shadows — **DONE (2026-07-29)**

> **Outcome.** Four cascades for the first directional light; every other light keeps tracing. **That asymmetry is the gate** — the reference still traces shadow rays, so if the cascaded lookup and the traced shadow disagree, `forward-lights` stops matching `lights-pbr`. Making both use the map would have them agree by construction and measure nothing. Agreement: **0.76 levels**, and the other two fixtures barely moved (1.77→1.83, 1.10→1.12).
>
> **One wide 2D atlas, four scissored sub-rects — not a texture array.** `VulkanImage` hardcodes `arrayLayers = 1`, `RenderGraph` hardcodes `layerCount = 1`, and the graph tracks one layout per whole image, so "cascade 0 written while cascade 1 read" is not expressible. The scissor is the load-bearing half: `renderArea` covers the whole atlas and a viewport transform does not clip.
>
> **Four choices inverted by reverse-Z**, each of which would silently produce a plausible-but-wrong image: border colour `FLOAT_OPAQUE_BLACK` (0 is the far plane, so black means "nearest occluder infinitely far" = lit); comparison `>=` not `<`; depth-bias constants **negative**; **front** faces culled in the shadow pass. Also: an `INT_` border colour on a float-format image is undefined behaviour, and this is the first sampler in the engine that reads its border at all.
>
> **Stability is asserted, not eyeballed.** `X3MathTest` (47 checks, no GPU, no display) proves properties that hold for any camera and light: splits ascend and reach the far plane exactly; every frustum slice fits its cascade at four yaws; rotation does not resize a cascade; **translation shifts it by whole texels**; a degenerate light gives finite matrices. It caught a real bug on its first run — `lookAtLH(center, center + L, up)` places the centre at its own origin, so its light-space coordinate is `(0,0,0)` and snapping is a no-op. Invisible in a still frame; shows only as crawling edges under motion.
>
> Soft shadows use a **deterministic** golden-angle disc, not a stochastic one: a random pattern converges in the tracer and stays noisy in the raster path, so the two would disagree by construction. Cost is banding instead of noise. Penumbra scales correctly — 11/19/26 px transition width for casters at 0.8/2.0/3.6 units.


*Roughly 3-4 weeks.*

**Cascaded shadow maps for the primary directional light.** Mature, cheap, extensively documented. Four cascades, stabilized to avoid shimmer under camera motion, with normal-offset or slope-scaled depth bias.

**BVH-traced shadow rays for soft shadows and secondary lights.** A shadow ray is a single BVH occlusion query — the cheapest possible reuse of the path tracer's intersector. Gives correct soft shadows with no cascade seams and no bias tuning, at the cost of tracing. Budget it per-light and fall back to shadow maps when the budget is exceeded.

**Skip virtual shadow maps.** They exist to solve Nanite-scale geometry density and carry meaningful overhead from non-virtualized draws. Without Nanite-class content they're not justified.

---

## Phase 9 — Asset cook pipeline — **PARTIAL (2026-07-30)**

> **Outcome — narrow on purpose.** `.x3mesh`: magic, version, a section table addressed by **ID** so new sections need no version bump, and **two independent layout guards** — `static_assert`s that break the *build* when a mirrored struct changes, and a header fingerprint that *refuses* an old file rather than loading it as progressively-wronger geometry. Neither substitutes for the other. The engine now **loads** cooked meshes (`X3_LOAD_COOKED=1`), with a staleness key of source size + mtime + filename — "is the cooked file newer" fails when a source is rolled back by a git checkout, leaving the cache newer *and wrong*.
>
> `X3AssetCook --self-test`: **66 checks**, including a round trip through the real load path at a **non-zero pool offset**, because a test at offset 0 cannot catch a missing rebase. Comparisons are `memcmp`, not value equality: a value compare calls `-0.0` and `+0.0` equal and they are different bytes going to the GPU.
>
> **NOT DONE:** BC7 texture compression, meshoptimizer, the binary scene format. `ProjectExporter` does not invoke the cook step, so the mode only helps against files cooked by hand.


*Roughly 4-6 weeks. Must land before Phase 10 — baked lightmaps need somewhere to live.*

Per decision 11: editor keeps the source workflow, export cooks.

**What's broken today.** Assets are re-imported from their original source paths on every project open (`.lrmeta` sidecars store `sourcePath`), synchronously and on the main thread, with BVH rebuilds each time. Moving or renaming a source file silently breaks a project. There is no compression, no binary format, and nowhere to store baked data.

**What the cook step produces:**

- **Compressed textures** — BC7 for desktop. Uncompressed textures are a large and completely unnecessary VRAM cost.
- **Prebuilt BVH**, serialized rather than rebuilt at load.
- **Optimized meshes** — vertex cache optimization, overdraw optimization, vertex fetch optimization. `meshoptimizer` does all three and is a small dependency.
- **Baked lightmaps and probe data** from Phase 10.
- **Binary scene format.** The YAML `.lrscn` format is right for the editor and wrong for shipping.

**Keep the editor on the source path** so iteration stays fast, and make the cook step part of `ProjectExporter`, which already does a competent job of producing a self-contained output folder with a renamed runtime binary. The dual load path is the cost of fast iteration; accept it, but add a "load cooked" mode in the editor so you can reproduce cook-only bugs without exporting.

---

## Phase 10 — Global illumination — **PARTIAL (2026-07-30)**

> **Outcome.** DDGI runs on the existing software BVH exactly as this plan predicted — `CheckRayCollision` is the intersection oracle, no RT hardware involved. 256 probes × 64 rays, octahedral irradiance and depth atlases with a one-texel border, Chebyshev visibility, one bounce per frame.
>
> **The light-leak scene this plan demanded found an ENGINE bug, not a GI tuning problem.** Sealed box, camera inside, ground truth 0.00 of 255: **128.10 → 51.40 → 1.77**.
> - **`IntersectTri` culled back faces unconditionally.** Right for camera and shadow rays; wrong for a ray starting *inside* geometry, which then sees only back faces and passes straight through the solid out to the sky. Every probe buried in a wall filled with skybox radiance. It also made **`Ray::frontFacing` dead code** — computed after traversal, and with back faces culled the test can only ever be true, so every consumer's back-face branch was unreachable. `g_TwoSidedTrace` is off by default; nothing else moved.
> - **The probe grid had four vertical levels.** A 13-unit auto-fitted volume gave 4.3 units of spacing, so a 3-unit-tall room contained **no probes at all**.
>
> Found via `ddgi-atlas`, a debug view of the probe atlas itself. Every other DDGI view answers "what does this surface receive"; that one answers "what do the probes *contain*", which is the only way to separate bad probe **data** from bad probe **selection**. Identical in a shaded image, nothing in common as bugs.
>
> Three parameters are the opposite of published defaults, each measured: the depth atlas must be **fp32** (Chebyshev subtracts two large near-equal numbers; in fp16 the result is noise and GI flickers in a way that reads as a race); `depthSharpness` **12 not 50** (at 64 rays, 50 gives 0.63 contributing rays per depth texel); `energyPreservation` **0.85 not 1** (the feedback gain is ~albedo × this, and at 1.0 a bright room brightens without bound).
>
> Lightmap UVs: region-grow charting compared against the chart **seed's** normal (a neighbour-relative rule lets the normal drift a threshold per step, so a cylinder grows one chart the whole way round and projects its far side onto its near side), skyline packing with a gutter, and a dilate pass. Output is **per-triangle-corner**, not per-vertex, because charting cuts the surface and splitting vertices would invalidate every BVH index. `X3LightmapTest`: **176 property checks**, mutation-tested.
>
> **NOT DONE:** there is **no lightmap bake pass** — UV generation only, and the UI says so. The residual 1.77 leak is recorded as the golden and is **not** zero: `AmbientIBL` samples the skybox with no occlusion anywhere DDGI is off, which is the remaining source. Not xatlas — planar projection, bounding boxes packed rather than chart outlines, so roughly half the usable resolution.


*Roughly 6-10 weeks. The phase where the path tracer investment pays off directly.*

**10a. Path tracer as lightmap baker.**

Retarget the existing tracer from a camera to lightmap UV space — iterate lightmap texels, trace from each texel's world position and normal, accumulate irradiance. The tracing code is unchanged; you're replacing the ray generation.

New work: **UV unwrapping** (integrate `xatlas` — this is the real cost of this sub-phase), lightmap atlas packing, and a dilate/seam-fix pass. Parallelize the bake across the Phase 4 job system; this is what makes it usable.

**10b. DDGI on the software BVH.**

DDGI is a grid of irradiance and visibility probes, updated each frame by tracing a modest number of rays per probe. The reference implementation traces via hardware RT, **but the algorithm is tracer-agnostic** — it needs a ray/scene intersection oracle and a probe update compute pass. Godot's SDFGI is described as a DDGI variant using SDFs instead of hardware ray tracing, which is the same substitution you're making with a BVH.

So probe ray dispatch is another entry point into the compute BVH intersector you already ship. This is the cleanest structural reuse in the entire plan.

Implementation: cascaded probe volumes around the camera, irradiance and depth/visibility octahedral maps per probe, temporal blending across frames, and the standard visibility-based leak mitigation (probes store depth moments, and irradiance lookups weight by the Chebyshev visibility test).

**Known issues to plan for:** light leaking through thin geometry is the classic DDGI failure and the visibility test is the mitigation, not a cure. Probe placement in interiors needs either heuristics or manual volumes. Ray budget per probe per frame is the main performance dial.

**10c. Combine.** Static geometry samples lightmaps, dynamic geometry samples probes, with a blend where they meet. Add SSAO for contact-scale occlusion that neither resolution captures.

---

## Phase 11 — Post-processing, temporal, and colour — **PARTIAL (2026-07-30)**

> **Outcome.** AgX, bloom, TAA, depth of field and motion blur, each with its own gate.
>
> **One tonemap module, three callers.** Reinhard was written out three times and happened to agree; a change to one would not have been caught by anything, and every raster-vs-reference comparison depends on the two landing in the same colour space. **The exposure scale is not optional**: Reinhard maps radiance 1.0 to mid-grey, so every light intensity and albedo in this project was authored against that response, while AgX expects 0.18. Dropping AgX in without the scale blew the image to white and reads as "AgX is broken" rather than "the exposure is wrong". Agreement *improved* across the change (0.76→0.59 levels).
>
> **TAA exposed two unordered read/write hazards**, both the same shape: reading and writing one storage image in a single dispatch. Neither crashes, neither trips validation — the first made three runs of the same build give three different rmse values, the second left a residual 0.0001, which is the worst size for a bug to be. Fixed by ping-ponging the history and splitting the resolve into resolve-then-copy. The jitter lives in a **separate matrix**: `viewProj` stays unjittered because the velocity pass projects with it, and folding the offset in makes TAA reproject by the jitter as well and the image swims.
>
> DoF weights each tap by its **own** circle of confusion, so a sharp foreground pixel cannot be dragged blurry by a blurry background neighbour. Measured: near sphere loses **16.7%** of its edge energy, the in-focus cube **2.6%**.
>
> **NOT DONE:** **XeSS** — it needs an external SDK that cannot be fetched in this environment. Volumetrics deferred by this plan itself. DoF and motion blur run on display-referred colour because the plan orders them after TAA; optically a blur belongs in linear light, and that is recorded at the site rather than silently fixed.


*Roughly 3-4 weeks. Highest fidelity-per-effort ratio in the entire plan. Do not defer it further than this.*

Ranked by perceived-quality return on implementation cost:

1. **TAA.** Roughly a week given velocity buffers already exist from Phase 7. The single largest image-quality improvement available.
2. **Tonemapping and colour management.** A few dozen lines for an enormous gain over naive clamping. **Recommend AgX** — it's Blender's default and has spread across open tooling, and it handles highlight desaturation far more gracefully than Reinhard. ACES is the alternative if film-pipeline matching matters more than looking good by default.
3. **Bloom.** A well-implemented progressive downsample/upsample bloom is cheap and disproportionately effective at selling HDR.
4. **Upscaling.** **XeSS is the vendor-neutral choice in 2026** — its cross-vendor HLSL/DP4a path runs on any SM6.4 GPU including AMD and NVIDIA. DLSS remains NVIDIA-only and proprietary. AMD has signalled intent to open-source the FSR4 wrapper while keeping core ML weights closed. Integrate XeSS as optional, after your own TAA works.
5. **Depth of field and motion blur.** Days each, meaningful cinematic payoff.
6. **Volumetrics.** Higher effort; defer until content demands it.

---

## Phase 12 — OpenPBR / MaterialX importer — **DONE AS SCOPED (2026-07-30)**

> **Outcome.** A targeted XML reader rather than the MaterialX library, exactly as this plan directs — no external dependency added. The reduction is documented as a table in the header **and reported per material from what the document actually sets**, so a dropped channel appears in the bake log instead of vanishing. **Silence keeps the engine default**: a document setting only `base_color` does not drag roughness from X3's 0.5 to OpenPBR's 0.3. Aliases cover OpenPBR v1.x, the draft names, and Autodesk Standard Surface — which is what Maya, Houdini and Substance actually write.
>
> `X3MtlxTest`: **102 checks**, mutation-tested.
>
> **NOT DONE:** validation against `adobe/openpbr-bsdf` is outstanding — offline, and the reference is not vendored. The header records exactly what that validation would consist of and which four mappings are expected to show large error.


*Roughly 3-5 weeks. Deliberately late — it's authoring-side and the runtime doesn't depend on it.*

Build an offline importer that reads MaterialX documents containing OpenPBR Surface nodes and bakes them down to the Phase 6 runtime material struct. This is the "authoring interchange" half of decision 3, and it's what buys you round-tripping with Substance, Maya, Houdini, and Arnold.

**Dependency decision to make here:** the MaterialX C++ library core is reasonably lean (C++17, Apache 2.0, uses stb_image), but `MATERIALX_BUILD_OIIO` and `MATERIALX_BUILD_OCIO` pull in substantially heavier trees. You only need document parsing and node graph evaluation, not ShaderGen. Consider whether a targeted `.mtlx` parser is cheaper than the full dependency — for a bake-time tool that only needs to read OpenPBR Surface parameter values, it likely is.

**The reduction is yours to define.** There is no blessed real-time subset of OpenPBR and no published reduced profile — the spec aspires to scale to real-time but doesn't say how. Document your mapping explicitly: which lobes map to which runtime parameters, which are approximated, which are dropped. Validate against Adobe's reference BSDF so you know the size of the error you're accepting.

Fix the broken `X3/libs/MaterialX` gitlink here if you go the full-library route.

---

## Phase 13 — Editor and runtime catch-up — **PARTIAL (2026-07-30)**

> **Outcome.** Asset deletion, entity hierarchy, physics gravity, a material editor and a lightmap bake UI.
>
> **The three original TODOs were all blocked by missing engine APIs**, and finding that out was most of the work. `AssetManager::RemoveAsset` was **declared and defined nowhere** — calling it was a link error. There was **no parent/child link in the engine at all**. The gravity control accepted edits and changed nothing, its backing `static` keeping them across project switches.
>
> Deleting a mesh is not a map erase: `MeshMetadata` offsets index shared append-only pool buffers, so removal compacts five buffers, rewrites every later asset's offsets, and **rebases `Gpu::TriRef`'s global vertex indices** — the part that renders garbage triangles if missed. The source file is only unlinked when it lives **inside** the project folder, because imports reference assets in place and the shipped fixtures point at `../SampleModels`; unconditional deletion would have destroyed the repo's sample models on the first click.
>
> **The transform caveat matters more than the hierarchy feature:** `TransformComponent`'s matrix is LOCAL and every consumer — renderer, physics, gizmo — treats it as WORLD. Parenting is organisational only; moving a parent does not move its children. `SetParent` therefore deliberately does **not** rebase into parent space, because under today's semantics that would make the child visibly teleport. `Scene::GetWorldMatrix` exists as the canonical composer, and the rebase must land in the same commit as the renderer change or all existing parented geometry jumps.
>
> Three pre-existing bugs fell out: `Scene::Copy` copied `ConstraintComponent`'s entity handle verbatim across registries; `LoadSceneFile` gave an entity with no `IDComponent` a fresh random GUID per lookup so its references never resolved; and `FirstPersonCameraComponent`/`FlowStateComponent` were in **neither** `Copy` nor `DuplicateEntity` while `RuntimeLayer` reads them every frame.
>
> Both new panels follow the rule this codebase settled on: **never present a control that does nothing**. The bake button is disabled and labelled "not implemented" because there is no bake pass; the progress bar reports completion only, with a tooltip saying why real progress needs the job system plumbed to the UI.
>
> **NOT DONE:** ImGui multi-viewport. Material-pool edits are live but never saved (`.lrmeta` stores only a GUID and a source path) — the panel offers "copy to scene overrides" as the route that persists, and says so in a banner.


*Ongoing, interleaved.*

- **ImGui multi-viewport under Vulkan** (`ImGuiContext.cpp:129-131`) — needs per-viewport swapchains, render passes, and framebuffers.
- **Runtime splash: dropped in Phase 1a.** Optional revival; `X3/res/made_with_X3.png` retained.
- **Render settings UI** for the new pipeline — cluster config, GI mode, probe density, shadow cascades, TAA and upscaler settings.
- **Lightmap bake UI** — trigger, progress, preview. Needs to be pleasant or nobody will bake.
- **Material editor** reflecting the Phase 6 struct, with live preview.
- **Existing TODOs**: asset deletion (`AssetsPanel.cpp`), child entity hierarchy (`SceneHierarchyPanel.cpp`), physics gravity wiring (`PhysicsSettingsPanel.cpp`).

---

## Cross-cutting concerns

**The component-maintenance smell will bite harder as this grows.** Adding a component today means editing `Components.h`, `Scene::Copy`, `Scene::DuplicateEntity`, and both serialization paths. Some components are *already* inconsistently handled — `LightComponent` and `FirstPersonCameraComponent` are serialized but not fully handled in duplicate/copy. Renderer work will add several components (lightmap UV set, probe volume, reflection probe, decal, material override). Consider a reflection or macro-based registration system before that happens, not after.

**Asset hot-reload** becomes much more valuable once materials and shaders are iterated on constantly. Slang's runtime compilation API supports session-based caching, which makes shader hot-reload genuinely practical.

**macOS/MoltenVK needs regular testing, not end-of-phase testing.** It's a translation layer with real feature gaps and different performance characteristics. Finding out at Phase 10 that a Phase 5 design doesn't work there is expensive.

---

## Sequencing and rough scale

```
P0  Hygiene              ░ days        ← DONE 2026-07-28
P1  Vulkan-only+correct  ███ 3-4w      ← DONE 2026-07-28
P2  Mesh attributes      ██ 2-3w       ← DONE 2026-07-29 (visual gate MET)
P3  Slang + codegen      ██ 2-3w       ← DONE 2026-07-29 (pixel-identity gate not run)
P4  Job system           █ 1-2w        ← DONE 2026-07-29
P5  Render graph         ███ 3-4w      ← DONE 2026-07-29
P6  Material + BSDF lib  ████ 4-6w     ← DONE 2026-07-29
P7  Forward+ raster      ███████ 6-10w ← DONE 2026-07-29
P8  Shadows              ███ 3-4w      ← DONE 2026-07-29
P9  Asset cook           ████ 4-6w     ← PARTIAL 2026-07-30 (no BC7 / scene format)
P10 GI: lightmaps+DDGI   ███████ 6-10w ← PARTIAL 2026-07-30 (no bake pass)
P11 Post/TAA/colour      ███ 3-4w      ← PARTIAL 2026-07-30 (no XeSS)
P12 OpenPBR importer     ███ 3-5w      ← DONE AS SCOPED 2026-07-30
P13 Editor catch-up      ░ interleaved ← PARTIAL 2026-07-30 (no multi-viewport)
```

**The duration estimates above were wrong by a large factor and are left unedited on purpose.** They read "roughly 12-18 months of solo work"; the phases were implemented across two days. Treat them as a record of what was expected, not as a scale to plan against. What the estimates *did* get right is the ordering and the three dependency constraints below — those held exactly.

**Roughly 12-18 months of solo work.** That estimate assumes sustained effort and will move considerably with available time and how much yak-shaving each phase turns up. Treat the ordering as more reliable than the durations.

**Three genuine dependency constraints**, everything else is negotiable:

- P1 blocks all rendering work. The engine currently has real synchronization hazards.
- P6 must precede P7. Raster shaders can't be written before the material representation exists.
- P9 must precede P10b. Baked lightmaps need a cooked asset format to live in.

**If you want visible progress sooner**, P2 and P11 are the two phases that most change what the engine looks like, and both are relatively cheap. P2 in particular takes the engine from flat-shaded and untextured to something that reads as modern.
