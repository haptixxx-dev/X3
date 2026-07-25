I have everything verified. Here is Part 1 of the merged spec.

---

# MERGED PHASE 1 SPEC — PART 1: Delete OpenGL and the GL-shaped interfaces

**Repo:** `/home/sarah/Coding/Haptixxx/X3`, branch `vulkan-migration`.
**Scope:** removal only. No Vulkan redesign, no resource-layer work, no correctness fixes, no dynamic-rendering work. Those are Parts 2–4 of this spec.
**Outcome:** a Vulkan-only tree with no `X3_GRAPHICS_API`, no `X3_USE_OPENGL`/`X3_USE_VULKAN`, no `X3/src/Platform/OpenGL/`, no GLEW/OpenGL link, two build presets, and no `RendererAPI` project setting.

## 0. Verification method used to write this part

Everything below was checked against the working tree on 2026-07-25, not against `HEAD`. Where a claim is "X is unused", the method is stated inline. Three claims were verified by **compiling**, not grepping, because the Phase 0 `DXC_COMMAND` regression proved grep-only verification insufficient:

- Baseline: `cmake --preset vulkan-debug && cmake --build build/vulkan-debug -j 14` → 0 occurrences of `error:` in the log; `build/vulkan-debug/Debug/X3Editor` (39,304,816 bytes) and `build/vulkan-debug/Debug/runtime/X3Runtime` produced.
- Baseline: `cmake --preset opengl-debug && cmake --build build/opengl-debug -j 14` → 0 occurrences of `error:`; `build/opengl-debug/Debug/X3Editor` (31,683,648 bytes) produced.
- Three single-TU compile probes (§3.9) were run using the exact command lines extracted from `build/vulkan-debug/compile_commands.json`, against copies in a scratch directory. **No repo source file was modified while writing this spec.**

## 1. DECISION — the runtime splash screen is DROPPED

`ENGINE_PLAN.md:144` (Phase 1d) requires that the OpenGL splash screen be *either* re-implemented on Vulkan *or* formally dropped, and states "Don't leave it silently absent." No part of this merged spec re-implements it.

**DECISION: the runtime splash screen is dropped. It is not re-implemented on Vulkan in Phase 1, and Phase 1d's splash obligation is hereby discharged as "dropped", not "deferred".**

Consequences to record:

- `X3/res/made_with_X3.png` (721,405 bytes, present on disk) is **retained**. Its only reader is `X3-Runtime/src/RuntimeLayer.cpp:40`, which this part deletes, so it becomes an orphaned asset — that is intentional. `X3/CMakeLists.txt:140-141` installs the entire `res/` tree, so it continues to ship. Retaining it costs ~700 KB and preserves the asset if Phase 13 ever revives the feature.
- The deletion is isolated in its own commit (Commit 2, §7) so it is trivially revertible and trivially citable from Phase 13.
- After Part 1 the runtime presents black until the first compute frame arrives.
- Add a line to `ENGINE_PLAN.md` §Phase 1d recording this decision, and to the Phase 13 bullet "Runtime splash on Vulkan, or formally drop it (from Phase 1d)" → change to "Runtime splash: **dropped in Phase 1a**. Optional revival; `X3/res/made_with_X3.png` retained."

## 2. Complete file inventory

### 2.1 Deleted outright — 16 files, one directory

`X3/src/Platform/OpenGL/` — `rm -r` the whole directory. Verified `ls | wc -l` = **16** (spec A's "17 files" was wrong; the count is 16):

```
OpenGLComputeShader.cpp        OpenGLComputeShader.h
OpenGLContext.cpp              OpenGLContext.h
OpenGLdebugFuncs.cpp           OpenGLdebugFuncs.h
OpenGLImage2D.cpp              OpenGLImage2D.h
OpenGLRendererAPI.cpp          OpenGLRendererAPI.h
OpenGLShaderStorageBuffer.cpp  OpenGLShaderStorageBuffer.h
OpenGLTexture2D.cpp            OpenGLTexture2D.h
OpenGLUniformBuffer.cpp        OpenGLUniformBuffer.h
```

Verified unused outside the directory:
- `grep -rn "Platform/OpenGL" X3/src X3-Editor/src X3-Runtime/src | grep -v '^X3/src/Platform/OpenGL/'` returns exactly 7 lines, all `#include`s inside `#ifdef X3_USE_OPENGL` blocks that §3 deletes: `IRendererAPI.cpp:4`, `ITexture2D.cpp:5`, `IImage2D.cpp:5`, `IUniformBuffer.cpp:5`, `IComputeShader.cpp:5`, `IShaderStorageBuffer.cpp:5`, `GLFWWindow.cpp:11`.
- `grep -rn "GLCall\|GLLogCall\|GLClearError" X3/src X3-Editor/src X3-Runtime/src | grep -v '^X3/src/Platform/OpenGL/'` returns **nothing**. `OpenGLdebugFuncs.h` has zero external consumers.
- The directory is already excluded from the Vulkan build by `X3/CMakeLists.txt:58`, and the Vulkan build links and runs, so nothing links against these objects.

**Not deleted:** `X3-Editor/libs/imgui-docking/imgui_impl_opengl3.cpp`, `imgui_impl_opengl3.h`, `imgui_impl_opengl3_loader.h`. These are vendored upstream Dear ImGui files. They stay on disk and are excluded by a CMake source filter (§4.5). Filtering keeps the vendored tree byte-identical to upstream; deleting them would fork it for no gain.

### 2.2 Files edited — 20 files

| # | File | What changes |
|---|---|---|
| 1 | `CMakeLists.txt` (root) | delete the `X3_GRAPHICS_API` cache variable |
| 2 | `CMakePresets.json` | 4 presets → 2 (`debug`, `release`) |
| 3 | `X3/CMakeLists.txt` | VMA/vk-bootstrap gate, source FILTER, link block, define block, dead comment |
| 4 | `X3-Editor/CMakeLists.txt` | drop conditional `find_package(OpenGL)` and `OpenGL::GL` link |
| 5 | `X3-Editor/libs/imgui-docking/CMakeLists.txt` | backend filter + Vulkan link become unconditional |
| 6 | `X3/src/Renderer/IRendererAPI.cpp` | factory `#ifdef`, `s_API` definition |
| 7 | `X3/src/Renderer/IRendererAPI.h` | `enum class API`, `GetAPI`, `SetAPI`, `s_API` |
| 8 | `X3/src/Renderer/IComputeShader.cpp` | factory `#ifdef` |
| 9 | `X3/src/Renderer/ITexture2D.cpp` | factory `#ifdef` |
| 10 | `X3/src/Renderer/IImage2D.cpp` | factory `#ifdef` |
| 11 | `X3/src/Renderer/IUniformBuffer.cpp` | factory `#ifdef` |
| 12 | `X3/src/Renderer/IShaderStorageBuffer.cpp` | factory `#ifdef` |
| 13 | `X3/src/Renderer/RenderSettings.h` | `RendererAPI` enum + member + both serialization lines |
| 14 | `X3/src/Renderer/Renderer.h` | `GetAPI`/`SetAPI` forwarders |
| 15 | `X3/src/Project/ProjectManager.cpp` | `SetAPI` block + now-unused include |
| 16 | `X3/src/Platform/Windows/GLFWWindow.cpp` | context creation + window hints `#ifdef`; unused `IRendererAPI.h` include |
| 17 | `X3-Editor/src/ImGuiContext.cpp` | five `#ifdef` sites; unused `IRendererAPI.h` include |
| 18 | `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.cpp` | four `#ifdef` sites |
| 19 | `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.h` | two `#ifdef` sites |
| 20 | `X3-Runtime/src/RuntimeLayer.h` + `.cpp` | splash + GL presentation removal |
| 21 | `TODO_VULKAN.md` | line 12 claims "All four builds compile" |

### 2.3 Files grep hits but must NOT be edited

- `X3/src/Project/Assets/AssetManager.cpp:259` — `stbi_set_flip_vertically_on_load(1); // OpenGL-style orientation`. See §6.7.
- `X3/src/Platform/Vulkan/VulkanImage2D.cpp:74`, `VulkanComputeShader.cpp:91`, `:96`, `VulkanContext.cpp:10`, `:903` — comments mentioning OpenGL. `VulkanContext.cpp:903` documents a live invariant (§6.5).
- `X3/res/shaders/PathTracing.comp:8-14`, `PBR.comp:6-12`, `Phong.comp:6-12` — the `SET(x)` macro block. Note the line numbers differ per file; spec A's ":6-14" for all three is wrong. See §6.12.
- `ORCHESTRATION.md`, `ENGINE_PLAN.md`, `docs/specs/*` — planning documents, not build inputs.

## 3. Per-file source edits

Convention: **SURVIVES** = the code left behind; **GUARD** = whether the preprocessor wrapper disappears.

### 3.1 `X3/src/Renderer/IRendererAPI.cpp` (29 lines, no trailing newline)

| Lines | Current | Action |
|---|---|---|
| 3–5 | `#ifdef X3_USE_OPENGL` / `#include "Platform/OpenGL/OpenGLRendererAPI.h"` / `#endif` | Delete all three lines. |
| 6–8 | `#ifdef X3_USE_VULKAN` / include / `#endif` | SURVIVES: `#include "Platform/Vulkan/VulkanRendererAPI.h"`. GUARD: gone. |
| 13–18 | `// Set API based on build configuration` + `#ifdef X3_USE_VULKAN` / `s_API = Vulkan` / `#else` / `s_API = OpenGL` / `#endif` | **Commit 3:** collapse to the single line `IRendererAPI::API IRendererAPI::s_API = IRendererAPI::API::Vulkan;` and drop the comment. **Commit 5:** delete that line entirely, when the declaration leaves the header. |
| 20–29 | `Create()` body: `#ifdef X3_USE_OPENGL` / `#elif defined(X3_USE_VULKAN)` / `#else` / `#endif` | SURVIVES: `return std::make_shared<VulkanRendererAPI>();`. GUARD: gone, including the `#else` branch's `LOG_ENGINE_CRITICAL("No graphics API defined at build time!");` and `return nullptr;`. |

Final file after Commit 5 is ~11 lines: two includes, `namespace X3 { std::shared_ptr<IRendererAPI> IRendererAPI::Create() { return std::make_shared<VulkanRendererAPI>(); } }`.

### 3.2 The five factory `.cpp` files

`X3/src/Renderer/IComputeShader.cpp`, `ITexture2D.cpp`, `IImage2D.cpp`, `IUniformBuffer.cpp`, `IShaderStorageBuffer.cpp`. All five are structurally identical; verified line-for-line. Same edit in each:

| Lines | Current | Action |
|---|---|---|
| 4–6 | `#ifdef X3_USE_OPENGL` / OpenGL include / `#endif` | Delete all three. |
| 7–9 | `#ifdef X3_USE_VULKAN` / Vulkan include / `#endif` | SURVIVES: the Vulkan `#include`. GUARD: gone. |
| 15–22 | `#ifdef X3_USE_OPENGL` / GL return / `#elif defined(X3_USE_VULKAN)` / Vulkan return / `#else` / `LOG_ENGINE_CRITICAL` + `return nullptr;` / `#endif` | SURVIVES: line 18 only, the Vulkan `return`. GUARD: gone; the `#else` fallback is deleted. |

Exact surviving return statements — copy verbatim, do not touch the argument lists:

```cpp
// IComputeShader.cpp
return std::make_shared<VulkanComputeShader>(filepath, workGroupSizes);
// ITexture2D.cpp
return std::make_shared<VulkanTexture2D>(data, width, height, textureUnit);
// IImage2D.cpp
return std::make_shared<VulkanImage2D>(data, width, height, imageUnit, imageType);
// IUniformBuffer.cpp
return std::make_shared<VulkanUniformBuffer>(size, bindingPoint, type);
// IShaderStorageBuffer.cpp
return std::make_shared<VulkanShaderStorageBuffer>(size, bindingPoint, type);
```

**`#include "Renderer/IRendererAPI.h"` at line 2 of each of the five: LEAVE IT.** It becomes unused once the `#else` branch's `LOG_ENGINE_CRITICAL` is gone, but all five files are deleted wholesale by the resource-layer part of this spec, so cleaning them up now is churn on a corpse. Do not remove it, do not agonize over it. (This resolves the ambiguity flagged as critique §5.11.)

### 3.3 `X3/src/Platform/Windows/GLFWWindow.cpp`

> The directory is named `Platform/Windows/` but contains the **cross-platform GLFW** window. It is not a Win32 backend. See §6.13.

| Lines | Current | Action |
|---|---|---|
| 8 | `#include "Renderer/IRendererAPI.h"` | **Delete.** Verified unused: `grep -n "IRendererAPI\|RendererAPI" GLFWWindow.cpp GLFWWindow.h` matches only this line. Verified by **compile probe** (§3.9 probe 1): the TU compiles clean without it. This is one of the two sites critique §2.2 identified as breaking when `IRendererAPI.h` is later deleted; removing it here closes the gap early. |
| 10–12 | `#ifdef X3_USE_OPENGL` / `#include "Platform/OpenGL/OpenGLContext.h"` / `#endif` | Delete all three. |
| 13–15 | `#ifdef X3_USE_VULKAN` / `#include "Platform/Vulkan/VulkanContext.h"` / `#endif` | SURVIVES: the include. GUARD: gone. |
| 25–30 | `// Set window hints based on renderer API` + `#ifdef X3_USE_VULKAN` / `VulkanContext::setWindowHints();` / `#else` / `OpenGLContext::setWindowHints();` / `#endif` | SURVIVES: `VulkanContext::setWindowHints();` (declared `X3/src/Platform/Vulkan/VulkanContext.h:23`, defined `VulkanContext.cpp:9`). GUARD: gone. Reword the comment to `// Vulkan needs GLFW_NO_API` or delete it. |
| 48–53 | `// Create appropriate context based on renderer API` + `#ifdef X3_USE_VULKAN` / `m_Context = new VulkanContext(m_NativeWindow);` / `#else` / `m_Context = new OpenGLContext(m_NativeWindow);` / `#endif` | SURVIVES: `m_Context = new VulkanContext(m_NativeWindow);`. GUARD: gone. |

`m_Context` stays typed `IRenderingContext*` (`GLFWWindow.h:53`) through Part 1. See §6.1.
`glfwSwapInterval(enabled)` at `:109` stays. See §6.8.

### 3.4 `X3-Editor/src/ImGuiContext.cpp` (314 lines)

> **Forward-reference for the implementer:** the Vulkan code inside these guards is preserved *verbatim* by Part 1, including `getOverlayRenderPass()`, `ensureFrameStarted()`, `beginOverlayRenderPass()` and `isRenderPassActive()`. Those calls are rewritten by the dynamic-rendering part of this spec, which is a **later commit**. Do not anticipate it here — collapse the guards and change nothing else.

| Lines | Current | Action |
|---|---|---|
| 4–6 | `#ifdef X3_USE_OPENGL` / `#include <imgui_impl_opengl3.h>` / `#endif` | Delete all three. |
| 7–11 | `#ifdef X3_USE_VULKAN` / `<vulkan/vulkan.h>`, `<imgui_impl_vulkan.h>`, `"Platform/Vulkan/VulkanContext.h"` / `#endif` | SURVIVES: all three includes. GUARD: gone. |
| 20 | `#include "Renderer/IRendererAPI.h"` | **Delete.** Verified unused: `grep -n "IRendererAPI\|RendererAPI" ImGuiContext.cpp` matches only this line. Verified by **compile probe** (§3.9 probe 2): the TU compiles clean without it (warnings from `entt.hpp` only, pre-existing). Second of the two sites from critique §2.2. |
| 36–41 | `// Shutdown the correct backend` + `#ifdef X3_USE_VULKAN` / `ImGui_ImplVulkan_Shutdown();` / `#else` / `ImGui_ImplOpenGL3_Shutdown();` / `#endif` | SURVIVES: `ImGui_ImplVulkan_Shutdown();`. GUARD: gone. Reword the comment to `// Shutdown the Vulkan backend`. |
| 129–131 | `#ifndef X3_USE_VULKAN` / `io.ConfigFlags \|= ImGuiConfigFlags_ViewportsEnable;` / `#endif` | **Inverted guard — the enclosed line is the OpenGL-only branch.** SURVIVES: **nothing**. Delete all three lines. Keep the explanatory comment at 132–133 and retarget it: `// Vulkan multi-viewport needs per-viewport swapchains — Phase 13.` |
| 157–201 | `#ifdef X3_USE_VULKAN` (158–197: Vulkan init) / `#else` (199–200: `ImGui_ImplGlfw_InitForOpenGL`, `ImGui_ImplOpenGL3_Init("#version 460")`) / `#endif` | SURVIVES: 158–197 unchanged. GUARD: gone. Delete 199–200. |
| 217–265 | `BeginFrame`: `#ifdef X3_USE_VULKAN` (218–262: swapchain-recreation re-init + `ImGui_ImplVulkan_NewFrame();`) / `#else` (264: `ImGui_ImplOpenGL3_NewFrame();`) / `#endif` | SURVIVES: 218–262 unchanged. GUARD: gone. Delete 264. |
| 274–297 | `EndFrame`: `#ifdef X3_USE_VULKAN` (275–294) / `#else` (296: `ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());`) / `#endif` | SURVIVES: 275–294 unchanged. GUARD: gone. Delete 296. |

**Leave lines 143–151 and 299–305 exactly as they are.** Both branch on `io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable`, which is never set once line 130 is deleted, so both become dead code. They are the Phase 13 re-enable point. See §6.11. Add one comment line above each: `// Dead while multi-viewport is disabled (Phase 13 re-enable point).`

### 3.5 `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.cpp` (verified `#ifdef` sites at 12, 21, 26, 284; `#else` at 290; `#endif` at 16, 23, 103, 293)

| Lines | Current | Action |
|---|---|---|
| 12–16 | `#ifdef X3_USE_VULKAN` / `"Platform/Vulkan/VulkanImage2D.h"`, `"Platform/Vulkan/VulkanContext.h"`, `<imgui_impl_vulkan.h>` / `#endif` | SURVIVES: all three includes. GUARD: gone. |
| 21–23 | destructor: `#ifdef X3_USE_VULKAN` / `CleanupVulkanResources();` / `#endif` | SURVIVES: the call. GUARD: gone. |
| 26–103 | `#ifdef X3_USE_VULKAN` / `CleanupVulkanResources()` and `GetImGuiTextureID()` definitions / `#endif` | SURVIVES: both function bodies verbatim. GUARD: gone. |
| 284–293 | `#ifdef X3_USE_VULKAN` (285–289: `GetImGuiTextureID` + `drawList->AddImage(textureID, TLImVec, BRImVec, {0,1}, {1,0});`) / `#else` (290–292: `// OpenGL can use the texture ID directly` + `drawList->AddImage((ImTextureID)(intptr_t)latestRenderedFrameShared->GetID(), …)`) / `#endif` | SURVIVES: 285–289. GUARD: gone. Delete 290–292 including the comment. |

Do not touch the UV arguments `{0, 1}, {1, 0}` — they are paired with `stbi_set_flip_vertically_on_load(1)` (§6.7).

### 3.6 `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.h` (73 lines)

| Lines | Current | Action |
|---|---|---|
| 8–10 | `#ifdef X3_USE_VULKAN` / `#include <vulkan/vulkan.h>` / `#endif` | SURVIVES: the include. GUARD: gone. |
| 63–71 | `#ifdef X3_USE_VULKAN` / `m_ImGuiTextureDescriptor`, `m_TextureSampler`, `m_LastRegisteredImageID`, `CleanupVulkanResources()`, `GetImGuiTextureID()` / `#endif` | SURVIVES: all five declarations. GUARD: gone. |

`int m_LastRegisteredImageID = -1;` at `:67` stays, and so does `IImage2D::GetID()`. See §6.3.

### 3.7 `X3-Runtime/src/RuntimeLayer.h` (67 lines, no trailing newline)

| Lines | Current | Action |
|---|---|---|
| 5–7 | `#ifdef X3_USE_OPENGL` / `#include <GL/glew.h>` / `#endif` | **Delete all three. Critical.** GLEW headers reach this TU only through `X3Engine`'s `PUBLIC` link to `GLEW::GLEW`; §4.4 removes that link, so this include stops resolving even under a stale `X3_USE_OPENGL`. |
| 8–11 | `#ifdef X3_USE_VULKAN` / `"Platform/Vulkan/VulkanContext.h"`, `"Platform/Vulkan/VulkanImage2D.h"` / `#endif` | SURVIVES: both includes. GUARD: gone. |
| 12 | `#include <chrono>` | **Delete.** Sole user is `m_SplashStartTime` (`:64`), which is deleted. |
| 30–32 | `#ifdef X3_USE_OPENGL` / `bool LoadLogoFromDisk(unsigned int*, int*, int*);` / `#endif` | **Delete the declaration and the guard.** |
| 36–40 | `#ifdef X3_USE_OPENGL` / `bool InitLogoResources();`, `void DestroyLogoResources();`, `void RenderLogo(float alpha);` / `#endif` | **Delete all three declarations and the guard.** |
| 48 | `unsigned int m_Framebuffer = 0;` | **Delete.** Unguarded but GL-only: written by `glGenFramebuffers`/`glDeleteFramebuffers` (`RuntimeLayer.cpp:109`, `:116`), read by `glBindFramebuffer` (`:164`), initialized at `:23` — all in deleted code. |
| 53 | `glm::ivec4 m_ViewportCoords; // x, y, width, height for glBlitFramebuffer` | **Keep the member.** Retarget the comment: `// x, y, x+width, y+height — consumed by VulkanContext::blitImageToSwapchain`. See §6.5. |
| 57–65 | `#ifdef X3_USE_OPENGL` / `m_ShowLogoScreen`, `m_LogoWidth`, `m_LogoHeight`, `m_LogoTexHandle`, `m_LogoVAO`, `m_LogoVBO`, `m_LogoProgram`, `m_LogoUniformLocationAlpha`, `m_LogoUniformLocationSampler`, `m_SplashStartTime` / `#endif` | **Delete the entire block including the guard and the `// splash screen (OpenGL only for now)` comment.** |

Keep `m_Window`, `m_Profiler`, `m_EventDispatcher`, `m_ProjectManager`, `m_CurrentFrame`, `m_ExportSettings`, `m_ViewportCoords`, `m_WindowSize`, `m_UpdateViewportCoordinates`, and `CalculateViewportCoordinates()`.

### 3.8 `X3-Runtime/src/RuntimeLayer.cpp` (377 lines)

| Lines | Current | Action |
|---|---|---|
| 6 | `#include <stb_image/stb_image.h>` | **Delete.** Verified sole users are `stbi_set_flip_vertically_on_load` (`:38`), `stbi_load` (`:42`), `stbi_image_free` (`:64`) — all inside the deleted splash loader. `grep -n "stbi_" RuntimeLayer.cpp` returns exactly those three. |
| 8–10 | `#ifdef X3_USE_OPENGL` / `#include <GL/glew.h>` / `#endif` | Delete all three. |
| 23 | `, m_Framebuffer(0)` in the constructor init-list | **Delete** (member removed, §3.7). |
| 26–31 | `#ifdef X3_USE_OPENGL` / `, m_ShowLogoScreen(true)` `, m_LogoWidth(0)` `, m_LogoHeight(0)` `, m_LogoTexHandle(0)` / `#endif` | **Delete.** Resulting init-list: `m_Window(window)`, `m_Profiler(profiler)`, `m_EventDispatcher(eventDispatcher)`, `m_ProjectManager(projectManager)`, `m_ViewportCoords(0,0,0,0)`, `m_WindowSize(0,0)`, `m_UpdateViewportCoordinates(false)`. |
| 35–71 | `#ifdef X3_USE_OPENGL` / `RuntimeLayer::LoadLogoFromDisk()` definition / `#endif` | **Delete the whole block.** |
| 81–92 | `onAttach`: `#ifdef X3_USE_OPENGL` / `if (m_ShowLogoScreen) { … InitLogoResources(); }` / `#endif` | **Delete the whole block.** `onAttach` then runs straight from `m_UpdateViewportCoordinates = true;` (`:79`) to the project-file scan at `:94`. |
| 108–110 | `onAttach`: `#ifdef X3_USE_OPENGL` / `glGenFramebuffers(1, &m_Framebuffer);` / `#endif` | **Delete.** `onAttach` now ends after the `UpdateRenderSettingsEvent` dispatch at `:106`. |
| 114–119 | `onDetach` body: `#ifdef X3_USE_OPENGL` / `if (m_Framebuffer) glDeleteFramebuffers(...); DestroyLogoResources();` / `#endif` | **Delete the body.** Keep the function — `onDetach()` is an `ILayer` override. It becomes `void RuntimeLayer::onDetach() {}`. (Note: the guard opens at **114**, not 113 as spec A claimed; `:113` is the function signature.) |
| 123–194 | `onUpdate`: `#ifdef X3_USE_OPENGL` (124–173: splash fade + `glBlitFramebuffer` presentation) / `#else` (174–193: Vulkan presentation) / `#endif` | **SURVIVES: 176–193** — the `if (m_CurrentFrame) { CalculateViewportCoordinates(); auto vulkanImage = std::dynamic_pointer_cast<VulkanImage2D>(m_CurrentFrame); … context->blitImageToSwapchain(…); }` block, verbatim. GUARD: gone. Delete 124–173. The `// Vulkan frame presentation` comment at `:175` may be dropped as redundant. |
| 213–304 | `#ifdef X3_USE_OPENGL` / `InitLogoResources()`, `DestroyLogoResources()`, `RenderLogo(float)` definitions / `#endif` | **Delete the whole block** (92 lines: inline GLSL 330 vertex/fragment source, VAO/VBO setup, `glDrawArrays`). |

`CalculateViewportCoordinates()` at **306–376** is **unchanged**. The Vulkan blit consumes its output; see §6.5.

After this edit `grep -rnE '\bgl[A-Z]|\bGL_|GLEW|GLuint|GLint' X3-Runtime/src` must return **nothing**.

### 3.9 Compile probes run to verify the "unused include" claims

Three probes, each compiling a scratch copy of the file with the exact command from `build/vulkan-debug/compile_commands.json` (PCH `-include` dropped, source dir added to `-I` so sibling includes resolve). No repo file was modified.

1. **`GLFWWindow.cpp` without `#include "Renderer/IRendererAPI.h"`** → compiled to a 847,288-byte object, zero diagnostics. Claim confirmed.
2. **`ImGuiContext.cpp` without `#include "Renderer/IRendererAPI.h"`** → compiled to a 1,066,552-byte object, only pre-existing `-Wdeprecated-literal-operator` warnings from `entt.hpp`. Claim confirmed.
3. **`Renderer.cpp` against a `Renderer.h` with `#include "Renderer/IRendererAPI.h"` (`:5`) AND the `GetAPI`/`SetAPI` forwarders (`:87-88`) removed**, injected via a shadowing `-I` — compiled to a 3,489,168-byte object, zero errors. This means `Renderer.h:5` is *also* removable, contradicting spec A §4.4's rationale and critique §2.2's endorsement of it: `Renderer.h` forward-declares `class IComputeShader;` **itself** at `Renderer.h:13`, so it does not need `IComputeShader.h` transitively through `IRendererAPI.h`. **Nevertheless: keep `Renderer.h:5`.** `IRendererAPI.h` survives Part 1 (§6.2) and every other consumer already gets it directly from `X3/src/X3.h:16`. Removing it buys nothing and it is deleted with the rest in a later part.

## 4. CMake changes

> **Every line number below was re-read from the current worktree on 2026-07-25.** The citations in specs A, B and D are stale — `X3/CMakeLists.txt` is now **141 lines** (last line has no trailing newline; `wc -l` reports 140), not the 134 spec A claims nor the 136 the critique claims. It gained the assimp `ASSIMP_BUILD_MINIZIP`/`ASSIMP_WARNINGS_AS_ERRORS` block (`:17-21`) and the Jolt `JPH_USE_VK`/`JPH_USE_DX12`/`JPH_USE_MTL` block (`:39-43`) since both were written.

### 4.1 `CMakeLists.txt` (root, 39 lines)

Delete lines **7–9**:

```cmake
# Graphics API selection (OpenGL or Vulkan)
set(X3_GRAPHICS_API "OpenGL" CACHE STRING "Graphics API to use (OpenGL or Vulkan)")
set_property(CACHE X3_GRAPHICS_API PROPERTY STRINGS "OpenGL" "Vulkan")
```

Nothing else in this file changes. `CMAKE_CXX_STANDARD 23` at `:26` is already correct post-Phase-0; `cmake_minimum_required(VERSION 4.1)` at `:1` is unchanged.

**This deletion is what makes the rest of §4 mandatory in the same commit.** The default value is `"OpenGL"`, so every `if(X3_GRAPHICS_API STREQUAL "Vulkan")` in the tree is *currently false by default*. Deleting the variable leaves them false, and they fail at **link** time, not configure time. All of §4 lands together or not at all.

### 4.2 `CMakePresets.json` — full replacement

Four presets → two. `binaryDir` is `${sourceDir}/build/${presetName}`, unchanged, so the new build directories are `build/debug` and `build/release`. The old `build/opengl-debug/` and `build/vulkan-debug/` become **orphaned, not hazardous** — CMake never reads them again because the preset names changed. (Spec A's rationale, "a stale cache will silently resurrect the removed variable", is wrong; the critique caught this and it is corrected here.) Delete them anyway to avoid confusion and reclaim disk.

```json
{
    "version": 6,
    "cmakeMinimumRequired": {
        "major": 3,
        "minor": 21,
        "patch": 0
    },
    "configurePresets": [
        {
            "name": "base",
            "hidden": true,
            "binaryDir": "${sourceDir}/build/${presetName}",
            "cacheVariables": {
                "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
            }
        },
        {
            "name": "debug",
            "displayName": "Debug",
            "inherits": "base",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug"
            }
        },
        {
            "name": "release",
            "displayName": "Release",
            "inherits": "base",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Release"
            }
        }
    ],
    "buildPresets": [
        {
            "name": "debug",
            "configurePreset": "debug"
        },
        {
            "name": "release",
            "configurePreset": "release"
        }
    ]
}
```

### 4.3 `X3/CMakeLists.txt` (141 lines) — five edits

**(a) Line 12** — delete the dead comment:
```cmake
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/Thirdparty/glew-cmake)
```
`X3/Thirdparty/` **does not exist** (verified: `ls X3/Thirdparty` → "No such file or directory"). Spec A said it was "an empty directory"; it is absent entirely. The neighbouring comment at `:11` (`Thirdparty/glfw`) is equally dead — delete it too, or leave both; not load-bearing either way. Prefer deleting both.

**(b) Lines 27–31** — VMA and vk-bootstrap become unconditional:

```cmake
# Vulkan libraries
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/VulkanMemoryAllocator)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/vk-bootstrap)
```

replacing:
```cmake
# Vulkan libraries (only if building with Vulkan)
if(X3_GRAPHICS_API STREQUAL "Vulkan")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/VulkanMemoryAllocator)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/vk-bootstrap)
endif()
```

Both are live git submodules with populated directories (`X3/libs/VulkanMemoryAllocator`, `X3/libs/vk-bootstrap`) — verified, and both are compiled into the working `vulkan-debug` build.

**(c) Lines 56–61** — the source-list FILTER. **Two-stage across two commits:**

- **Commit 1** (OpenGL sources still on disk): replace the whole `if/else/endif` with one unconditional line, so the tree stays buildable while `X3/src/Platform/OpenGL/*.cpp` still exist and would otherwise be globbed into `X3_SOURCES` by `file(GLOB_RECURSE ...)` at `:54`:
  ```cmake
  # Filter out the OpenGL backend (deleted in a following commit)
  list(FILTER X3_SOURCES EXCLUDE REGEX ".*/Platform/OpenGL/.*")
  ```
- **Commit 4** (after `git rm -r X3/src/Platform/OpenGL/`): **delete that line and its comment entirely.**

**(d) Lines 71–85** — the API-specific link block collapses to:

```cmake
# Vulkan + windowing
find_package(Vulkan REQUIRED)
target_link_libraries(X3Engine PUBLIC Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
find_package(glfw3 REQUIRED)
target_link_libraries(X3Engine PUBLIC glfw)
```

Removed: `find_package(OpenGL REQUIRED)` (`:81`), `find_package(GLEW REQUIRED)` (`:82`), and the `OpenGL::GL GLEW::GLEW` entries from the link line at `:84`. **`find_package(glfw3 REQUIRED)` and the `glfw` link must survive** — GLFW is the windowing layer for Vulkan too. See §6.9.

**(e) Lines 125–130** — the graphics-API define block. **Two-stage across two commits:**

- **Commit 1**: replace
  ```cmake
  # Graphics API define
  if(X3_GRAPHICS_API STREQUAL "Vulkan")
      target_compile_definitions(X3Engine PUBLIC X3_USE_VULKAN)
  else()
      target_compile_definitions(X3Engine PUBLIC X3_USE_OPENGL)
  endif()
  ```
  with the single unconditional line
  ```cmake
  target_compile_definitions(X3Engine PUBLIC X3_USE_VULKAN)
  ```
  The `#ifdef X3_USE_VULKAN` sites in source still need it at this point.
- **Commit 3** (after every `#ifdef X3_USE_VULKAN` site is collapsed): **delete that line and the `# Graphics API define` comment entirely.**

**Do not touch lines 87–112** — the `glslc` → SPIR-V custom commands. `find_program(GLSLC_EXECUTABLE NAMES glslc HINTS Vulkan::glslc REQUIRED)` is at **`:88`** (spec A said 81) and already runs unconditionally. It must continue to.

**Do not touch lines 32–44** — the Jolt Physics configuration. In particular **`set(JPH_USE_VK OFF CACHE BOOL "" FORCE)` at `:41` is load-bearing and must not be removed.** Jolt auto-enables `JPH_USE_VK` when Vulkan is found, and then tries to compile its test HLSL shader with `dxc`. §4.3(b) makes Vulkan unconditional, which makes this `OFF` more important, not less. This is the exact shape of the Phase 0 `DXC_COMMAND` regression (a grep-only "it's dead" conclusion that was dead for OpenGL and load-bearing for Vulkan); the current tree fixes it by disabling Jolt's GPU backends rather than by hardcoding a dxc path. **Leave `:39-43` alone.**

**Do not touch lines 140–141** — `install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/res/ DESTINATION engine_res)`. This is what keeps `made_with_X3.png` shipping (§1).

### 4.4 `X3-Editor/CMakeLists.txt` (55 lines)

Replace lines **3–7** and **14–18**:

```cmake
# Find GLFW to provide dependencies for imgui
find_package(glfw3 REQUIRED)
```
```cmake
# Provide GLFW dependencies to imgui target
target_link_libraries(imgui PRIVATE glfw)
```

i.e. delete `if(NOT X3_GRAPHICS_API STREQUAL "Vulkan")` / `find_package(OpenGL REQUIRED)` / `endif()` (`:5-7`) and `if(NOT X3_GRAPHICS_API STREQUAL "Vulkan")` / `target_link_libraries(imgui PRIVATE OpenGL::GL)` / `endif()` (`:16-18`), and retarget the comments at `:3` and `:14`.

Nothing else changes. `add_dependencies(X3Editor X3Runtime)` at `:33` stays (breaking it is an `ENGINE_PLAN.md:100` Phase 0 item, not this part's business).

### 4.5 `X3-Editor/libs/imgui-docking/CMakeLists.txt` (27 lines, no trailing newline) — **HIGHEST-RISK FILE**

This file is not mentioned in `ENGINE_PLAN.md` Phase 1a and is the single most likely miss. Both of its `if(X3_GRAPHICS_API STREQUAL "Vulkan")` blocks (`:12` and `:24`) evaluate **false** once §4.1 deletes the variable, which would (a) compile `imgui_impl_opengl3.cpp` into the `imgui` static library and *exclude* `imgui_impl_vulkan.cpp`, and (b) drop `Vulkan::Vulkan` from the link — producing undefined references to every `ImGui_ImplVulkan_*` symbol referenced by `ImGuiContext.cpp` and `ViewportPanel.cpp`. **It fails at link time, not configure time.**

**Note on critique §5.12 — the `main.cpp` filter is ALREADY PRESENT.** A Phase 0 agent added it. Verified: `X3-Editor/libs/imgui-docking/CMakeLists.txt:7-9` currently reads

```cmake
# main.cpp is ImGui's own demo application. It defines main() and calls the
# OpenGL backend directly, so it breaks the link once a backend is excluded.
list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
```

and `git diff` confirms it is a worktree modification not yet committed. `X3-Editor/libs/imgui-docking/main.cpp` does define `int main(int, char**)` at **line 38**. **Do not add this filter a second time. Preserve it verbatim.** Its comment is accurate: without it, `main.cpp` calls `ImGui_ImplOpenGL3_*` directly and breaks the link the moment `imgui_impl_opengl3.cpp` is excluded.

Replace lines **11–27** with:

```cmake
# imgui_impl_opengl3.cpp is vendored upstream but never built — Vulkan-only engine.
list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*imgui_impl_opengl3\\.cpp$")

target_sources(imgui PRIVATE "${IMGUI_SOURCES}")

target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Vulkan REQUIRED)
target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan)
```

Full resulting file:

```cmake
cmake_minimum_required(VERSION 3.16)
project(imgui)

add_library(imgui)
file(GLOB_RECURSE IMGUI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# main.cpp is ImGui's own demo application. It defines main() at line 38 and calls
# the OpenGL backend directly, so it breaks the link once a backend is excluded.
list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

# imgui_impl_opengl3.cpp is vendored upstream but never built — Vulkan-only engine.
list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*imgui_impl_opengl3\\.cpp$")

target_sources(imgui PRIVATE "${IMGUI_SOURCES}")

target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Vulkan REQUIRED)
target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan)
```

`imgui_demo.cpp` is globbed and compiled — that is pre-existing and fine; it has no `main()`.
`imgui_impl_glfw.{cpp,h}` are globbed and compiled — **required**. See §6.10.

### 4.6 `X3-Runtime/CMakeLists.txt`

**No changes.** It links only `X3Engine` (`:9`), inheriting Vulkan and GLFW through `PUBLIC` propagation. This is precisely *why* `#include <GL/glew.h>` in `RuntimeLayer.h:6` resolves today, and why §3.7 must remove it in the same series.

### 4.7 `TODO_VULKAN.md`

Line **12** reads:
```
- **Build configurations** — All four builds compile (OpenGL Debug/Release, Vulkan Debug/Release)
```
Replace with:
```
- **Build configurations** — Vulkan-only Debug/Release both compile
```
Cosmetic; do it in the docs commit.

## 5. The ProjectManager `RendererAPI` trap

`ENGINE_PLAN.md:118` describes it: `ProjectManager.cpp:114-119` reads a `RendererAPI` enum from the project file and calls `IRendererAPI::SetAPI()`, but every factory resolves by `#ifdef`. A project marked "Vulkan" opened in an OpenGL binary *reported* Vulkan and silently used OpenGL objects. Five coupled sites. Delete them all in one commit.

### 5.1 `X3/src/Renderer/RenderSettings.h` (79 lines)

| Lines | Content | Action |
|---|---|---|
| 14–17 | `enum class RendererAPI { OpenGL = 0, Vulkan = 1 };` | **Delete.** Verified referenced only from `:29`, `:31`, `:45`, `:61` of this file and `ProjectManager.cpp:114`. |
| 27–32 | `// Use Vulkan on macOS (OpenGL 4.1 lacks compute shaders), OpenGL elsewhere` + `#ifdef __APPLE__` / `RendererAPI rendererAPI = RendererAPI::Vulkan;` / `#else` / `RendererAPI rendererAPI = RendererAPI::OpenGL;` / `#endif` | **Delete lines 27–32 entirely.** Keep `:26` (`ShaderType shaderType = ShaderType::PATH_TRACING;`). This removes the only `__APPLE__` conditional in the struct. |
| 45 | `rsNode["rendererAPI"] = static_cast<int>(rendererAPI);` in `SerializeToYamlNode` | **Delete.** |
| 61 | `if (auto n = rsNode["rendererAPI"]) rendererAPI = static_cast<RendererAPI>(n.as<int>());` in `DeserializeFromYamlNode` | **Delete.** |

`enum class ShaderType` (`:8-12`) is unrelated and **stays**. `bool vSync = true;` (`:38`) and its deserialization at `:70` **stay** — vSync-to-present-mode wiring is a later part of this spec, not a dead field.

### 5.2 `X3/src/Renderer/IRendererAPI.h` (29 lines, no trailing newline)

| Lines | Content | Action |
|---|---|---|
| 11–15 | `enum class API { None = 0, OpenGL = 1, Vulkan = 2, };` | **Delete**, along with the now-redundant `public:` at `:10` (there is a second `public:` at `:17`). |
| 23 | `static API GetAPI() { return s_API; }` | **Delete.** |
| 24 | `static void SetAPI(API api) { s_API = api; }` | **Delete.** |
| 26–27 | `private:` / `static API s_API;` | **Delete.** |

Resulting class:
```cpp
class IRendererAPI {
public:
    virtual void Init() = 0;
    virtual void Clear(const glm::vec4& color) = 0;
    virtual void SetViewportSize(uint32_t width, uint32_t height) = 0;

    static std::shared_ptr<IRendererAPI> Create();
};
```

**The class and its `Create()` factory both survive Part 1.** See §6.2.

### 5.3 `X3/src/Renderer/IRendererAPI.cpp`

Delete the `s_API` **definition** (the single line left after §3.1's Commit-3 collapse). If this is missed, the TU will not link once the declaration leaves the header.

### 5.4 `X3/src/Renderer/Renderer.h`

Delete lines **87–88**:
```cpp
inline static IRendererAPI::API GetAPI() { return IRendererAPI::GetAPI(); } // getter
inline static void SetAPI(IRendererAPI::API api) { IRendererAPI::SetAPI(api); } // setter
```
Verified dead: `grep -rn "Renderer::GetAPI\|Renderer::SetAPI"` across `X3/src`, `X3-Editor/src`, `X3-Runtime/src` returns **zero** call sites. Keep `#include "Renderer/IRendererAPI.h"` at `Renderer.h:5` (see §3.9 probe 3 — it is removable but pointless to remove).

### 5.5 `X3/src/Project/ProjectManager.cpp`

Delete lines **113–119**:
```cpp
// Set the renderer API based on project settings
IRendererAPI::API rendererAPI = (m_ProjectFile.runtimeRenderSettings.rendererAPI == RendererAPI::Vulkan)
    ? IRendererAPI::API::Vulkan
    : IRendererAPI::API::OpenGL;
IRendererAPI::SetAPI(rendererAPI);
LOG_ENGINE_INFO("OpenProject: Setting renderer API to {}",
    (rendererAPI == IRendererAPI::API::Vulkan) ? "Vulkan" : "OpenGL");
```

Then delete `#include "Renderer/IRendererAPI.h"` at **line 4** — verified to be the file's only remaining `IRendererAPI` reference after the above (`grep -n "IRendererAPI\|RendererAPI" ProjectManager.cpp` → `:4`, `:114`, `:116`, `:117`, `:119` only).

`OpenProject()` then flows from the `if (!projectFile)` warning at `:109-111` straight to `m_AssetManager = std::make_shared<AssetManager>();` at `:121`.

### 5.6 Serialization compatibility — **removing the YAML key breaks nothing. Definitive answer.**

I read both loaders in full. **No migration, no version bump, no compatibility shim, no fixture edits.**

**Reading old `.lrproj` files that still contain `rendererAPI:`.** `RenderSettings::DeserializeFromYamlNode` (`X3/src/Renderer/RenderSettings.h:54-77`) is a sequence of independent optional lookups against a `YAML::Node`:

```cpp
*this = RenderSettings{};                                       // :55  reset to defaults
try {
    if (auto n = rsNode["debugMode"]) debugMode = n.as<int>();  // :57
    …
    if (auto n = rsNode["rendererAPI"]) rendererAPI = …;        // :61  ← the deleted line
    …
    return true;
}
catch (const std::exception&) { return false; }                 // :74-76
```

It never enumerates the mapping's keys, never validates a schema, and never rejects unknown keys. yaml-cpp parses a mapping containing `rendererAPI: 0` without complaint; with the lookup gone, the key is simply never queried and is silently ignored. Every other field deserializes identically. The `catch` at `:74` is not reached — an unknown key cannot throw here because nothing reads it.

**Writing new files without the key.** `SerializeToYamlNode` (`:40-52`) just stops emitting `rendererAPI`. An older binary reading a newer file hits `if (auto n = rsNode["rendererAPI"])` → the node is undefined → falsy → keeps its compiled-in platform default from `:29`/`:31`. Forward-compatible as well as backward-compatible.

**Both persisted `RenderSettings` call sites confirmed:**
- `X3/src/Project/ProjectManager.cpp:49-50` — `.lrproj`, under node `RuntimeRenderSettings`.
- `X3-Editor/src/EditorState.cpp:54-55` — `EditorState.yaml`, under node `EditorRenderSettings`.

Both go through the same `DeserializeFromYamlNode`, so the analysis covers every persisted copy.

**Behavioural consequence: none.** `IRendererAPI::SetAPI()` mutated `s_API`, and `GetAPI()` has **zero callers** (verified: `grep -rn "GetAPI()"` across all three source trees matches only the definition at `IRendererAPI.h:23` and the dead forwarder at `Renderer.h:87`). Every factory already resolved by `#ifdef`. Removing this deletes a lie, not a behaviour.

**No fixtures to migrate.** `find . -name "*.lrproj" -not -path "./build/*"` returns **nothing** — there are no in-tree project files. `X3-Editor/res/EditorState.yaml` is `.gitignore`d at **line 14** (line 15 is `X3-Editor/res/imgui.ini`; the critique's correction to spec A is right). `ProjectExporter.cpp` copies the `.lrproj` byte-for-byte (`:62` `copyInto`) and renames it (`:110-112`); it never re-serializes `RenderSettings`, so exported projects are unaffected either way.

**No editor UI exposes it.** `grep -rn "rendererAPI\|RendererAPI" X3-Editor/src` matches only `ImGuiContext.cpp:20`'s include, which §3.4 deletes. There is no settings panel to update.

## 6. Looks deletable — but is not

1. **`X3/src/Renderer/IRenderingContext.{h,cpp}`.** A two-method abstract base (`init()`, `swapBuffers()`) whose only remaining implementer is `VulkanContext` (`VulkanContext.h:15`). Reads as pure portable-RHI vestige. **Keep it in Part 1.** `GLFWWindow.h:53` stores `IRenderingContext* m_Context`; `GLFWWindow.cpp:82` (`onUpdate`) and `:90` (`swapBuffers`) dispatch through it; `IWindow.h:37` declares `swapBuffers()` on the window interface; `application.cpp:69` calls it. Removing it means retyping `m_Context` to `VulkanContext*` and splitting `swapBuffers()` into `beginFrame()`/`endFrame()`/`present()` — which is exactly the frame-lifecycle work in a **later part** of this spec (`ENGINE_PLAN.md:131`). Touching it here entangles a mechanical deletion with an interface redesign.

2. **`IRendererAPI` the class, and `IRendererAPI::Create()`.** Only the `enum class API` / `GetAPI` / `SetAPI` / `s_API` half dies in §5.2. The class survives Part 1: `X3/src/Core/application.cpp:27` does `_RendererAPI = IRendererAPI::Create();` then `->Init()`, `application.h:12` forward-declares it and `:28` stores a `std::shared_ptr<IRendererAPI>`, and `VulkanRendererAPI` (`VulkanRendererAPI.h:9`) is its only implementer. It is deleted by the resource-layer part, not here.

3. **`IImage2D::GetID()` (`IImage2D.h:19`) and `ITexture2D::GetID()` (`ITexture2D.h:13`).** Both return `int`. After the GL blit at `RuntimeLayer.cpp:165` and the GL `AddImage` at `ViewportPanel.cpp:291` are deleted, they *look* orphaned. They are not: **`ViewportPanel.cpp:49-50`** uses `int currentImageID = image->GetID();` as the cache key deciding whether to re-register the ImGui Vulkan descriptor against `m_LastRegisteredImageID`. Removing `GetID()` in Part 1 silently breaks descriptor invalidation on image recreation. The `int` return truncating a 64-bit `VkImage` is a real bug and is scheduled for the resource-layer part (`ENGINE_PLAN.md:127`).

4. **`enum struct Image2DType` (`IImage2D.h:8-12`) and `Bind()`/`Unbind()`/`AddData()`/`ChangeImageUnit()`/`ChangeTextureUnit()`.** All GL vestiges, all explicitly listed for removal in the resource-layer table at `ENGINE_PLAN.md:124-131`. They are load-bearing today (`Renderer.cpp` still passes `Image2DType::LR_READ_WRITE` and calls `AddData(offset, size, ptr)`) and their replacements do not exist yet. **Remove none of them in Part 1.**

5. **`RuntimeLayer::CalculateViewportCoordinates()` (`RuntimeLayer.cpp:306-376`) and `m_ViewportCoords`.** The `(x, y, x+width, y+height)` packing is literally `glBlitFramebuffer`'s convention and the header comment at `RuntimeLayer.h:53` says so. **The Vulkan path depends on it.** `VulkanContext.cpp:903-906` documents and consumes exactly that packing:
   ```cpp
   // viewport: x, y, x+width, y+height (matching OpenGL glBlitFramebuffer convention)
   blitRegion.dstOffsets[0] = {viewport.x, windowSize.y - viewport.w, 0}; // Flip Y
   blitRegion.dstOffsets[1] = {viewport.z, windowSize.y - viewport.y, 1}; // Flip Y
   ```
   (The comment is at `:903`, `dstOffsets` at `:905-906`.) **Do not "modernize" the convention while deleting GL.** Retarget the comments; leave the arithmetic byte-identical. Later parts of this spec repeat this instruction — it is that easy to break.

6. **`X3/res/made_with_X3.png`.** Its only reader (`RuntimeLayer.cpp:40`) is deleted by §3.8, so it looks orphaned. **Keep it** — §1's DECISION explicitly retains it, and `X3/CMakeLists.txt:140-141` installs the whole `res/` tree regardless.

7. **`stbi_set_flip_vertically_on_load(1); // OpenGL-style orientation` at `AssetManager.cpp:259`.** The comment invites deletion. **Do not touch it.** The flip is baked into two downstream consumers: the skybox sampling in all three compute shaders, and `ViewportPanel.cpp:288`'s `drawList->AddImage(..., {0,1}, {1,0})` flipped UVs. Changing the load orientation flips every texture in the engine. If the convention should change, that is a deliberate mesh/texture-pipeline decision in Part 5 (Phase 2), and it must land together with the paired shader change.

8. **`glfwSwapInterval(enabled)` in `GLFWWindowIMPL::setVSync` (`GLFWWindow.cpp:109`).** A GL/EGL-context call that is a no-op on a Vulkan-only GLFW window. **Leave it.** `RuntimeLayer.cpp:75` and `application` still call `setVSync`, and gutting the body here would make `m_VSync` silently meaningless. Wiring vSync to the swapchain present mode is a later part (`ENGINE_PLAN.md:139`).

9. **`find_package(glfw3 REQUIRED)` and `target_link_libraries(… glfw)`** at `X3/CMakeLists.txt:77-78` (inside the block §4.3(d) rewrites) and `X3-Editor/CMakeLists.txt:4`, `:15`. These sit adjacent to the OpenGL calls in the same `if/else` and are easy to delete by proximity. GLFW is the windowing layer for Vulkan (`GLFWWindow.h:3`, `VulkanContext::setWindowHints()`, `ImGui_ImplGlfw_InitForVulkan`). **All must survive.**

10. **`imgui_impl_glfw.cpp` / `imgui_impl_glfw.h`** in `X3-Editor/libs/imgui-docking/`. Name-adjacent to `imgui_impl_opengl3.*` but this is the *platform* backend, used by `ImGuiContext.cpp:3`, `:158`, `:266` and by `ViewportPanel`. Only the `opengl3` files are filter candidates.

11. **`ImGuiConfigFlags_ViewportsEnable` handling at `ImGuiContext.cpp:143-151` and `:299-305`.** Once `:130` is deleted the flag is never set, so both blocks become dead. **Keep them.** They are the exact re-enable point for Phase 13 (`ENGINE_PLAN.md:375`), and `glfwGetCurrentContext()`/`glfwMakeContextCurrent()` at `:301`/`:304` are the GL-context save/restore that a future Vulkan multi-viewport path must *replace*, not reconstruct from memory.

12. **The `SET(x)` macro block in the compute shaders** — `PathTracing.comp:8-14`, `PBR.comp:6-12`, `Phong.comp:6-12` (note the differing offsets; PathTracing.comp has four extra credit lines at the top). The `#else` branch (`#define SET(x)` empty) is dead under `glslc`, which predefines `VULKAN=100`. **Do not collapse it in Part 1.** Phase 0 verified the committed `.spv` are byte-identical to a fresh rebuild; editing shader source here means re-verifying that for zero functional gain, and the Slang migration (Phase 3) rewrites these files anyway. The three `// OpenGL doesn't support Vulkan descriptor sets (set = X)` comments are the only OpenGL residue under `res/` and they are harmless.

13. **`X3/src/Platform/Windows/`.** The directory name suggests a Win32 backend to delete alongside `Platform/OpenGL/`. It contains `GLFWWindow.{h,cpp}` — the **only** window implementation, instantiated unconditionally by `IWindow.cpp:8`. The misleading name deserves a rename, but not in this part.

14. **`X3/src/Renderer/IRenderingContext.cpp`** is a one-line file (`#include "Renderer/IRenderingContext.h"`) existing only to give the TU a home. Deleting it is harmless but pointless; it goes when `IRenderingContext` goes, in a later part.

## 7. Commit boundaries and per-commit verification gates

**Invariant: after every commit the tree configures and builds, and both executables are produced.** Never delete a file before nothing references it; never remove a macro definition before its `#ifdef` sites are gone.

**Gate command template.** Use this after every commit below, substituting the preset. Never judge the build by the exit code alone.

```sh
# fresh configure + build
rm -rf build/<preset>
cmake --preset <preset> 2>&1 | tee /tmp/x3-cfg.log
cmake --build build/<preset> -j 14 2>&1 | tee /tmp/x3-build.log

# THE ACTUAL GATE — all three must pass
grep -c 'error:' /tmp/x3-build.log          # must print 0
test -f build/<preset>/Debug/X3Editor          && echo "editor OK"
test -f build/<preset>/Debug/runtime/X3Runtime && echo "runtime OK"
```

(For the `release` preset substitute `Release` for `Debug` in the paths. `RUNTIME_OUTPUT_DIRECTORY` is `${CMAKE_BINARY_DIR}/$<CONFIG>` for the editor and `${CMAKE_BINARY_DIR}/$<CONFIG>/runtime` for the runtime — `X3-Editor/CMakeLists.txt:36`, `X3-Runtime/CMakeLists.txt:13`.)

**Baseline before starting**, using the *old* presets, to confirm the starting state:
```sh
cmake --preset vulkan-debug && cmake --build build/vulkan-debug -j 14   # 0 error:, X3Editor 39 MB
cmake --preset opengl-debug && cmake --build build/opengl-debug -j 14   # 0 error:, X3Editor 32 MB
```
Both were verified passing on 2026-07-25.

---

### Commit 0 — `chore: Phase 0 repo and build hygiene`

Commit the pre-existing uncommitted working-tree changes verbatim. Current `git status --short`:
```
 M .gitignore                                   (added *.spv)
M  .gitmodules                                  (SDL/MaterialX entries removed)
 M TODO_VULKAN.md                               (rewritten as superseded pointer)
 M X3-Editor/CMakeLists.txt                     (OpenGL find_package made conditional)
 M X3-Editor/libs/imgui-docking/CMakeLists.txt  (main.cpp filter + conditional backend)
 M X3-Runtime/CMakeLists.txt                    (cxx_std_20 removed)
 M X3-Runtime/src/RuntimeLayer.cpp              (dead empty ifdef removed)
 M X3/CMakeLists.txt                            (DXC_COMMAND + cxx_std_20 removed, assimp + Jolt flags)
D  X3/libs/{MaterialX,SDL,SDL_image,SDL_mixer,SDL_ttf}
D  X3/res/shaders/{PathTracing,PBR,Phong}.comp.spv
 D comp.spv
?? ENGINE_PLAN.md, ORCHESTRATION.md, docs/
```
Also `git add ENGINE_PLAN.md ORCHESTRATION.md docs/`.

Two `ENGINE_PLAN.md` Phase 0 items are **already done** — do not go looking for them: the dead empty `#ifdef X3_USE_OPENGL` block formerly at `RuntimeLayer.cpp:211-214`, and `set(DXC_COMMAND ...)` formerly at `X3/CMakeLists.txt:33`. The DXC removal is *safe only because* `set(JPH_USE_VK OFF ...)` was added at `X3/CMakeLists.txt:41` in the same change — see §4.3.

**Gate:** `vulkan-debug` and `opengl-debug` both pass the template. (This is the last commit at which `opengl-debug` exists.)

---

### Commit 1 — `build: make the build Vulkan-only`

- Root `CMakeLists.txt`: delete `:7-9` (§4.1).
- `CMakePresets.json`: full replacement (§4.2).
- `X3/CMakeLists.txt`: §4.3 (a) dead comments, (b) unconditional VMA/vk-bootstrap, (c) **first stage** — unconditional OpenGL FILTER, (d) collapsed link block, (e) **first stage** — unconditional `X3_USE_VULKAN`.
- `X3-Editor/CMakeLists.txt`: §4.4.
- `X3-Editor/libs/imgui-docking/CMakeLists.txt`: §4.5 (preserving the existing `main.cpp` filter).
- `rm -rf build/opengl-debug build/vulkan-debug` (housekeeping; not part of the commit — `build/` is `.gitignore`d at line 3).

**State after:** `X3_GRAPHICS_API` no longer exists anywhere. `X3_USE_VULKAN` is always defined, `X3_USE_OPENGL` never. Every `#ifdef X3_USE_OPENGL` branch is present but unreachable. `X3/src/Platform/OpenGL/*.cpp` are on disk but excluded from `X3_SOURCES`. GLEW and OpenGL are unlinked.

**This is the highest-value single commit** — it is where any build regression shows up, and it is independently bisectable.

**Gate:**
```sh
grep -rn "X3_GRAPHICS_API" . --include='*.txt' --include='*.json' | grep -v '^./build/'   # must be EMPTY
# then the template for BOTH new presets:
rm -rf build/debug   && cmake --preset debug   && cmake --build build/debug -j 14
rm -rf build/release && cmake --preset release && cmake --build build/release -j 14
```
Both must produce 0 `error:` and both binaries. Additionally confirm the imgui backend actually linked:
```sh
nm -C build/debug/lib/libimgui.a 2>/dev/null | grep -c ImGui_ImplVulkan_Init   # must be >= 1
nm -C build/debug/lib/libimgui.a 2>/dev/null | grep -c ImGui_ImplOpenGL3_Init  # must be 0
```
(If `libimgui.a` is elsewhere, `find build/debug -name 'libimgui*'`.) This catches the §4.5 failure mode directly rather than waiting for a link error.

---

### Commit 2 — `runtime: drop the OpenGL splash screen and GL presentation path`

- `X3-Runtime/src/RuntimeLayer.h` (§3.7) and `X3-Runtime/src/RuntimeLayer.cpp` (§3.8).
- Record the §1 DECISION in `ENGINE_PLAN.md` (Phase 1d and Phase 13 bullets).

Isolated because it is the **only functional regression** in Part 1. Keeping it standalone makes it trivially revertible and trivially citable from Phase 13.

**Gate:**
```sh
grep -rnE '\bgl[A-Z][A-Za-z]*\(|\bGL_[A-Z_]+|\bGLuint\b|\bGLint\b|\bGLenum\b|GLEW' X3-Runtime/src | grep -v glfw   # EMPTY
grep -rn 'stbi_' X3-Runtime/src                                                                                      # EMPTY
```
Then the build template for `debug`. **Also run the runtime executable once** against an exported project and confirm it renders the compute output with no splash and no crash — this is the only commit in Part 1 with a runtime-behaviour change, so a compile-only gate is insufficient.

---

### Commit 3 — `renderer: collapse the graphics-API preprocessor guards`

- `X3/src/Renderer/IRendererAPI.cpp` (§3.1) — **guard collapse only**; leave `s_API` as the single unconditional line for Commit 5.
- `X3/src/Renderer/{IComputeShader,ITexture2D,IImage2D,IUniformBuffer,IShaderStorageBuffer}.cpp` (§3.2).
- `X3/src/Platform/Windows/GLFWWindow.cpp` (§3.3), including deleting the unused `IRendererAPI.h` include at `:8`.
- `X3-Editor/src/ImGuiContext.cpp` (§3.4), including deleting the unused `IRendererAPI.h` include at `:20`.
- `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.{h,cpp}` (§3.5, §3.6).
- `X3/CMakeLists.txt`: §4.3(e) **second stage** — delete the `X3_USE_VULKAN` define and its comment.

**Gate:**
```sh
grep -rn "X3_USE_OPENGL\|X3_USE_VULKAN" \
  X3/src X3-Editor/src X3-Runtime/src \
  CMakeLists.txt X3/CMakeLists.txt X3-Editor/CMakeLists.txt X3-Runtime/CMakeLists.txt \
  X3-Editor/libs/imgui-docking/CMakeLists.txt
# must return NOTHING
```
Then the build template for `debug` **and** `release`. **Launch the editor once** and confirm the viewport renders and ImGui draws — this commit touches ImGui init and the viewport texture path, and a compile gate would not catch a mis-collapsed guard that dropped a needed line.

**State after:** every `Platform/OpenGL/*` header is orphaned — nothing includes it.

---

### Commit 4 — `engine: delete the OpenGL backend`

- `git rm -r X3/src/Platform/OpenGL/`
- `X3/CMakeLists.txt`: §4.3(c) **second stage** — delete the FILTER line and its comment.

Pure deletion. Zero risk if Commit 3's gate passed. If the FILTER removal is forgotten, CMake still configures (the regex matches nothing) — but leaving it is misleading dead build logic.

**Gate:**
```sh
ls X3/src/Platform          # must print exactly: Vulkan  Windows
grep -rn "Platform/OpenGL" X3/src X3-Editor/src X3-Runtime/src X3/CMakeLists.txt   # EMPTY
```
Then the build template for `debug`.

---

### Commit 5 — `project: remove the RendererAPI setting`

- All of §5: `RenderSettings.h`, `IRendererAPI.h`, `IRendererAPI.cpp` (the `s_API` definition), `Renderer.h`, `ProjectManager.cpp`.

Deliberately last: it is the only commit with a persisted-format implication, and isolating it makes the §5.6 compatibility reasoning reviewable on its own.

**Gate:**
```sh
grep -rn "RendererAPI\|GetAPI\|SetAPI\|s_API" X3/src X3-Editor/src X3-Runtime/src
# ONLY these are permitted to remain:
#   - the class name IRendererAPI (declaration, Create(), application.cpp/h)
#   - the class name VulkanRendererAPI
#   - #include "Renderer/IRendererAPI.h" lines
# No 'enum class RendererAPI', no GetAPI, no SetAPI, no s_API.
grep -rn "rendererAPI" X3/src X3-Editor/src X3-Runtime/src   # EMPTY
```
Then the build template for `debug` and `release`. **Serialization check:** open any existing `.lrproj` containing `rendererAPI:` in the editor, confirm it loads with no warning and that `resolution`, `raysPerPixel`, `bouncesPerRay`, `accumulate` and `vSync` all round-trip; then save it and confirm the key is gone from the written file and the project still opens.

---

### Commit 6 — `docs: reflect the Vulkan-only build`

- `TODO_VULKAN.md:12` (§4.7).
- Mark Phase 1a complete in `ENGINE_PLAN.md`, and confirm the §1 DECISION text landed in Commit 2 is present.

**Gate:** none beyond `git diff --stat` showing only `.md` files.

---

### Post-flight verification for the whole of Part 1

```sh
grep -rn "X3_USE_OPENGL\|X3_USE_VULKAN\|X3_GRAPHICS_API" . \
  --include='*.h' --include='*.cpp' --include='*.txt' --include='*.json' | grep -v '^./build/'
grep -rniE 'glew|glad|opengl' X3/src X3-Editor/src X3-Runtime/src
grep -rnE '\bgl[A-Z][A-Za-z]*\(|\bGL_[A-Z_]+|\bGLuint\b|\bGLint\b|\bGLenum\b' X3/src X3-Editor/src X3-Runtime/src | grep -v glfw
ls X3/src/Platform          # → Vulkan  Windows
```

The **first** grep must be empty. The **second** must return exactly this benign residue and nothing else — all comments, all deliberately retained:
- `X3/src/Project/Assets/AssetManager.cpp:259` — `// OpenGL-style orientation` (§6.7)
- `X3/src/Platform/Vulkan/VulkanImage2D.cpp:74` — `// RGBA32F to match OpenGL`
- `X3/src/Platform/Vulkan/VulkanComputeShader.cpp:91`, `:96` — `// placeholder for API compatibility with OpenGL`
- `X3/src/Platform/Vulkan/VulkanContext.cpp:10` — `// For Vulkan, we don't need OpenGL context`
- `X3/src/Platform/Vulkan/VulkanContext.cpp:903` — the `glBlitFramebuffer` convention comment (§6.5, live invariant)

The **third** must be empty. (`res/shaders/*.comp` are not scanned by these greps; their three `// OpenGL doesn't support Vulkan descriptor sets` comments at `PathTracing.comp:8`, `PBR.comp:6`, `Phong.comp:6` are also retained — §6.12.)

Finally, both presets, both configurations, fresh:
```sh
for p in debug release; do rm -rf build/$p; cmake --preset $p && cmake --build build/$p -j 14; done
```
0 `error:` in each log; `X3Editor` and `runtime/X3Runtime` present under both `build/debug/Debug/` and `build/release/Release/`.

## 8. Handoff notes to the following parts

- `IRendererAPI` (the class), `IRenderingContext`, `IImage2D`, `ITexture2D`, `IComputeShader`, `IUniformBuffer`, `IShaderStorageBuffer` and their five factory `.cpp` files all survive Part 1 intact and are deleted by the resource-layer part. The five factory `.cpp` files each still carry an unused `#include "Renderer/IRendererAPI.h"` at line 2 — deliberately (§3.2). The two *other* sites (`GLFWWindow.cpp:8`, `ImGuiContext.cpp:20`) have already been removed here, so deleting `IRendererAPI.h` later cannot break them.
- `ImGuiContext.cpp` after Part 1 has no preprocessor guards, and its Vulkan bodies (`Init` at what was `:158-197`, the swapchain re-init at what was `:218-262`, `EndFrame` at what was `:275-294`) are byte-identical to today's. The dynamic-rendering part rewrites all three; its line citations should be recomputed against the post-Commit-3 file, since the guard removal shifts everything.
- `X3/CMakeLists.txt`'s `find_package(Vulkan REQUIRED)` is unconditional after Commit 1 (§4.3(d)). The dynamic-rendering part additionally requires `require_api_version(1, 3, 0)` in `VulkanContext::createInstance` and `set_minimum_version(1, 3)` in `pickPhysicalDevice`; the installed headers are `VK_HEADER_VERSION_COMPLETE = 1.4.350` (`/usr/include/vulkan/vulkan_core.h`), so no CMake change is needed for that.
- `set(JPH_USE_VK OFF CACHE BOOL "" FORCE)` at `X3/CMakeLists.txt:41` must remain `OFF` in every later part. It is what allows `DXC_COMMAND` to stay deleted.