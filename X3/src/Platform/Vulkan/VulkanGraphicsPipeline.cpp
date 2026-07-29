#include "Platform/Vulkan/VulkanGraphicsPipeline.h"

#include "Platform/Vulkan/VulkanContext.h"
#include "Core/Log.h"

#include <fstream>

namespace X3
{

namespace
{
	// Same reader as VulkanComputePipeline's, including the multiple-of-4 check
	// that vkCreateShaderModule requires and would otherwise fail obscurely on.
	std::vector<uint32_t> readSpirv(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open()) {
			LOG_ENGINE_ERROR("Graphics pipeline: cannot open SPIR-V '{}'", path.string());
			return {};
		}

		const std::streamsize bytes = file.tellg();
		if (bytes <= 0 || (bytes % 4) != 0) {
			LOG_ENGINE_ERROR("Graphics pipeline: '{}' is {} bytes, not a positive multiple of 4",
			                 path.string(), static_cast<long long>(bytes));
			return {};
		}

		std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4);
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(words.data()), bytes)) {
			LOG_ENGINE_ERROR("Graphics pipeline: short read on '{}'", path.string());
			return {};
		}
		return words;
	}

	VkShaderModule makeModule(VkDevice device, const std::filesystem::path& path)
	{
		const std::vector<uint32_t> code = readSpirv(path);
		if (code.empty()) return VK_NULL_HANDLE;

		VkShaderModuleCreateInfo mi{};
		mi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		mi.codeSize = code.size() * sizeof(uint32_t);
		mi.pCode    = code.data();

		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &mi, nullptr, &module) != VK_SUCCESS) {
			LOG_ENGINE_ERROR("Graphics pipeline: vkCreateShaderModule failed for '{}'", path.string());
			return VK_NULL_HANDLE;
		}
		return module;
	}
}

VulkanGraphicsPipeline::VulkanGraphicsPipeline(VulkanContext& ctx, const GraphicsPipelineDesc& desc)
	: m_Ctx(&ctx)
	, m_PushConstantSize(desc.pushConstantSize)
	, m_DebugName(desc.debugName)
{
	const char* name = desc.debugName ? desc.debugName : "<unnamed>";

	if (desc.setLayouts.empty()) {
		LOG_ENGINE_ERROR("Graphics pipeline '{}': no descriptor set layouts declared", name);
		return;
	}
	for (size_t set = 0; set < desc.setLayouts.size(); ++set) {
		if (desc.setLayouts[set].empty()) {
			LOG_ENGINE_ERROR("Graphics pipeline '{}': descriptor set {} is empty -- set indices "
			                 "must be contiguous from 0 with no gaps", name, set);
			return;
		}
	}

	VkDevice device = ctx.getDevice();

	VkShaderModule vertModule = makeModule(device, desc.vertexSpirv);
	if (vertModule == VK_NULL_HANDLE) return;

	// A FRAGMENT STAGE IS OPTIONAL. The depth prepass has none: declaring one
	// that writes nothing is slower than declaring none, because a fragment
	// shader with side effects defeats the early-Z rejection the prepass exists
	// to enable.
	VkShaderModule fragModule = VK_NULL_HANDLE;
	if (!desc.fragmentSpirv.empty()) {
		fragModule = makeModule(device, desc.fragmentSpirv);
		if (fragModule == VK_NULL_HANDLE) {
			vkDestroyShaderModule(device, vertModule, nullptr);
			return;
		}
	}

	auto cleanupModules = [&]() {
		vkDestroyShaderModule(device, vertModule, nullptr);
		if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragModule, nullptr);
	};

	m_SetLayouts.reserve(desc.setLayouts.size());
	std::vector<VkDescriptorSetLayout> rawLayouts;
	rawLayouts.reserve(desc.setLayouts.size());
	for (const std::vector<DescriptorBindingDesc>& bindings : desc.setLayouts) {
		m_SetLayouts.emplace_back(ctx, std::span<const DescriptorBindingDesc>(bindings));
		rawLayouts.push_back(m_SetLayouts.back().handle());
	}

	VkPushConstantRange pcRange{};
	// BOTH STAGES. The per-draw entity index is needed by the vertex shader to
	// find its transform and by the fragment shader to find its material, and a
	// range visible to only one of them is a validation error at the other's
	// first read.
	pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pcRange.offset     = 0;
	pcRange.size       = desc.pushConstantSize;

	VkPipelineLayoutCreateInfo pli{};
	pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pli.setLayoutCount         = static_cast<uint32_t>(rawLayouts.size());
	pli.pSetLayouts            = rawLayouts.data();
	pli.pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u;
	pli.pPushConstantRanges    = desc.pushConstantSize > 0 ? &pcRange : nullptr;

	if (vkCreatePipelineLayout(device, &pli, nullptr, &m_Layout) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Graphics pipeline '{}': vkCreatePipelineLayout failed", name);
		m_Layout = VK_NULL_HANDLE;
		m_SetLayouts.clear();
		cleanupModules();
		return;
	}

	std::vector<VkPipelineShaderStageCreateInfo> stages;
	{
		VkPipelineShaderStageCreateInfo s{};
		s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		s.stage  = VK_SHADER_STAGE_VERTEX_BIT;
		s.module = vertModule;
		s.pName  = desc.vertexEntry.c_str();
		stages.push_back(s);
	}
	if (fragModule != VK_NULL_HANDLE) {
		VkPipelineShaderStageCreateInfo s{};
		s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		s.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		s.module = fragModule;
		s.pName  = desc.fragmentEntry.c_str();
		stages.push_back(s);
	}

	// EMPTY VERTEX INPUT STATE. Vertices are pulled from the Gpu::Vertex
	// StructuredBuffer with SV_VertexID -- see the header for why. This is not an
	// omission; a pipeline with no attributes and no bindings is exactly what
	// vertex pulling requires.
	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	// Viewport and scissor are DYNAMIC, so a resolution change does not rebuild
	// every pipeline. The editor's viewport resizes continuously while a panel
	// edge is dragged, and a rebuild per frame during a drag is a visible stall.
	VkPipelineViewportStateCreateInfo viewport{};
	viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic{};
	dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates    = dynamicStates;

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode    = desc.cullMode;
	raster.frontFace   = desc.frontFace;
	raster.lineWidth   = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable  = desc.depthTest  ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp   = desc.depthCompare;

	VkPipelineColorBlendAttachmentState blendAttachment{};
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
	                               | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blendAttachment.blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
	// Standard src-alpha over, which is what the transparent pass wants and what
	// nothing else enables.
	blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
	blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;

	std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
		desc.colorFormats.size(), blendAttachment);

	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
	colorBlend.pAttachments    = blendAttachments.empty() ? nullptr : blendAttachments.data();

	// DYNAMIC RENDERING: formats are declared here instead of by a VkRenderPass,
	// and the attachments themselves are named at draw time. There is no
	// VkRenderPass and no VkFramebuffer anywhere in this engine.
	VkPipelineRenderingCreateInfo rendering{};
	rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering.colorAttachmentCount    = static_cast<uint32_t>(desc.colorFormats.size());
	rendering.pColorAttachmentFormats = desc.colorFormats.empty() ? nullptr : desc.colorFormats.data();
	rendering.depthAttachmentFormat   = desc.depthFormat;

	VkGraphicsPipelineCreateInfo ci{};
	ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	ci.pNext               = &rendering;
	ci.stageCount          = static_cast<uint32_t>(stages.size());
	ci.pStages             = stages.data();
	ci.pVertexInputState   = &vertexInput;
	ci.pInputAssemblyState = &inputAssembly;
	ci.pViewportState      = &viewport;
	ci.pRasterizationState = &raster;
	ci.pMultisampleState   = &multisample;
	ci.pDepthStencilState  = &depthStencil;
	ci.pColorBlendState    = &colorBlend;
	ci.pDynamicState       = &dynamic;
	ci.layout              = m_Layout;

	const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_Pipeline);
	cleanupModules();   // modules are consumed by pipeline creation

	if (result != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Graphics pipeline '{}': vkCreateGraphicsPipelines failed ({})",
		                 name, static_cast<int>(result));
		m_Pipeline = VK_NULL_HANDLE;
		vkDestroyPipelineLayout(device, m_Layout, nullptr);
		m_Layout = VK_NULL_HANDLE;
		m_SetLayouts.clear();
		return;
	}

	LOG_ENGINE_INFO("Graphics pipeline '{}' created ({} descriptor sets, {} colour attachments, "
	                "depth {})",
	                name, m_SetLayouts.size(), desc.colorFormats.size(),
	                desc.depthFormat != VK_FORMAT_UNDEFINED ? "yes" : "no");
}

void VulkanGraphicsPipeline::destroy()
{
	// INLINE, not deferred -- identical contract to VulkanComputePipeline. A
	// pipeline is only displaced at shutdown, after vkDeviceWaitIdle.
	if (!m_Ctx) return;
	VkDevice device = m_Ctx->getDevice();
	if (m_Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, m_Pipeline, nullptr);
	if (m_Layout   != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_Layout, nullptr);
	m_Pipeline = VK_NULL_HANDLE;
	m_Layout   = VK_NULL_HANDLE;
	m_SetLayouts.clear();
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline()
{
	destroy();
}

VulkanGraphicsPipeline::VulkanGraphicsPipeline(VulkanGraphicsPipeline&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Pipeline(other.m_Pipeline)
	, m_Layout(other.m_Layout)
	, m_PushConstantSize(other.m_PushConstantSize)
	, m_SetLayouts(std::move(other.m_SetLayouts))
	, m_DebugName(other.m_DebugName)
{
	other.m_Ctx              = nullptr;
	other.m_Pipeline         = VK_NULL_HANDLE;
	other.m_Layout           = VK_NULL_HANDLE;
	other.m_PushConstantSize = 0;
	other.m_SetLayouts.clear();
}

VulkanGraphicsPipeline& VulkanGraphicsPipeline::operator=(VulkanGraphicsPipeline&& other) noexcept
{
	if (this == &other) return *this;
	destroy();

	m_Ctx              = other.m_Ctx;
	m_Pipeline         = other.m_Pipeline;
	m_Layout           = other.m_Layout;
	m_PushConstantSize = other.m_PushConstantSize;
	m_SetLayouts       = std::move(other.m_SetLayouts);
	m_DebugName        = other.m_DebugName;

	other.m_Ctx              = nullptr;
	other.m_Pipeline         = VK_NULL_HANDLE;
	other.m_Layout           = VK_NULL_HANDLE;
	other.m_PushConstantSize = 0;
	other.m_SetLayouts.clear();
	return *this;
}

void VulkanGraphicsPipeline::bind(const FrameContext& frame,
                                  std::span<const VkDescriptorSet> sets,
                                  VkExtent2D extent) const
{
	assert(valid() && "bind() on an invalid graphics pipeline");
	assert(sets.size() == m_SetLayouts.size() &&
	       "descriptor set count does not match the pipeline's layout count");
	for (VkDescriptorSet set : sets)
		assert(set != VK_NULL_HANDLE && "null descriptor set handed to bind()");

	VkCommandBuffer cmd = frame.cmd();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Layout, 0,
	                        static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

	// Y IS NOT FLIPPED HERE. The projection matrix already negates [1][1] (see
	// Renderer::SetupGPUResources), which is one of the two usual places to do
	// it. Doing it in both cancels out, and the symptom is a vertically mirrored
	// image that looks like a readback bug rather than a projection one.
	VkViewport vp{};
	vp.x        = 0.0f;
	vp.y        = 0.0f;
	vp.width    = float(extent.width);
	vp.height   = float(extent.height);
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &vp);

	VkRect2D scissor{};
	scissor.offset = { 0, 0 };
	scissor.extent = extent;
	vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void VulkanGraphicsPipeline::pushConstants(const FrameContext& frame,
                                           const void* data, uint32_t size) const
{
	assert(valid() && "pushConstants() on an invalid graphics pipeline");
	assert(size <= m_PushConstantSize &&
	       "push constant write larger than the range the pipeline declared");
	vkCmdPushConstants(frame.cmd(), m_Layout,
	                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
	                   0, size, data);
}

}
