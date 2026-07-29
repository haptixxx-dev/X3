#include "VulkanContext.h"
#include "Core/Log.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace X3
{

// Declared in VulkanTypes.h. Construction failure in this layer is fatal rather
// than silently producing a half-built object with a null handle.
void VkCheck(VkResult result, const char* expr, const char* file, int line)
{
	if (result == VK_SUCCESS)
		return;

	LOG_ENGINE_CRITICAL("Vulkan call failed ({}): {} at {}:{}",
		static_cast<int>(result), expr, file, line);
	throw std::runtime_error(std::string("Vulkan call failed: ") + expr);
}

void VulkanContext::setWindowHints() {
	// For Vulkan, we don't need OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
}

VulkanContext::VulkanContext(GLFWwindow* window, bool vsync)
	: m_NativeWindow(window)
	, m_VSync(vsync) {
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
	createSwapchain(VK_NULL_HANDLE);
	createCommandPool();
	createCommandBuffers();
	createDescriptorPool();
	createSyncObjects();

	m_Staging = VulkanStagingArena(*this);
	createDummyResources();

	LOG_ENGINE_INFO("Successfully initialized Vulkan context!");

	// NOTE: no frame is begun here, and no render pass exists to begin. The old
	// m_FirstFrame hack existed only because init() could not open a render pass
	// before ImGui's font upload; with no render pass in beginFrame() it has no
	// purpose and is gone. Application::run owns the frame boundary now.
}

void VulkanContext::createInstance() {
	// Use vk-bootstrap to simplify Vulkan instance creation
	vkb::InstanceBuilder builder;

	builder.set_app_name("X3 Engine")
		.request_validation_layers(m_EnableValidationLayers)
		.use_default_debug_messenger()
		// 1.3 for dynamicRendering + synchronization2 as core features.
		.require_api_version(1, 3, 0);

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
	// Vulkan 1.3 core AND the VK_KHR_dynamic_rendering extension. BOTH, and this
	// is the single most likely thing to get wrong.
	//
	// With dynamicRendering enabled through VkPhysicalDeviceVulkan13Features and
	// no extension: vkGetInstanceProcAddr(inst, "vkCmdBeginRenderingKHR") returns
	// a non-NULL loader trampoline while vkGetDeviceProcAddr(dev,
	// "vkCmdBeginRenderingKHR") returns NULL. The vendored ImGui resolves its
	// pointer through the INSTANCE path (imgui_impl_vulkan.cpp:1098-1099) and
	// asserts non-NULL (:1101-1102) -- so the assert passes and the failure
	// surfaces at the first RenderDrawData instead of at init.
	// imgui_impl_vulkan.h:89 says so verbatim: "Need to explicitly enable
	// VK_KHR_dynamic_rendering extension to use this, even for Vulkan 1.3."
	//
	// Do NOT also pass VkPhysicalDeviceDynamicRenderingFeatures:
	// VUID-VkDeviceCreateInfo-pNext-06532 forbids an aggregate 1.3 struct and the
	// individual structs it subsumes in one pNext chain. The 1.3 struct is a
	// superset.
	VkPhysicalDeviceVulkan13Features features13{};
	features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features13.dynamicRendering = VK_TRUE;
	// Enabled deliberately UNUSED this phase: every barrier here is in
	// synchronization-1 form to match blitImageToSwapchain, and mixing forms in
	// one commit makes the diff unreadable. Enabling the bit now makes the later
	// conversion a pure edit.
	features13.synchronization2 = VK_TRUE;

	// Phase 2's material texture table is a fixed-size array of combined image
	// samplers indexed by a material index that is DIVERGENT across lanes in a
	// path tracer, so the array index is non-uniform and the shader qualifies it
	// with nonuniformEXT(). That needs descriptorIndexing plus the non-uniform
	// sampled-image capability; the base dynamic-indexing feature lives in the
	// core 1.0 feature struct rather than the 1.2 one.
	//
	// Deliberately NOT requested: runtimeDescriptorArray,
	// descriptorBindingVariableDescriptorCount, descriptorBindingPartiallyBound
	// and every updateAfterBind variant. A fixed-size array whose every element
	// is always written needs none of them, and keeping the feature set this
	// small is the whole reason the fixed-size binding model was chosen over
	// full bindless -- descriptor indexing is the weakest part of MoltenVK's
	// coverage and macOS is a supported target.
	VkPhysicalDeviceVulkan12Features features12{};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12.descriptorIndexing = VK_TRUE;
	features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

	VkPhysicalDeviceFeatures features10{};
	features10.shaderSampledImageArrayDynamicIndexing = VK_TRUE;

	// ONE DESCRIPTOR TABLE SERVES EVERY PIPELINE, and it declares writable
	// storage images and buffers -- the render target, the BSDF LUT, the cluster
	// grid. Phase 7c put a FRAGMENT shader on that table, and without this every
	// such binding would have to carry NonWritable in the fragment stage
	// (VUID-RuntimeSpirv-NonWritable-06340), which Slang has no reason to emit
	// for a resource declared RW in the shared Bindings module.
	//
	// Enabling it is not a workaround for the shared table: the forward pass will
	// need it for real the moment anything writes from a fragment shader.
	features10.fragmentStoresAndAtomics = VK_TRUE;

	// Slang lowers SV_VertexID through the DrawParameters capability, so the
	// depth prepass's vertex shader will not load without this. It is core in
	// Vulkan 1.1 and free to enable.
	VkPhysicalDeviceVulkan11Features features11{};
	features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	features11.shaderDrawParameters = VK_TRUE;

	vkb::PhysicalDeviceSelector selector{ m_VkbInstance };
	selector.set_surface(m_Surface)
		.set_minimum_version(1, 3)
		.set_required_features(features10)
		.set_required_features_11(features11)
		.set_required_features_12(features12)
		.set_required_features_13(features13)
		.add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

	auto phys_ret = selector.select();
	if (!phys_ret) {
		LOG_ENGINE_CRITICAL("Failed to select Vulkan Physical Device: {}", phys_ret.error().message());
		throw std::runtime_error("Failed to select Vulkan Physical Device");
	}

	m_VkbPhysicalDevice = phys_ret.value();
	m_PhysicalDevice = m_VkbPhysicalDevice.physical_device;

	// Cache the limits rather than throwing the properties away: the per-frame
	// ring stride is computed from minUniformBufferOffsetAlignment /
	// minStorageBufferOffsetAlignment / nonCoherentAtomSize on every reallocation.
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);
	m_Limits = properties.limits;
	LOG_ENGINE_INFO("Selected GPU: {}", properties.deviceName);
}

void VulkanContext::createLogicalDevice() {
	// No feature configuration here: DeviceBuilder reads the selected
	// PhysicalDevice's extensions_to_enable and extended_features_chain, both
	// populated by the selector above. Hand-building a VkPhysicalDeviceFeatures2
	// and passing it via add_pNext is an ERROR in vk-bootstrap
	// (DeviceError::VkPhysicalDeviceFeatures2_in_pNext_chain_while_using_add_required_extension_features).
	vkb::DeviceBuilder device_builder{ m_VkbPhysicalDevice };

	auto dev_ret = device_builder.build();
	if (!dev_ret) {
		LOG_ENGINE_CRITICAL("Failed to create Vulkan logical device: {}", dev_ret.error().message());
		throw std::runtime_error("Failed to create Vulkan logical device");
	}

	m_VkbDevice = dev_ret.value();
	m_Device = m_VkbDevice.device;

	auto queue_index_ret = m_VkbDevice.get_queue_index(vkb::QueueType::graphics);
	if (!queue_index_ret) {
		LOG_ENGINE_CRITICAL("Failed to get graphics queue family index");
		throw std::runtime_error("Failed to get graphics queue family index");
	}
	m_GraphicsQueueFamily = queue_index_ret.value();

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
	allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;

	if (vmaCreateAllocator(&allocatorInfo, &m_Allocator) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("Failed to create VMA allocator!");
		throw std::runtime_error("Failed to create VMA allocator");
	}

	LOG_ENGINE_INFO("VMA allocator created successfully");
}

void VulkanContext::createSwapchain(VkSwapchainKHR oldSwapchain) {
	vkb::SwapchainBuilder swapchain_builder{ m_VkbDevice };
	swapchain_builder.set_old_swapchain(oldSwapchain);

	// Present-mode availability is vk-bootstrap's job: detail::find_present_mode
	// walks the desired list in order and falls back to FIFO unconditionally, and
	// FIFO is the one mode every VK_KHR_swapchain implementation must support, so
	// the fallback can never fail. Do NOT hand-roll
	// vkGetPhysicalDeviceSurfacePresentModesKHR.
	if (m_VSync) {
		// What a user ticking "VSync" means. Not FIFO_RELAXED -- it tears on late
		// frames, which is precisely what they asked not to happen.
		swapchain_builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
	} else {
		// set_desired_present_mode INSERTS AT THE FRONT (VkBootstrap.cpp:2110-2113)
		// and add_fallback_present_mode APPENDS -- calling the former twice would
		// reverse the intended order.
		swapchain_builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR);
		swapchain_builder.add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR);
		swapchain_builder.add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR);
	}

	// The runtime path clears and blits INTO the swapchain image, which requires
	// TRANSFER_DST. vk-bootstrap's default is COLOR_ATTACHMENT only. add_ ORs;
	// set_ REPLACES and would strip COLOR_ATTACHMENT, which the editor's
	// rendering block needs.
	swapchain_builder.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);

	auto swap_ret = swapchain_builder.build();
	if (!swap_ret) {
		LOG_ENGINE_CRITICAL("Failed to create Vulkan swapchain: {}", swap_ret.error().message());
		throw std::runtime_error("Failed to create Vulkan swapchain");
	}

	m_VkbSwapchain = swap_ret.value();
	m_Swapchain = m_VkbSwapchain.swapchain;
	m_SwapchainImageFormat = static_cast<VkFormat>(m_VkbSwapchain.image_format);
	m_SwapchainExtent = m_VkbSwapchain.extent;

	m_SwapchainImages = m_VkbSwapchain.get_images().value();
	m_SwapchainImageViews = m_VkbSwapchain.get_image_views().value();

	// Report what was actually GRANTED, not what was requested, so "vsync doesn't
	// work" is diagnosable from a log.
	LOG_ENGINE_INFO("Swapchain: {} images, {}x{}, format {}, present mode {}, usage 0x{:x}",
		m_SwapchainImages.size(), m_SwapchainExtent.width, m_SwapchainExtent.height,
		static_cast<int>(m_SwapchainImageFormat), static_cast<int>(m_VkbSwapchain.present_mode),
		static_cast<uint32_t>(m_VkbSwapchain.image_usage_flags));
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
	m_CommandBuffers.resize(FRAMES_IN_FLIGHT);

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
	m_InFlightFences.resize(FRAMES_IN_FLIGHT);
	m_ImageAvailableSemaphores.resize(FRAMES_IN_FLIGHT);

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signalled: the fence invariant

	for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
		X3_VK_CHECK(vkCreateFence(m_Device, &fenceInfo, nullptr, &m_InFlightFences[i]));
		X3_VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]));
	}

	ensureRenderFinishedSemaphores();

	LOG_ENGINE_INFO("Synchronization objects created successfully");
}

void VulkanContext::ensureRenderFinishedSemaphores() {
	VkSemaphoreCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	while (m_RenderFinishedSemaphores.size() < m_SwapchainImages.size()) {
		VkSemaphore s = VK_NULL_HANDLE;
		X3_VK_CHECK(vkCreateSemaphore(m_Device, &info, nullptr, &s));
		m_RenderFinishedSemaphores.push_back(s);
	}
	// The array may end up LONGER than the image count after a shrink. That is
	// harmless: indexing by m_ImageIndex never reads past the current count, and
	// every semaphore is destroyed exactly once, in cleanup().
}

void VulkanContext::createDescriptorPool() {
	// Large enough for ImGui plus the engine's own sets.
	VkDescriptorPoolSize pool_sizes[] = {
		{ VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		// Sized for the material texture table, which is an ARRAY binding:
		// 3 compute pipelines x FRAMES_IN_FLIGHT sets x 128 elements = 768
		// combined image samplers before a single skybox or ImGui viewport
		// texture is counted. 1000 would have been exhausted by the engine's own
		// sets alone. Under-sizing here surfaces as a vkAllocateDescriptorSets
		// failure at pipeline creation, which is at least loud.
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 },
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
	// LOAD-BEARING, not incidental: it is what makes deferFreeDescriptorSets legal.
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

void VulkanContext::createDummyResources() {
	// Built out of frame, through the one surviving blocking helper. These are
	// what DescriptorWriter::flush()'s always-write rule falls back to when a
	// binding has nothing real to point at (an absent skybox, an empty light
	// list): an unwritten descriptor read by a dispatch is
	// VUID-vkCmdDispatch-None-08114 and, in practice, undefined memory.
	assert(!m_FrameActive && "dummy resources must be built outside a frame");

	const uint32_t blackPixel = 0x00000000u;   // 1x1 opaque-black RGBA is 0,0,0,255
	const uint8_t  blackRGBA[4] = { 0, 0, 0, 255 };
	(void)blackPixel;

	TextureDesc desc{};
	desc.width     = 1;
	desc.height    = 1;
	desc.format    = VK_FORMAT_R8G8B8A8_SRGB;
	desc.mipLevels = 1;
	desc.debugName = "X3::dummyTexture";
	m_DummyTexture = std::make_unique<VulkanTexture>(*this, desc, blackRGBA);

	m_DummyStorageBuffer = std::make_unique<VulkanBuffer>(*this, BufferKind::Storage, 256, "X3::dummyStorageBuffer");
	m_DummyUniformBuffer = std::make_unique<VulkanBuffer>(*this, BufferKind::Uniform, 256, "X3::dummyUniformBuffer");

	// Zero them: a shader reading a dummy binding must read zeroes, not whatever
	// the allocator handed back.
	{
		std::vector<uint8_t> zeroes(256, 0);
		VkBufferCreateInfo bi{};
		bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bi.size = 256;
		bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo ai{};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

		VkBuffer staging = VK_NULL_HANDLE;
		VmaAllocation stagingAlloc = VK_NULL_HANDLE;
		VmaAllocationInfo stagingInfo{};
		X3_VK_CHECK(vmaCreateBuffer(m_Allocator, &bi, &ai, &staging, &stagingAlloc, &stagingInfo));
		std::memcpy(stagingInfo.pMappedData, zeroes.data(), zeroes.size());

		VkCommandBuffer cmd = beginSingleTimeCommands();
		VkBufferCopy region{ 0, 0, 256 };
		vkCmdCopyBuffer(cmd, staging, m_DummyStorageBuffer->handle(), 1, &region);
		vkCmdCopyBuffer(cmd, staging, m_DummyUniformBuffer->handle(), 1, &region);
		endSingleTimeCommands(cmd);

		vmaDestroyBuffer(m_Allocator, staging, stagingAlloc);
	}

	LOG_ENGINE_INFO("Dummy resources created (1x1 texture, 2x 256 B buffers)");
}

// =============================================================================
// FRAME LIFECYCLE
// =============================================================================

const FrameContext* VulkanContext::beginFrame() {
	assert(!m_FrameActive && "beginFrame() called twice without endFrame()");

	// A vSync toggle dispatched from an ImGui checkbox lands mid-frame and cannot
	// recreate the swapchain there. Apply it here, outside any frame.
	if (m_PresentModeDirty) {
		m_PresentModeDirty = false;
		recreateSwapchain();
	}

	// 1. Wait for the GPU to finish with this frame slot. THE guarantee behind
	//    every per-frame ring in the engine.
	X3_VK_CHECK(vkWaitForFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame], VK_TRUE, UINT64_MAX));

	// 2. Publish completion and reclaim. This must run even if the acquire below
	//    fails, or a click-and-drag resize starves the deletion queue.
	//
	//    Take this formulation, not the equivalent-looking
	//    `m_FrameNumber - retireFrame >= FRAMES_IN_FLIGHT`: that one underflows on
	//    uint64_t during the first two frames and frees resources a frame early.
	if (m_FrameNumber >= FRAMES_IN_FLIGHT)
		m_CompletedFrame = m_FrameNumber - FRAMES_IN_FLIGHT;
	drainDeletionQueue();
	m_Staging.reset(m_CurrentFrame);

	// 3. Acquire. imageAvailable is indexed by FRAME SLOT.
	VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
		m_ImageAvailableSemaphores[m_CurrentFrame], VK_NULL_HANDLE, &m_ImageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		// OUT_OF_DATE does NOT signal the semaphore, so nothing is stranded.
		recreateSwapchain();
		return nullptr;   // no counter advances; the same slot is retried
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		LOG_ENGINE_ERROR("vkAcquireNextImageKHR failed ({})", static_cast<int>(result));
		return nullptr;
	}
	// SUBOPTIMAL_KHR DOES signal the semaphore; proceed and let present() handle it.

	// 4. Per-frame swapchain state. Acquired contents are undefined, which is what
	//    lets the first barrier of the frame discard rather than preserve.
	m_SwapchainImageLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
	m_SwapchainImageWritten = false;
	m_RenderingBlockOpen    = false;

	// 5. Open the command buffer. NO rendering block is opened here.
	vkResetCommandBuffer(m_CommandBuffers[m_CurrentFrame], 0);
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = 0;
	beginInfo.pInheritanceInfo = nullptr;

	if (vkBeginCommandBuffer(m_CommandBuffers[m_CurrentFrame], &beginInfo) != VK_SUCCESS) {
		// The acquire semaphore is already signalled and nothing will wait on it;
		// there is no legal way to un-signal it. Only OOM or DEVICE_LOST get here.
		LOG_ENGINE_CRITICAL("vkBeginCommandBuffer failed; acquire semaphore stranded");
		throw std::runtime_error("Failed to begin recording command buffer");
	}

	m_Frame.m_Context = this;
	m_Frame.m_Cmd     = m_CommandBuffers[m_CurrentFrame];
	m_Frame.m_Index   = m_CurrentFrame;
	m_Frame.m_Number  = m_FrameNumber;
	m_FrameActive     = true;
	return &m_Frame;
}

void VulkanContext::endFrame() {
	assert(m_FrameActive && "endFrame() without a successful beginFrame()");
	assert(!m_RenderingBlockOpen && "a rendering block was left open across endFrame()");

	VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

	// --- Nothing-was-written fallback. -------------------------------------
	// Reachable in the runtime whenever Renderer::Render returned nullptr (no
	// camera), and in the editor if ImGui rendering is ever skipped. Presenting
	// an image in VK_IMAGE_LAYOUT_UNDEFINED is invalid.
	//
	// An empty LOAD_OP_CLEAR rendering block, NOT vkCmdClearColorImage: the block
	// needs only VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, which vk-bootstrap always
	// requests, whereas the clear would make TRANSFER_DST a hard requirement for
	// the editor too -- on exactly the platform (MoltenVK) most likely to refuse
	// it.
	if (!m_SwapchainImageWritten) {
		beginSwapchainRendering();
		endSwapchainRendering();
	}

	// --- Present transition. The runtime path already did this in the blit. ---
	if (m_SwapchainImageLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
		transitionSwapchainImage(cmd,
			m_SwapchainImageLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          0);
		m_SwapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		LOG_ENGINE_CRITICAL("vkEndCommandBuffer failed");
		throw std::runtime_error("Failed to record command buffer");
	}

	VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[m_CurrentFrame] };

	// ONE mask covering BOTH paths, because endFrame is shared and cannot know
	// whether the editor (COLOR_ATTACHMENT_OUTPUT) or the runtime (TRANSFER) ran.
	// pWaitDstStageMask defines the second synchronization scope of the semaphore
	// wait: only the listed stages, and stages logically later, are blocked until
	// it signals. The old COLOR_ATTACHMENT_OUTPUT-only mask left the runtime's
	// vkCmdClearColorImage/vkCmdBlitImage free to run before the image was
	// available -- and, now that the layout transition is explicit rather than
	// implicit in a render pass, it covered the transition too.
	VkPipelineStageFlags waitStages[] = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
	};
	// renderFinished is indexed by IMAGE INDEX. See the member comment.
	VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[m_ImageIndex] };

	VkSubmitInfo submitInfo{};
	submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount   = 1;
	submitInfo.pWaitSemaphores      = waitSemaphores;
	submitInfo.pWaitDstStageMask    = waitStages;
	submitInfo.commandBufferCount   = 1;
	submitInfo.pCommandBuffers      = &cmd;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores    = signalSemaphores;

	// FENCE-LEAK FIX: reset immediately before the submit with nothing between
	// that can fail. The old code reset in beginFrame and could then return false
	// from vkBeginCommandBuffer, leaving the fence unsignalled with nothing
	// pending -- the next vkWaitForFences blocked forever on UINT64_MAX.
	vkResetFences(m_Device, 1, &m_InFlightFences[m_CurrentFrame]);
	if (vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, m_InFlightFences[m_CurrentFrame]) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("vkQueueSubmit failed");
		// The fence was reset with nothing pending; re-signal it so the next
		// beginFrame does not block forever.
		vkQueueSubmit(m_GraphicsQueue, 0, nullptr, m_InFlightFences[m_CurrentFrame]);
	}

	m_FrameActive = false;
}

void VulkanContext::present() {
	VkSemaphore    waitSemaphores[] = { m_RenderFinishedSemaphores[m_ImageIndex] };
	VkSwapchainKHR swapchains[]     = { m_Swapchain };

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores    = waitSemaphores;
	presentInfo.swapchainCount     = 1;
	presentInfo.pSwapchains        = swapchains;
	presentInfo.pImageIndices      = &m_ImageIndex;
	presentInfo.pResults           = nullptr;

	VkResult result = vkQueuePresentKHR(m_PresentQueue, &presentInfo);

	// Advance BOTH counters together, BEFORE any recreation, so they stay
	// consistent on either path.
	m_CurrentFrame = (m_CurrentFrame + 1) % FRAMES_IN_FLIGHT;
	++m_FrameNumber;

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
		recreateSwapchain();   // does vkDeviceWaitIdle; all fences end up signalled
	} else if (result != VK_SUCCESS) {
		LOG_ENGINE_ERROR("vkQueuePresentKHR failed ({})", static_cast<int>(result));
	}
}

// =============================================================================
// DYNAMIC RENDERING
// =============================================================================

void VulkanContext::transitionSwapchainImage(VkCommandBuffer cmd,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
        VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
	VkImageMemoryBarrier b{};
	b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout           = oldLayout;
	b.newLayout           = newLayout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image               = m_SwapchainImages[m_ImageIndex];
	b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	b.srcAccessMask       = srcAccess;
	b.dstAccessMask       = dstAccess;
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanContext::beginSwapchainRendering(VkClearColorValue clear) {
	assert(m_FrameActive && "beginSwapchainRendering() outside a frame");
	assert(!m_RenderingBlockOpen && "beginSwapchainRendering() without endSwapchainRendering()");

	VkCommandBuffer cmd = m_CommandBuffers[m_CurrentFrame];

	// No render pass => no automatic layout transition. This barrier is mandatory.
	// srcStage MUST be COLOR_ATTACHMENT_OUTPUT, not TOP_OF_PIPE: the acquire
	// semaphore is waited at that stage and the transition must be ordered after
	// the wait. A TOP_OF_PIPE source scope is empty and orders nothing.
	transitionSwapchainImage(cmd,
		m_SwapchainImageLayout,                          // UNDEFINED on first write this frame
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,   0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	m_SwapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkRenderingAttachmentInfo color{};
	color.sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	color.imageView        = m_SwapchainImageViews[m_ImageIndex];
	color.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.resolveMode      = VK_RESOLVE_MODE_NONE;
	// CLEAR, never LOAD. LOAD_OP_LOAD reads the attachment with
	// COLOR_ATTACHMENT_READ at COLOR_ATTACHMENT_OUTPUT, which the barrier above
	// does not grant -- the old overlay pass had exactly that read-after-write
	// hazard on every editor frame. Here it is eliminated by construction: one
	// block per frame in the editor, none in the runtime, nothing loads.
	color.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
	color.clearValue.color = clear;

	VkRenderingInfo info{};
	info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
	info.renderArea.offset    = { 0, 0 };
	info.renderArea.extent    = m_SwapchainExtent;
	info.layerCount           = 1;
	info.viewMask             = 0;   // no multiview
	info.colorAttachmentCount = 1;
	info.pColorAttachments    = &color;
	info.pDepthAttachment     = nullptr;
	info.pStencilAttachment   = nullptr;
	// NOT suspending/resuming: MoltenVK maps each vkCmdBeginRendering onto a fresh
	// MTLRenderCommandEncoder and suspend/resume is the weakest-supported corner
	// of the feature there. One block per frame, flags = 0.
	info.flags                = 0;

	vkCmdBeginRendering(cmd, &info);
	m_RenderingBlockOpen    = true;
	m_SwapchainImageWritten = true;
}

void VulkanContext::endSwapchainRendering() {
	assert(m_RenderingBlockOpen && "endSwapchainRendering() without beginSwapchainRendering()");
	vkCmdEndRendering(m_CommandBuffers[m_CurrentFrame]);
	m_RenderingBlockOpen = false;
}

// =============================================================================
// STAGING / DEFERRED DESTRUCTION / SHARED RESOURCES
// =============================================================================

StagingAlloc VulkanContext::stage(const FrameContext& frame, VkDeviceSize size, VkDeviceSize alignment) {
	return m_Staging.allocate(frame, size, alignment);
}

void VulkanContext::deferDestroy(VkBuffer buffer, VmaAllocation allocation) {
	if (buffer == VK_NULL_HANDLE && allocation == VK_NULL_HANDLE)
		return;   // move-assignment onto an empty object is normal, not an error
	PendingDelete d;
	d.retireFrame = m_FrameNumber;
	d.buffer      = buffer;
	d.allocation  = allocation;
	m_DeletionQueue.push_back(std::move(d));
}

void VulkanContext::deferDestroy(VkImage image, VmaAllocation allocation, VkImageView view) {
	if (image == VK_NULL_HANDLE && allocation == VK_NULL_HANDLE && view == VK_NULL_HANDLE)
		return;
	PendingDelete d;
	d.retireFrame = m_FrameNumber;
	d.image       = image;
	d.allocation  = allocation;
	d.view        = view;
	m_DeletionQueue.push_back(std::move(d));
}

void VulkanContext::deferFreeDescriptorSets(std::span<const VkDescriptorSet> sets) {
	if (sets.empty())
		return;
	PendingDelete d;
	d.retireFrame = m_FrameNumber;
	d.sets.assign(sets.begin(), sets.end());   // COPIED; the caller's storage does not outlive the call
	m_DeletionQueue.push_back(std::move(d));
}

// Destroys one entry. Order within an entry is view, then image/buffer.
static void destroyPending(VkDevice device, VmaAllocator allocator, VkDescriptorPool pool,
                           VkImageView view, VkImage image, VkBuffer buffer, VmaAllocation allocation,
                           std::vector<VkDescriptorSet>& sets) {
	if (view != VK_NULL_HANDLE)
		vkDestroyImageView(device, view, nullptr);
	if (image != VK_NULL_HANDLE)
		vmaDestroyImage(allocator, image, allocation);
	else if (buffer != VK_NULL_HANDLE)
		vmaDestroyBuffer(allocator, buffer, allocation);
	else if (allocation != VK_NULL_HANDLE)
		vmaFreeMemory(allocator, allocation);
	if (!sets.empty())
		vkFreeDescriptorSets(device, pool, static_cast<uint32_t>(sets.size()), sets.data());
}

void VulkanContext::drainDeletionQueue() {
	if (m_DeletionQueue.empty())
		return;

	auto it = std::remove_if(m_DeletionQueue.begin(), m_DeletionQueue.end(),
		[&](PendingDelete& d) {
			if (d.retireFrame > m_CompletedFrame)
				return false;
			destroyPending(m_Device, m_Allocator, m_DescriptorPool,
			               d.view, d.image, d.buffer, d.allocation, d.sets);
			return true;
		});
	m_DeletionQueue.erase(it, m_DeletionQueue.end());
}

void VulkanContext::drainDeletionQueueFully() {
	if (m_Device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_Device);
	for (auto& d : m_DeletionQueue) {
		destroyPending(m_Device, m_Allocator, m_DescriptorPool,
		               d.view, d.image, d.buffer, d.allocation, d.sets);
	}
	m_DeletionQueue.clear();
}

// THIS LOOKUP IS THE WHOLE CONTRACT: the returned VkSampler is SHARED, and every
// distinction that matters to a caller must be a field of SamplerDesc, hashed in
// std::hash<SamplerDesc> AND compared by its operator==. When compareEnable,
// compareOp and borderColor were hardcoded below instead of being desc fields,
// a caller that needed a shadow-comparison sampler had no way to express it and
// would have silently received the colour-texture sampler that happened to share
// its filter/address/mipmap/maxLod -- a valid handle, clean validation, and
// undefined values out of the shadow fetch. That failure class is closed only for
// as long as new sampler state arrives as a keyed desc field; adding a parameter
// here without adding it to the key reopens it exactly.
VkSampler VulkanContext::getSampler(const SamplerDesc& desc) {
	if (auto it = m_Samplers.find(desc); it != m_Samplers.end())
		return it->second;

	VkSamplerCreateInfo info{};
	info.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter        = desc.filter;
	info.minFilter        = desc.filter;
	info.mipmapMode       = desc.mipmapMode;
	info.addressModeU     = desc.addressMode;
	info.addressModeV     = desc.addressMode;
	info.addressModeW     = desc.addressMode;
	info.mipLodBias       = 0.0f;
	info.anisotropyEnable = VK_FALSE;
	info.maxAnisotropy    = 1.0f;

	// Comparison sampling: with this enabled the filter unit tests each texel
	// against the shader's reference value and bilerps the 0/1 RESULTS, so the
	// fetch returns an occlusion fraction instead of a meaningless blend of
	// depths -- hardware PCF, for free. See SamplerDesc in VulkanTypes.h for why
	// a shadow map under this engine's REVERSE-Z projection wants
	// VK_COMPARE_OP_GREATER (near = 1, far = 0, depth test GREATER, depth clears
	// to 0), not the LESS that published shadow-map code assumes, and why the
	// border that reads as "fully lit" outside a cascade is consequently
	// FLOAT_OPAQUE_BLACK rather than the usual FLOAT_OPAQUE_WHITE.
	//
	// compareOp is forwarded unconditionally. Vulkan ignores it while
	// compareEnable is VK_FALSE but still requires a valid enum, and forwarding
	// it unconditionally keeps this function a pure function of the desc -- the
	// same property the hash relies on. It replaces a hardcoded
	// VK_COMPARE_OP_ALWAYS, which was never observable: ALWAYS with comparison
	// off is the same disabled sampler as LESS with comparison off.
	info.compareEnable    = desc.compareEnable ? VK_TRUE : VK_FALSE;
	info.compareOp        = desc.compareOp;
	info.minLod           = 0.0f;
	info.maxLod           = desc.maxLod;

	// borderColor is only ever consulted under an ADDRESS_MODE_CLAMP_TO_BORDER,
	// which is why the previously hardcoded INT_OPAQUE_BLACK was harmless for
	// the REPEAT-addressed colour samplers that are the only callers today. It
	// stops being harmless the moment a shadow cascade asks for CLAMP_TO_BORDER:
	// sampling a float-format image (kDepthFormat is VK_FORMAT_D32_SFLOAT)
	// through an INT_ border colour yields UNDEFINED values, so a border-clamped
	// depth sampler MUST pass a FLOAT_ variant. The desc default stays
	// INT_OPAQUE_BLACK so no already-cached sampler changes behaviour.
	info.borderColor      = desc.borderColor;
	info.unnormalizedCoordinates = VK_FALSE;

	VkSampler sampler = VK_NULL_HANDLE;
	X3_VK_CHECK(vkCreateSampler(m_Device, &info, nullptr, &sampler));
	m_Samplers.emplace(desc, sampler);
	return sampler;
}

// =============================================================================
// THE ONE SURVIVING BLOCKING HELPER
// =============================================================================

VkCommandBuffer VulkanContext::beginSingleTimeCommands() {
	// THIS ASSERT IS ADJUDICATION.md's GATE, not a grep for wait-idle sites. The
	// corrected wait-idle ruling permits a blocking upload only out of frame --
	// ImGui fonts and the dummy resources, i.e. initialisation and teardown -- and
	// this is what makes "none of the permitted wait sites is reachable from a
	// frame" checkable. It was a warn-once while the legacy VulkanImage2D /
	// VulkanTexture2D upload paths still called it from inside
	// Renderer::SetupGPUResources; those classes are deleted and every in-frame
	// upload now goes through stage() + frame.cmd().
	assert(!m_FrameActive && "blocking upload inside a frame -- use ctx.stage() + frame.cmd()");

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = m_CommandPool;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	X3_VK_CHECK(vkAllocateCommandBuffers(m_Device, &allocInfo, &cmd));

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	X3_VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
	return cmd;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer cmd) {
	X3_VK_CHECK(vkEndCommandBuffer(cmd));

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;

	X3_VK_CHECK(vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
	X3_VK_CHECK(vkQueueWaitIdle(m_GraphicsQueue));

	vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &cmd);
}

void VulkanContext::readbackImage(VulkanImage& src, uint32_t& outWidth, uint32_t& outHeight,
                                  std::vector<float>& outRgba) {
	assert(!m_FrameActive &&
	       "readbackImage blocks and must not run inside a frame -- render the "
	       "frames first, then read");
	assert(src.valid() && "readbackImage on an unallocated image");
	assert(src.format() == VK_FORMAT_R32G32B32A32_SFLOAT &&
	       "readbackImage assumes the engine's RGBA32F render target format");

	const VkExtent2D extent = src.extent();
	outWidth  = extent.width;
	outHeight = extent.height;

	constexpr VkDeviceSize kBytesPerTexel = 4 * sizeof(float);
	const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * kBytesPerTexel;

	// A throwaway host-visible staging buffer. Not VulkanBuffer: that one is
	// device-local and has no map(), by design -- adding a host-visible mode to
	// it for a tool would widen an API the whole renderer depends on.
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size        = bytes;
	bufferInfo.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
	                | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VkBuffer          staging     = VK_NULL_HANDLE;
	VmaAllocation     stagingAlloc = VK_NULL_HANDLE;
	VmaAllocationInfo stagingInfo{};
	X3_VK_CHECK(vmaCreateBuffer(m_Allocator, &bufferInfo, &allocInfo,
	                            &staging, &stagingAlloc, &stagingInfo));

	// THE ORIGINAL LAYOUT IS RESTORED. VulkanImage tracks m_Layout and the next
	// frame derives its barrier from it, so leaving the image in
	// TRANSFER_SRC_OPTIMAL here would make every later transition start from a
	// lie.
	const VkImageLayout originalLayout = src.layout();

	VkCommandBuffer cmd = beginSingleTimeCommands();

	auto recordBarrier = [&](VkImageLayout from, VkImageLayout to,
	                         VkAccessFlags srcAccess, VkAccessFlags dstAccess,
	                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
		VkImageMemoryBarrier barrier{};
		barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout           = from;
		barrier.newLayout           = to;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image               = src.handle();
		barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		barrier.srcAccessMask       = srcAccess;
		barrier.dstAccessMask       = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	};

	recordBarrier(originalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	              VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkBufferImageCopy region{};
	region.bufferOffset      = 0;
	region.bufferRowLength   = 0;   // tightly packed
	region.bufferImageHeight = 0;
	region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageOffset       = { 0, 0, 0 };
	region.imageExtent       = { extent.width, extent.height, 1 };
	vkCmdCopyImageToBuffer(cmd, src.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       staging, 1, &region);

	recordBarrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, originalLayout,
	              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

	endSingleTimeCommands(cmd);   // submits AND waits, so the map below is safe

	X3_VK_CHECK(vmaInvalidateAllocation(m_Allocator, stagingAlloc, 0, bytes));

	outRgba.resize(size_t(extent.width) * extent.height * 4);
	std::memcpy(outRgba.data(), stagingInfo.pMappedData, bytes);

	vmaDestroyBuffer(m_Allocator, staging, stagingAlloc);
}

// =============================================================================
// SWAPCHAIN
// =============================================================================

void VulkanContext::setVSync(bool enabled) {
	if (enabled == m_VSync)
		return;
	m_VSync = enabled;
	if (m_FrameActive) {
		// An ImGui checkbox dispatches this while the frame command buffer is
		// recording. Destroying the swapchain there would be catastrophic.
		m_PresentModeDirty = true;
		return;
	}
	recreateSwapchain();
}

void VulkanContext::recreateSwapchain() {
	vkDeviceWaitIdle(m_Device);

	// Check if the window is minimized
	int width = 0, height = 0;
	glfwGetFramebufferSize(m_NativeWindow, &width, &height);
	while (width == 0 || height == 0) {
		glfwGetFramebufferSize(m_NativeWindow, &width, &height);
		glfwWaitEvents();
	}

	LOG_ENGINE_INFO("Recreating swapchain (new size: {}x{})", width, height);

	// Keep the old swapchain alive across creation so the driver can retire it
	// incrementally instead of tearing everything down. The old code called
	// cleanupSwapchain() first, which nulled m_Swapchain, so set_old_swapchain()
	// was ALWAYS VK_NULL_HANDLE and every resize was a full teardown with a
	// visible black flash.
	VkSwapchainKHR           oldSwapchain = m_Swapchain;
	std::vector<VkImageView> oldViews     = std::move(m_SwapchainImageViews);
	m_SwapchainImageViews.clear();

	createSwapchain(oldSwapchain);

	for (auto view : oldViews)
		vkDestroyImageView(m_Device, view, nullptr);
	if (oldSwapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(m_Device, oldSwapchain, nullptr);

	// Grow only; never destroy. See ensureRenderFinishedSemaphores().
	ensureRenderFinishedSemaphores();

	// Set flag for ImGui to detect and update its resources
	m_SwapchainRecreated = true;

	LOG_ENGINE_INFO("Swapchain recreation complete");
}

void VulkanContext::cleanupSwapchain() {
	// No framebuffers to destroy: dynamic rendering has none.
	for (auto imageView : m_SwapchainImageViews) {
		vkDestroyImageView(m_Device, imageView, nullptr);
	}
	m_SwapchainImageViews.clear();

	if (m_Swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
		m_Swapchain = VK_NULL_HANDLE;
	}

	m_SwapchainImages.clear();
}

void VulkanContext::cleanup() {
	if (m_Device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(m_Device);

		// The dummies route their handles through deferDestroy(), so they must be
		// released BEFORE the queue is drained.
		m_DummyTexture.reset();
		m_DummyStorageBuffer.reset();
		m_DummyUniformBuffer.reset();
		drainDeletionQueueFully();

		m_Staging.destroy();

		for (auto& [desc, sampler] : m_Samplers) {
			vkDestroySampler(m_Device, sampler, nullptr);
		}
		m_Samplers.clear();

		// SWAPCHAIN BEFORE SEMAPHORES. vkDeviceWaitIdle waits for QUEUE
		// operations; vkQueuePresentKHR hands its semaphore wait to the
		// presentation engine, which is not a queue operation and is explicitly
		// not covered by device-idle (VUID-vkDestroySemaphore-semaphore-01137).
		// Destroying the swapchain first is what closes that window at shutdown.
		cleanupSwapchain();

		for (auto semaphore : m_ImageAvailableSemaphores) {
			if (semaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(m_Device, semaphore, nullptr);
		}
		for (auto semaphore : m_RenderFinishedSemaphores) {
			if (semaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(m_Device, semaphore, nullptr);
		}
		m_ImageAvailableSemaphores.clear();
		m_RenderFinishedSemaphores.clear();

		for (auto fence : m_InFlightFences) {
			if (fence != VK_NULL_HANDLE)
				vkDestroyFence(m_Device, fence, nullptr);
		}
		m_InFlightFences.clear();

		if (m_CommandPool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
			m_CommandPool = VK_NULL_HANDLE;
		}

		if (m_DescriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
			m_DescriptorPool = VK_NULL_HANDLE;
		}

		if (m_Allocator != VK_NULL_HANDLE) {
			vmaDestroyAllocator(m_Allocator);
			m_Allocator = VK_NULL_HANDLE;
		}

		vkDestroyDevice(m_Device, nullptr);
		m_Device = VK_NULL_HANDLE;
	}

	if (m_Surface != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
		m_Surface = VK_NULL_HANDLE;
	}

	if (m_DebugMessenger != VK_NULL_HANDLE) {
		auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
			m_Instance, "vkDestroyDebugUtilsMessengerEXT");
		if (func != nullptr) {
			func(m_Instance, m_DebugMessenger, nullptr);
		}
		m_DebugMessenger = VK_NULL_HANDLE;
	}

	if (m_Instance != VK_NULL_HANDLE) {
		vkDestroyInstance(m_Instance, nullptr);
		m_Instance = VK_NULL_HANDLE;
	}
}

// =============================================================================
// BOUND-RESOURCE REGISTRY (deleted with VulkanComputeShader in Part 3)
// =============================================================================

// =============================================================================
// PRESENTATION
// =============================================================================

void VulkanContext::blitImageToSwapchain(const FrameContext& frame, VulkanImage& src,
                                         glm::ivec4 viewport, glm::ivec2 windowSize) {
	assert(m_FrameActive && "blitImageToSwapchain() outside a frame");
	assert(&frame.context() == this && "blitting with a frame from a different context");
	assert(!m_RenderingBlockOpen &&
	       "vkCmdBlitImage/vkCmdClearColorImage are transfer commands and must be "
	       "recorded outside a rendering block");
	assert(src.valid() && "blitImageToSwapchain() with an unallocated source image");

	VkCommandBuffer cmd = frame.cmd();

	// The source's layout is TRACKED, not passed in. That is the whole point of
	// the adjudicated signature: this function takes the image through
	// GENERAL -> TRANSFER_SRC_OPTIMAL -> GENERAL every runtime frame, and the old
	// (VkImage, VkImageLayout) form left the caller to remember that. A descriptor
	// written from the middle of the round trip then claimed GENERAL for an image
	// in TRANSFER_SRC_OPTIMAL -- VUID-VkWriteDescriptorSet-descriptorType-04152.
	src.transition(frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	// Transition swapchain image to TRANSFER_DST_OPTIMAL.
	// oldLayout = UNDEFINED is correct and stays: acquired contents are undefined
	// and discarding is what we want. srcStage was TOP_OF_PIPE, which orders
	// NOTHING; it must name a stage present in the submit's pWaitDstStageMask, so
	// it is TRANSFER.
	transitionSwapchainImage(cmd,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

	m_SwapchainImageLayout  = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	m_SwapchainImageWritten = true;

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

	const VkExtent2D srcExtent = src.extent();

	// Blit the source image to the viewport region of the swapchain
	VkImageBlit blitRegion{};
	blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.srcSubresource.mipLevel = 0;
	blitRegion.srcSubresource.baseArrayLayer = 0;
	blitRegion.srcSubresource.layerCount = 1;
	blitRegion.srcOffsets[0] = {0, 0, 0};
	blitRegion.srcOffsets[1] = {static_cast<int32_t>(srcExtent.width),
	                            static_cast<int32_t>(srcExtent.height), 1};

	blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.dstSubresource.mipLevel = 0;
	blitRegion.dstSubresource.baseArrayLayer = 0;
	blitRegion.dstSubresource.layerCount = 1;
	// viewport: x, y, x+width, y+height (matching OpenGL glBlitFramebuffer convention)
	// DO NOT CHANGE THE Y-FLIP. These two lines and
	// RuntimeLayer::CalculateViewportCoordinates() are a matched pair: the
	// viewport arrives with a bottom-left origin and the subtraction converts it
	// to Vulkan's top-left origin. Changing either half in isolation flips the
	// runtime image.
	blitRegion.dstOffsets[0] = {viewport.x, windowSize.y - viewport.w, 0}; // Flip Y
	blitRegion.dstOffsets[1] = {viewport.z, windowSize.y - viewport.y, 1}; // Flip Y

	vkCmdBlitImage(cmd,
		src.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		m_SwapchainImages[m_ImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blitRegion, VK_FILTER_LINEAR);

	// Transition swapchain image to PRESENT_SRC. Under render passes this
	// duplicated finalLayout; now it is the only thing producing PRESENT_SRC_KHR
	// on the runtime path, and endFrame()'s transition correctly skips because
	// the tracked layout already matches.
	transitionSwapchainImage(cmd,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);

	m_SwapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	// Back to GENERAL for the next frame's compute shader, through the tracked
	// transition so the image's own layout state stays true.
	src.transition(frame, VK_IMAGE_LAYOUT_GENERAL,
	               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

}
