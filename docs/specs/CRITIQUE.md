I verified the four specs against the actual repo. Report follows.

---

# CRITIQUE OF SPECS A–D

## 0. Headline

**Spec D is not executable as written.** It is authored against the pre-Phase-1b API surface (`IShaderStorageBuffer::Create`, `VulkanComputeShader`, `VulkanContext`'s global binding registry, `ITexture2D::Create`) — every one of which Spec B deletes. Since D is Phase 2 and B is Phase 1b, D's §5–§7 mechanics must be rewritten onto B's layer before anyone starts. See §1.1.

**Specs B and C propose incompatible solutions to the same three bugs** (per-frame descriptor sets, per-frame buffer rings, deferred destruction). They cannot both land. See §1.2.

**Spec C contains five real defects that no other spec covers** (A4, A7, A8, A9, and the missing `set_required_features`). If C is discarded wholesale in favour of B, those bugs survive Phase 1. See §2.1.

---

## 1. CONTRADICTIONS

### 1.1 D vs B — D targets APIs that B deletes (BLOCKER)

| D says | Reality after B |
|---|---|
| §7.2 "two `IShaderStorageBuffer::Create(size, 7\|8, STATIC_DRAW)` members on `Renderer` (`Renderer.h:117`)" | `IShaderStorageBuffer` and `Renderer.h:117` are both gone (B §2.8, §5.2). Must be `VulkanBuffer` + `DescriptorWriter::storageBuffer(7/8, …)`. |
| §6.2 item 2 "Fix `descriptorCount`. `VulkanComputeShader.cpp:206` hardcodes `write.descriptorCount = 1`" | `VulkanComputeShader.cpp` is deleted (B §2.8). The equivalent is B's `DescriptorWriter`, which has *no* array-write method at all. |
| §6.2 item 3 "Array-aware registry … add `registerSampledImageArray` to `VulkanContext.h:44-56`" | B §4.4 deletes the entire registry (`VulkanContext.h:39-52`, `:175-178`, `VulkanContext.cpp:810-824`). |
| §6.2 item 4 "Add the descriptor table entry. `VulkanComputeShader.cpp:19-22`, set 0" | The table moves to `kSet0[]` in `Renderer.cpp` (B §5.1). |
| §6.4 "Add a format parameter … `ITexture2D.h:10`" | `ITexture2D` deleted; the equivalent is `TextureDesc::format` which B §2.4 **already has**. |
| §6.6/§6.7 mip chain + shared sampler in `VulkanTexture2D::createImage` | B §2.4 replaces this class with `VulkanTexture` and moves samplers to `VulkanContext::getSampler(SamplerDesc)`. D's `ImageDesc::mipLevels` already exists in B §2.3. |
| §5.3 "`pScene->MeshEntityLookupTable.emplace_back(…)`" with 8 args | B keeps this shape, but `Gpu::MeshEntityHandle` in D §1.4 has no constructor (unlike the current one at `Renderer.h:50-55`). Relies on C++20 parenthesised aggregate init inside `emplace_back`. Works at C++23 (`CMakeLists.txt:26`) but is a silent trap if anyone drops the standard. |

**Action:** D §5.3, §6.2, §6.4, §6.6, §6.7, §7.2 must be rewritten against B's `VulkanBuffer` / `VulkanTexture` / `VulkanDescriptorSetLayout` / `DescriptorWriter`. D's §1–§4 (the mesh format, BVH change, static_asserts, importer rewrite) are unaffected and stand.

### 1.2 B vs C — mutually exclusive designs for the same bugs

| Topic | B | C | Verdict |
|---|---|---|---|
| **Descriptor sets** | Delete `VulkanComputeShader`; `VulkanDescriptorSetRing` of `FRAMES_IN_FLIGHT` sets per (pipeline, set), written via `DescriptorWriter` with a completeness assert (§4). | Keep `VulkanComputeShader`; flat `m_DescriptorSets[frame * m_SetCount + set]`, keep `updateDescriptorSets()` pulling from the global registry (§1). | **Take B.** C §1 is ~150 lines of work on a file B deletes. |
| **Descriptor pool** | Reuse `m_DescriptorPool` (`VulkanContext.cpp:460-490`), which has `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` (`:479`) — B's `deferFreeDescriptorSets` requires it. | Dedicated per-shader pool with `flags = 0`, "no FREE_DESCRIPTOR_SET: sets live as long as the shader". | **Directly incompatible.** Take B's (reuse the shared pool with the FREE bit). |
| **Buffer rings** | New `VulkanRingBuffer` with `write(frame, …)`; explicitly argues (§1.4) "an offset-into-one-allocation signature *cannot* express 'write into this frame's slot'". | Keep `AddData(offset, size, data)` and hide the frame slot inside it via `currentFrameOffset()`; extend the registry structs with a `VkDeviceSize offset`. | **Take B.** C's design keeps exactly the GL-shaped API `ENGINE_PLAN.md:118` orders removed, and depends on the registry B deletes. |
| **Deferred destruction** | `ctx.deferDestroy(VkBuffer, VmaAllocation)` / `(VkImage, VmaAllocation, VkImageView)` / `deferFreeDescriptorSets`, drained in `beginFrame`. | Four separate `deferDestroy*` entry points + `PendingDelete` with five handle fields; routes `VulkanUniformBuffer.cpp:45`, `VulkanShaderStorageBuffer.cpp:46`, `VulkanImage2D.cpp:42/45`, `VulkanTexture2D.cpp:26/29/32` through it — all four classes B deletes. | Take B's API shape; C's §0.2 retire-frame arithmetic (`retireFrame <= m_CompletedFrame`) is the better-specified half — merge it in. |
| **`m_RenderPass`** | Keep it; add `beginSwapchainRenderPass()` using `m_RenderPass` (LOAD_OP_CLEAR) alongside `beginOverlayRenderPass()` (§2.7). | A2(c)(4): "**Delete `m_RenderPass`**, `createFramebuffers`' dependence on it, and the dead `beginRenderPass()`". | **Contradiction.** Note `createFramebuffers` builds framebuffers against `m_RenderPass` (`VulkanContext.cpp:256`) — the overlay pass currently reuses them by render-pass compatibility. If C wins, `createFramebuffers` must be repointed at `m_OverlayRenderPass`; neither spec says that. |
| **Semaphore indexing** | `present()` "advances `m_CurrentFrame`, `m_CurrentSemaphoreIndex` and `m_FrameNumber`" — keeps `m_CurrentSemaphoreIndex`. | A9(c)(1): "**Delete `m_CurrentSemaphoreIndex`** (`VulkanContext.h:154`, `.cpp:457`, `:711`)"; imageAvailable indexed by `m_CurrentFrame`, renderFinished by `m_ImageIndex`. | **Take C.** C is correct: `vkAcquireNextImageKHR` (`VulkanContext.cpp:351-352`) is not required to round-robin, so signal/wait pairing on `m_CurrentSemaphoreIndex` is unsound. B must adopt this. |
| **`useDoubleBuffering`** | Delete it (§5.2, §5.11) — `m_Frames[frame.index()]` subsumes it. | A10.6 agrees it should go, but §9 verification step 6 still says "Toggle `accumulate` and `useDoubleBuffering`". | Cosmetic inconsistency inside C. |

### 1.3 A vs B on the `I*` files — not a contradiction, but read the order

A §6 items 1–2 deliberately **keep** `IRenderingContext`, `IRendererAPI` and `IRendererAPI::Create()` through Phase 1a; B step 2 deletes all of them. That is correct layering. But B's *step 2* deletes `IRendererAPI.h` while its *step 3g* deletes `IComputeShader.h`, and `IRendererAPI.h:4` is `#include "Renderer/IComputeShader.h"` — the order works only in that direction. Do not reorder.

### 1.4 A vs D on `stbi_set_flip_vertically_on_load`

A §6 item 7 forbids touching `AssetManager.cpp:259`; D §6.5 removes it. Not a conflict — A defers to Phase 2, D executes in Phase 2 — but D must be the *only* one that touches it, and D's paired fix at `PathTracing.comp:207` (`0.5 - asin(...)`) must land in the same commit as D says, plus the identical lines in `PBR.comp` and `Phong.comp`.

---

## 2. GAPS — work owned by nobody

### 2.1 C's findings that B silently drops

These are real, verified, and B's rewrite does not address them:

- **A7 — swapchain images lack `VK_IMAGE_USAGE_TRANSFER_DST_BIT`.** Confirmed: `VulkanContext::createSwapchain` (`VulkanContext.cpp:149-173`) never calls `set_image_usage_flags`; vk-bootstrap's default is `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` alone (`X3/libs/vk-bootstrap/src/VkBootstrap.h:1002`). `blitImageToSwapchain` does `vkCmdClearColorImage` (`:887`) and `vkCmdBlitImage` (`:908`) into it. B's §2.7 `endFrame()` proposes "transitioning + **clearing** it if nothing rendered" — same violation. **B must adopt `add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)`.**
- **A4 — acquire semaphore waited at `COLOR_ATTACHMENT_OUTPUT_BIT` (`VulkanContext.cpp:417`) while the runtime path writes the image at `TRANSFER`.** B's `endFrame()` description doesn't mention `pWaitDstStageMask` at all.
- **A8 — overlay pass `loadOp = LOAD` (`VulkanContext.cpp:224`) with no `VK_ACCESS_COLOR_ATTACHMENT_READ_BIT` in either the subpass dependency (`:202`, shared with the main pass at `:238`) or the barrier (`:616`).** B keeps `beginOverlayRenderPass()` "unchanged".
- **A9 — semaphores destroyed in `recreateSwapchain` (`VulkanContext.cpp:686-691`) after only `vkDeviceWaitIdle` (`:665`), which does not cover outstanding presents.** Also: `set_old_swapchain(m_Swapchain)` at `:154` is always `VK_NULL_HANDLE` because `cleanupSwapchain()` runs first (`:678`) and nulls it (`:735`) — verified.
- **Device features.** `createLogicalDevice` (`VulkanContext.cpp:95-104`) requests *nothing*; `pickPhysicalDevice` (`:74-93`) sets no required features. Only D §6.2 item 1 adds `set_required_features_12`. B never touches device creation. If D's texture array lands, this must land with it.

### 2.2 Includes that A leaves behind and B doesn't list

`grep` for `#include "Renderer/IRendererAPI.h"` finds two sites in neither spec's edit table:

- `X3/src/Platform/Windows/GLFWWindow.cpp:8` — unguarded, unused. A §2.3 omits it; B §5.6 omits it. Breaks when B deletes the header.
- `X3-Editor/src/ImGuiContext.cpp:20` — A calls it "safe to delete; optional"; B §5.10 doesn't list it. Same breakage.

Also `X3/src/Renderer/IRendererAPI.h:4` includes `IComputeShader.h`, which is why A §4.4's "keep `#include "Renderer/IRendererAPI.h"` at `Renderer.h:5` — `Renderer.h` still needs it transitively" is true. Worth stating explicitly in the merged spec.

### 2.3 Descriptor arrays — the D↔B seam

D needs `layout(SET(0) binding = 2) uniform sampler2D u_MaterialTextures[128]`. B's `DescriptorBindingDesc` has a `count` field (§2.5) but `DescriptorWriter` has **no method that writes more than one descriptor**: every `uniformBuffer`/`storageImage`/`sampledImage` overload writes `descriptorCount = 1`, and its `m_ImageInfos` is a flat `std::vector` reserved by `maxWrites`. D assumes it can extend `VulkanComputeShader`, which is gone. **Nobody owns adding `DescriptorWriter::sampledImageArray(uint32_t binding, std::span<const VulkanTexture* const>)` and the contiguous-info-block invariant it needs.** Assign it to B (the layer owner), not D.

### 2.4 Phase 1d has no implementation owner

`ENGINE_PLAN.md:133` requires the runtime splash be re-implemented on Vulkan **or formally dropped**. A Commit 2 drops it (deleting `RuntimeLayer.cpp:35-71, 81-92, 108-110, 113-120, 123-173, 213-304`). B §5.9 explicitly says "Phase 1d … is out of scope for 1b; do not add a Vulkan splash here." No spec ever re-implements it. That is fine as a decision, but it should be recorded as a decision in `ENGINE_PLAN.md`, not left as an implicit consequence of a deletion commit. `X3/res/made_with_X3.png` exists and A correctly says to keep it.

### 2.5 Lifetime regression B introduces and does not close

B §5.7 changes `NewFrameRenderedEvent::frame` from `std::shared_ptr<IImage2D>` (`RenderEvents.h:12`) to a raw `VulkanImage*`, and `ViewportPanel::m_LatestRenderedFrame` from `std::weak_ptr` (`ViewportPanel.h:46`) to a raw pointer. B justifies this by dispatch being synchronous — true — but misses that **both consumers cache the pointer across frames**: `ViewportPanel.cpp:121` stores it and `:205` reads it on subsequent frames, and `RenderLayer::onUpdate` only dispatches when `m_ProjectManager->ProjectIsOpen()` (`RenderLayer.cpp:26`). Close a project (or shut down) and the panel holds a dangling `VulkanImage*` where `weak_ptr::lock()` previously returned null. B needs either a `NewFrameRenderedEvent(nullptr)` on project close, or the panel must clear it on a project-closed event.

### 2.6 Other unowned seams

- **A `TriCount` accounting bug D's rewrite inherits.** `AssetManager.cpp:196-198` sets `metadata->TriCount` from `sum(mNumFaces)`, but the emit loop skips non-triangle faces (`:218 if (face.mNumIndices != 3) continue;`). Under `aiProcess_Triangulate` this is normally a no-op, but it means `TriCount` can exceed the number of triangles actually appended — and `BVHAccel` (`:232`) is then built over a range that overruns into the next mesh. D's two-pass sizing (§4.3 step 2) must count *emitted* triangles, not `mNumFaces`. D doesn't say this.
- **`CheckRayCollision` clobbers the shadow-ray `t` limit.** `PathTracing.comp:314` does `ray.t = INF_T;` unconditionally, defeating `IsInShadow`'s `shadowRay.t = distToLight - SURFACE_BIAS` (`:345`). Result is still correct but shadow rays never early-out. D's §7.9 occlusion fast path is the right place to fix it; D doesn't mention the bug.
- **`Renderer.cpp:198`** — `m_Profiler->timer("Renderer::SetupGPUResources()")` discards the returned `std::shared_ptr<ScopeTimer>` (`Profiler.h:89`), so the timer destructs immediately and the measurement is meaningless. C §6(d) proposes using exactly this measurement to verify the upload batching. Fix the missing `auto t =` first or the verification is worthless.

---

## 3. FACTUAL ERRORS

### 3.1 `X3/CMakeLists.txt` line numbers are **−2 in all of A, B and D**

The file is 136 lines in the current worktree. From line ~17 onward every citation is two low:

| Spec claim | Actual |
|---|---|
| A §3.2(a) "lines 25-29" VMA/vk-bootstrap gate | **27-31** |
| A §3.2(b) "lines 49-54" source FILTER | **51-56** |
| A §3.2(c) "lines 64-78" API link block | **66-80** |
| A §3.2(d) "lines 118-123" define block | **120-125** |
| A "do not touch lines 80-105"; "`find_program` at line 81" | **82-107**; **83** |
| A §6 item 6 "`X3/CMakeLists.txt:133-134` installs `res/`" | **135-136** |
| B build note "`:47` globs `src/*.cpp`"; "line 50-54 filters" | **49**; **51-56** |
| D §3.1 "add `-I` to the glslc command at line 93"; "`DEPENDS` at line 94" | **95**; **96** |

A explicitly claims "All line numbers in this spec refer to the current working-tree content, not `HEAD`" — that claim is false for this file. (`X3/CMakeLists.txt` is modified in the worktree: the assimp `+6` block and the `-2` removals of `DXC_COMMAND` and `cxx_std_20`.) Every other file I spot-checked in A, B and C is accurate.

### 3.2 Spec D §6.2 item 5 — descriptor pool sizing is a *reduction*

D says "Raise `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` to **4096** and `maxSets` to **256**." `VulkanContext.cpp:480` already sets `maxSets = 1000`. Setting it to 256 is a 4× cut and would break ImGui's per-texture allocations. Should read "raise `maxSets` if needed; it is already 1000".

### 3.3 Spec D — `mesh.h` citations point at `aiAnimMesh`, not `aiMesh`

D's ground-truth table cites "`aiMesh::HasTextureCoords(0)`, `mTangents`, `mBitangents` | `mesh.h:488-498, 568, 588`" and its §4.2 table cites `HasNormals()` at `:559`, `HasTangentsAndBitangents()` at `:568`, `HasTextureCoords(0)` at `:588`. Those are all members of **`aiAnimMesh`**. The `aiMesh` versions are: `mTangents` **:707**, `mBitangents` **:718**, `HasNormals()` **:916**, `HasTangentsAndBitangents()` **:926**, `HasTextureCoords()` **:943**.

### 3.4 Smaller ones

| Spec | Claim | Actual |
|---|---|---|
| A §1.1 | "17 files, one directory" in `X3/src/Platform/OpenGL/` | **16 files** (the table itself lists 16 correctly) |
| A §3.2(e) | "`X3/Thirdparty/` is an empty directory" | The directory **does not exist**; the comment at `X3/CMakeLists.txt:12` is dead in a stronger sense than A says |
| A §4.6 | "`X3-Editor/res/EditorState.yaml` is `.gitignore`d (line 15)" | line **14**; line 15 is `X3-Editor/res/imgui.ini` |
| A §0 | Deleting `build/opengl-debug`/`build/vulkan-debug` because "the preset `binaryDir` changes and a stale cache will silently resurrect the removed variable" | `binaryDir` is `${sourceDir}/build/${presetName}` (unchanged); the *preset names* change, so `build/debug`/`build/release` are fresh dirs. The old caches are orphaned, not hazardous. Delete them anyway, but the stated rationale is wrong. |
| A §6 item 5 | viewport-packing comment "`VulkanContext.cpp:903-907`" | comment at **903-904**, `dstOffsets` at **905-906** |
| A §2.1 | "`IRendererAPI.cpp` (30 lines)"; "§4.2 `IRendererAPI.h` (29 lines)" | **29** and **28** |
| B §5.8 | "the `.lock()` sites above `:260`" | the only `.lock()` is `ViewportPanel.cpp:205` |
| B §5.2 | Cache size fields at "`Renderer.h:34-38`" | **35-38** (34 is the comment) |
| C §6(a) | "`VulkanTexture2D.cpp:105` / `:128` — the skybox upload" — correct, but C also lists `flushUploadsBlocking` callers including "`VulkanContext::init()` teardown" | `init()` (`VulkanContext.cpp:24-43`) has no teardown; the real out-of-frame path is `ImGuiContext.cpp:192-197` only |
| D §7 | "`PBR.comp` … hit shading at `:463`/`:367`" | unverified; I only confirmed the `PathTracing.comp` numbers, all of which are correct |
| D | "Both sample models (`SampleModels/*.glb`) embed their textures" | Files exist (`stanford_bunny_pbr.glb`, `stanford_dragon_pbr.glb`) but I could not verify embedding without a parser. Treat as unconfirmed. |

Everything else I checked in A, B, C is accurate — including all the load-bearing ones: `VulkanComputeShader.cpp:113-118/157-180/182-284/206/363/381-384`, `VulkanContext.cpp:344-396/398-458/460-490/492-500/502-533/535-576/578-637/639-661/663-717/810-824/826-954`, `VulkanContext.h:39-52/146/153-154/166/168/174-178`, `Renderer.cpp:17-18/26/32/44/201-209/219-234/238-249/253-302/306-351/356-378`, `ViewportPanel.cpp:45-102/284-293`, `ImGuiContext.cpp:129-133/143-151/157-201/217-265/274-297`, `RuntimeLayer.{h,cpp}` in full, `BVHAccel.{h,cpp}`, `AssetManager.{h,cpp}`, `PathTracing.comp`, and the zero-caller claims for `ChangeTextureUnit`, `ReadData`, `SetBindingPoint`, `SetViewportSize`, `Renderer::GetAPI/SetAPI`.

---

## 4. SEQUENCING

The four orderings do not compose. Concrete problems:

1. **C §8 step 6 and B step 2 are the same change** (restructure `Application::run`, delete `swapBuffers`/`ensureFrameStarted`/`m_FirstFrame`). Doing it twice is a merge conflict; doing it once means picking whose version of `endFrame`/`present` wins.
2. **C §8 steps 1–4, 9, 10 all operate on classes B deletes** (`VulkanComputeShader`, `VulkanUniformBuffer`, `VulkanShaderStorageBuffer`, `VulkanImage2D`, `VulkanTexture2D`). Running C first means writing them and then deleting them.
3. **C's own dependency chain is right and must be preserved**: §0.2 (deletion queue) *must* precede §6 (batched uploads), because `endSingleTimeCommands`' `vkQueueWaitIdle` (`VulkanContext.cpp:530`) is currently the only thing making `Renderer.cpp:202-203` / `:244` image recreation safe. B's step 1.2 adds the deletion queue and its step 3d adds staged uploads — same order, so B is fine, but *only* if B's step 1.2 actually lands the deletion queue before step 3.
4. **A's Commit 3 gate** (`grep X3_USE_VULKAN` → nothing) is correct, but note A Commit 1 already removed `find_package(OpenGL)`/`GLEW`, so `RuntimeLayer.h:5-7`'s `#include <GL/glew.h>` is dead-but-present until Commit 2. That's safe (the guard is never defined) and A says so — good.
5. **D step 1 ("pure rename + move, no behaviour change; verify the engine still renders identically") is impossible if Phase 1 is not finished**, since the renderer is mid-rewrite. D must start after B step 4.

**Recommended merged order:**

```
P0  A §0 commit                                    (already staged)
P1a A Commits 1–6                                  (unchanged; fix the CMake line offsets)
P1b/c  merged, in this order:
  1. B step 1 (new layer, additive) + C §0.1 frame counter + C §0.2 retire-frame maths
     folded into B's deferDestroy, + C §4 (validation gating, custom debug callback,
     syncval + best-practices) — do C §4 FIRST inside this step; it is the verification tool
  2. B step 2 (frame lifecycle) AND, in the same commit, C's A2 / A4 / A7 / A8 / A9:
       - no render pass in beginFrame
       - pWaitDstStageMask = TRANSFER | COLOR_ATTACHMENT_OUTPUT
       - swapchain add_image_usage_flags(TRANSFER_DST)
       - overlay pass gets its OWN subpass dependency with COLOR_ATTACHMENT_READ
       - imageAvailable[MAX_FRAMES_IN_FLIGHT] by m_CurrentFrame,
         renderFinished[imageCount] by m_ImageIndex; delete m_CurrentSemaphoreIndex
       - decide m_RenderPass: if deleted, repoint createFramebuffers at m_OverlayRenderPass
       - C's A9(6) fence-leak fix (vkResetFences moved next to vkQueueSubmit)
     + C §5 vSync → present mode (same function as A7's usage flag change)
  3. B steps 3a–3g, 4                              (C §1, §2, §3, §6, A1, A3, A5, A6, A10.6
                                                    are all subsumed; C §3's HOST_ACCESS
                                                    change and A5's compute-stage masks must
                                                    be carried into VulkanTexture/VulkanImage)
  4. B adds DescriptorWriter array support         (new; see §2.3)
  5. Device features for descriptor indexing       (D §6.2 item 1, moved here)
P2  D §1–§4 (format, BVH, static_asserts, importer, primitives)
    D §5–§7 REWRITTEN against B's layer
```

C's A10.1 (`VulkanRendererAPI::SetViewportSize`), A10.2 (`beginRenderPass`), A10.3 (`getMinImageCount`) are all covered by B's deletions. C's A10.4 is covered by B's per-image `m_ImGuiDescriptors` map. C's A10.5 (`VulkanImage2D.cpp:93` dead `dataSize`, `:119` `int` overflow) dies with the class.

---

## 5. UNDER-SPECIFIED, BY SEVERITY

**Severity 1 — an implementer will guess wrong**

1. **`endFrame()`'s "clear it if nothing rendered" (B §2.7).** B gives one sentence for the hardest part of the new lifecycle: what happens to the acquired swapchain image when neither the editor overlay pass nor the runtime blit touched it. Needs: exact layout tracking (`m_SwapchainImageLayout`), the exact barrier stages/access, and whether the clear is `vkCmdClearColorImage` (needs `TRANSFER_DST`) or a render pass with `LOAD_OP_CLEAR`. This is the seam where A4/A7/A8 live.
2. **Which render passes survive.** B keeps two, C keeps one. `createFramebuffers` (`VulkanContext.cpp:248-270`) hard-codes `framebufferInfo.renderPass = m_RenderPass` (`:256`). Whoever deletes `m_RenderPass` must repoint this. Unstated in both.
3. **`blitImageToSwapchain`'s new signature.** B §2.7 lists it under "unchanged getters" but B §5.9 says "Cleanest: change the signature to `void blitImageToSwapchain(const FrameContext&, VulkanImage& src, glm::ivec4 viewport, glm::ivec2 windowSize)`". Pick one. The Y-flip packing at `VulkanContext.cpp:905-906` and `CalculateViewportCoordinates()` (`RuntimeLayer.cpp:306-376`) must not change either way — A §6 item 5 is right about that.
4. **B's `DescriptorWriter::raw()` bridge during steps 3b–3f.** B says a not-yet-migrated binding "takes the raw descriptor info straight out of `VulkanContext::getBoundStorageBuffers()`". But by step 3a the resources are still old classes whose `Bind()` populates the registry — and `Renderer::Draw` no longer calls `Bind()` (B §5.3 deletes `Renderer.cpp:26` and `:371`). The old `Bind()` calls in `SetupGPUResources` (`Renderer.cpp:219/231/261/273/285/298/319/333/347`) must stay through step 3f. B doesn't say that.
5. **`m_LastRegisteredImageID` / `GetID()` transition.** A §6 item 3 correctly identifies `ViewportPanel.cpp:49-52` as the reason `IImage2D::GetID()` must survive 1a. B §5.8 replaces it with `image.id()` + a multi-entry map, but doesn't say when the map is evicted (it will grow one entry per resolution change forever, each holding a `VkDescriptorSet` from `m_DescriptorPool` whose `maxSets` is 1000).

**Severity 2 — costly to get wrong, recoverable**

6. **`FRAMES_IN_FLIGHT` vs `MAX_FRAMES_IN_FLIGHT`.** B §3 says "make `MAX_FRAMES_IN_FLIGHT` public (it is currently private at `VulkanContext.h:146`) or, better, define it *as* `FRAMES_IN_FLIGHT` and delete the duplicate", then writes a `static_assert` inside the private section that requires the first option. Pick one. (Note `getMaxFramesInFlight()` at `VulkanContext.h:88` already exposes the value.)
7. **`Renderer::Init(VulkanContext&)` vs `VulkanContext::Get()`.** B §5.2 takes a reference; B §5.7 has `RenderLayer.cpp:19` call `m_Renderer.Init(*VulkanContext::Get())`; B §5.6 has `Application` hold `_Context`. Three ways to reach the same singleton. Settle on one.
8. **Staging arena sizing and growth.** B §3 says "default 8 MiB … If a single request exceeds the remaining space, grow the arena (defer-destroying the old one)". Growing a *bump allocator whose earlier allocations are already referenced by recorded `vkCmdCopyBuffer` calls in the current command buffer* is not safe — the old buffer must stay alive until the frame retires (which defer-destroy does) *and* the already-recorded copies still reference it, which is fine, but new allocations must come from the new buffer while `stage()` returns the correct `VkBuffer` per allocation. B's `StagingAlloc` does return the buffer per-call, so it works — but the spec never says the arena can be multi-buffer within a frame. State it.
9. **D's `TriRef.i0/i1/i2` are documented as global indices** into `AssetPool::VertexBuffer`, but `MeshMetadata::firstVertexIdx` is also added. Which one does the shader use? D §7.7 uses `tr.i0` directly (global) — so `firstVertexIdx` is CPU-only bookkeeping. Say so, or an implementer will double-add it.
10. **D §5.4's slot-count reconciliation** is assigned to "the inspector's mesh-assignment path, not `Renderer::Parse`". `SceneHierarchyPanel.cpp:53`'s `GetOrAddComponent<MaterialComponent>()` is a second entry point. Both need it, or `Renderer::Parse` needs the `max(1u, materialSlotCount)` clamp D already specifies (it does — §5.3 — so this is survivable).
11. **A's optional cleanups are ambiguous.** "`#include "Renderer/IRendererAPI.h"` at line 2 of each [factory] … is unused but harmless … If you do remove it, remove it from all five together." Since B deletes all five files anyway, just say "leave them".

**Severity 3 — worth a sentence**

12. `X3-Editor/libs/imgui-docking/main.cpp` contains `int main(int, char**)` at line 38 and is globbed into the `imgui` static library (`imgui-docking/CMakeLists.txt:5`). A is right that it links today (unreferenced static-lib member), and right to leave it — but it is one `--whole-archive` away from a duplicate-`main` link error. Add `list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")` while you're editing that file.
13. C §9's `VK_LAYER_KHRONOS_VALIDATION_*` env-var names are hedged as "confirm the exact spelling against the installed SDK". Since nothing builds on this machine, that hedge is correct — but the merged spec should say "use `vkconfig`" as the primary path, not the env vars.
14. D §8.5 is right that `.spv` files must be rebuilt — Phase 0's `.gitignore` change is already staged (`.gitignore:18 *.spv`), so this is already handled; drop the caveat.