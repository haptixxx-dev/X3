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

	// Don't call beginFrame() here - let the first swapBuffers() call handle it
	// This avoids conflicts with ImGui font upload which creates its own command buffer
	m_FirstFrame = true;
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

	// Create overlay render pass for ImGui (preserves existing content after blit)
	VkAttachmentDescription overlayAttachment{};
	overlayAttachment.format = m_SwapchainImageFormat;
	overlayAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	overlayAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // Preserve existing content
	overlayAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	overlayAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	overlayAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	overlayAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	overlayAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkRenderPassCreateInfo overlayRenderPassInfo{};
	overlayRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	overlayRenderPassInfo.attachmentCount = 1;
	overlayRenderPassInfo.pAttachments = &overlayAttachment;
	overlayRenderPassInfo.subpassCount = 1;
	overlayRenderPassInfo.pSubpasses = &subpass;  // Same subpass config
	overlayRenderPassInfo.dependencyCount = 1;
	overlayRenderPassInfo.pDependencies = &dependency;  // Same dependency

	if (vkCreateRenderPass(m_Device, &overlayRenderPassInfo, nullptr, &m_OverlayRenderPass) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("Failed to create overlay render pass!");
		throw std::runtime_error("Failed to create overlay render pass");
	}

	LOG_ENGINE_INFO("Vulkan overlay render pass created successfully");
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
	// Fences are per-frame-in-flight
	m_InFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

	// Semaphores are per-swapchain-image to avoid reuse issues
	m_ImageAvailableSemaphores.resize(m_SwapchainImages.size());
	m_RenderFinishedSemaphores.resize(m_SwapchainImages.size());

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled so first frame doesn't wait

	// Create fences for frames in flight
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (vkCreateFence(m_Device, &fenceInfo, nullptr, &m_InFlightFences[i]) != VK_SUCCESS) {
			LOG_ENGINE_CRITICAL("Failed to create fence!");
			throw std::runtime_error("Failed to create synchronization objects");
		}
	}

	// Create semaphores for each swapchain image
	for (size_t i = 0; i < m_SwapchainImages.size(); i++) {
		if (vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]) != VK_SUCCESS ||
			vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]) != VK_SUCCESS) {
			LOG_ENGINE_CRITICAL("Failed to create semaphore!");
			throw std::runtime_error("Failed to create synchronization objects");
		}
	}

	LOG_ENGINE_INFO("Synchronization objects created successfully");
}

bool VulkanContext::beginFrame() {
	// Wait for the previous frame to finish
	vkWaitForFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame], VK_TRUE, UINT64_MAX);

	// Acquire the next image from the swapchain
	// Use the current semaphore index (not image index) to avoid reuse issues
	// The semaphore index cycles independently of which swapchain image we get
	VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
		m_ImageAvailableSemaphores[m_CurrentSemaphoreIndex], VK_NULL_HANDLE, &m_ImageIndex);

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
	m_RenderPassActive = true;

	return true;
}

void VulkanContext::endFrame() {
	// End render pass (only if it's still active - blitImageToSwapchain may have ended it)
	if (m_RenderPassActive) {
		vkCmdEndRenderPass(m_CommandBuffers[m_CurrentFrame]);
		m_RenderPassActive = false;
	}

	// End command buffer
	if (vkEndCommandBuffer(m_CommandBuffers[m_CurrentFrame]) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to record command buffer!");
		return;
	}

	// Submit command buffer
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	// Wait on the semaphore for the acquired image
	VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[m_CurrentSemaphoreIndex] };
	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &m_CommandBuffers[m_CurrentFrame];

	// Signal the semaphore when rendering is finished
	VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[m_CurrentSemaphoreIndex] };
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

	// Move to next frame and semaphore index
	m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	m_CurrentSemaphoreIndex = (m_CurrentSemaphoreIndex + 1) % m_SwapchainImages.size();
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

void VulkanContext::ensureFrameStarted() {
	if (m_FirstFrame) {
		m_FirstFrame = false;
		bool success = beginFrame();
		if (!success) {
			LOG_ENGINE_ERROR("Failed to begin first frame!");
		}
	}
}

VkCommandBuffer VulkanContext::beginSingleTimeCommands() {
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = m_CommandPool;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer cmd;
	vkAllocateCommandBuffers(m_Device, &allocInfo, &cmd);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(cmd, &beginInfo);

	return cmd;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer cmd) {
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;

	vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(m_GraphicsQueue);

	vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &cmd);
}

void VulkanContext::beginRenderPass() {
	if (m_RenderPassActive) {
		return; // Already active, nothing to do
	}

	VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

	// Transition swapchain image from PRESENT_SRC_KHR to COLOR_ATTACHMENT_OPTIMAL
	// (blitImageToSwapchain leaves it in PRESENT_SRC_KHR)
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = m_SwapchainImages[m_ImageIndex];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);

	// Begin overlay render pass (preserves existing content from blit)
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = m_OverlayRenderPass;  // Use overlay pass that loads content
	renderPassInfo.framebuffer = m_Framebuffers[m_ImageIndex];
	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = m_SwapchainExtent;
	renderPassInfo.clearValueCount = 0;
	renderPassInfo.pClearValues = nullptr;

	vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	m_RenderPassActive = true;
}

void VulkanContext::beginOverlayRenderPass() {
	LOG_ENGINE_INFO("beginOverlayRenderPass: start, m_RenderPassActive={}", m_RenderPassActive);

	// Validate handles
	if (m_OverlayRenderPass == VK_NULL_HANDLE) {
		LOG_ENGINE_CRITICAL("beginOverlayRenderPass: m_OverlayRenderPass is NULL!");
		return;
	}
	if (m_ImageIndex >= m_Framebuffers.size()) {
		LOG_ENGINE_CRITICAL("beginOverlayRenderPass: m_ImageIndex {} out of bounds (size={})", m_ImageIndex, m_Framebuffers.size());
		return;
	}

	VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

	// End current render pass if active (could be main or overlay)
	if (m_RenderPassActive) {
		LOG_ENGINE_INFO("beginOverlayRenderPass: ending current render pass");
		vkCmdEndRenderPass(cmd);
		m_RenderPassActive = false;
	}

	LOG_ENGINE_INFO("beginOverlayRenderPass: transitioning image layout");
	// After main render pass ends, image is in PRESENT_SRC_KHR
	// Transition to COLOR_ATTACHMENT_OPTIMAL for overlay render pass
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = m_SwapchainImages[m_ImageIndex];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);

	LOG_ENGINE_INFO("beginOverlayRenderPass: starting overlay render pass, framebuffer={}", m_ImageIndex);
	// Begin overlay render pass
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = m_OverlayRenderPass;
	renderPassInfo.framebuffer = m_Framebuffers[m_ImageIndex];
	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = m_SwapchainExtent;
	renderPassInfo.clearValueCount = 0;
	renderPassInfo.pClearValues = nullptr;

	vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	m_RenderPassActive = true;
	LOG_ENGINE_INFO("beginOverlayRenderPass: done");
}

void VulkanContext::swapBuffers() {
	// In Vulkan, swapBuffers presents the rendered frame
	// The application loop is: pollEvents() -> Clear() -> onUpdate() -> swapBuffers()

	// Handle first frame - beginFrame() wasn't called in init() to avoid conflicts with ImGui font upload
	if (m_FirstFrame) {
		ensureFrameStarted();
		return; // Don't end frame on first call - we just started it
	}

	endFrame();

	bool success = beginFrame();
	if (!success) {
		// Swapchain might need recreation, try again
		LOG_ENGINE_WARN("beginFrame failed after endFrame, retrying...");
		success = beginFrame();
		if (!success) {
			LOG_ENGINE_ERROR("Failed to begin frame after retry!");
			return;
		}
	}
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

	// Recreate semaphores in case swapchain image count changed
	// First, clean up old semaphores
	for (auto semaphore : m_ImageAvailableSemaphores) {
		vkDestroySemaphore(m_Device, semaphore, nullptr);
	}
	for (auto semaphore : m_RenderFinishedSemaphores) {
		vkDestroySemaphore(m_Device, semaphore, nullptr);
	}
	m_ImageAvailableSemaphores.clear();
	m_RenderFinishedSemaphores.clear();

	// Recreate semaphores for new swapchain image count
	m_ImageAvailableSemaphores.resize(m_SwapchainImages.size());
	m_RenderFinishedSemaphores.resize(m_SwapchainImages.size());

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	for (size_t i = 0; i < m_SwapchainImages.size(); i++) {
		if (vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]) != VK_SUCCESS ||
			vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]) != VK_SUCCESS) {
			LOG_ENGINE_CRITICAL("Failed to recreate semaphores!");
			throw std::runtime_error("Failed to recreate semaphores");
		}
	}

	// Reset semaphore index
	m_CurrentSemaphoreIndex = 0;

	// Set flag for ImGui to detect and update its resources
	m_SwapchainRecreated = true;

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
		// Semaphores are per-swapchain-image
		for (auto semaphore : m_ImageAvailableSemaphores) {
			if (semaphore != VK_NULL_HANDLE) {
				vkDestroySemaphore(m_Device, semaphore, nullptr);
			}
		}
		for (auto semaphore : m_RenderFinishedSemaphores) {
			if (semaphore != VK_NULL_HANDLE) {
				vkDestroySemaphore(m_Device, semaphore, nullptr);
			}
		}
		// Fences are per-frame-in-flight
		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
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

		// Cleanup render passes
		if (m_RenderPass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
		}
		if (m_OverlayRenderPass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(m_Device, m_OverlayRenderPass, nullptr);
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

void VulkanContext::blitImageToSwapchain(VkImage sourceImage, VkImageLayout currentLayout,
                                          uint32_t srcWidth, uint32_t srcHeight,
                                          glm::ivec4 viewport, glm::ivec2 windowSize) {
	VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

	// End the render pass if it's active (we need to be outside render pass for blitting)
	if (m_RenderPassActive) {
		vkCmdEndRenderPass(cmd);
		m_RenderPassActive = false;
	}

	// Transition source image to TRANSFER_SRC_OPTIMAL
	VkImageMemoryBarrier srcBarrier{};
	srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	srcBarrier.oldLayout = currentLayout;
	srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	srcBarrier.image = sourceImage;
	srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	srcBarrier.subresourceRange.baseMipLevel = 0;
	srcBarrier.subresourceRange.levelCount = 1;
	srcBarrier.subresourceRange.baseArrayLayer = 0;
	srcBarrier.subresourceRange.layerCount = 1;
	srcBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

	// Transition swapchain image to TRANSFER_DST_OPTIMAL
	VkImageMemoryBarrier dstBarrier{};
	dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // We don't care about previous contents
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.image = m_SwapchainImages[m_ImageIndex];
	dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	dstBarrier.subresourceRange.baseMipLevel = 0;
	dstBarrier.subresourceRange.levelCount = 1;
	dstBarrier.subresourceRange.baseArrayLayer = 0;
	dstBarrier.subresourceRange.layerCount = 1;
	dstBarrier.srcAccessMask = 0;
	dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

	// Clear the swapchain image first (for letterboxing)
	VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 1.0f}};
	VkImageSubresourceRange clearRange{};
	clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	clearRange.baseMipLevel = 0;
	clearRange.levelCount = 1;
	clearRange.baseArrayLayer = 0;
	clearRange.layerCount = 1;
	vkCmdClearColorImage(cmd, m_SwapchainImages[m_ImageIndex],
	                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);

	// Blit the source image to the viewport region of the swapchain
	VkImageBlit blitRegion{};
	blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.srcSubresource.mipLevel = 0;
	blitRegion.srcSubresource.baseArrayLayer = 0;
	blitRegion.srcSubresource.layerCount = 1;
	blitRegion.srcOffsets[0] = {0, 0, 0};
	blitRegion.srcOffsets[1] = {static_cast<int32_t>(srcWidth), static_cast<int32_t>(srcHeight), 1};

	blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.dstSubresource.mipLevel = 0;
	blitRegion.dstSubresource.baseArrayLayer = 0;
	blitRegion.dstSubresource.layerCount = 1;
	// viewport: x, y, x+width, y+height (matching OpenGL glBlitFramebuffer convention)
	// Note: Vulkan Y is top-down, so we may need to flip
	blitRegion.dstOffsets[0] = {viewport.x, windowSize.y - viewport.w, 0}; // Flip Y
	blitRegion.dstOffsets[1] = {viewport.z, windowSize.y - viewport.y, 1}; // Flip Y

	vkCmdBlitImage(cmd,
		sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		m_SwapchainImages[m_ImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blitRegion, VK_FILTER_LINEAR);

	// Transition swapchain image to PRESENT_SRC
	VkImageMemoryBarrier presentBarrier{};
	presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	presentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	presentBarrier.image = m_SwapchainImages[m_ImageIndex];
	presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	presentBarrier.subresourceRange.baseMipLevel = 0;
	presentBarrier.subresourceRange.levelCount = 1;
	presentBarrier.subresourceRange.baseArrayLayer = 0;
	presentBarrier.subresourceRange.layerCount = 1;
	presentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	presentBarrier.dstAccessMask = 0;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, nullptr, 0, nullptr, 1, &presentBarrier);

	// Transition source image back to GENERAL for next frame's compute shader
	VkImageMemoryBarrier srcBackBarrier{};
	srcBackBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	srcBackBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	srcBackBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	srcBackBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	srcBackBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	srcBackBarrier.image = sourceImage;
	srcBackBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	srcBackBarrier.subresourceRange.baseMipLevel = 0;
	srcBackBarrier.subresourceRange.levelCount = 1;
	srcBackBarrier.subresourceRange.baseArrayLayer = 0;
	srcBackBarrier.subresourceRange.layerCount = 1;
	srcBackBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	srcBackBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &srcBackBarrier);
}

}
