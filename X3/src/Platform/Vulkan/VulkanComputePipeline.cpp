#include "Platform/Vulkan/VulkanComputePipeline.h"

#include "Platform/Vulkan/VulkanContext.h"
#include "Core/Log.h"

#include <fstream>

namespace X3
{

namespace
{
	// Reads the whole file as SPIR-V words. Returns an empty vector on any
	// failure, including a size that is not a multiple of 4 -- vkCreateShaderModule
	// requires codeSize to be a multiple of 4 and would otherwise be handed a
	// truncated module.
	std::vector<uint32_t> readSpirv(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open()) {
			LOG_ENGINE_ERROR("Compute pipeline: cannot open SPIR-V '{}'", path.string());
			return {};
		}

		const std::streamsize bytes = file.tellg();
		if (bytes <= 0 || (bytes % 4) != 0) {
			LOG_ENGINE_ERROR("Compute pipeline: '{}' is {} bytes, not a positive multiple of 4",
			                 path.string(), static_cast<long long>(bytes));
			return {};
		}

		std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4);
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(words.data()), bytes)) {
			LOG_ENGINE_ERROR("Compute pipeline: short read on '{}'", path.string());
			return {};
		}
		return words;
	}
}

// -----------------------------------------------------------------------------
// Construction. DOES NOT THROW: every failure logs and returns with m_Pipeline
// still VK_NULL_HANDLE, so valid() is false and Renderer::GetOrLoadShader hands
// back nullptr. A missing or malformed .spv is a content error the editor has to
// survive, which is why this is the one class in the layer that does not go
// through X3_VK_CHECK.
// -----------------------------------------------------------------------------
VulkanComputePipeline::VulkanComputePipeline(VulkanContext& ctx, const ComputePipelineDesc& desc)
	: m_Ctx(&ctx)
	, m_Path(desc.spirvPath)
	, m_PushConstantSize(desc.pushConstantSize)
{
	// Contiguity, checked rather than assumed. VulkanComputeShader used to resize
	// its layout vector with a VK_NULL_HANDLE fill, so a gap in the set numbers
	// produced a null layout handle that vkCreatePipelineLayout accepted as input
	// and rejected obscurely. An empty entry is a hard failure here.
	if (desc.setLayouts.empty()) {
		LOG_ENGINE_ERROR("Compute pipeline '{}': no descriptor set layouts declared", m_Path.string());
		return;
	}
	for (size_t set = 0; set < desc.setLayouts.size(); ++set) {
		if (desc.setLayouts[set].empty()) {
			LOG_ENGINE_ERROR("Compute pipeline '{}': descriptor set {} is empty -- set indices "
			                 "must be contiguous from 0 with no gaps", m_Path.string(), set);
			return;
		}
	}

	const std::vector<uint32_t> code = readSpirv(desc.spirvPath);
	if (code.empty())
		return;   // readSpirv already logged

	VkShaderModuleCreateInfo mi{};
	mi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	mi.codeSize = code.size() * sizeof(uint32_t);
	mi.pCode    = code.data();
	if (vkCreateShaderModule(ctx.getDevice(), &mi, nullptr, &m_Module) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Compute pipeline '{}': vkCreateShaderModule failed", m_Path.string());
		m_Module = VK_NULL_HANDLE;
		return;
	}

	// The layouts live HERE, by value, for the life of the pipeline. Every
	// VulkanDescriptorSetRing allocated from one keeps a pointer into this vector
	// -- which is why moving a pipeline is a shutdown-only operation.
	m_SetLayouts.reserve(desc.setLayouts.size());
	std::vector<VkDescriptorSetLayout> rawLayouts;
	rawLayouts.reserve(desc.setLayouts.size());
	for (const std::vector<DescriptorBindingDesc>& bindings : desc.setLayouts) {
		m_SetLayouts.emplace_back(ctx, std::span<const DescriptorBindingDesc>(bindings));
		rawLayouts.push_back(m_SetLayouts.back().handle());
	}

	VkPushConstantRange pcRange{};
	pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcRange.offset     = 0;
	pcRange.size       = desc.pushConstantSize;

	VkPipelineLayoutCreateInfo pli{};
	pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pli.setLayoutCount         = static_cast<uint32_t>(rawLayouts.size());
	pli.pSetLayouts            = rawLayouts.data();
	pli.pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u;
	pli.pPushConstantRanges    = desc.pushConstantSize > 0 ? &pcRange : nullptr;

	if (vkCreatePipelineLayout(ctx.getDevice(), &pli, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Compute pipeline '{}': vkCreatePipelineLayout failed", m_Path.string());
		m_PipelineLayout = VK_NULL_HANDLE;
		vkDestroyShaderModule(ctx.getDevice(), m_Module, nullptr);
		m_Module = VK_NULL_HANDLE;
		m_SetLayouts.clear();
		return;
	}

	VkPipelineShaderStageCreateInfo stage{};
	stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = m_Module;
	stage.pName  = desc.entryPoint.c_str();

	VkComputePipelineCreateInfo ci{};
	ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	ci.stage  = stage;
	ci.layout = m_PipelineLayout;

	if (vkCreateComputePipelines(ctx.getDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &m_Pipeline) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Compute pipeline '{}': vkCreateComputePipelines failed", m_Path.string());
		m_Pipeline = VK_NULL_HANDLE;
		vkDestroyPipelineLayout(ctx.getDevice(), m_PipelineLayout, nullptr);
		m_PipelineLayout = VK_NULL_HANDLE;
		vkDestroyShaderModule(ctx.getDevice(), m_Module, nullptr);
		m_Module = VK_NULL_HANDLE;
		m_SetLayouts.clear();
		return;
	}

	LOG_ENGINE_INFO("Compute pipeline '{}' created ({} descriptor sets)",
	                desc.debugName ? desc.debugName : m_Path.string(), m_SetLayouts.size());
}

VulkanComputePipeline::~VulkanComputePipeline()
{
	// INLINE, not deferred -- see the ownership note in the header. A pipeline is
	// only ever destroyed in Renderer::Shutdown(), after vkDeviceWaitIdle. The
	// deletion queue takes buffers and images on purpose; adding pipeline
	// overloads would imply mid-frame reassignment is supported, and it is not.
	if (!m_Ctx)
		return;

	VkDevice device = m_Ctx->getDevice();
	if (m_Pipeline != VK_NULL_HANDLE)       vkDestroyPipeline(device, m_Pipeline, nullptr);
	if (m_PipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
	if (m_Module != VK_NULL_HANDLE)         vkDestroyShaderModule(device, m_Module, nullptr);
	// m_SetLayouts destroy themselves, each with vkDestroyDescriptorSetLayout.
}

VulkanComputePipeline::VulkanComputePipeline(VulkanComputePipeline&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Path(std::move(other.m_Path))
	, m_Module(other.m_Module)
	, m_Pipeline(other.m_Pipeline)
	, m_PipelineLayout(other.m_PipelineLayout)
	, m_SetLayouts(std::move(other.m_SetLayouts))
	, m_PushConstantSize(other.m_PushConstantSize)
{
	other.m_Ctx              = nullptr;
	other.m_Module           = VK_NULL_HANDLE;
	other.m_Pipeline         = VK_NULL_HANDLE;
	other.m_PipelineLayout   = VK_NULL_HANDLE;
	other.m_PushConstantSize = 0;
	other.m_SetLayouts.clear();
	other.m_Path.clear();
}

VulkanComputePipeline& VulkanComputePipeline::operator=(VulkanComputePipeline&& other) noexcept
{
	if (this == &other)
		return *this;

	assert((m_Ctx == nullptr || other.m_Ctx == nullptr || m_Ctx == other.m_Ctx) &&
	       "cross-context move of a compute pipeline");

	// SHUTDOWN-ONLY, and destroyed inline for the reason in the destructor. Moving
	// this object also moves m_SetLayouts, which dangles every
	// VulkanDescriptorSetRing allocated from those layouts. No assert can catch
	// that; the restriction is stated and kept by construction (rings and
	// pipelines are created together in GetOrLoadShader and die together in
	// Shutdown()).
	if (m_Ctx) {
		VkDevice device = m_Ctx->getDevice();
		if (m_Pipeline != VK_NULL_HANDLE)       vkDestroyPipeline(device, m_Pipeline, nullptr);
		if (m_PipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
		if (m_Module != VK_NULL_HANDLE)         vkDestroyShaderModule(device, m_Module, nullptr);
	}
	m_SetLayouts.clear();

	m_Ctx              = other.m_Ctx;
	m_Path             = std::move(other.m_Path);
	m_Module           = other.m_Module;
	m_Pipeline         = other.m_Pipeline;
	m_PipelineLayout   = other.m_PipelineLayout;
	m_SetLayouts       = std::move(other.m_SetLayouts);
	m_PushConstantSize = other.m_PushConstantSize;

	other.m_Ctx              = nullptr;
	other.m_Module           = VK_NULL_HANDLE;
	other.m_Pipeline         = VK_NULL_HANDLE;
	other.m_PipelineLayout   = VK_NULL_HANDLE;
	other.m_PushConstantSize = 0;
	other.m_SetLayouts.clear();
	other.m_Path.clear();
	return *this;
}

void VulkanComputePipeline::dispatch(const FrameContext& frame,
                                     std::span<const VkDescriptorSet> sets,
                                     uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) const
{
	assert(valid() && "dispatch() on an invalid pipeline");
	assert(sets.size() == setCount() &&
	       "dispatch() needs exactly one descriptor set per set index declared by the pipeline");
	assert(groupsX > 0 && groupsY > 0 && groupsZ > 0 && "dispatch() with a zero group count");

	// THE STANDING GUARD against VUID-vkCmdDispatch-renderpass. Nothing opens a
	// rendering block around a dispatch today -- ImGui's block is the last thing
	// in the editor frame and the runtime opens none -- and this assert is what
	// stops a future raster pass reintroducing it quietly.
	assert(!frame.context().renderingBlockOpen() &&
	       "vkCmdDispatch inside a rendering block is invalid");

#ifndef NDEBUG
	for (VkDescriptorSet s : sets)
		assert(s != VK_NULL_HANDLE && "dispatch() with a null descriptor set");
#endif

	VkCommandBuffer cmd = frame.cmd();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout,
	                        0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

	// GROUP COUNTS, not local sizes. The old m_WorkGroupSizes was named for local
	// sizes and held group counts; it is a parameter now and spelled `groups*`.
	vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);

	// INSERTS NO BARRIERS, deliberately. Synchronisation is the caller's:
	// Renderer::Draw records image.barrier()/image.transition() around this with
	// the stages it actually needs. The unconditional COMPUTE -> FRAGMENT barrier
	// the old shader class emitted was wrong for the runtime (whose consumer is a
	// TRANSFER blit) and fired whether or not anything read the result.
}

void VulkanComputePipeline::pushConstants(const FrameContext& frame, const void* data, uint32_t size) const
{
	assert(valid() && "pushConstants() on an invalid pipeline");
	assert(size == m_PushConstantSize &&
	       "push constant size differs from the size this pipeline was built with -- "
	       "a struct grew without ComputePipelineDesc::pushConstantSize growing with it");
	assert(data != nullptr && "pushConstants() with null data");

	vkCmdPushConstants(frame.cmd(), m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
}

}
