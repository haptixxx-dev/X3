#pragma once

// =============================================================================
// VulkanGraphicsPipeline -- the first thing in this engine that draws geometry.
//
// Before Phase 7 the renderer had ONLY ever dispatched compute. There was no
// graphics pipeline, no vertex input state, no depth state, and no draw
// submission path anywhere in the codebase.
//
// NO VERTEX INPUT STATE, DELIBERATELY. This engine pulls vertices from the
// StructuredBuffer it already uploads (Gpu::Vertex, set 2 binding 8) using
// SV_VertexID, rather than binding a vertex buffer and declaring attributes.
// Three reasons:
//   * The buffer already exists and is already bound -- the path tracer reads
//     the same one at every hit. A separate vertex buffer would be a second copy
//     of identical data, and Phase 9's cook step would then have to produce both.
//   * Vertex input state is per-pipeline, so every attribute layout is another
//     pipeline permutation. Pulling makes the layout a shader-side concern.
//   * It is what makes the raster path and the path tracer read the SAME bytes
//     through the SAME struct. If they disagreed about vertex layout, "validate
//     the rasterizer against the reference" would be comparing two different
//     meshes.
// An index buffer is still bound: vkCmdDrawIndexed needs one, and post-transform
// vertex reuse is the entire point of indexed drawing.
//
// NO RENDER PASS AND NO FRAMEBUFFER. Dynamic rendering is core in Vulkan 1.3 and
// this engine requires it (locked decision 12). Attachment formats are declared
// here via VkPipelineRenderingCreateInfo and the attachments themselves are named
// at draw time by the render graph.
//
// Move semantics: exactly VulkanComputePipeline's contract, for exactly the same
// reasons -- the displaced handles are destroyed INLINE rather than deferred, so
// a pipeline may only be displaced at shutdown after vkDeviceWaitIdle, and every
// VulkanDescriptorSetRing allocated from its layouts holds a raw pointer into
// its m_SetLayouts vector. Rings and pipelines live and die together.
// =============================================================================

#include "Platform/Vulkan/VulkanDescriptors.h"
#include "Platform/Vulkan/VulkanTypes.h"

#include <cassert>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace X3
{

class VulkanContext;

struct GraphicsPipelineDesc
{
	// FULL paths to the .spv files, extension included -- the caller appends it,
	// matching ComputePipelineDesc. A fragment stage is OPTIONAL: a depth prepass
	// legitimately has none, and declaring one that writes nothing is slower than
	// declaring none at all because it defeats early-Z rejection.
	std::filesystem::path vertexSpirv;
	std::filesystem::path fragmentSpirv;      // empty == depth-only pipeline
	std::string           vertexEntry   = "main";
	std::string           fragmentEntry = "main";

	// index == descriptor set number; contiguous from 0; no entry may be empty.
	std::vector<std::vector<DescriptorBindingDesc>> setLayouts;

	uint32_t pushConstantSize = 0;   // 0 == no push constant range

	// Attachment formats, for VkPipelineRenderingCreateInfo. These must match
	// what the render graph actually binds, and a mismatch is a validation error
	// at draw time rather than at pipeline creation.
	std::vector<VkFormat> colorFormats;
	VkFormat              depthFormat = VK_FORMAT_UNDEFINED;

	bool        depthTest  = true;
	bool        depthWrite = true;
	// GREATER, not LESS, because the projection is REVERSE-Z: near maps to 1 and
	// far to 0 so that float precision and the projection's hyperbolic
	// distribution cancel instead of compounding. The depth buffer clears to 0.
	VkCompareOp depthCompare = VK_COMPARE_OP_GREATER;

	VkCullModeFlags cullMode  = VK_CULL_MODE_BACK_BIT;

	// COUNTER_CLOCKWISE, and this is a matched pair with the projection matrix.
	//
	// It is determined by two things together: the projection is LEFT-HANDED
	// (Trace.slang generates camera rays along +Z, so the raster path must agree)
	// and it carries NO `proj[1][1] *= -1` Y-flip, because going left-handed
	// already inverts Y relative to glm's right-handed forms. Either one of those
	// on its own reverses apparent winding; both together cancel.
	//
	// Getting it backwards is not a validation error and not a crash. Closed
	// meshes look unchanged -- with reverse-Z GREATER testing the nearest surface
	// wins whether or not back faces were drawn -- and only single-sided geometry
	// silently vanishes. Measured on the `lights` fixture: CCW is bit-identical to
	// VK_CULL_MODE_NONE, while CW drops coverage from 66% to 14% by culling the
	// ground plane.
	//
	// If the handedness or the Y-flip ever changes, re-measure rather than reason
	// about it -- that is what `--filter depth` and CULL_MODE_NONE are for.
	VkFrontFace     frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	// Alpha blending for the transparent pass. Off means the colour attachment is
	// written directly.
	bool blendEnable = false;

	const char* debugName = nullptr;
};

class VulkanGraphicsPipeline
{
public:
	VulkanGraphicsPipeline() = default;

	// DOES NOT THROW ON FAILURE: logs and leaves valid() == false, matching
	// VulkanComputePipeline. A missing or malformed .spv is a content error the
	// editor must survive rather than a reason to abort.
	VulkanGraphicsPipeline(VulkanContext& ctx, const GraphicsPipelineDesc& desc);
	~VulkanGraphicsPipeline();

	VulkanGraphicsPipeline(VulkanGraphicsPipeline&&) noexcept;
	VulkanGraphicsPipeline& operator=(VulkanGraphicsPipeline&&) noexcept;
	VulkanGraphicsPipeline(const VulkanGraphicsPipeline&)            = delete;
	VulkanGraphicsPipeline& operator=(const VulkanGraphicsPipeline&) = delete;

	bool     valid()    const { return m_Pipeline != VK_NULL_HANDLE; }
	uint32_t setCount() const { return uint32_t(m_SetLayouts.size()); }

	const VulkanDescriptorSetLayout& setLayout(uint32_t set) const
	{
		assert(set < m_SetLayouts.size() && "descriptor set index out of range for this pipeline");
		return m_SetLayouts[set];
	}

	// vkCmdBindPipeline + vkCmdBindDescriptorSets, plus the dynamic viewport and
	// scissor. Call once per pass, then issue draws.
	//
	// VIEWPORT AND SCISSOR ARE DYNAMIC STATE rather than baked in, so a
	// resolution change does not rebuild every pipeline. That matters more here
	// than it sounds: the editor's viewport resizes continuously while dragging a
	// panel edge, and a pipeline rebuild per frame during a drag is a visible
	// stall.
	void bind(const FrameContext& frame,
	          std::span<const VkDescriptorSet> sets,
	          VkExtent2D extent) const;

	// vkCmdPushConstants into the bound pipeline layout. `size` must not exceed
	// the pushConstantSize the pipeline declared; asserted.
	void pushConstants(const FrameContext& frame, const void* data, uint32_t size) const;

	VkPipeline       handle() const { return m_Pipeline; }
	VkPipelineLayout layout() const { return m_Layout; }

private:
	void destroy();

	VulkanContext*   m_Ctx      = nullptr;
	VkPipeline       m_Pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_Layout   = VK_NULL_HANDLE;
	uint32_t         m_PushConstantSize = 0;
	std::vector<VulkanDescriptorSetLayout> m_SetLayouts;
	const char*      m_DebugName = nullptr;
};

}
