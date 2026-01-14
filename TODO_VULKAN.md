# Vulkan Backend Implementation TODO

## Missing Vulkan Implementations

### High Priority (Core Functionality)

- [x] **VulkanImage2D** - `X3/src/Platform/Vulkan/VulkanImage2D.h/.cpp`
  - ~~Currently returns `nullptr` in `IImage2D::Create()` for Vulkan builds~~
  - Needed for compute shader output images
  - Reference: `OpenGLImage2D` implementation

- [x] **VulkanShaderStorageBuffer** - `X3/src/Platform/Vulkan/VulkanShaderStorageBuffer.h/.cpp`
  - ~~Currently returns `nullptr` in `IShaderStorageBuffer::Create()` for Vulkan builds~~
  - Needed for BVH, mesh, material, and light data storage
  - Reference: `OpenGLShaderStorageBuffer` implementation

### Medium Priority (Runtime/Editor)

- [x] **RuntimeLayer Vulkan Frame Presentation** - `X3-Runtime/src/RuntimeLayer.cpp`
  - ~~`onUpdate()` has empty `#else` block for Vulkan~~
  - Uses `VulkanContext::blitImageToSwapchain()` for presentation
  - ~~Needs framebuffer blitting equivalent~~

- [ ] **RuntimeLayer Vulkan Splash Screen** - `X3-Runtime/src/RuntimeLayer.cpp`
  - Logo rendering is OpenGL-only
  - Could use Vulkan render pass or skip splash on Vulkan

### Low Priority (Polish)

- [ ] **Remove unused OpenGL dependencies from Vulkan build**
  - imgui library still links both backends
  - Consider building imgui conditionally or as static library

- [ ] **Shader compilation flags**
  - Add `-DVULKAN` define when compiling shaders for Vulkan builds
  - Currently shaders check `#ifdef VULKAN` for descriptor set syntax

## Files Modified for Build-Time API Selection

Reference for where guards were added:

```
CMakeLists.txt                          # X3_GRAPHICS_API option
X3/CMakeLists.txt                       # Conditional linking/sources
X3/src/Renderer/IRendererAPI.cpp        # Factory guards
X3/src/Renderer/IComputeShader.cpp      # Factory guards
X3/src/Renderer/ITexture2D.cpp          # Factory guards
X3/src/Renderer/IImage2D.cpp            # Factory guards (+ TODO stub)
X3/src/Renderer/IUniformBuffer.cpp      # Factory guards
X3/src/Renderer/IShaderStorageBuffer.cpp # Factory guards (+ TODO stub)
X3/src/Platform/Windows/GLFWWindow.cpp  # Context creation guards
X3-Editor/src/ImGuiContext.cpp          # ImGui backend guards
X3-Runtime/src/RuntimeLayer.h           # Member variable guards
X3-Runtime/src/RuntimeLayer.cpp         # OpenGL code guards
```

## Testing Checklist

- [x] OpenGL Debug build compiles
- [x] OpenGL Release build compiles
- [x] Vulkan Debug build compiles
- [x] Vulkan Release build compiles
- [ ] OpenGL build runs correctly
- [ ] Vulkan build runs correctly (requires above TODOs)
