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

// The BSDF energy LUT: the split-sum (scale, bias) pair for THIS FILE'S GGX
// lobe, so E_spec = f0 * r + g exactly rather than approximately.
//
// A STORAGE IMAGE, not a sampled texture, and read with a hand-written bilinear
// fetch. It is written by the bake pass and read by every shading pass, and
// binding it both ways would mean two descriptors and a sampler for a 32x32
// image whose filtering is four loads and three lerps.
[[vk::image_format("rgba32f")]]
[[vk::binding(3, 0)]] public RWTexture2D<float4> u_BsdfLut;

// The depth buffer the prepass wrote. Sampled, not a storage image: D32_SFLOAT
// is not a storage-image format on most hardware.
[[vk::binding(4, 0)]] public Sampler2D u_SceneDepth;

// The velocity buffer the velocity pass wrote: screen-space motion in UV units,
// (current UV - previous UV). Sampled for the same reason the depth buffer is --
// RG16F is a colour attachment here, not a storage format anything writes from a
// compute shader.
[[vk::binding(5, 0)]] public Sampler2D u_SceneVelocity;

// The cascaded shadow map. ONE WIDE 2D ATLAS, cascades side by side, rather than
// a texture array: VulkanImage hardcodes arrayLayers = 1 and the render graph
// tracks one layout per whole image, so a per-layer read/write hazard is not
// expressible. An atlas needs none of that -- each cascade is a scissored
// sub-rect and sampling is an offset into the u coordinate.
//
// A PLAIN Sampler2D, not a SamplerComparisonState. Hardware PCF would be one
// tap instead of nine, but it needs a comparison sampler at this binding and the
// generated descriptor table would have to learn that resource kind first -- see
// scripts/gen_descriptor_tables.py, which exits rather than guessing at a shape
// it does not recognise. The manual 3x3 in Trace.slang is the honest starting
// point and the comparison path is a measured optimisation, not a rewrite.
[[vk::binding(6, 0)]] public Sampler2D u_ShadowAtlas;

// Bloom ping-pong, both at HALF the render resolution. Two fixed bindings rather
// than a mip chain of N: one descriptor table serves every pipeline here, so an
// N-level chain would mean N bindings written by every pass in the engine. Two
// images and four passes buy a real bloom without that.
//
// A PROGRESSIVE DOWNSAMPLE/UPSAMPLE PYRAMID IS THE BETTER ALGORITHM and is what
// ENGINE_PLAN.md asks for: it reaches a far wider radius for the same cost and
// does not band on large bright areas. It needs either per-pass descriptor
// variance or mip support in VulkanImage, neither of which exists. This is the
// honest smaller version, and the upgrade is a change to these two declarations
// plus the pass list.
[[vk::image_format("rgba16f")]]
[[vk::binding(7, 0)]] public RWTexture2D<float4> u_BloomA;
[[vk::image_format("rgba16f")]]
[[vk::binding(8, 0)]] public RWTexture2D<float4> u_BloomB;

// The TAA history, PING-PONGED: read one, write the other, swapped every frame.
//
// ONE IMAGE READ AND WRITTEN BY THE SAME DISPATCH IS A RACE, and a subtle one.
// The resolve reads history at the REPROJECTED texel and writes at its own, and
// with any camera motion those differ -- so another invocation may overwrite the
// texel this one is about to read. Nothing orders them. It does not crash and it
// does not trip validation; it makes the output differ between runs of the same
// build, which is what caught it: three runs of the same golden gave three
// different rmse values.
[[vk::image_format("rgba16f")]]
[[vk::binding(9, 0)]] public RWTexture2D<float4> u_TaaHistoryRead;
[[vk::image_format("rgba16f")]]
[[vk::binding(10, 0)]] public RWTexture2D<float4> u_TaaHistoryWrite;

// DDGI probe atlases. See Ddgi.slang for the tile layout and why each carries a
// one-texel border.
//
// THE DEPTH ATLAS IS 32-BIT, and that is not a spare-precision choice. Chebyshev
// computes variance as E[r^2] - E[r]^2, a difference of two large near-equal
// numbers. In fp16 at a mean distance of 100 both are around 1e4 with ~1e-3
// relative precision, so the subtraction returns noise, the visibility weight
// goes random per texel, and the GI flickers in a way that reads as a race and
// is not one. Irradiance has no such subtraction and fp16 is fine there.
[[vk::image_format("rgba16f")]]
[[vk::binding(11, 0)]] public RWTexture2D<float4> u_DdgiIrradiance;
[[vk::image_format("rg32f")]]
[[vk::binding(12, 0)]] public RWTexture2D<float2> u_DdgiDepth;

// ---- Set 1: uniform buffers -------------------------------------------------
[[vk::binding(0, 1)]] public ConstantBuffer<CameraUBO>   u_Camera;
[[vk::binding(1, 1)]] public ConstantBuffer<SettingsUBO> u_Settings;
[[vk::binding(2, 1)]] public ConstantBuffer<ShadowUBO>   u_Shadow;
[[vk::binding(3, 1)]] public ConstantBuffer<DdgiUBO>     u_Ddgi;

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

// Flat triangle list: three GLOBAL vertex indices per triangle, in
// TriPositionBuffer order. Derived from TriRefBuffer at upload time rather than
// stored in the asset pool, because it is pure redundancy on the CPU side -- the
// rasterizer needs it as a real VK_BUFFER_USAGE_INDEX_BUFFER allocation and
// nothing else reads it.
[[vk::binding(10, 2)]] public StructuredBuffer<uint>            MeshIndexBuffer;

// ---- Clustered Forward+ ------------------------------------------------------
// RW, not read-only, because ClusterBuild and LightCull write them and every
// other pass reads them -- and one descriptor table serves every pipeline, so
// there is exactly one declaration and it has to be the writable one. A
// StructuredBuffer and an RWStructuredBuffer over the same VkBuffer would be two
// bindings for one resource.
//
// The grid is X*Y*Z clusters; ClusterLightGrid holds a COUNT per cluster and the
// indices live at a fixed offset, cluster * X3_MAX_LIGHTS_PER_CLUSTER. There is
// no offset table because there is nothing to compact.
[[vk::binding(11, 2)]] public RWStructuredBuffer<ClusterAABB>   ClusterAABBBuffer;
[[vk::binding(12, 2)]] public RWStructuredBuffer<uint>          ClusterLightGrid;
[[vk::binding(13, 2)]] public RWStructuredBuffer<uint>          ClusterLightIndices;

// LAST FRAME'S transforms, indexed exactly as TransformBuffer is. The velocity
// pass needs where a vertex WAS, and nothing in the scene data records that --
// the CPU keeps the previous frame's upload alive in a second buffer.
//
// Valid only when the entity set did not change between frames; Renderer copies
// the current transforms in when it did, which reports zero motion rather than
// garbage motion for an entity whose index now means something else.
[[vk::binding(14, 2)]] public StructuredBuffer<float4x4>        PrevTransformBuffer;

// Per-probe ray results from DdgiProbeTrace, consumed by DdgiProbeBlend.
// xyz radiance, w distance -- negative distance flags a backface hit.
[[vk::binding(15, 2)]] public RWStructuredBuffer<float4>       DdgiRayDataBuffer;
