#include "Platform/Vulkan/VulkanDescriptors.h"

#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanImage.h"

#include <algorithm>

namespace X3
{

namespace
{
	// The two families DescriptorWriter keeps separate info vectors for. Anything
	// not in either is not writable through this class -- there is no
	// VkBufferView path (texel buffers) and no acceleration structure path, and
	// adding one means adding a third vector, not widening one of these.
	bool isImageDescriptor(VkDescriptorType type)
	{
		switch (type) {
			case VK_DESCRIPTOR_TYPE_SAMPLER:
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
				return true;
			default:
				return false;
		}
	}

	bool isBufferDescriptor(VkDescriptorType type)
	{
		switch (type) {
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
				return true;
			default:
				return false;
		}
	}
}

// =============================================================================
// VulkanDescriptorSetLayout
// =============================================================================

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanContext& ctx,
                                                     std::span<const DescriptorBindingDesc> bindings)
	: m_Ctx(&ctx)
	, m_Bindings(bindings.begin(), bindings.end())
{
	std::vector<VkDescriptorSetLayoutBinding> vkBindings;
	vkBindings.reserve(m_Bindings.size());
	for (const DescriptorBindingDesc& b : m_Bindings) {
		assert(b.type != VK_DESCRIPTOR_TYPE_MAX_ENUM && "binding declared with no descriptor type");
		assert(b.count > 0 && "binding declared with count 0");
		assert((isImageDescriptor(b.type) || isBufferDescriptor(b.type)) &&
		       "descriptor type is neither image nor buffer -- DescriptorWriter cannot write it");

		VkDescriptorSetLayoutBinding lb{};
		lb.binding            = b.binding;
		lb.descriptorType     = b.type;
		lb.descriptorCount    = b.count;
		lb.stageFlags         = b.stages;
		lb.pImmutableSamplers = nullptr;
		vkBindings.push_back(lb);
	}

	VkDescriptorSetLayoutCreateInfo ci{};
	ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	ci.bindingCount = static_cast<uint32_t>(vkBindings.size());
	ci.pBindings    = vkBindings.empty() ? nullptr : vkBindings.data();
	// No UPDATE_AFTER_BIND and no UPDATE_UNUSED_WHILE_PENDING, deliberately. Those
	// flags would make VUID-vkUpdateDescriptorSets-None-03047 unreportable rather
	// than unreachable; the per-frame ring makes it unreachable, which is the fix
	// that also stops frame N-1's dispatch reading frame N's data.
	ci.flags        = 0;

	X3_VK_CHECK(vkCreateDescriptorSetLayout(m_Ctx->getDevice(), &ci, nullptr, &m_Layout));
}

VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout()
{
	// INLINE, not deferred -- see the ownership note in the header. A layout is
	// only ever destroyed with its owning pipeline, in Renderer::Shutdown(), after
	// vkDeviceWaitIdle.
	if (m_Ctx && m_Layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_Ctx->getDevice(), m_Layout, nullptr);
}

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanDescriptorSetLayout&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Layout(other.m_Layout)
	, m_Bindings(std::move(other.m_Bindings))
{
	other.m_Ctx    = nullptr;
	other.m_Layout = VK_NULL_HANDLE;
	other.m_Bindings.clear();
}

VulkanDescriptorSetLayout& VulkanDescriptorSetLayout::operator=(VulkanDescriptorSetLayout&& other) noexcept
{
	if (this == &other)
		return *this;

	assert((m_Ctx == nullptr || other.m_Ctx == nullptr || m_Ctx == other.m_Ctx) &&
	       "cross-context move of a descriptor set layout");

	if (m_Ctx && m_Layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_Ctx->getDevice(), m_Layout, nullptr);

	m_Ctx      = other.m_Ctx;
	m_Layout   = other.m_Layout;
	m_Bindings = std::move(other.m_Bindings);

	other.m_Ctx    = nullptr;
	other.m_Layout = VK_NULL_HANDLE;
	other.m_Bindings.clear();
	return *this;
}

const DescriptorBindingDesc* VulkanDescriptorSetLayout::find(uint32_t binding) const
{
	auto it = std::find_if(m_Bindings.begin(), m_Bindings.end(),
	                       [binding](const DescriptorBindingDesc& b) { return b.binding == binding; });
	return it == m_Bindings.end() ? nullptr : &*it;
}

uint32_t VulkanDescriptorSetLayout::imageDescriptorCount() const
{
	uint32_t n = 0;
	for (const DescriptorBindingDesc& b : m_Bindings)
		if (isImageDescriptor(b.type)) n += b.count;
	return n;
}

uint32_t VulkanDescriptorSetLayout::bufferDescriptorCount() const
{
	uint32_t n = 0;
	for (const DescriptorBindingDesc& b : m_Bindings)
		if (isBufferDescriptor(b.type)) n += b.count;
	return n;
}

// =============================================================================
// VulkanDescriptorSetRing
// =============================================================================

VulkanDescriptorSetRing::VulkanDescriptorSetRing(VulkanContext& ctx,
                                                 const VulkanDescriptorSetLayout& layout)
	: m_Ctx(&ctx)
	, m_Layout(&layout)
{
	assert(layout.valid() && "descriptor set ring allocated from an invalid layout");

	// ONE vkAllocateDescriptorSets with the same layout handle repeated
	// FRAMES_IN_FLIGHT times. Allocated once, here, never lazily inside dispatch.
	std::array<VkDescriptorSetLayout, FRAMES_IN_FLIGHT> layouts{};
	layouts.fill(layout.handle());

	VkDescriptorSetAllocateInfo ai{};
	ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	ai.descriptorPool     = m_Ctx->getDescriptorPool();
	ai.descriptorSetCount = FRAMES_IN_FLIGHT;
	ai.pSetLayouts        = layouts.data();

	X3_VK_CHECK(vkAllocateDescriptorSets(m_Ctx->getDevice(), &ai, m_Sets.data()));
}

VulkanDescriptorSetRing::~VulkanDescriptorSetRing()
{
	// DEFERRED, never vkFreeDescriptorSets directly: a pending command buffer may
	// still have one of these bound. This is what makes
	// VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT on the shared pool
	// load-bearing rather than incidental.
	if (m_Ctx && m_Sets[0] != VK_NULL_HANDLE)
		m_Ctx->deferFreeDescriptorSets(m_Sets);
}

VulkanDescriptorSetRing::VulkanDescriptorSetRing(VulkanDescriptorSetRing&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Layout(other.m_Layout)
	, m_Sets(other.m_Sets)
{
	other.m_Ctx    = nullptr;
	other.m_Layout = nullptr;
	other.m_Sets.fill(VK_NULL_HANDLE);
}

VulkanDescriptorSetRing& VulkanDescriptorSetRing::operator=(VulkanDescriptorSetRing&& other) noexcept
{
	if (this == &other)
		return *this;

	if (m_Ctx && m_Sets[0] != VK_NULL_HANDLE)
		m_Ctx->deferFreeDescriptorSets(m_Sets);

	assert((m_Ctx == nullptr || other.m_Ctx == nullptr || m_Ctx == other.m_Ctx) &&
	       "cross-context move: the deferred free would go on the wrong queue");

	m_Ctx    = other.m_Ctx;
	m_Layout = other.m_Layout;
	m_Sets   = other.m_Sets;

	other.m_Ctx    = nullptr;
	other.m_Layout = nullptr;
	other.m_Sets.fill(VK_NULL_HANDLE);
	return *this;
}

// =============================================================================
// DescriptorWriter
// =============================================================================

DescriptorWriter::DescriptorWriter(VulkanContext& ctx,
                                   const VulkanDescriptorSetRing& ring,
                                   const FrameContext& frame)
	// Defined in terms of the canonical form, with both arguments read out of the
	// one object that allocated the second from the first -- which is the whole
	// point of this overload existing.
	: DescriptorWriter(ctx, ring.layout(), ring.get(frame))
{
	assert(&frame.context() == &ctx &&
	       "writing a descriptor set with a frame from a different context");
}

DescriptorWriter::DescriptorWriter(VulkanContext& ctx,
                                   const VulkanDescriptorSetLayout& layout,
                                   VkDescriptorSet dst)
	: m_Ctx(&ctx)
	, m_Device(ctx.getDevice())
	, m_Layout(&layout)
	, m_Dst(dst)
{
	assert(dst != VK_NULL_HANDLE && "DescriptorWriter with a null destination set");

	// THE CONTIGUOUS-INFO-BLOCK INVARIANT, clause 1: reserve exactly what the
	// layout declares and never grow. Every append asserts against capacity, so a
	// write the layout does not declare fails at the append rather than
	// reallocating and dangling every pointer flush() is about to resolve.
	m_BufferInfos.reserve(layout.bufferDescriptorCount());
	m_ImageInfos.reserve(layout.imageDescriptorCount());
	m_Pending.reserve(layout.bindings().size());
}

// -----------------------------------------------------------------------------
// Appends. Each validates the binding against the layout BEFORE touching the
// info vectors, so a mismatched write cannot half-apply.
// -----------------------------------------------------------------------------

DescriptorWriter& DescriptorWriter::raw(uint32_t binding, VkDescriptorType type,
                                        const VkDescriptorBufferInfo& info)
{
	assert(!m_Flushed && "DescriptorWriter written after flush()");
	assert(isBufferDescriptor(type) && "raw() buffer overload used with an image descriptor type");

	const DescriptorBindingDesc* decl = m_Layout->find(binding);
	assert(decl && "writing a binding the layout does not declare");
	assert(decl->type == type && "descriptor type does not match the layout's declaration");
	assert(decl->count == 1 && "single-descriptor write to an array binding");
	assert(info.buffer != VK_NULL_HANDLE && "descriptor written with a null buffer");
	assert(m_BufferInfos.size() < m_BufferInfos.capacity() &&
	       "buffer info capacity exceeded -- a binding was written twice");

	const uint32_t base = static_cast<uint32_t>(m_BufferInfos.size());
	m_BufferInfos.push_back(info);
	m_Pending.push_back(PendingWrite{ binding, type, 1, base, false });
	return *this;
}

DescriptorWriter& DescriptorWriter::raw(uint32_t binding, VkDescriptorType type,
                                        const VkDescriptorImageInfo& info)
{
	assert(!m_Flushed && "DescriptorWriter written after flush()");
	assert(isImageDescriptor(type) && "raw() image overload used with a buffer descriptor type");

	const DescriptorBindingDesc* decl = m_Layout->find(binding);
	assert(decl && "writing a binding the layout does not declare");
	assert(decl->type == type && "descriptor type does not match the layout's declaration");
	assert(decl->count == 1 && "single-descriptor write to an array binding");
	assert(info.imageView != VK_NULL_HANDLE && "descriptor written with a null image view");
	assert(m_ImageInfos.size() < m_ImageInfos.capacity() &&
	       "image info capacity exceeded -- a binding was written twice");

	const uint32_t base = static_cast<uint32_t>(m_ImageInfos.size());
	m_ImageInfos.push_back(info);
	m_Pending.push_back(PendingWrite{ binding, type, 1, base, true });
	return *this;
}

DescriptorWriter& DescriptorWriter::uniformBuffer(uint32_t binding, const VulkanRingBuffer& buffer,
                                                  const FrameContext& frame)
{
	// descriptor(frame) aims at slot frame.index() with range = sizePerFrame(),
	// not VK_WHOLE_SIZE -- a whole-size range would let the shader read the other
	// frame's slot.
	return raw(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer.descriptor(frame));
}

DescriptorWriter& DescriptorWriter::uniformBuffer(uint32_t binding, const VulkanBuffer& buffer)
{
	return raw(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer.descriptor());
}

DescriptorWriter& DescriptorWriter::storageBuffer(uint32_t binding, const VulkanRingBuffer& buffer,
                                                  const FrameContext& frame)
{
	return raw(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer.descriptor(frame));
}

DescriptorWriter& DescriptorWriter::storageBuffer(uint32_t binding, const VulkanBuffer& buffer)
{
	return raw(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer.descriptor());
}

DescriptorWriter& DescriptorWriter::storageImage(uint32_t binding, const VulkanImage& image)
{
	// storageDescriptor() derives the layout from the image's tracked state and
	// asserts it is GENERAL. That assert is the call-site ordering check: if it
	// fires, the caller forgot a transition(frame, GENERAL, ...) before writing.
	return raw(binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, image.storageDescriptor());
}

DescriptorWriter& DescriptorWriter::sampledImage(uint32_t binding, const VulkanTexture& texture)
{
	return raw(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture.descriptor());
}

DescriptorWriter& DescriptorWriter::sampledImage(uint32_t binding, const VulkanImage& image,
                                                 VkSampler sampler, VkImageLayout layout)
{
	VkDescriptorImageInfo info{};
	info.sampler     = sampler;
	info.imageView   = image.view();
	info.imageLayout = layout;
	return raw(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info);
}

DescriptorWriter& DescriptorWriter::sampledImageArray(uint32_t binding,
                                                      std::span<const VulkanTexture* const> textures)
{
	assert(!m_Flushed && "DescriptorWriter written after flush()");

	const DescriptorBindingDesc* decl = m_Layout->find(binding);
	assert(decl && "writing a binding the layout does not declare");
	assert(decl->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
	       "sampledImageArray() on a binding that is not a combined image sampler");
	// EXACTLY the declared count. There is no PARTIALLY_BOUND on these layouts, so
	// an unwritten element is undefined behaviour on access, not merely unused.
	assert(textures.size() == decl->count &&
	       "sampledImageArray() size must equal the binding's declared count");
	assert(m_ImageInfos.size() + textures.size() <= m_ImageInfos.capacity() &&
	       "image info capacity exceeded -- a binding was written twice");

	// CLAUSE 2 OF THE CONTIGUOUS-INFO-BLOCK INVARIANT: one uninterrupted loop, so
	// [base, base + count) is contiguous by construction.
	const uint32_t base = static_cast<uint32_t>(m_ImageInfos.size());
	for (const VulkanTexture* t : textures) {
		// No holes: an absent or half-built texture becomes the dummy, which is
		// what the context's dummy resources exist for.
		m_ImageInfos.push_back((t && t->valid()) ? t->descriptor()
		                                         : m_Ctx->dummyTexture().descriptor());
	}

	m_Pending.push_back(PendingWrite{ binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	                                  static_cast<uint32_t>(textures.size()), base, true });
	return *this;
}

void DescriptorWriter::flush()
{
	assert(!m_Flushed && "flush() called twice -- the second call would issue an empty update");

#ifndef NDEBUG
	// COMPLETENESS: every binding the layout declares must have been written
	// exactly once, with the declared type and descriptorCount. Anything less and
	// the set keeps what it held FRAMES_IN_FLIGHT frames ago -- handles the
	// deletion queue may already have freed.
	for (const DescriptorBindingDesc& decl : m_Layout->bindings()) {
		int written = 0;
		for (const PendingWrite& w : m_Pending) {
			if (w.binding != decl.binding) continue;
			++written;
			assert(w.type == decl.type && "written descriptor type differs from the layout");
			assert(w.count == decl.count && "written descriptor count differs from the layout");
		}
		assert(written == 1 &&
		       "every binding in the layout must be written exactly once per frame -- "
		       "use ctx.dummyTexture()/dummyStorageBuffer()/dummyUniformBuffer() for absent resources");
	}
#endif

	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve(m_Pending.size());
	for (const PendingWrite& w : m_Pending) {
		VkWriteDescriptorSet write{};
		write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet          = m_Dst;
		write.dstBinding      = w.binding;
		write.dstArrayElement = 0;
		write.descriptorCount = w.count;
		write.descriptorType  = w.type;
		// CLAUSE 3: pointers resolved HERE, never captured at append time. Both
		// vectors are done growing by now, so data() is final.
		write.pImageInfo      = w.isImage ? m_ImageInfos.data()  + w.infoBase : nullptr;
		write.pBufferInfo     = w.isImage ? nullptr : m_BufferInfos.data() + w.infoBase;
		writes.push_back(write);
	}

	if (!writes.empty())
		vkUpdateDescriptorSets(m_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

	// LAST STATEMENT, success path only: this is what ~DescriptorWriter tests.
	m_Flushed = true;
}

}
