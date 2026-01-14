#pragma once

#include "Renderer/IComputeShader.h"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace X3
{

struct DescriptorBinding {
	uint32_t binding;
	VkDescriptorType type;
	uint32_t count;
	VkShaderStageFlags stageFlags;
};

struct DescriptorSetInfo {
	uint32_t set;
	std::vector<DescriptorBinding> bindings;
};

class VulkanComputeShader : public IComputeShader {
public:
	VulkanComputeShader(const std::string& filepath, const glm::uvec3& workGroupSizes = glm::uvec3(1, 1, 1));
	~VulkanComputeShader();

	void Bind() override;
	void Unbind() override;
	void Dispatch() override;

	// Getters
	uint32_t GetID() override { return m_ShaderID; }
	glm::uvec3 getWorkGroupSizes() override { return m_WorkGroupSizes; }
	std::string getFilePath() override { return m_Filepath; }

	// Setters
	void setWorkGroupSizes(const glm::uvec3 workGroupSizes) override { m_WorkGroupSizes = workGroupSizes; }

	// Vulkan-specific getters
	VkPipeline getPipeline() const { return m_Pipeline; }
	VkPipelineLayout getPipelineLayout() const { return m_PipelineLayout; }
	const std::vector<VkDescriptorSetLayout>& getDescriptorSetLayouts() const { return m_DescriptorSetLayouts; }

	// Descriptor set management
	void addDescriptorSet(uint32_t set, const std::vector<DescriptorBinding>& bindings);
	void setPushConstantRange(uint32_t offset, uint32_t size, VkShaderStageFlags stageFlags);

private:
	void loadShaderFromFile(const std::string& filepath);
	void createShaderModule(const std::vector<char>& code);
	void createDescriptorSetLayouts();
	void createPipeline();

private:
	static inline uint32_t s_NextShaderID = 1;

	uint32_t m_ShaderID;
	std::string m_Filepath;
	glm::uvec3 m_WorkGroupSizes;

	VkShaderModule m_ShaderModule = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

	std::vector<VkDescriptorSetLayout> m_DescriptorSetLayouts;
	std::unordered_map<uint32_t, DescriptorSetInfo> m_DescriptorSetInfos;

	VkPushConstantRange m_PushConstantRange{};
	bool m_HasPushConstants = false;

	// Descriptor set management for binding resources
	std::vector<VkDescriptorSet> m_DescriptorSets;
	bool m_DescriptorSetsAllocated = false;
	
	// Keep these alive for vkUpdateDescriptorSets
	// We need a list of infos for each set, and for each binding in that set
	// Map: Set Index -> Binding Index -> Buffer/Image Info
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, VkDescriptorBufferInfo>> m_BufferInfos;
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, VkDescriptorImageInfo>> m_ImageInfos;
	std::vector<VkWriteDescriptorSet> m_WriteDescriptorSets;

	void allocateDescriptorSets();
	void updateDescriptorSets();
};

}
