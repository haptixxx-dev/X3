# Vulkan Backend Implementation — Superseded

This file is superseded by `ENGINE_PLAN.md`, which is the authoritative plan for all engine work.

## Completed

The following were verified as complete during the Vulkan migration:

- **VulkanImage2D** — Implemented for compute shader output
- **VulkanShaderStorageBuffer** — Implemented for BVH, mesh, material, and light data
- **RuntimeLayer Vulkan frame presentation** — Via `VulkanContext::blitImageToSwapchain()`
- **Build configurations** — Vulkan-only Debug/Release both compile

## Correction Preserved

The old item "Add `-DVULKAN` define when compiling shaders" is **unnecessary and verified as such**. The `glslc` compiler automatically predefines `VULKAN=100` when targeting Vulkan. This was confirmed by:

1. Compiling a probe shader containing `#ifndef VULKAN / #error / #endif` — it compiled clean
2. Disassembling `PathTracing.comp.spv` with `spirv-dis` — shows correct `DescriptorSet 0/1/2` decorations matching C++ descriptor tables
3. Verifying the committed `.spv` files are byte-identical to a fresh rebuild — they are not stale

## Remaining Work

- Vulkan splash screen in RuntimeLayer → **Phase 1d** in ENGINE_PLAN.md
- ImGui multi-viewport under Vulkan → **Phase 13** in ENGINE_PLAN.md
