// =============================================================================
// Bindings -- every descriptor this engine's compute shaders declare.
//
// All three entry points declare the SAME three sets, which is why one module
// serves all of them and why one C++ descriptor table serves all three
// pipelines. Set 0 is images, set 1 uniforms, set 2 storage buffers.
//
// THE C++ SIDE IS GENERATED FROM THIS, NOT HAND-WRITTEN. slangc emits reflection
// JSON, scripts/gen_descriptor_tables.py turns it into
// X3/src/Renderer/Generated/DescriptorTables.h, and Renderer.cpp includes that.
// Adding a binding here and forgetting the C++ table is no longer possible --
// which was the whole failure mode of the hand-synced table this replaces.
//
// Explicit [[vk::binding(binding, set)]] rather than ParameterBlock<T>: the
// binding numbers must match what the pre-Slang GLSL used, because Phase 3's
// gate is that the rendered output is unchanged. ParameterBlock would assign
// them itself and any drift would be invisible until a frame looked wrong.
// =============================================================================

module Bindings;

import GpuTypes;
__exported import GpuTypes;

// ---- Set 0: images ----------------------------------------------------------
[[vk::image_format("rgba32f")]]
[[vk::binding(0, 0)]] public RWTexture2D<float4> rayTracingTexture;

[[vk::binding(1, 0)]] public Sampler2D skyboxTexture;

// The material texture table. Fixed size, EVERY element always written (unused
// ones point at a shared 1x1 dummy), indexed with NonUniformResourceIndex()
// because the material index diverges across lanes in a path tracer.
[[vk::binding(2, 0)]] public Sampler2D u_MaterialTextures[X3_MAX_MATERIAL_TEXTURES];

// ---- Set 1: uniform buffers -------------------------------------------------
[[vk::binding(0, 1)]] public ConstantBuffer<CameraUBO>   u_Camera;
[[vk::binding(1, 1)]] public ConstantBuffer<SettingsUBO> u_Settings;

// ---- Set 2: storage buffers -------------------------------------------------
[[vk::binding(0, 2)]] public StructuredBuffer<EntityHandle>      EntityLookupTable;
[[vk::binding(1, 2)]] public StructuredBuffer<float4x4>          TransformBuffer;
[[vk::binding(2, 2)]] public StructuredBuffer<Material>          MaterialBuffer;
[[vk::binding(3, 2)]] public StructuredBuffer<TrianglePositions> TriPositionBuffer;
[[vk::binding(4, 2)]] public StructuredBuffer<BVHNode>           NodeBuffer;
[[vk::binding(5, 2)]] public StructuredBuffer<uint>              BvhPrimIndex;
[[vk::binding(6, 2)]] public StructuredBuffer<LightData>         LightBuffer;
[[vk::binding(7, 2)]] public StructuredBuffer<TriRef>            TriRefBuffer;
[[vk::binding(8, 2)]] public StructuredBuffer<Vertex>            VertexBuffer;
[[vk::binding(9, 2)]] public StructuredBuffer<MaterialExt>      MaterialExtBuffer;
