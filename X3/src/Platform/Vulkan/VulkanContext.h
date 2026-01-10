#pragma once

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <vector>
#include "Renderer/IRenderingContext.h"

namespace X3
{

class VulkanContext : public IRenderingContext {
public:
	VulkanContext(GLFWwindow* window);
	~VulkanContext();

	void init() override;
	void swapBuffers() override;

	static void setWindowHints();

	// Vulkan-specific getters
	VkInstance getInstance() const { return m_Instance; }
	VkPhysicalDevice getPhysicalDevice() const { return m_PhysicalDevice; }
	VkDevice getDevice() const { return m_Device; }
	VkQueue getGraphicsQueue() const { return m_GraphicsQueue; }
	VkSurfaceKHR getSurface() const { return m_Surface; }
	VkCommandPool getCommandPool() const { return m_CommandPool; }
	VkRenderPass getRenderPass() const { return m_RenderPass; }
	VmaAllocator getAllocator() const { return m_Allocator; }
	VkDescriptorPool getDescriptorPool() const { return m_DescriptorPool; }
	uint32_t getGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }

	// Singleton access to the current Vulkan context
	static VulkanContext* Get() { return s_Instance; }

	// Frame management
	uint32_t getCurrentFrame() const { return m_CurrentFrame; }
	VkCommandBuffer getCurrentCommandBuffer() const { return m_CommandBuffers[m_CurrentFrame]; }

	// Render loop functions
	bool beginFrame();
	void endFrame();

	// Swapchain management
	void recreateSwapchain();

private:
	void createInstance();
	void pickPhysicalDevice();
	void createLogicalDevice();
	void createSurface();
	void createSwapchain();
	void createAllocator();
	void createRenderPass();
	void createFramebuffers();
	void createCommandPool();
	void createCommandBuffers();
	void createSyncObjects();
	void createDescriptorPool();
	void cleanup();
	void cleanupSwapchain();

private:
	GLFWwindow* m_NativeWindow = nullptr;

	VkInstance m_Instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
	VkDevice m_Device = VK_NULL_HANDLE;
	VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
	VkQueue m_PresentQueue = VK_NULL_HANDLE;
	VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
	VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;

	// VMA allocator
	VmaAllocator m_Allocator = VK_NULL_HANDLE;

	// Swapchain resources
	std::vector<VkImage> m_SwapchainImages;
	std::vector<VkImageView> m_SwapchainImageViews;
	std::vector<VkFramebuffer> m_Framebuffers;
	VkFormat m_SwapchainImageFormat;
	VkExtent2D m_SwapchainExtent;

	// Render pass
	VkRenderPass m_RenderPass = VK_NULL_HANDLE;

	// Descriptor pool for ImGui and other resources
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

	// Queue family indices
	uint32_t m_GraphicsQueueFamily = 0;

	// Command buffers
	VkCommandPool m_CommandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_CommandBuffers;

	// Synchronization objects (double buffering)
	static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
	// Per-frame resources (indexed by current frame)
	std::vector<VkFence> m_InFlightFences;
	// Per-swapchain-image resources (indexed by swapchain image count)
	// We need one semaphore pair per swapchain image to avoid reuse issues
	std::vector<VkSemaphore> m_ImageAvailableSemaphores;
	std::vector<VkSemaphore> m_RenderFinishedSemaphores;
	uint32_t m_CurrentFrame = 0;
	uint32_t m_CurrentSemaphoreIndex = 0; // Cycles through swapchain images
	uint32_t m_ImageIndex = 0;

	// Debug messenger for validation layers
	VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

	// Store vk-bootstrap wrapper objects for proper API usage
	vkb::Instance m_VkbInstance;
	vkb::PhysicalDevice m_VkbPhysicalDevice;
	vkb::Device m_VkbDevice;
	vkb::Swapchain m_VkbSwapchain;

	bool m_EnableValidationLayers = true; // Disable in release builds

	// Singleton instance for resource access
	static inline VulkanContext* s_Instance = nullptr;
};

}
