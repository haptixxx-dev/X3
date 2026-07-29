// GENERATED FILE -- DO NOT EDIT.
//
// Produced by scripts/gen_descriptor_tables.py from the Slang reflection
// output of X3/res/shaders/*.slang. Edit Bindings.slang and rebuild; the
// build regenerates this and a stale copy cannot survive a compile.
//
// This replaces the hand-written table that used to live in Renderer.cpp,
// matched to the shader source by comment. The C++ can no longer disagree
// with the shader, because it is derived from it.
#pragma once

#include "Platform/Vulkan/VulkanTypes.h"

#include <vector>

namespace X3::Generated
{

	// ONE TABLE SERVES EVERY PIPELINE, compute and raster alike, so every
	// binding must be visible to every stage that might consume it. Declaring
	// only COMPUTE was correct until Phase 7 added a vertex stage, and the
	// failure is VUID-VkGraphicsPipelineCreateInfo-layout-07988 at pipeline
	// creation -- loud, but only once a raster pipeline exists to trip it.
	//
	// Declaring a stage that never reads a binding costs nothing: stageFlags
	// is a visibility mask, not a promise of use.
	inline constexpr VkShaderStageFlags kAllStages =
		VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	// Set N is entry N. Every compute pipeline in this engine uses this same
	// table, because every entry point imports the same Bindings.slang.
	inline const std::vector<std::vector<DescriptorBindingDesc>> kComputeSetLayouts = {
		// ---- set 0 ----
		{
			{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kAllStages},   // rayTracingTexture
			{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kAllStages},   // skyboxTexture
			{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128, kAllStages},   // u_MaterialTextures
			{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kAllStages},   // u_BsdfLut
			{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kAllStages},   // u_SceneDepth
			{5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kAllStages},   // u_SceneVelocity
			{6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kAllStages},   // u_ShadowAtlas
		},
		// ---- set 1 ----
		{
			{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kAllStages},   // u_Camera
			{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kAllStages},   // u_Settings
			{2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kAllStages},   // u_Shadow
		},
		// ---- set 2 ----
		{
			{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // EntityLookupTable
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // TransformBuffer
			{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // MaterialBuffer
			{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // TriPositionBuffer
			{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // NodeBuffer
			{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // BvhPrimIndex
			{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // LightBuffer
			{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // TriRefBuffer
			{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // VertexBuffer
			{9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // MaterialExtBuffer
			{10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // MeshIndexBuffer
			{11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // ClusterAABBBuffer
			{12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // ClusterLightGrid
			{13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // ClusterLightIndices
			{14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages},   // PrevTransformBuffer
		},
	};

	// Array binding declared by the shader, for cross-checking against
	// the C++ constant that fills it.
	inline constexpr uint32_t kU_MaterialTexturesCount = 128u;

}
