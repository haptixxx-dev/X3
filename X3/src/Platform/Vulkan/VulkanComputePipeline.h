#pragma once

// =============================================================================
// VulkanComputePipeline.h -- the compute pipeline object. Replaces
// IComputeShader / VulkanComputeShader (MERGED-3 §3.2.7).
//
// WHY THIS FILE EXISTS AT ALL, stated plainly: VulkanDescriptors.h's own worked
// example -- the one at the top of that file, which is the layer's canonical
// "how a caller writes for the current frame" -- reads
//
//     DescriptorWriter w(ctx, ring, frame);
//     ...
//     pipeline.dispatch(frame, sets, gx, gy, gz);
//
// and an earlier revision of it named `pipeline.setLayout(2)`. Neither
// `setLayout` nor `dispatch` was declared anywhere in the tree. A worked example
// that calls two functions that do not exist is not documentation; it is a
// promise the next agent has to guess at. This header is that promise written
// down.
//
// It owns the descriptor set LAYOUTS. That is the ownership fact everything else
// in the layer is anchored to:
//   * VulkanDescriptorSetRing borrows a layout from here and keeps a pointer to
//     it (VulkanDescriptorSetRing::layout()), which is what lets DescriptorWriter
//     take a (ring, frame) pair instead of a mismatchable (layout, set) pair.
//   * VulkanDescriptorSetLayout's destructor is allowed to call
//     vkDestroyDescriptorSetLayout directly, without the deferred queue, ONLY
//     because layouts die here, with their pipeline, in Renderer::Shutdown()
//     after vkDeviceWaitIdle.
// Both facts stop being true if a layout is ever owned somewhere else. Do not
// move them.
//
// Ownership: Renderer owns pipelines by value in
// `std::unordered_map<ShaderType, VulkanComputePipeline>`. GetOrLoadShader
// returns a raw `VulkanComputePipeline*` into that map, which is stable across
// rehash because std::unordered_map never moves its elements. Destroyed in
// Renderer::Shutdown(), which runs from RenderLayer::onDetach() after
// vkDeviceWaitIdle.
//
// THE RENDER-PASS BUG THIS CLASS IS THE OTHER HALF OF. ADJUDICATION.md's last
// section: today beginFrame() opens m_RenderPass (VulkanContext.cpp:392) and
// leaves it open for the whole frame, and VulkanComputeShader::Dispatch records
// vkCmdDispatch (VulkanComputeShader.cpp:137) and a vkCmdPipelineBarrier
// (:146-153) into it. vkCmdDispatch inside a render pass instance is invalid
// (VUID-vkCmdDispatch-renderpass) and the barrier is invalid too, because a
// barrier inside a render pass needs a matching subpass self-dependency and
// createRenderPass declares only EXTERNAL -> 0. The dynamic-rendering migration
// dissolves it: beginFrame() opens no rendering block, so dispatch() below
// records at top level. dispatch() therefore asserts that no rendering block is
// open, which is the standing guard against the bug coming back.
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

// -----------------------------------------------------------------------------
// Creation description. `setLayouts` is index == descriptor set number, and the
// set indices must be contiguous from 0.
//
// The contiguity requirement is a real fix, not a restatement:
// VulkanComputeShader::createDescriptorSetLayouts resized its vector with a
// VK_NULL_HANDLE fill (VulkanComputeShader.cpp:363), so a gap in the set numbers
// produced a null layout handle that was then happily passed to
// vkCreatePipelineLayout. Here, an empty entry is a hard failure: the constructor
// logs and leaves valid() == false.
// -----------------------------------------------------------------------------
struct ComputePipelineDesc
{
	// FULL path to the .spv, extension included. Unlike
	// VulkanComputeShader::loadShaderFromFile (VulkanComputeShader.cpp:302),
	// ".spv" is NOT appended here. The caller appends it; §3.7.3 shows where.
	std::filesystem::path spirvPath;
	std::string           entryPoint = "main";

	// index == descriptor set number; contiguous from 0; no entry may be empty.
	std::vector<std::vector<DescriptorBindingDesc>> setLayouts;

	uint32_t              pushConstantSize = 0;   // 0 == no push constant range
	const char*           debugName        = nullptr;
};

// -----------------------------------------------------------------------------
// One VkPipeline + its VkPipelineLayout + the VulkanDescriptorSetLayouts it was
// built from.
//
// Move semantics: THE MOVE-ASSIGNMENT CONTRACT in VulkanTypes.h, with the same
// deviation VulkanDescriptorSetLayout carries and for the same reason. The
// displaced VkPipeline / VkPipelineLayout / VkShaderModule are destroyed INLINE,
// not through ctx.deferDestroy() -- the deletion queue's overloads take buffers
// and images, and adding pipeline overloads would imply that reassigning a
// pipeline mid-frame is supported. It is not. A pipeline is only ever displaced
// at shutdown, after vkDeviceWaitIdle, or by a shader hot-reload that must
// itself wait idle first. Reassigning one while a command buffer that bound it
// is pending is undefined behaviour that no assert here can catch, which is why
// the restriction is stated rather than defended.
//
// A further reason the same restriction is not optional: moving a pipeline moves
// its m_SetLayouts vector, and every VulkanDescriptorSetRing allocated from those
// layouts holds a raw pointer INTO that vector (see
// VulkanDescriptorSetRing::layout()). A move therefore dangles every ring
// derived from it. Rings and pipelines are created together in
// Renderer::GetOrLoadShader and destroyed together in Renderer::Shutdown(); keep
// it that way.
// -----------------------------------------------------------------------------
class VulkanComputePipeline
{
public:
	VulkanComputePipeline() = default;

	// DOES NOT THROW ON FAILURE: logs and leaves valid() == false. This is the
	// one class in the layer that reports failure rather than throwing through
	// X3_VK_CHECK, because a missing or malformed .spv is a content error the
	// editor must survive -- Renderer::GetOrLoadShader returns nullptr and the
	// frame draws nothing, exactly as IComputeShader::GetID() == 0 used to mean
	// (Renderer.cpp:32, :44).
	VulkanComputePipeline(VulkanContext& ctx, const ComputePipelineDesc& desc);
	~VulkanComputePipeline();

	VulkanComputePipeline(VulkanComputePipeline&&) noexcept;
	VulkanComputePipeline& operator=(VulkanComputePipeline&&) noexcept;
	VulkanComputePipeline(const VulkanComputePipeline&)            = delete;
	VulkanComputePipeline& operator=(const VulkanComputePipeline&) = delete;

	// Replaces `IComputeShader::GetID() != 0` as the validity test.
	bool     valid()    const { return m_Pipeline != VK_NULL_HANDLE; }
	uint32_t setCount() const { return uint32_t(m_SetLayouts.size()); }

	// The layout of descriptor set `set`. This is what VulkanDescriptorSetRing is
	// constructed from and what DescriptorWriter validates completeness against.
	// The reference is stable for the life of the pipeline and must not outlive
	// it (see the move note above).
	//
	// Bounds-checked in debug rather than left to vector::operator[]: `set` is a
	// literal at every call site in Renderer::Draw (0, 1, 2), and the failure
	// mode of getting it wrong -- a pipeline with two sets asked for setLayout(2)
	// -- is an out-of-bounds read that yields a plausible-looking layout object
	// rather than a crash.
	const VulkanDescriptorSetLayout& setLayout(uint32_t set) const
	{
		assert(set < m_SetLayouts.size() && "descriptor set index out of range for this pipeline");
		return m_SetLayouts[set];
	}

	// vkCmdBindPipeline + vkCmdBindDescriptorSets(firstSet = 0) + vkCmdDispatch,
	// all into frame.cmd().
	//
	// `sets` must be exactly setCount() entries, index == set number, each one
	// already written for THIS frame -- i.e. each came from
	// `ring.get(frame)` and its DescriptorWriter has been flushed. Asserted in
	// debug: sets.size() == setCount(), no entry VK_NULL_HANDLE.
	//
	// Arguments are group COUNTS, not local sizes. That is the second half of a
	// real bug: VulkanComputeShader::m_WorkGroupSizes (VulkanComputeShader.h:39)
	// was named for local sizes but held group counts (Renderer.cpp:372-376), so
	// the name said one thing and every call site meant the other. It is a
	// parameter now, and it is spelled `groups*`.
	//
	// INSERTS NO BARRIERS. The post-dispatch COMPUTE -> FRAGMENT barrier that
	// VulkanComputeShader::Dispatch used to emit unconditionally
	// (VulkanComputeShader.cpp:139-154) is deleted: it was recorded inside an
	// open render pass with no self-dependency, it hardcoded FRAGMENT_SHADER as
	// the consumer when the runtime's consumer is a TRANSFER blit, and it fired
	// whether or not anything read the result. Synchronisation is the caller's:
	// Renderer::Draw records image.barrier(...) / image.transition(...) around
	// this call with the stages it actually needs.
	//
	// Asserts that no rendering block is open on the context (see the note at the
	// top of this file -- VUID-vkCmdDispatch-renderpass). Nothing in the engine
	// opens one around a dispatch after the dynamic-rendering migration; the
	// assert is there so that a future raster pass cannot reintroduce it quietly.
	void dispatch(const FrameContext& frame,
	              std::span<const VkDescriptorSet> sets,
	              uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) const;

	// vkCmdPushConstants into frame.cmd(), VK_SHADER_STAGE_COMPUTE_BIT, offset 0.
	// Asserts size == the pushConstantSize this pipeline was built with, so a
	// struct that grows without the desc growing with it fails loudly instead of
	// writing a truncated range.
	void pushConstants(const FrameContext& frame, const void* data, uint32_t size) const;

	VkPipeline                   handle() const { return m_Pipeline; }
	VkPipelineLayout             layout() const { return m_PipelineLayout; }
	const std::filesystem::path& path()   const { return m_Path; }

private:
	VulkanContext*                         m_Ctx              = nullptr;
	std::filesystem::path                  m_Path;
	VkShaderModule                         m_Module           = VK_NULL_HANDLE;
	VkPipeline                             m_Pipeline         = VK_NULL_HANDLE;
	VkPipelineLayout                       m_PipelineLayout   = VK_NULL_HANDLE;
	std::vector<VulkanDescriptorSetLayout> m_SetLayouts;
	uint32_t                               m_PushConstantSize = 0;
};

}
