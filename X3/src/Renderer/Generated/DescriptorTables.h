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

	// Set N is entry N. Every compute pipeline in this engine uses this same
	// table, because every entry point imports the same Bindings.slang.
	inline const std::vector<std::vector<DescriptorBindingDesc>> kComputeSetLayouts = {
		// ---- set 0 ----
		{
			{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // rayTracingTexture
			{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // skyboxTexture
			{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128, VK_SHADER_STAGE_COMPUTE_BIT},   // u_MaterialTextures
		},
		// ---- set 1 ----
		{
			{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // u_Camera
			{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // u_Settings
		},
		// ---- set 2 ----
		{
			{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // EntityLookupTable
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // TransformBuffer
			{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // MaterialBuffer
			{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // TriPositionBuffer
			{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // NodeBuffer
			{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // BvhPrimIndex
			{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // LightBuffer
			{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // TriRefBuffer
			{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},   // VertexBuffer
		},
	};

	// Array binding declared by the shader, for cross-checking against
	// the C++ constant that fills it.
	inline constexpr uint32_t kU_MaterialTexturesCount = 128u;

}
