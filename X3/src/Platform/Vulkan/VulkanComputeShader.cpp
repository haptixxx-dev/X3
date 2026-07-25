#include "VulkanComputeShader.h"
#include "VulkanContext.h"
#include "Core/Log.h"
#include <fstream>
#include <vector>
#include <stdexcept>

namespace X3
{

VulkanComputeShader::VulkanComputeShader(const std::string& filepath, const glm::uvec3& workGroupSizes)
	: m_ShaderID(s_NextShaderID++), m_Filepath(filepath), m_WorkGroupSizes(workGroupSizes) {

	loadShaderFromFile(filepath);

	// Configure descriptor sets for compute shaders
	// This configuration matches the binding layout used in PathTracing/PBR/Phong shaders
	// Set 0: Images and samplers
	std::vector<DescriptorBinding> set0Bindings = {
		{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},           // rayTracingTexture
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT}   // skyboxTexture
	};
	addDescriptorSet(0, set0Bindings);

	// Set 1: Uniform buffers
	std::vector<DescriptorBinding> set1Bindings = {
		{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},          // CameraUBO
		{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}           // SettingsUBO
	};
	addDescriptorSet(1, set1Bindings);

	// Set 2: Storage buffers (SSBOs)
	std::vector<DescriptorBinding> set2Bindings = {
		{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},          // EntityLookupSSBO
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},          // TransformSSBO
		{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},          // MaterialSSBO
		{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},          // MeshBufferSSBO
		{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},          // NodeBufferSSBO
		{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},          // IndexBufferSSBO
		{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}           // LightBufferSSBO
	};
	addDescriptorSet(2, set2Bindings);

	createDescriptorSetLayouts();
	createPipeline();

	LOG_ENGINE_INFO("Created Vulkan Compute Shader: {} (ID: {}, workgroup size: {}x{}x{})",
		filepath, m_ShaderID, workGroupSizes.x, workGroupSizes.y, workGroupSizes.z);
}

VulkanComputeShader::~VulkanComputeShader() {
	auto context = VulkanContext::Get();
	if (!context) return;

	VkDevice device = context->getDevice();

	// Wait for device to be idle before cleanup
	vkDeviceWaitIdle(device);

	// Destroy pipeline
	if (m_Pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(device, m_Pipeline, nullptr);
		m_Pipeline = VK_NULL_HANDLE;
	}

	// Destroy pipeline layout
	if (m_PipelineLayout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
		m_PipelineLayout = VK_NULL_HANDLE;
	}

	// Destroy descriptor set layouts
	for (auto layout : m_DescriptorSetLayouts) {
		if (layout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(device, layout, nullptr);
		}
	}
	m_DescriptorSetLayouts.clear();

	// Destroy shader module
	if (m_ShaderModule != VK_NULL_HANDLE) {
		vkDestroyShaderModule(device, m_ShaderModule, nullptr);
		m_ShaderModule = VK_NULL_HANDLE;
	}

	LOG_ENGINE_INFO("Destroyed Vulkan Compute Shader: {} (ID: {})", m_Filepath, m_ShaderID);
}

void VulkanComputeShader::Bind() {
	// In Vulkan, shaders are bound through command buffers during dispatch
	// This is a placeholder for API compatibility with OpenGL
}

void VulkanComputeShader::Unbind() {
	// In Vulkan, unbinding is handled differently
	// This is a placeholder for API compatibility with OpenGL
}

void VulkanComputeShader::Dispatch() {
	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_ERROR("VulkanContext not available for compute dispatch");
		return;
	}

	// The first frame is begun lazily (VulkanContext::init deliberately does not
	// call beginFrame, and swapBuffers only begins the *next* frame). Until a
	// project is open the editor renders nothing, so ImGuiContext::EndFrame was
	// always the first recorder and its ensureFrameStarted() call covered this.
	// With a scene loaded the compute dispatch records first, and without this
	// call frame 1 is recorded into a command buffer that was never begun:
	// validation reports vkCmdBindPipeline/vkCmdBindDescriptorSets/vkCmdDispatch/
	// vkCmdPipelineBarrier "-commandBuffer-recording" and the NVIDIA driver then
	// aborts in the next vkBeginCommandBuffer ("free(): invalid pointer").
	// Same idiom as ImGuiContext::EndFrame; Phase 1 Part 2 deletes the whole
	// ensureFrameStarted/m_FirstFrame mechanism in favour of an explicit frame.
	context->ensureFrameStarted();
	VkCommandBuffer cmd = context->getCurrentCommandBuffer();
	if (cmd == VK_NULL_HANDLE) {
		LOG_ENGINE_ERROR("No active command buffer for compute dispatch");
		return;
	}

	// Allocate descriptor sets if not done yet
	if (!m_DescriptorSetsAllocated) {
		allocateDescriptorSets();
	}

	// Update descriptor sets with current bound resources
	updateDescriptorSets();

	// Bind the compute pipeline
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);

	// Bind descriptor sets
	if (!m_DescriptorSets.empty()) {
		vkCmdBindDescriptorSets(
			cmd,
			VK_PIPELINE_BIND_POINT_COMPUTE,
			m_PipelineLayout,
			0, // first set
			static_cast<uint32_t>(m_DescriptorSets.size()),
			m_DescriptorSets.data(),
			0, nullptr // no dynamic offsets
		);
	}

	// Dispatch the compute shader with the specified work group sizes
	vkCmdDispatch(cmd, m_WorkGroupSizes.x, m_WorkGroupSizes.y, m_WorkGroupSizes.z);

	// Memory barrier to ensure compute shader writes are visible before subsequent reads
	// This is critical for ImGui to correctly sample the rendered scene image
	VkMemoryBarrier memoryBarrier{};
	memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,   // src: compute shader finished writing
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,  // dst: fragment shader will read
		0,
		1, &memoryBarrier,
		0, nullptr,
		0, nullptr
	);
}

void VulkanComputeShader::allocateDescriptorSets() {
	auto context = VulkanContext::Get();
	if (!context) return;

	VkDevice device = context->getDevice();
	VkDescriptorPool pool = context->getDescriptorPool();

	// Resize descriptor sets vector to match layouts
	m_DescriptorSets.resize(m_DescriptorSetLayouts.size());

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = static_cast<uint32_t>(m_DescriptorSetLayouts.size());
	allocInfo.pSetLayouts = m_DescriptorSetLayouts.data();

	if (vkAllocateDescriptorSets(device, &allocInfo, m_DescriptorSets.data()) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to allocate descriptor sets for compute shader");
		throw std::runtime_error("Failed to allocate descriptor sets");
	}

	m_DescriptorSetsAllocated = true;
	LOG_ENGINE_INFO("Allocated {} descriptor sets for compute shader", m_DescriptorSets.size());
}

void VulkanComputeShader::updateDescriptorSets() {
	auto context = VulkanContext::Get();
	if (!context) return;

	VkDevice device = context->getDevice();

	// Clear previous writes and infos
	m_WriteDescriptorSets.clear();
	m_BufferInfos.clear();
	m_ImageInfos.clear();

	// Iterate through all sets and bindings
	for (const auto& [setNum, info] : m_DescriptorSetInfos) {
		if (setNum >= m_DescriptorSets.size()) continue;

		VkDescriptorSet descriptorSet = m_DescriptorSets[setNum];

		for (const auto& binding : info.bindings) {
			VkWriteDescriptorSet write{};
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = descriptorSet;
			write.dstBinding = binding.binding;
			write.dstArrayElement = 0;
			write.descriptorType = binding.type;
			write.descriptorCount = 1; // Assuming count is 1 for now

			bool bindingFound = false;

			// Handle different descriptor types
			switch (binding.type) {
				case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
					const auto& boundImages = context->getBoundStorageImages();
					if (boundImages.find(binding.binding) != boundImages.end()) {
						VkDescriptorImageInfo imageInfo{};
						imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
						imageInfo.imageView = boundImages.at(binding.binding).imageView;
						imageInfo.sampler = VK_NULL_HANDLE;

						m_ImageInfos[setNum][binding.binding] = imageInfo;
						write.pImageInfo = &m_ImageInfos[setNum][binding.binding];
						bindingFound = true;
					}
					break;
				}
				case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
					const auto& boundImages = context->getBoundSampledImages();
					if (boundImages.find(binding.binding) != boundImages.end()) {
						VkDescriptorImageInfo imageInfo{};
						imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
						imageInfo.imageView = boundImages.at(binding.binding).imageView;
						imageInfo.sampler = boundImages.at(binding.binding).sampler;

						m_ImageInfos[setNum][binding.binding] = imageInfo;
						write.pImageInfo = &m_ImageInfos[setNum][binding.binding];
						bindingFound = true;
					}
					break;
				}
				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
					const auto& boundBuffers = context->getBoundUniformBuffers();
					if (boundBuffers.find(binding.binding) != boundBuffers.end()) {
						VkDescriptorBufferInfo bufferInfo{};
						bufferInfo.buffer = boundBuffers.at(binding.binding).buffer;
						bufferInfo.offset = 0;
						bufferInfo.range = boundBuffers.at(binding.binding).size;

						m_BufferInfos[setNum][binding.binding] = bufferInfo;
						write.pBufferInfo = &m_BufferInfos[setNum][binding.binding];
						bindingFound = true;
					}
					break;
				}
				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
					const auto& boundBuffers = context->getBoundStorageBuffers();
					if (boundBuffers.find(binding.binding) != boundBuffers.end()) {
						VkDescriptorBufferInfo bufferInfo{};
						bufferInfo.buffer = boundBuffers.at(binding.binding).buffer;
						bufferInfo.offset = 0;
						bufferInfo.range = boundBuffers.at(binding.binding).size;

						m_BufferInfos[setNum][binding.binding] = bufferInfo;
						write.pBufferInfo = &m_BufferInfos[setNum][binding.binding];
						bindingFound = true;
					}
					break;
				}
				default:
					LOG_ENGINE_WARN("Unsupported descriptor type in compute shader: {}", static_cast<int>(binding.type));
					break;
			}

			if (bindingFound) {
				m_WriteDescriptorSets.push_back(write);
			} else {
				// LOG_ENGINE_WARN("No resource bound for set {} binding {}", setNum, binding.binding);
			}
		}
	}

	if (!m_WriteDescriptorSets.empty()) {
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(m_WriteDescriptorSets.size()), m_WriteDescriptorSets.data(), 0, nullptr);
	}
}

void VulkanComputeShader::addDescriptorSet(uint32_t set, const std::vector<DescriptorBinding>& bindings) {
	DescriptorSetInfo info;
	info.set = set;
	info.bindings = bindings;
	m_DescriptorSetInfos[set] = info;
}

void VulkanComputeShader::setPushConstantRange(uint32_t offset, uint32_t size, VkShaderStageFlags stageFlags) {
	m_PushConstantRange.offset = offset;
	m_PushConstantRange.size = size;
	m_PushConstantRange.stageFlags = stageFlags;
	m_HasPushConstants = true;
}

void VulkanComputeShader::loadShaderFromFile(const std::string& filepath) {
	// Load SPIR-V shader code from .spv file
	std::string spvPath = filepath + ".spv";

	std::ifstream file(spvPath, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		LOG_ENGINE_ERROR("Failed to open SPIR-V shader file: {}", spvPath);
		throw std::runtime_error("Failed to open SPIR-V shader file: " + spvPath);
	}

	size_t fileSize = static_cast<size_t>(file.tellg());
	std::vector<char> buffer(fileSize);

	file.seekg(0);
	file.read(buffer.data(), fileSize);
	file.close();

	LOG_ENGINE_INFO("Loaded SPIR-V shader: {} ({} bytes)", spvPath, fileSize);

	createShaderModule(buffer);
}

void VulkanComputeShader::createShaderModule(const std::vector<char>& code) {
	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_ERROR("VulkanContext not available");
		throw std::runtime_error("VulkanContext not available");
	}

	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size();
	createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

	if (vkCreateShaderModule(context->getDevice(), &createInfo, nullptr, &m_ShaderModule) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create shader module");
		throw std::runtime_error("Failed to create shader module");
	}

	LOG_ENGINE_INFO("Created shader module successfully");
}

void VulkanComputeShader::createDescriptorSetLayouts() {
	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_ERROR("VulkanContext not available");
		throw std::runtime_error("VulkanContext not available");
	}

	VkDevice device = context->getDevice();

	// If no descriptor sets were added, we still need at least an empty array
	if (m_DescriptorSetInfos.empty()) {
		LOG_ENGINE_INFO("No descriptor sets configured for shader");
		return;
	}

	// Find the maximum set number to determine how many layouts we need
	uint32_t maxSet = 0;
	for (const auto& [setNum, info] : m_DescriptorSetInfos) {
		maxSet = std::max(maxSet, setNum);
	}

	m_DescriptorSetLayouts.resize(maxSet + 1, VK_NULL_HANDLE);

	// Create descriptor set layouts
	for (const auto& [setNum, info] : m_DescriptorSetInfos) {
		std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
		layoutBindings.reserve(info.bindings.size());

		for (const auto& binding : info.bindings) {
			VkDescriptorSetLayoutBinding layoutBinding{};
			layoutBinding.binding = binding.binding;
			layoutBinding.descriptorType = binding.type;
			layoutBinding.descriptorCount = binding.count;
			layoutBinding.stageFlags = binding.stageFlags;
			layoutBinding.pImmutableSamplers = nullptr;

			layoutBindings.push_back(layoutBinding);
		}

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
		layoutInfo.pBindings = layoutBindings.data();

		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayouts[setNum]) != VK_SUCCESS) {
			LOG_ENGINE_ERROR("Failed to create descriptor set layout for set {}", setNum);
			throw std::runtime_error("Failed to create descriptor set layout");
		}

		LOG_ENGINE_INFO("Created descriptor set layout for set {} with {} bindings", setNum, layoutBindings.size());
	}
}

void VulkanComputeShader::createPipeline() {
	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_ERROR("VulkanContext not available");
		throw std::runtime_error("VulkanContext not available");
	}

	VkDevice device = context->getDevice();

	// Create pipeline layout
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(m_DescriptorSetLayouts.size());
	pipelineLayoutInfo.pSetLayouts = m_DescriptorSetLayouts.empty() ? nullptr : m_DescriptorSetLayouts.data();

	if (m_HasPushConstants) {
		pipelineLayoutInfo.pushConstantRangeCount = 1;
		pipelineLayoutInfo.pPushConstantRanges = &m_PushConstantRange;
		LOG_ENGINE_INFO("Pipeline configured with push constants (offset: {}, size: {})",
			m_PushConstantRange.offset, m_PushConstantRange.size);
	} else {
		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = nullptr;
	}

	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create pipeline layout");
		throw std::runtime_error("Failed to create pipeline layout");
	}

	LOG_ENGINE_INFO("Created pipeline layout with {} descriptor sets", m_DescriptorSetLayouts.size());

	// Create compute pipeline
	VkPipelineShaderStageCreateInfo shaderStageInfo{};
	shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	shaderStageInfo.module = m_ShaderModule;
	shaderStageInfo.pName = "main"; // Entry point function name

	VkComputePipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = shaderStageInfo;
	pipelineInfo.layout = m_PipelineLayout;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex = -1;

	if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create compute pipeline");
		throw std::runtime_error("Failed to create compute pipeline");
	}

	LOG_ENGINE_INFO("Created compute pipeline successfully");
}

}
