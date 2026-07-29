#include "Renderer/TextureTable.h"

#include "Platform/Vulkan/VulkanContext.h"
#include "Project/Assets/AssetManager.h"

namespace X3
{

	void TextureTable::init(VulkanContext& ctx) {
		m_Ctx = &ctx;
		m_Textures.clear();
		m_IndexOf.clear();
		m_Textures.reserve(MAX_MATERIAL_TEXTURES);

		// SLOT 0 IS THE DUMMY, and it is what makes "every element always
		// written" affordable. Without PARTIALLY_BOUND, reading an element that
		// was never written is undefined behaviour, not a validation error -- so
		// unused elements must point at something real.
		const uint32_t white = 0xFFFFFFFFu;
		TextureDesc desc;
		desc.width     = 1;
		desc.height    = 1;
		desc.format    = VK_FORMAT_R8G8B8A8_UNORM;
		desc.mipLevels = 1;
		desc.debugName = "MaterialTable::dummy";
		// The OUT-OF-FRAME constructor, which blocks. Legal here and only here:
		// init() runs from Renderer::Init, before any frame exists.
		m_Textures.emplace_back(ctx, desc, &white);

		rebuildBound();
	}

	void TextureTable::shutdown() {
		m_Textures.clear();
		m_IndexOf.clear();
		m_Bound.clear();
		m_Ctx = nullptr;
	}

	void TextureTable::invalidate() {
		if (m_Textures.size() > 1)
			m_Textures.resize(1);   // keep the dummy
		m_IndexOf.clear();
		rebuildBound();
	}

	void TextureTable::rebuildBound() {
		// EVERY element, every time -- sampledImageArray writes the whole binding
		// in one VkWriteDescriptorSet and requires exactly `count` descriptors.
		m_Bound.assign(MAX_MATERIAL_TEXTURES, nullptr);
		const VulkanTexture* dummy = m_Textures.empty() ? nullptr : &m_Textures[0];
		for (uint32_t i = 0; i < MAX_MATERIAL_TEXTURES; ++i)
			m_Bound[i] = (i < m_Textures.size() && m_Textures[i].valid()) ? &m_Textures[i] : dummy;
	}

	uint32_t TextureTable::getOrCreate(const FrameContext& frame, const AssetPool& assetPool,
	                                   LR_GUID guid) {
		if (guid == LR_GUID::INVALID)
			return Gpu::INVALID_TEXTURE;

		if (auto it = m_IndexOf.find(guid); it != m_IndexOf.end())
			return it->second;

		auto pixelsIt = assetPool.Textures.find(guid);
		if (pixelsIt == assetPool.Textures.end()) {
			LOG_ENGINE_WARN("TextureTable: material references texture GUID {} which has no pixels "
			                "in the asset pool; falling back to the scalar factor", (uint64_t)guid);
			return Gpu::INVALID_TEXTURE;
		}

		if (m_Textures.size() >= MAX_MATERIAL_TEXTURES) {
			LOG_ENGINE_WARN("TextureTable: full at {} entries; texture GUID {} not bound. "
			                "Raise MAX_MATERIAL_TEXTURES (and its GLSL mirror and the pool sizing).",
			                MAX_MATERIAL_TEXTURES, (uint64_t)guid);
			return Gpu::INVALID_TEXTURE;
		}

		const TexturePixels& px = pixelsIt->second;
		if (px.data.empty() || px.width <= 0 || px.height <= 0) {
			LOG_ENGINE_WARN("TextureTable: texture GUID {} has no usable pixel data", (uint64_t)guid);
			return Gpu::INVALID_TEXTURE;
		}

		TextureDesc desc;
		desc.width     = static_cast<uint32_t>(px.width);
		desc.height    = static_cast<uint32_t>(px.height);
		// COLOUR SPACE COMES FROM THE ASSET, not from the call site. Base colour
		// and emissive were imported as sRGB; normal and ORM maps as linear.
		// Sampling a normal map through an sRGB view applies an EOTF to data that
		// is not light, and the result looks like a shading bug rather than a
		// format bug.
		desc.format    = px.isSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		desc.mipLevels = 1;
		desc.debugName = "MaterialTexture";

		const uint32_t index = static_cast<uint32_t>(m_Textures.size());
		// THE IN-FRAME CONSTRUCTOR: staged into the frame's arena and recorded
		// into frame.cmd(). No vkQueueWaitIdle, so loading a textured model does
		// not stall the pipeline once per image.
		m_Textures.emplace_back(frame.context(), frame, desc, px.data.data());
		m_IndexOf[guid] = index;

		// m_Textures may have reallocated; every pointer in m_Bound is now stale.
		rebuildBound();
		return index;
	}

	Gpu::Material TextureTable::resolve(const FrameContext& frame, const AssetPool& assetPool,
	                                    const MaterialDesc& desc) {
		Gpu::Material out;
		out.emission  = desc.emission;
		out.color     = desc.color;
		out.pbrParams = glm::vec4(desc.metallic, desc.roughness, desc.ao, desc.normalScale);
		out.textures  = glm::uvec4(
			getOrCreate(frame, assetPool, desc.baseColorTex),
			getOrCreate(frame, assetPool, desc.normalTex),
			getOrCreate(frame, assetPool, desc.metalRoughTex),
			getOrCreate(frame, assetPool, desc.emissiveTex));
		return out;
	}

}
