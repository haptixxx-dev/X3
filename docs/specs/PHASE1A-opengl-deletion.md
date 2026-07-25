# EXECUTION SPEC — Phase 1a: Delete the OpenGL Backend

**Repo:** `/home/sarah/Coding/Haptixxx/X3`, branch `vulkan-migration`
**Scope:** removal only. No Vulkan redesign (that is 1b), no correctness fixes (1c), no splash re-implementation (1d).
**Constraint:** the machine cannot build (`vulkan-headers` missing). Every step below is verified by reading, not compiling.

---

## 0. PRE-FLIGHT (do this before touching anything)

The working tree already contains **uncommitted Phase 0 work** (`git status`):

```
 M .gitignore                                   (added *.spv)
M  .gitmodules                                  (SDL/MaterialX entries removed)
 M TODO_VULKAN.md                               (rewritten as superseded pointer)
 M X3-Editor/CMakeLists.txt                     (OpenGL find_package made conditional)
 M X3-Editor/libs/imgui-docking/CMakeLists.txt  (backend selection made conditional)
 M X3-Runtime/CMakeLists.txt                    (cxx_std_20 removed)
 M X3-Runtime/src/RuntimeLayer.cpp              (dead empty ifdef at old :211-214 removed)
 M X3/CMakeLists.txt                            (DXC_COMMAND + cxx_std_20 removed, assimp flags)
D  X3/libs/{MaterialX,SDL,SDL_image,SDL_mixer,SDL_ttf}
 D comp.spv
?? ENGINE_PLAN.md, ORCHESTRATION.md
```

**Commit this as "Phase 0: repo and build hygiene" first.** All line numbers in this spec refer to the **current working-tree content**, not `HEAD`. Two consequences:

- `ENGINE_PLAN.md:83` ("delete the dead empty `#ifdef X3_USE_OPENGL` block at `RuntimeLayer.cpp:211-214`") is **already done**. Do not look for it.
- `ENGINE_PLAN.md:88` (`set(DXC_COMMAND ...)` at `X3/CMakeLists.txt:33`) is **already done**.

Also: stale configure caches exist at `build/opengl-debug/` and `build/vulkan-debug/`. Both pin `X3_GRAPHICS_API` in `CMakeCache.txt`. **Delete both directories** as part of commit 1 — the preset `binaryDir` changes and a stale cache will silently resurrect the removed variable.

---

## 1. COMPLETE FILE INVENTORY

### 1.1 Files deleted outright (17 files, one directory)

`X3/src/Platform/OpenGL/` — delete the whole directory:

| File |
|---|
| `X3/src/Platform/OpenGL/OpenGLComputeShader.cpp` |
| `X3/src/Platform/OpenGL/OpenGLComputeShader.h` |
| `X3/src/Platform/OpenGL/OpenGLContext.cpp` |
| `X3/src/Platform/OpenGL/OpenGLContext.h` |
| `X3/src/Platform/OpenGL/OpenGLdebugFuncs.cpp` |
| `X3/src/Platform/OpenGL/OpenGLdebugFuncs.h` |
| `X3/src/Platform/OpenGL/OpenGLImage2D.cpp` |
| `X3/src/Platform/OpenGL/OpenGLImage2D.h` |
| `X3/src/Platform/OpenGL/OpenGLRendererAPI.cpp` |
| `X3/src/Platform/OpenGL/OpenGLRendererAPI.h` |
| `X3/src/Platform/OpenGL/OpenGLShaderStorageBuffer.cpp` |
| `X3/src/Platform/OpenGL/OpenGLShaderStorageBuffer.h` |
| `X3/src/Platform/OpenGL/OpenGLTexture2D.cpp` |
| `X3/src/Platform/OpenGL/OpenGLTexture2D.h` |
| `X3/src/Platform/OpenGL/OpenGLUniformBuffer.cpp` |
| `X3/src/Platform/OpenGL/OpenGLUniformBuffer.h` |

Nothing outside `X3/src/Platform/OpenGL/` and the six `I*.cpp` factories + `GLFWWindow.cpp` includes any of these headers (verified by grep for `Platform/OpenGL/`). `GLCall` / `GLLogCall` / `GLClearError` from `OpenGLdebugFuncs.h` have **zero** callers outside that directory.

**Optional, recommended as a separate change:** `X3-Editor/libs/imgui-docking/imgui_impl_opengl3.cpp`, `imgui_impl_opengl3.h`, `imgui_impl_opengl3_loader.h`. These are vendored upstream Dear ImGui files. See §3.4 — if you keep them, the CMake filter that excludes them must become **unconditional**.

### 1.2 Files needing edits (13 files)

| # | File | Why |
|---|---|---|
| 1 | `CMakeLists.txt` (root) | `X3_GRAPHICS_API` cache variable |
| 2 | `CMakePresets.json` | four presets → two |
| 3 | `X3/CMakeLists.txt` | VMA/vk-bootstrap gate, source FILTER, find_package, define |
| 4 | `X3-Editor/CMakeLists.txt` | OpenGL find_package + link |
| 5 | `X3-Editor/libs/imgui-docking/CMakeLists.txt` | backend source filter + Vulkan link gate |
| 6 | `X3/src/Renderer/IRendererAPI.cpp` | factory `#ifdef`, `s_API` definition |
| 7 | `X3/src/Renderer/IRendererAPI.h` | `enum class API`, `GetAPI`, `SetAPI`, `s_API` |
| 8 | `X3/src/Renderer/IComputeShader.cpp` | factory `#ifdef` |
| 9 | `X3/src/Renderer/ITexture2D.cpp` | factory `#ifdef` |
| 10 | `X3/src/Renderer/IImage2D.cpp` | factory `#ifdef` |
| 11 | `X3/src/Renderer/IUniformBuffer.cpp` | factory `#ifdef` |
| 12 | `X3/src/Renderer/IShaderStorageBuffer.cpp` | factory `#ifdef` |
| 13 | `X3/src/Renderer/RenderSettings.h` | `RendererAPI` enum + member + serialization |
| 14 | `X3/src/Renderer/Renderer.h` | `GetAPI`/`SetAPI` forwarders |
| 15 | `X3/src/Project/ProjectManager.cpp` | `SetAPI` call + now-unused include |
| 16 | `X3/src/Platform/Windows/GLFWWindow.cpp` | context creation + window hints `#ifdef` |
| 17 | `X3-Editor/src/ImGuiContext.cpp` | backend init/shutdown/frame `#ifdef` |
| 18 | `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.cpp` | texture-ID `#ifdef` |
| 19 | `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.h` | `#ifdef X3_USE_VULKAN` guards |
| 20 | `X3-Runtime/src/RuntimeLayer.cpp` | splash + presentation `#ifdef` |
| 21 | `X3-Runtime/src/RuntimeLayer.h` | splash members + includes |
| 22 | `TODO_VULKAN.md` | line 12 claims "all four builds compile" |

**Files that grep hits but need NO edit:**
- `X3/src/Project/Assets/AssetManager.cpp:259` — `stbi_set_flip_vertically_on_load(1); // OpenGL-style orientation`. See §6.
- `X3/src/Platform/Vulkan/VulkanImage2D.cpp:74`, `VulkanComputeShader.cpp:91,96`, `VulkanContext.cpp:10,903` — comments mentioning OpenGL. Leave; `VulkanContext.cpp:903`'s comment documents a live invariant (§6).
- `X3/res/shaders/{PathTracing,PBR,Phong}.comp:6-14` — the `SET(x)` macro. Leave for Phase 3 (§6).
- `ORCHESTRATION.md:54`, `ENGINE_PLAN.md` — planning docs, not build inputs.
- `X3/CMakeLists.txt:12` — `# add_subdirectory(.../Thirdparty/glew-cmake)` is already commented out and `X3/Thirdparty/` is empty. Delete the comment line opportunistically; it is not load-bearing.

---

## 2. PER-FILE EDIT SPEC — SOURCE

For each preprocessor site: **SURVIVES** = the code that remains; **GUARD** = whether the `#ifdef` wrapper disappears entirely.

### 2.1 `X3/src/Renderer/IRendererAPI.cpp` (30 lines)

| Lines | Directive | Action |
|---|---|---|
| 3–5 | `#ifdef X3_USE_OPENGL` / `#include "Platform/OpenGL/OpenGLRendererAPI.h"` / `#endif` | Delete all three lines. |
| 6–8 | `#ifdef X3_USE_VULKAN` / include / `#endif` | SURVIVES: `#include "Platform/Vulkan/VulkanRendererAPI.h"`. GUARD: gone. |
| 13–18 | `#ifdef X3_USE_VULKAN` / `s_API = Vulkan` / `#else` / `s_API = OpenGL` / `#endif` | **Delete the entire block including the `// Set API based on build configuration` comment at line 13.** `s_API` itself is removed (§4). |
| 20–29 | `Create()` body: `#ifdef X3_USE_OPENGL` / `#elif defined(X3_USE_VULKAN)` / `#else` / `#endif` | SURVIVES: `return std::make_shared<VulkanRendererAPI>();`. GUARD: gone, including the `#else` `LOG_ENGINE_CRITICAL("No graphics API defined at build time!")` fallback. |

Resulting file is ~12 lines: include, `#include "Platform/Vulkan/VulkanRendererAPI.h"`, namespace, one-line `Create()`.

### 2.2 `X3/src/Renderer/IComputeShader.cpp`, `ITexture2D.cpp`, `IImage2D.cpp`, `IUniformBuffer.cpp`, `IShaderStorageBuffer.cpp`

These five are structurally **identical**. Same edit in each:

| Lines | Directive | Action |
|---|---|---|
| 4–6 | `#ifdef X3_USE_OPENGL` / OpenGL include / `#endif` | Delete all three. |
| 7–9 | `#ifdef X3_USE_VULKAN` / Vulkan include / `#endif` | SURVIVES: the Vulkan `#include`. GUARD: gone. |
| 15–22 | `#ifdef X3_USE_OPENGL` / `#elif defined(X3_USE_VULKAN)` / `#else` / `#endif` | SURVIVES: line 18, the `std::make_shared<Vulkan…>(…)` return. GUARD: gone, `#else` fallback deleted. |

Exact surviving return statements (verbatim, keep the argument lists unchanged):

- `IComputeShader.cpp` → `return std::make_shared<VulkanComputeShader>(filepath, workGroupSizes);`
- `ITexture2D.cpp` → `return std::make_shared<VulkanTexture2D>(data, width, height, textureUnit);`
- `IImage2D.cpp` → `return std::make_shared<VulkanImage2D>(data, width, height, imageUnit, imageType);`
- `IUniformBuffer.cpp` → `return std::make_shared<VulkanUniformBuffer>(size, bindingPoint, type);`
- `IShaderStorageBuffer.cpp` → `return std::make_shared<VulkanShaderStorageBuffer>(size, bindingPoint, type);`

**Do not** delete the `#include "Renderer/IRendererAPI.h"` at line 2 of each — it is unused in the collapsed body but harmless, and removing it is a separate cleanup. (If you do remove it, remove it from all five together and verify `LOG_ENGINE_CRITICAL` is no longer referenced — it is not, after the `#else` branches go.)

### 2.3 `X3/src/Platform/Windows/GLFWWindow.cpp`

> Note the directory name: `Platform/Windows/` holds the **cross-platform GLFW** window. Do not confuse it with a Win32 backend.

| Lines | Directive | Action |
|---|---|---|
| 10–12 | `#ifdef X3_USE_OPENGL` / `#include "Platform/OpenGL/OpenGLContext.h"` / `#endif` | Delete all three. |
| 13–15 | `#ifdef X3_USE_VULKAN` / `#include "Platform/Vulkan/VulkanContext.h"` / `#endif` | SURVIVES: the include. GUARD: gone. |
| 25–30 | comment + `#ifdef X3_USE_VULKAN` / `VulkanContext::setWindowHints();` / `#else` / `OpenGLContext::setWindowHints();` / `#endif` | SURVIVES: `VulkanContext::setWindowHints();` (declared `X3/src/Platform/Vulkan/VulkanContext.h:23`, defined `VulkanContext.cpp:9`). GUARD: gone. |
| 48–53 | comment + `#ifdef X3_USE_VULKAN` / `m_Context = new VulkanContext(m_NativeWindow);` / `#else` / `m_Context = new OpenGLContext(...)` / `#endif` | SURVIVES: `m_Context = new VulkanContext(m_NativeWindow);`. GUARD: gone. |

`m_Context` stays typed as `IRenderingContext*` (`GLFWWindow.h:53`) in Phase 1a. See §6.

### 2.4 `X3-Editor/src/ImGuiContext.cpp`

| Lines | Directive | Action |
|---|---|---|
| 4–6 | `#ifdef X3_USE_OPENGL` / `#include <imgui_impl_opengl3.h>` / `#endif` | Delete all three. |
| 7–11 | `#ifdef X3_USE_VULKAN` / `<vulkan/vulkan.h>`, `<imgui_impl_vulkan.h>`, `"Platform/Vulkan/VulkanContext.h"` / `#endif` | SURVIVES: all three includes. GUARD: gone. |
| 36–41 | destructor: `#ifdef X3_USE_VULKAN` / `ImGui_ImplVulkan_Shutdown();` / `#else` / `ImGui_ImplOpenGL3_Shutdown();` / `#endif` | SURVIVES: `ImGui_ImplVulkan_Shutdown();`. GUARD: gone. Keep the `// Shutdown the correct backend` comment or reword to `// Shutdown the Vulkan backend`. |
| 129–131 | `#ifndef X3_USE_VULKAN` / `io.ConfigFlags \|= ImGuiConfigFlags_ViewportsEnable;` / `#endif` | **Inverted guard — the enclosed line is the OpenGL-only branch.** SURVIVES: nothing. Delete all three lines. GUARD: gone. Keep the explanatory comment at 132–133 and retarget it to Phase 13. |
| 157–201 | `#ifdef X3_USE_VULKAN` (Vulkan init, 157–197) / `#else` (198–200: `ImGui_ImplGlfw_InitForOpenGL`, `ImGui_ImplOpenGL3_Init("#version 460")`) / `#endif` | SURVIVES: lines 158–197 (the whole Vulkan `init_info` setup, `ImGui_ImplVulkan_Init`, `ImGui_ImplVulkan_CreateFontsTexture`, `vkDeviceWaitIdle`). GUARD: gone. Delete 198–200. |
| 217–265 | `BeginFrame`: `#ifdef X3_USE_VULKAN` (218–262, swapchain-recreation re-init + `ImGui_ImplVulkan_NewFrame()`) / `#else` (263–264: `ImGui_ImplOpenGL3_NewFrame();`) / `#endif` | SURVIVES: 218–262. GUARD: gone. |
| 274–297 | `EndFrame`: `#ifdef X3_USE_VULKAN` (275–294) / `#else` (295–296: `ImGui_ImplOpenGL3_RenderDrawData(...)`) / `#endif` | SURVIVES: 275–294 (`ensureFrameStarted`, `beginOverlayRenderPass`, `getCurrentCommandBuffer`, asserts, `ImGui_ImplVulkan_RenderDrawData`). GUARD: gone. |

**Leave lines 143–151 and 299–305 exactly as they are.** Both test `io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable`, which is now never set, so both become dead code. They are the re-enable point for Phase 13. Add a one-line comment noting this; do not delete.

`#include "Renderer/IRendererAPI.h"` at line 20 is unused after this edit (no `IRendererAPI` symbol is referenced in the file). Safe to delete; optional.

### 2.5 `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.cpp`

| Lines | Directive | Action |
|---|---|---|
| 12–16 | `#ifdef X3_USE_VULKAN` / VulkanImage2D, VulkanContext, imgui_impl_vulkan includes / `#endif` | SURVIVES: the three includes. GUARD: gone. |
| 21–23 | destructor: `#ifdef X3_USE_VULKAN` / `CleanupVulkanResources();` / `#endif` | SURVIVES: the call. GUARD: gone. |
| 26–103 | `#ifdef X3_USE_VULKAN` / `CleanupVulkanResources()` + `GetImGuiTextureID()` definitions / `#endif` | SURVIVES: both function bodies. GUARD: gone. |
| 284–293 | `#ifdef X3_USE_VULKAN` (285–289: `GetImGuiTextureID` + `drawList->AddImage(textureID, …)`) / `#else` (290–292: `drawList->AddImage((ImTextureID)(intptr_t)…->GetID(), …)`) / `#endif` | SURVIVES: 285–289. GUARD: gone. Delete the `// OpenGL can use the texture ID directly` comment with the `#else` branch. |

### 2.6 `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.h`

| Lines | Directive | Action |
|---|---|---|
| 8–10 | `#ifdef X3_USE_VULKAN` / `#include <vulkan/vulkan.h>` / `#endif` | SURVIVES: the include. GUARD: gone. |
| 63–71 | `#ifdef X3_USE_VULKAN` / `m_ImGuiTextureDescriptor`, `m_TextureSampler`, `m_LastRegisteredImageID`, `CleanupVulkanResources()`, `GetImGuiTextureID()` / `#endif` | SURVIVES: all five declarations. GUARD: gone. |

### 2.7 `X3-Runtime/src/RuntimeLayer.h` (67 lines)

| Lines | Directive | Action |
|---|---|---|
| 5–7 | `#ifdef X3_USE_OPENGL` / `#include <GL/glew.h>` / `#endif` | Delete all three. **Critical:** GLEW headers reach this TU only via `X3Engine`'s `PUBLIC` link to `GLEW::GLEW`; once §3.2 removes it, this include fails to resolve. |
| 8–11 | `#ifdef X3_USE_VULKAN` / VulkanContext + VulkanImage2D includes / `#endif` | SURVIVES: both includes. GUARD: gone. |
| 12 | `#include <chrono>` | Delete — its only user is `m_SplashStartTime` (line 64). |
| 30–32 | `#ifdef X3_USE_OPENGL` / `bool LoadLogoFromDisk(unsigned int*, int*, int*);` / `#endif` | **Delete the declaration and the guard.** |
| 36–40 | `#ifdef X3_USE_OPENGL` / `InitLogoResources()`, `DestroyLogoResources()`, `RenderLogo(float)` / `#endif` | **Delete all three declarations and the guard.** |
| 48 | `unsigned int m_Framebuffer = 0;` | **Delete.** Unguarded, but its only writes/reads are `glGenFramebuffers`/`glDeleteFramebuffers`/`glBindFramebuffer` in the GL branch, plus the constructor initializer at `RuntimeLayer.cpp:23`. |
| 57–65 | `#ifdef X3_USE_OPENGL` / splash members (`m_ShowLogoScreen`, `m_LogoWidth/Height`, `m_LogoTexHandle`, `m_LogoVAO/VBO/Program`, `m_LogoUniformLocation*`, `m_SplashStartTime`) / `#endif` | **Delete the entire block and the guard.** |

Keep `m_CurrentFrame`, `m_ExportSettings`, `m_ViewportCoords`, `m_WindowSize`, `m_UpdateViewportCoordinates` — all used by the Vulkan path. Retarget the `// x, y, width, height for glBlitFramebuffer` comment on line 53 to mention `VulkanContext::blitImageToSwapchain` (§6, item 5).

### 2.8 `X3-Runtime/src/RuntimeLayer.cpp` (377 lines)

| Lines | Directive | Action |
|---|---|---|
| 6 | `#include <stb_image/stb_image.h>` | Delete — sole users are `stbi_set_flip_vertically_on_load`/`stbi_load`/`stbi_image_free` at 38/42/64, inside the deleted splash. |
| 8–10 | `#ifdef X3_USE_OPENGL` / `#include <GL/glew.h>` / `#endif` | Delete all three. |
| 26–31 | constructor init-list `#ifdef X3_USE_OPENGL` / `, m_ShowLogoScreen(true) , m_LogoWidth(0) , m_LogoHeight(0) , m_LogoTexHandle(0)` / `#endif` | **Delete.** Also delete `, m_Framebuffer(0)` at line 23. Result: init list runs `m_ProjectManager(projectManager)`, `m_ViewportCoords(0,0,0,0)`, `m_WindowSize(0,0)`, `m_UpdateViewportCoordinates(false)`. |
| 35–71 | `#ifdef X3_USE_OPENGL` / `LoadLogoFromDisk()` definition / `#endif` | **Delete the whole block.** |
| 81–92 | `onAttach`: `#ifdef X3_USE_OPENGL` / splash-init `if (m_ShowLogoScreen) {…}` / `#endif` | **Delete the whole block.** |
| 108–110 | `onAttach`: `#ifdef X3_USE_OPENGL` / `glGenFramebuffers(1, &m_Framebuffer);` / `#endif` | **Delete.** `onAttach` then ends after the `UpdateRenderSettingsEvent` dispatch at line 106. |
| 113–120 | `onDetach` body: `#ifdef X3_USE_OPENGL` / framebuffer delete + `DestroyLogoResources()` / `#endif` | **Delete the body.** `onDetach()` becomes an empty function — keep the function (it is an `ILayer` override). |
| 123–194 | `onUpdate`: `#ifdef X3_USE_OPENGL` (124–173: splash fade + `glBlitFramebuffer` presentation) / `#else` (174–193: Vulkan presentation) / `#endif` | **SURVIVES: lines 175–193** — the `if (m_CurrentFrame) { CalculateViewportCoordinates(); … context->blitImageToSwapchain(…); }` block. GUARD: gone. Delete 124–173 entirely. Keep the `// Vulkan frame presentation` comment or drop it as redundant. |
| 213–304 | `#ifdef X3_USE_OPENGL` / `InitLogoResources()`, `DestroyLogoResources()`, `RenderLogo(float)` definitions / `#endif` | **Delete the whole block** (92 lines: inline GLSL 330 shader source, VAO/VBO setup, draw). |

`CalculateViewportCoordinates()` (306–376) is **unchanged** — the Vulkan blit consumes its output.

**This is a deliberate feature removal.** `ENGINE_PLAN.md:133` (Phase 1d) requires the splash be either re-implemented on Vulkan or formally dropped. Phase 1a drops it; leave `X3/res/made_with_X3.png` on disk (it is the asset Phase 1d would re-consume — see §6 item 6).

---

## 3. CMAKE CHANGES

### 3.1 `CMakeLists.txt` (root, 39 lines)

Delete lines 7–9:

```cmake
# Graphics API selection (OpenGL or Vulkan)
set(X3_GRAPHICS_API "OpenGL" CACHE STRING "Graphics API to use (OpenGL or Vulkan)")
set_property(CACHE X3_GRAPHICS_API PROPERTY STRINGS "OpenGL" "Vulkan")
```

Nothing else in this file changes. `CMAKE_CXX_STANDARD 23` at line 26 is already correct post-Phase-0.

### 3.2 `X3/CMakeLists.txt` (134 lines)

**(a) Lines 25–29** — VMA and vk-bootstrap become unconditional:

```cmake
# Vulkan libraries
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/VulkanMemoryAllocator)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/vk-bootstrap)
```

Both are live git submodules (`.gitmodules`) and both directories are present (`X3/libs/VulkanMemoryAllocator`, `X3/libs/vk-bootstrap`).

**(b) Lines 49–54** — the source-list FILTER. Two-stage:

- *Commit 1* (OpenGL sources still on disk): replace the `if/else/endif` with a single unconditional line so the tree stays buildable while the `.cpp` files still exist:
  ```cmake
  list(FILTER X3_SOURCES EXCLUDE REGEX ".*/Platform/OpenGL/.*")
  ```
- *Commit 4* (after `rm -r X3/src/Platform/OpenGL/`): **delete that line and its `# Filter out unused graphics API platform files` comment entirely.**

**(c) Lines 64–78** — the API-specific link block collapses to:

```cmake
# Vulkan + windowing
find_package(Vulkan REQUIRED)
target_link_libraries(X3Engine PUBLIC Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
find_package(glfw3 REQUIRED)
target_link_libraries(X3Engine PUBLIC glfw)
```

Removed: `find_package(OpenGL REQUIRED)` (line 74), `find_package(GLEW REQUIRED)` (line 75), and `OpenGL::GL GLEW::GLEW` from the link line (77). **`find_package(glfw3 REQUIRED)` and `glfw` stay** — GLFW is the windowing layer for both.

**(d) Lines 118–123** — the define block:

- *Commit 1*: replace with unconditional `target_compile_definitions(X3Engine PUBLIC X3_USE_VULKAN)`. The guards in source still need it.
- *Commit 3* (after all `#ifdef X3_USE_VULKAN` sites are collapsed): **delete the define and its `# Graphics API define` comment entirely.** Verify with `grep -rn "X3_USE_VULKAN" X3/src X3-Editor/src X3-Runtime/src` returning nothing.

**(e) Line 12** — delete the dead comment `# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/Thirdparty/glew-cmake)`. (`X3/Thirdparty/` is an empty directory.)

**Do not touch** lines 80–105 (the `glslc` → SPIR-V custom commands). `find_program(GLSLC_EXECUTABLE …)` at line 81 currently runs unconditionally and must continue to.

### 3.3 `X3-Editor/CMakeLists.txt` (55 lines)

Delete lines 5–7 and 16–18, and retarget the comments on lines 3 and 14:

```cmake
# Find GLFW to provide dependencies for imgui
find_package(glfw3 REQUIRED)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/implot)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui-docking)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/icon-fonts-headers)
add_subdirectory(${CMAKE_SOURCE_DIR}/X3/libs/ImGuizmo ${CMAKE_BINARY_DIR}/ImGuizmo)

# Provide GLFW dependencies to imgui target
target_link_libraries(imgui PRIVATE glfw)
```

### 3.4 `X3-Editor/libs/imgui-docking/CMakeLists.txt` (23 lines) — **HIGHEST-RISK FILE**

This file is **not mentioned in `ENGINE_PLAN.md:105`** and is the single most likely miss. Both of its `if(X3_GRAPHICS_API STREQUAL "Vulkan")` blocks (lines 8 and 20) evaluate to **false** once the variable is deleted, which would (a) compile `imgui_impl_opengl3.cpp` into the `imgui` static library and exclude `imgui_impl_vulkan.cpp`, and (b) drop `Vulkan::Vulkan` from the link — producing undefined references to every `ImGui_ImplVulkan_*` symbol used by `ImGuiContext.cpp` and `ViewportPanel.cpp`. It fails at **link time**, not configure time, which makes it slow to diagnose.

Replace lines 6–23 with:

```cmake
# imgui_impl_opengl3.cpp is vendored upstream but never built — Vulkan-only engine.
list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*imgui_impl_opengl3\\.cpp$")

target_sources(imgui PRIVATE "${IMGUI_SOURCES}")

target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Vulkan REQUIRED)
target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan)
```

Note `file(GLOB_RECURSE IMGUI_SOURCES "*.cpp")` at line 5 also picks up `X3-Editor/libs/imgui-docking/main.cpp`. That is pre-existing behaviour that currently links; **do not change it in this phase.**

If you instead delete `imgui_impl_opengl3.{cpp,h}` and `imgui_impl_opengl3_loader.h` from disk, the `list(FILTER …)` line can go too — but then the vendored tree diverges from upstream. Filtering is the lower-risk choice.

### 3.5 `X3-Runtime/CMakeLists.txt`

**No changes.** It links only `X3Engine` (line 9), inheriting Vulkan and GLFW through `PUBLIC` propagation. (This is *why* `#include <GL/glew.h>` in `RuntimeLayer.h` resolved today, and why §2.7 must remove it.)

### 3.6 `CMakePresets.json` — full replacement

Four presets → two. Note `binaryDir` is `${sourceDir}/build/${presetName}`, so the new build dirs are `build/debug` and `build/release`; the stale `build/opengl-debug/` and `build/vulkan-debug/` must be deleted.

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

### 3.7 `TODO_VULKAN.md`

Line 12 reads `- **Build configurations** — All four builds compile (OpenGL Debug/Release, Vulkan Debug/Release)`. Replace with a note that the build is Vulkan-only Debug/Release, or delete the bullet. Cosmetic; do it in the docs commit.

---

## 4. THE PROJECTMANAGER API-SELECTION TRAP

Four coupled sites. Delete all of them in one commit.

### 4.1 `X3/src/Renderer/RenderSettings.h` (79 lines)

| Lines | Content | Action |
|---|---|---|
| 14–17 | `enum class RendererAPI { OpenGL = 0, Vulkan = 1 };` | **Delete.** Only referenced from lines 29/31/45/61 of this file and `ProjectManager.cpp:114-116`. |
| 26–32 | The `#ifdef __APPLE__` / `RendererAPI rendererAPI = RendererAPI::Vulkan;` / `#else` / `RendererAPI rendererAPI = RendererAPI::OpenGL;` / `#endif` block, plus the `// Use Vulkan on macOS (OpenGL 4.1 lacks compute shaders), OpenGL elsewhere` comment at line 27 | **Delete lines 27–32 entirely** (keep line 26, `ShaderType shaderType = ShaderType::PATH_TRACING;`). This removes the only `__APPLE__` conditional in the struct. |
| 45 | `rsNode["rendererAPI"] = static_cast<int>(rendererAPI);` in `SerializeToYamlNode` | **Delete.** |
| 61 | `if (auto n = rsNode["rendererAPI"]) rendererAPI = static_cast<RendererAPI>(n.as<int>());` in `DeserializeFromYamlNode` | **Delete.** |

`enum class ShaderType` (lines 8–12) is **unrelated and stays.**

### 4.2 `X3/src/Renderer/IRendererAPI.h` (29 lines)

| Lines | Content | Action |
|---|---|---|
| 11–15 | `enum class API { None = 0, OpenGL = 1, Vulkan = 2 };` | **Delete**, together with the now-empty `public:` label on line 10 if it leaves a dangling section. |
| 23 | `static API GetAPI() { return s_API; }` | **Delete.** |
| 24 | `static void SetAPI(API api) { s_API = api; }` | **Delete.** |
| 26–27 | `private:` / `static API s_API;` | **Delete.** |

Surviving class: `Init()`, `Clear(const glm::vec4&)`, `SetViewportSize(uint32_t, uint32_t)` pure virtuals, plus `static std::shared_ptr<IRendererAPI> Create();` (line 22). **`IRendererAPI` and its `Create()` factory both stay** — `X3/src/Core/application.cpp:27-28` calls `IRendererAPI::Create()` then `->Init()`, and `VulkanRendererAPI` derives from it (`VulkanRendererAPI.h:9`). Only the enum-and-static-state half is being removed.

### 4.3 `X3/src/Renderer/IRendererAPI.cpp`

Remove the `s_API` **definition** — lines 13–18 (already covered in §2.1). If this is missed, the file will not link once the declaration is gone from the header.

### 4.4 `X3/src/Renderer/Renderer.h`

Delete lines 87–88:

```cpp
inline static IRendererAPI::API GetAPI() { return IRendererAPI::GetAPI(); } // getter
inline static void SetAPI(IRendererAPI::API api) { IRendererAPI::SetAPI(api); } // setter
```

`grep -rn "Renderer::GetAPI\|Renderer::SetAPI"` across `X3/src`, `X3-Editor/src`, `X3-Runtime/src` returns **zero** call sites. These forwarders are dead already. Keep `#include "Renderer/IRendererAPI.h"` at `Renderer.h:5` — `Renderer.h` still needs it transitively.

### 4.5 `X3/src/Project/ProjectManager.cpp`

Delete lines 113–119:

```cpp
// Set the renderer API based on project settings
IRendererAPI::API rendererAPI = (m_ProjectFile.runtimeRenderSettings.rendererAPI == RendererAPI::Vulkan)
    ? IRendererAPI::API::Vulkan
    : IRendererAPI::API::OpenGL;
IRendererAPI::SetAPI(rendererAPI);
LOG_ENGINE_INFO("OpenProject: Setting renderer API to {}",
    (rendererAPI == IRendererAPI::API::Vulkan) ? "Vulkan" : "OpenGL");
```

Then delete `#include "Renderer/IRendererAPI.h"` at line 4 — it becomes the file's only unused include (grep confirms `IRendererAPI` appears nowhere else in `ProjectManager.cpp`).

`OpenProject()` then flows straight from the `if (!projectFile)` warning at 109–111 to `m_AssetManager = std::make_shared<AssetManager>();` at line 121.

### 4.6 Serialization compatibility — **answer: removing the key breaks nothing**

I read both loaders. Verdict: **no migration, no version bump, no compatibility shim required.**

**Reading old files that still contain `rendererAPI:`.** `RenderSettings::DeserializeFromYamlNode` (`RenderSettings.h:54-77`) is written as a sequence of independent optional lookups:

```cpp
*this = RenderSettings{};                                    // :55  reset to defaults
if (auto n = rsNode["debugMode"]) debugMode = n.as<int>();   // :57
…
if (auto n = rsNode["rendererAPI"]) rendererAPI = …;         // :61  ← the line being deleted
```

It never enumerates the mapping's keys and never validates a schema. yaml-cpp will happily parse a mapping containing `rendererAPI: 0`; with the lookup gone, that key is simply never queried and is silently ignored. Every other field still deserializes identically. The `try`/`catch (const std::exception&)` at 56/74 is not reached.

Two call sites confirm this covers every persisted `RenderSettings`:
- `X3/src/Project/ProjectManager.cpp:49-50` — `.lrproj`, node `RuntimeRenderSettings`.
- `X3-Editor/src/EditorState.cpp:54-55` — `EditorState.yaml`, node `EditorRenderSettings`.

**Writing new files without the key.** `SerializeToYamlNode` (`:40-52`) just stops emitting `rendererAPI`. An *older* binary reading a *new* file hits `if (auto n = rsNode["rendererAPI"])` → false → keeps its platform default from line 29/31. Forward-compatible too.

**Behavioural consequence: none.** This is exactly the trap `ENGINE_PLAN.md:107` describes — every factory already resolved by `#ifdef`, so `SetAPI()` only ever mutated a reporting variable that nothing consumed (§4.4: zero callers of `GetAPI`). Removing it deletes a lie, not a behaviour.

**No in-tree fixtures to migrate:** `find . -name "*.lrproj"` (excluding `build/`) returns nothing, and `X3-Editor/res/EditorState.yaml` is `.gitignore`d (line 15). `ProjectExporter.cpp` copies the `.lrproj` byte-for-byte and renames it (`:34, :61, :110`); it never re-serializes `RenderSettings`, so exported projects are unaffected.

**One thing to leave alone:** `if (auto n = rsNode["vSync"]) vSync = n.as<bool>();` at `:70`. `RenderSettings::vSync` has no references under `Platform/Vulkan/` today — that is Phase 1c item 5, not a dead field to delete here.

---

## 5. SAFE ORDERING AND COMMIT BOUNDARIES

The invariant: **after every commit the tree is configurable and buildable** (modulo the missing `vulkan-headers` on this machine). Never delete a file before nothing references it, and never remove a macro definition before its `#ifdef` sites are gone.

### Commit 0 — `chore: Phase 0 repo and build hygiene`
Commit the pre-existing uncommitted working-tree changes verbatim (§0). Do not mix them into the OpenGL deletion. Also `git add ENGINE_PLAN.md ORCHESTRATION.md` (currently untracked).

### Commit 1 — `build: make the build Vulkan-only`
- Root `CMakeLists.txt`: delete lines 7–9 (§3.1).
- `CMakePresets.json`: full replacement (§3.6).
- `X3/CMakeLists.txt`: §3.2 (a), (b) *first stage — keep the unconditional OpenGL FILTER*, (c), (d) *first stage — unconditional `X3_USE_VULKAN`*, (e).
- `X3-Editor/CMakeLists.txt`: §3.3.
- `X3-Editor/libs/imgui-docking/CMakeLists.txt`: §3.4.
- `rm -rf build/opengl-debug build/vulkan-debug`.

**State after:** `X3_GRAPHICS_API` no longer exists. `X3_USE_VULKAN` is always defined, `X3_USE_OPENGL` never is. All `#ifdef X3_USE_OPENGL` branches are present but unreachable. `X3/src/Platform/OpenGL/*.cpp` are on disk but excluded from the source list. GLEW and OpenGL are unlinked. **This is the highest-value single commit — it is where a build regression would show up, and it is independently bisectable.**

### Commit 2 — `runtime: drop the OpenGL splash screen and GL presentation path`
- `X3-Runtime/src/RuntimeLayer.h` (§2.7) and `X3-Runtime/src/RuntimeLayer.cpp` (§2.8).

Isolated because it is the only **functional regression** in Phase 1a (Phase 1d owes a Vulkan replacement or a formal drop). Keeping it in its own commit makes it trivially revertable and trivially referenceable from Phase 1d.

**State after:** `X3-Runtime` no longer references any GL symbol or GLEW header. Verify: `grep -rnE '\bgl[A-Z]|\bGL_|GLEW' X3-Runtime/src` → empty.

### Commit 3 — `renderer: collapse the graphics-API preprocessor guards`
- `X3/src/Renderer/{IRendererAPI,IComputeShader,ITexture2D,IImage2D,IUniformBuffer,IShaderStorageBuffer}.cpp` (§2.1, §2.2) — **guard collapse only; leave `s_API` for commit 5** (i.e. in `IRendererAPI.cpp` collapse lines 3–8 and 20–29 now, and reduce 13–18 to the single unconditional `IRendererAPI::API IRendererAPI::s_API = IRendererAPI::API::Vulkan;`).
- `X3/src/Platform/Windows/GLFWWindow.cpp` (§2.3).
- `X3-Editor/src/ImGuiContext.cpp` (§2.4).
- `X3-Editor/src/Panels/ViewportPanel/ViewportPanel.{h,cpp}` (§2.5, §2.6).
- `X3/CMakeLists.txt`: §3.2 (d) *second stage — delete the `X3_USE_VULKAN` define.*

Gate before committing: `grep -rn "X3_USE_OPENGL\|X3_USE_VULKAN" X3/src X3-Editor/src X3-Runtime/src CMakeLists.txt X3/CMakeLists.txt X3-Editor/CMakeLists.txt X3-Runtime/CMakeLists.txt X3-Editor/libs/imgui-docking/CMakeLists.txt` → **must return nothing.**

**State after:** every `Platform/OpenGL/*` header is orphaned — nothing includes it.

### Commit 4 — `engine: delete the OpenGL backend`
- `git rm -r X3/src/Platform/OpenGL/`
- `X3/CMakeLists.txt`: §3.2 (b) *second stage — delete the FILTER line and its comment.*

Pure deletion. Zero risk if commit 3's gate passed. If the FILTER removal is forgotten, CMake still configures (the regex just matches nothing) — but leaving it is misleading dead build logic.

### Commit 5 — `project: remove the RendererAPI setting`
- All of §4: `RenderSettings.h`, `IRendererAPI.h`, `IRendererAPI.cpp` (`s_API` definition), `Renderer.h`, `ProjectManager.cpp`.

Deliberately last: it is the only commit with a persisted-format implication, and isolating it makes the compatibility reasoning in §4.6 reviewable on its own.

Gate: `grep -rn "RendererAPI\|GetAPI\|SetAPI\|s_API" X3/src X3-Editor/src X3-Runtime/src` → only `IRendererAPI` the class name, `VulkanRendererAPI`, and `#include "Renderer/IRendererAPI.h"` should remain.

### Commit 6 — `docs: reflect the Vulkan-only build`
- `TODO_VULKAN.md:12` (§3.7). Optionally note in `ENGINE_PLAN.md` that 1a is complete.

### Post-flight verification (no compiler required)

```
grep -rn "X3_USE_OPENGL\|X3_USE_VULKAN\|X3_GRAPHICS_API" . --include='*.h' --include='*.cpp' --include='*.txt' --include='*.json' | grep -v '^./build/'
grep -rniE 'glew|glad|opengl' X3/src X3-Editor/src X3-Runtime/src
grep -rnE '\bgl[A-Z][A-Za-z]*\(|\bGL_[A-Z_]+|\bGLuint\b|\bGLint\b|\bGLenum\b' X3/src X3-Editor/src X3-Runtime/src | grep -v glfw
ls X3/src/Platform          # → Vulkan  Windows
```

Expected residue from the second grep, all benign comments to be left alone: `AssetManager.cpp:259`, `VulkanImage2D.cpp:74`, `VulkanComputeShader.cpp:91,96`, `VulkanContext.cpp:10,903`, and the three `res/shaders/*.comp:6`.

---

## 6. LOOKS DELETABLE — BUT IS NOT

1. **`X3/src/Renderer/IRenderingContext.{h,cpp}`.** A two-method abstract base (`init()`, `swapBuffers()`) whose only remaining implementer is `VulkanContext`. It reads as pure portable-RHI vestige. **Keep it in 1a.** `GLFWWindow.h:53` stores `IRenderingContext* m_Context` and `GLFWWindow.cpp:82` (`onUpdate`) and `:89` (`swapBuffers`) dispatch through it. Removing it means retyping `m_Context` to `VulkanContext*` and dealing with `swapBuffers()` — which is exactly the `ENGINE_PLAN.md:120` item (`swapBuffers()` → `beginFrame()`/`endFrame()`/`present()`). That is **1b**. Touching it here entangles a mechanical deletion with an interface redesign.

2. **`IRendererAPI` the class, and `IRendererAPI::Create()`.** Only the `enum class API` / `GetAPI` / `SetAPI` / `s_API` half dies. The class survives: `X3/src/Core/application.cpp:27-28` does `_RendererAPI = IRendererAPI::Create(); _RendererAPI->Init();`, `application.cpp:55,57` calls `_RendererAPI->Clear(...)`, `application.h:12,28` forward-declares and stores it, and `VulkanRendererAPI` (`VulkanRendererAPI.h:9`) is its only implementer.

3. **`IImage2D::GetID()` (`IImage2D.h:19`) and `ITexture2D::GetID()` (`ITexture2D.h:13`).** After the GL blit in `RuntimeLayer.cpp:165` and the GL `AddImage` in `ViewportPanel.cpp:291` are deleted, `GetID()` *looks* orphaned. It is not: **`ViewportPanel.cpp:49-52`** uses `image->GetID()` as the cache key that decides whether to re-register the ImGui Vulkan descriptor (`m_LastRegisteredImageID`). Removing `GetID()` in 1a silently breaks descriptor invalidation on image recreation. Its `int` return truncating a 64-bit `VkImage` is a real bug — `ENGINE_PLAN.md:116` schedules it for **1b**.

4. **`enum struct Image2DType` (`IImage2D.h:8-12`) and `Bind()`/`Unbind()`/`AddData()`/`ChangeImageUnit()`/`ChangeTextureUnit()`.** All GL vestiges, all explicitly listed in the 1b table (`ENGINE_PLAN.md:113-121`). `Renderer.cpp:202-203` still passes `Image2DType::LR_READ_WRITE`; `Renderer.cpp:220-226` still calls `AddData(offset, size, ptr)`. **Do not remove any of these in 1a** — they are load-bearing today and their replacements do not exist yet.

5. **`X3-Runtime/src/RuntimeLayer.cpp:306-376` `CalculateViewportCoordinates()` and the `m_ViewportCoords` `glm::ivec4`.** The `(x, y, x+width, y+height)` packing is literally `glBlitFramebuffer`'s convention and the header comment says so (`RuntimeLayer.h:53`). **The Vulkan path depends on it**: `VulkanContext.cpp:903-907` documents and consumes exactly that packing —
   ```cpp
   // viewport: x, y, x+width, y+height (matching OpenGL glBlitFramebuffer convention)
   blitRegion.dstOffsets[0] = {viewport.x, windowSize.y - viewport.w, 0}; // Flip Y
   blitRegion.dstOffsets[1] = {viewport.z, windowSize.y - viewport.y, 1}; // Flip Y
   ```
   Do not "modernize" the convention while deleting GL. Retarget the comments; leave the arithmetic alone.

6. **`X3/res/made_with_X3.png`.** Its only reader (`RuntimeLayer.cpp:40`) is being deleted, so it looks like an orphan. **Keep it** — Phase 1d (`ENGINE_PLAN.md:133`) may re-implement the splash on Vulkan, and `X3/CMakeLists.txt:133-134` installs the whole `res/` tree regardless. Deleting a ~KB image buys nothing and loses the asset.

7. **`stbi_set_flip_vertically_on_load(1); // OpenGL-style orientation` at `AssetManager.cpp:259`.** The comment invites deletion. **Do not touch it.** The flip is baked into two downstream consumers: the skybox sampling in the compute shaders, and `ViewportPanel.cpp:288` which draws with flipped UVs `{0,1},{1,0}`. Changing the load orientation flips every texture in the engine. If the convention should change, that is a deliberate Phase 2 texture-pipeline decision.

8. **`glfwSwapInterval(enabled)` in `GLFWWindowIMPL::setVSync` (`GLFWWindow.cpp:110`).** A GL/EGL-context call that is a no-op with a Vulkan-only GLFW window. **Leave it.** `RuntimeLayer.cpp:75` and `application` still call `setVSync`; ripping the body out here would make `m_VSync` silently meaningless. Wiring vSync to the swapchain present mode is `ENGINE_PLAN.md:128`, **Phase 1c item 5**.

9. **`find_package(glfw3 REQUIRED)` and `target_link_libraries(… glfw)`** in `X3/CMakeLists.txt:70-71` and `X3-Editor/CMakeLists.txt:4,15`. These sit right next to the OpenGL calls in the same `if/else` and are easy to remove by proximity. GLFW is the windowing layer for the Vulkan path (`GLFWWindow.h:3`, `VulkanContext::setWindowHints()`, `ImGui_ImplGlfw_InitForVulkan`). **Both must survive.**

10. **`imgui_impl_glfw.cpp` / `imgui_impl_glfw.h`** in `X3-Editor/libs/imgui-docking/`. Name-adjacent to `imgui_impl_opengl3.*` but is the platform backend, used by `ImGuiContext.cpp:3,158,266` and `ViewportPanel`. Only the `opengl3` files are candidates.

11. **`ImGuiConfigFlags_ViewportsEnable` handling at `ImGuiContext.cpp:143-151` and `:299-305`.** Once line 130 is deleted the flag is never set, so both blocks become dead. **Keep them.** They are the exact re-enable point for Phase 13 (`ENGINE_PLAN.md:364`), and `glfwGetCurrentContext()`/`glfwMakeContextCurrent()` at 301/304 are the GL-context save/restore that a future Vulkan multi-viewport path will need replaced, not deleted-and-rewritten from memory.

12. **The `SET(x)` macro block in `X3/res/shaders/{PathTracing,PBR,Phong}.comp:6-14`.** The `#else` branch (`#define SET(x)` empty) is dead under `glslc`, which predefines `VULKAN=100`. **Do not collapse it in 1a.** Phase 0 verified the committed `.spv` are byte-identical to a fresh rebuild; editing shader source here means re-verifying that for zero functional gain, and Phase 3 rewrites these files in Slang anyway (`ENGINE_PLAN.md:163-192`). The three `// OpenGL doesn't support Vulkan descriptor sets` comments are the only OpenGL residue in `res/` and they are harmless.

13. **`X3/src/Platform/Windows/`.** The directory name suggests a Win32 backend to delete alongside `Platform/OpenGL/`. It contains `GLFWWindow.{h,cpp}` — the **only** window implementation, instantiated unconditionally by `IWindow.cpp:8`. (The misleading directory name is worth a rename, but not in this phase.)

14. **`X3/src/Renderer/IRenderingContext.cpp`** is a one-line file (`#include "Renderer/IRenderingContext.h"`). It exists solely to give the TU a home. Deleting it is harmless but pointless; it goes when `IRenderingContext` goes, in 1b.