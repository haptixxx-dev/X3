#include "VulkanContext.h"
#include "Core/Log.h"
#include <vector>
#include <stdexcept>

namespace X3
{

void VulkanContext::setWindowHints() {
	// For Vulkan, we don't need OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
}

VulkanContext::VulkanContext(GLFWwindow* window)
	: m_NativeWindow(window) {
	s_Instance = this;
}

VulkanContext::~VulkanContext() {
	cleanup();
	s_Instance = nullptr;
}

void VulkanContext::init() {
	createInstance();
	createSurface();
	pickPhysicalDevice();
	createLogicalDevice();
	createAllocator();
	createSwapchain();
	createRenderPass();
	createFramebuffers();
	createCommandPool();
	createCommandBuffers();
	createDescriptorPool();
	createSyncObjects();

	LOG_ENGINE_INFO("Successfully initialized Vulkan context!");
}

void VulkanContext::createInstance() {
	// Use vk-bootstrap to simplify Vulkan instance creation
	vkb::InstanceBuilder builder;

	builder.set_app_name("X3 Engine")
		.request_validation_layers(m_EnableValidationLayers)
		.use_default_debug_messenger()
		.require_api_version(1, 2, 0);  // Vulkan 1.2 minimum

	auto inst_ret = builder.build();
	if (!inst_ret) {
		LOG_ENGINE_CRITICAL("Failed to create Vulkan instance: {}", inst_ret.error().message());
		throw std::runtime_error("Failed to create Vulkan instance");
	}

	m_VkbInstance = inst_ret.value();
	m_Instance = m_VkbInstance.instance;
	m_DebugMessenger = m_VkbInstance.debug_messenger;

	LOG_ENGINE_INFO("Vulkan instance created successfully");
}

void VulkanContext::createSurface() {
	if (glfwCreateWindowSurface(m_Instance, m_NativeWindow, nullptr, &m_Surface) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("Failed to create window surface!");
		throw std::runtime_error("Failed to create window surface");
	}
}

void VulkanContext::pickPhysicalDevice() {
	// Use vk-bootstrap to select a suitable GPU
	vkb::PhysicalDeviceSelector selector{ m_VkbInstance };
	selector.set_surface(m_Surface)
		.set_minimum_version(1, 2);  // Require Vulkan 1.2

	auto phys_ret = selector.select();
	if (!phys_ret) {
		LOG_ENGINE_CRITICAL("Failed to select Vulkan Physical Device: {}", phys_ret.error().message());
		throw std::runtime_error("Failed to select Vulkan Physical Device");
	}

	m_VkbPhysicalDevice = phys_ret.value();
	m_PhysicalDevice = m_VkbPhysicalDevice.physical_device;

	// Log GPU info
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);
	LOG_ENGINE_INFO("Selected GPU: {}", properties.deviceName);
}

void VulkanContext::createLogicalDevice() {
	// Use vk-bootstrap to create logical device
	vkb::DeviceBuilder device_builder{ m_VkbPhysicalDevice };

	auto dev_ret = device_builder.build();
	if (!dev_ret) {
		LOG_ENGINE_CRITICAL("Failed to create Vulkan logical device: {}", dev_ret.error().message());
		throw std::runtime_error("Failed to create Vulkan logical device");
	}

	m_VkbDevice = dev_ret.value();
	m_Device = m_VkbDevice.device;

	// Get graphics queue family index
	auto queue_index_ret = m_VkbDevice.get_queue_index(vkb::QueueType::graphics);
	if (!queue_index_ret) {
		LOG_ENGINE_CRITICAL("Failed to get graphics queue family index");
		throw std::runtime_error("Failed to get graphics queue family index");
	}
	m_GraphicsQueueFamily = queue_index_ret.value();

	// Get queue handles
	auto graphics_queue_ret = m_VkbDevice.get_queue(vkb::QueueType::graphics);
	if (!graphics_queue_ret) {
		LOG_ENGINE_CRITICAL("Failed to get graphics queue");
		throw std::runtime_error("Failed to get graphics queue");
	}
	m_GraphicsQueue = graphics_queue_ret.value();

	auto present_queue_ret = m_VkbDevice.get_queue(vkb::QueueType::present);
	if (!present_queue_ret) {
		LOG_ENGINE_CRITICAL("Failed to get present queue");
		throw std::runtime_error("Failed to get present queue");
	}
	m_PresentQueue = present_queue_ret.value();

	LOG_ENGINE_INFO("Vulkan logical device created successfully");
}

void VulkanContext::createAllocator() {
	VmaAllocatorCreateInfo allocatorInfo{};
	allocatorInfo.instance = m_Instance;
	allocatorInfo.physicalDevice = m_PhysicalDevice;
	allocatorInfo.device = m_Device;
	allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;

	if (vmaCreateAllocator(&allocatorInfo, &m_Allocator) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("Failed to create VMA allocator!");
		throw std::runtime_error("Failed to create VMA allocator");
	}

	LOG_ENGINE_INFO("VMA allocator created successfully");
}

void VulkanContext::createSwapchain() {
	// Use vk-bootstrap to create swapchain
	vkb::SwapchainBuilder swapchain_builder{ m_VkbDevice };

	auto swap_ret = swapchain_builder
		.set_old_swapchain(m_Swapchain)
		.build();

	if (!swap_ret) {
		LOG_ENGINE_CRITICAL("Failed to create Vulkan swapchain: {}", swap_ret.error().message());
		throw std::runtime_error("Failed to create Vulkan swapchain");
	}

	m_VkbSwapchain = swap_ret.value();
	m_Swapchain = m_VkbSwapchain.swapchain;
	m_SwapchainImageFormat = static_cast<VkFormat>(m_VkbSwapchain.image_format);
	m_SwapchainExtent = m_VkbSwapchain.extent;

	// Get swapchain images
	m_SwapchainImages = m_VkbSwapchain.get_images().value();
	m_SwapchainImageViews = m_VkbSwapchain.get_image_views().value();

	LOG_ENGINE_INFO("Vulkan swapchain created successfully ({} images, {}x{})",
		m_SwapchainImages.size(), m_SwapchainExtent.width, m_SwapchainExtent.height);
}

void VulkanContext::createRenderPass() {
	// Simple render pass with one color attachment
	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = m_SwapchainImageFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colorAttachmentRef{};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachmentRef;

	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 1;
	renderPassInfo.pAttachments = &colorAttachment;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;

	if (vkCreateRenderPass(m_Device, &renderPassInfo, nullptr, &m_RenderPass) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("Failed to create render pass!");
		throw std::runtime_error("Failed to create render pass");
	}

	LOG_ENGINE_INFO("Vulkan render pass created successfully");
}

void VulkanContext::createFramebuffers() {
	m_Framebuffers.resize(m_SwapchainImageViews.size());

	for (size_t i = 0; i < m_SwapchainImageViews.size(); i++) {
		VkImageView attachments[] = { m_SwapchainImageViews[i] };

		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = m_RenderPass;
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = attachments;
		framebufferInfo.width = m_SwapchainExtent.width;
		framebufferInfo.height = m_SwapchainExtent.height;
		framebufferInfo.layers = 1;

		if (vkCreateFramebuffer(m_Device, &framebufferInfo, nullptr, &m_Framebuffers[i]) != VK_SUCCESS) {
			LOG_ENGINE_CRITICAL("Failed to create framebuffer!");
			throw std::runtime_error("Failed to create framebuffer");
		}
	}

	LOG_ENGINE_INFO("Created {} framebuffers", m_Framebuffers.size());
}

void VulkanContext::createCommandPool() {
	auto graphics_queue_index = m_VkbDevice.get_queue_index(vkb::QueueType::graphics);
	if (!graphics_queue_index) {
		LOG_ENGINE_CRITICAL("Failed to get graphics queue index");
		throw std::runtime_error("Failed to get graphics queue index");
	}

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = graphics_queue_index.value();

	if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("Failed to create command pool!");
		throw std::runtime_error("Failed to create command pool");
	}

	LOG_ENGINE_INFO("Command pool created successfully");
}

void VulkanContext::createCommandBuffers() {
	m_CommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_CommandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = static_cast<uint32_t>(m_CommandBuffers.size());

	if (vkAllocateCommandBuffers(m_Device, &allocInfo, m_CommandBuffers.data()) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("Failed to allocate command buffers!");
		throw std::runtime_error("Failed to allocate command buffers");
	}

	LOG_ENGINE_INFO("Created {} command buffers", m_CommandBuffers.size());
}

void VulkanContext::createSyncObjects() {
	m_ImageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	m_RenderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	m_InFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled so first frame doesn't wait

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]) != VK_SUCCESS ||
			vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]) != VK_SUCCESS ||
			vkCreateFence(m_Device, &fenceInfo, nullptr, &m_InFlightFences[i]) != VK_SUCCESS) {
			LOG_ENGINE_CRITICAL("Failed to create synchronization objects!");
			throw std::runtime_error("Failed to create synchronization objects");
		}
	}

	LOG_ENGINE_INFO("Synchronization objects created successfully");
}

bool VulkanContext::beginFrame() {
	// Wait for the previous frame to finish
	vkWaitForFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame], VK_TRUE, UINT64_MAX);

	// Acquire the next image from the swapchain
	VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
		m_ImageAvailableSemaphores[m_CurrentFrame], VK_NULL_HANDLE, &m_ImageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		// Swapchain needs to be recreated (window resize, etc.)
		recreateSwapchain();
		return false;
	} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		LOG_ENGINE_ERROR("Failed to acquire swapchain image!");
		return false;
	}

	// Only reset the fence if we're submitting work
	vkResetFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame]);

	// Reset command buffer
	vkResetCommandBuffer(m_CommandBuffers[m_CurrentFrame], 0);

	// Begin command buffer
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = 0;
	beginInfo.pInheritanceInfo = nullptr;

	if (vkBeginCommandBuffer(m_CommandBuffers[m_CurrentFrame], &beginInfo) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to begin recording command buffer!");
		return false;
	}

	// Begin render pass
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = m_RenderPass;
	renderPassInfo.framebuffer = m_Framebuffers[m_ImageIndex];
	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = m_SwapchainExtent;

	VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
	renderPassInfo.clearValueCount = 1;
	renderPassInfo.pClearValues = &clearColor;

	vkCmdBeginRenderPass(m_CommandBuffers[m_CurrentFrame], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

	return true;
}

void VulkanContext::endFrame() {
	// End render pass
	vkCmdEndRenderPass(m_CommandBuffers[m_CurrentFrame]);

	// End command buffer
	if (vkEndCommandBuffer(m_CommandBuffers[m_CurrentFrame]) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to record command buffer!");
		return;
	}

	// Submit command buffer
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[m_CurrentFrame] };
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &m_CommandBuffers[m_CurrentFrame];

	VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[m_CurrentFrame] };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	if (vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, m_InFlightFences[m_CurrentFrame]) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to submit draw command buffer!");
		return;
	}

	// Present
	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;

	VkSwapchainKHR swapchains[] = { m_Swapchain };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapchains;
	presentInfo.pImageIndices = &m_ImageIndex;
	presentInfo.pResults = nullptr;

	VkResult result = vkQueuePresentKHR(m_PresentQueue, &presentInfo);

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
		// Swapchain needs to be recreated
		recreateSwapchain();
	} else if (result != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to present swapchain image!");
	}

	// Move to next frame
	m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanContext::createDescriptorPool() {
	// Create a descriptor pool for ImGui and other UI resources
	// This pool needs to be large enough for ImGui's needs
	VkDescriptorPoolSize pool_sizes[] = {
		{ VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
	};

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;

	if (vkCreateDescriptorPool(m_Device, &pool_info, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("Failed to create descriptor pool!");
		throw std::runtime_error("Failed to create descriptor pool");
	}

	LOG_ENGINE_INFO("Descriptor pool created successfully");
}

void VulkanContext::swapBuffers() {
	// In Vulkan, we need to manage frame lifecycle differently than OpenGL
	// The application loop calls: window.onUpdate() -> swapBuffers() -> rendering commands
	// So swapBuffers() needs to:
	// 1. End the previous frame (if there was one)
	// 2. Begin a new frame for upcoming rendering commands

	static bool isFirstFrame = true;

	if (!isFirstFrame) {
		// End the previous frame
		endFrame();
	}

	// Begin the new frame for upcoming rendering commands
	bool success = beginFrame();
	if (!success && !isFirstFrame) {
		// If beginFrame fails after the first frame, try once more
		// (this can happen during swapchain recreation)
		success = beginFrame();
	}

	isFirstFrame = false;
}

void VulkanContext::recreateSwapchain() {
	// Wait for device to be idle
	vkDeviceWaitIdle(m_Device);

	// Check if the window is minimized
	int width = 0, height = 0;
	glfwGetFramebufferSize(m_NativeWindow, &width, &height);
	while (width == 0 || height == 0) {
		glfwGetFramebufferSize(m_NativeWindow, &width, &height);
		glfwWaitEvents();
	}

	LOG_ENGINE_INFO("Recreating swapchain (new size: {}x{})", width, height);

	// Clean up old swapchain resources
	cleanupSwapchain();

	// Recreate swapchain and dependent resources
	createSwapchain();
	createFramebuffers();

	LOG_ENGINE_INFO("Swapchain recreation complete");
}

void VulkanContext::cleanupSwapchain() {
	// Cleanup framebuffers
	for (auto framebuffer : m_Framebuffers) {
		vkDestroyFramebuffer(m_Device, framebuffer, nullptr);
	}
	m_Framebuffers.clear();

	// Cleanup swapchain image views
	for (auto imageView : m_SwapchainImageViews) {
		vkDestroyImageView(m_Device, imageView, nullptr);
	}
	m_SwapchainImageViews.clear();

	// Cleanup swapchain
	if (m_Swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
		m_Swapchain = VK_NULL_HANDLE;
	}

	m_SwapchainImages.clear();
}

void VulkanContext::cleanup() {
	if (m_Device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(m_Device);

		// Cleanup synchronization objects
		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
			if (m_ImageAvailableSemaphores[i] != VK_NULL_HANDLE) {
				vkDestroySemaphore(m_Device, m_ImageAvailableSemaphores[i], nullptr);
			}
			if (m_RenderFinishedSemaphores[i] != VK_NULL_HANDLE) {
				vkDestroySemaphore(m_Device, m_RenderFinishedSemaphores[i], nullptr);
			}
			if (m_InFlightFences[i] != VK_NULL_HANDLE) {
				vkDestroyFence(m_Device, m_InFlightFences[i], nullptr);
			}
		}

		// Cleanup command pool (automatically frees command buffers)
		if (m_CommandPool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
		}

		// Cleanup descriptor pool
		if (m_DescriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
		}

		// Cleanup render pass
		if (m_RenderPass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
		}

		// Cleanup swapchain resources
		cleanupSwapchain();

		// Cleanup VMA allocator
		if (m_Allocator != VK_NULL_HANDLE) {
			vmaDestroyAllocator(m_Allocator);
		}

		vkDestroyDevice(m_Device, nullptr);
	}

	if (m_Surface != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
	}

	if (m_DebugMessenger != VK_NULL_HANDLE) {
		auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
			m_Instance, "vkDestroyDebugUtilsMessengerEXT");
		if (func != nullptr) {
			func(m_Instance, m_DebugMessenger, nullptr);
		}
	}

	if (m_Instance != VK_NULL_HANDLE) {
		vkDestroyInstance(m_Instance, nullptr);
	}
}

}
