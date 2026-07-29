#pragma once

// =============================================================================
// TextureTable.h -- the bound array of material textures, and the one place that
// turns an authoring-side MaterialDesc into a runtime Gpu::Material.
//
// THE BINDING MODEL: a FIXED-SIZE array of combined image samplers,
// `uniform sampler2D u_MaterialTextures[MAX_MATERIAL_TEXTURES]` at set 0
// binding 2, with EVERY element always written -- unused ones point at slot 0's
// 1x1 white dummy. Indexed with nonuniformEXT() in the shader, because the
// material index diverges across lanes in a path tracer.
//
// Two alternatives were rejected:
//   * sampler2DArray requires every layer to share dimensions and format. Real
//     material textures are heterogeneous; a single 4K normal map would force a
//     global resize.
//   * Full bindless (VARIABLE_DESCRIPTOR_COUNT + UPDATE_AFTER_BIND +
//     PARTIALLY_BOUND) is far more feature surface than a 128-entry table needs,
//     and descriptor indexing is the weakest part of MoltenVK's coverage. macOS
//     is a supported target and gets tested continuously, so the risk is not
//     worth a win this phase does not need.
//
// Phase 7 wants exactly the same thing -- one bound table, index sourced from
// the material struct -- so this does not get redone for the rasterizer. Phase
// 9's BC7 changes the FORMAT of the images, not the binding model. The only
// thing that forces a change here is exceeding MAX_MATERIAL_TEXTURES, at which
// point the constant, the GLSL mirror and the descriptor pool sizing are the
// three places that care.
// =============================================================================

#include "Platform/Vulkan/VulkanImage.h"
#include "Project/Assets/MaterialDesc.h"
#include "Renderer/GpuTypes.h"

#include <unordered_map>
#include <vector>

namespace X3
{

class VulkanContext;
struct AssetPool;

// Must equal X3_MAX_MATERIAL_TEXTURES in res/shaders/GpuTypes.glsl and the
// `count` on set 0 binding 2 in Renderer.cpp's kComputeSetLayouts.
inline constexpr uint32_t MAX_MATERIAL_TEXTURES = 128;

class TextureTable
{
public:
	TextureTable() = default;

	// Creates the 1x1 white dummy that occupies slot 0 and backs every unused
	// element. Runs out of frame, from Renderer::Init.
	void init(VulkanContext& ctx);

	// Releases every owned texture. Must run after vkDeviceWaitIdle, which
	// Renderer::Shutdown guarantees.
	void shutdown();

	// Converts an authoring material to its runtime form, uploading any textures
	// it references that are not in the table yet.
	//
	// THIS IS THE ONLY GUID -> INDEX RESOLVE IN THE ENGINE, and it deliberately
	// serves both sources of materials: a MaterialComponent override and a mesh's
	// imported material are both MaterialDesc and both come through here, so they
	// cannot drift apart in how a field is interpreted.
	//
	// A GUID with no pixels in the asset pool, or a full table, resolves to
	// Gpu::INVALID_TEXTURE with a warning -- never a failed frame. The shader
	// falls back to the scalar factor.
	Gpu::Material resolve(const FrameContext& frame, const AssetPool& assetPool,
	                      const MaterialDesc& desc);

	// The MAX_MATERIAL_TEXTURES-long span DescriptorWriter::sampledImageArray
	// wants. Every element is non-null; unused ones are the dummy. Valid until
	// the next resolve() that uploads something.
	std::span<const VulkanTexture* const> descriptors() const { return m_Bound; }

	// Every uploaded texture is dropped and slot assignment restarts at 1. Called
	// when the asset pool's texture version changes, so a re-imported project
	// does not accumulate dead entries. Must not run mid-frame -- the textures it
	// drops route through deferDestroy, but the table's own bound span is read
	// during Draw.
	void invalidate();

private:
	uint32_t getOrCreate(const FrameContext& frame, const AssetPool& assetPool,
	                     LR_GUID guid);

	VulkanContext* m_Ctx = nullptr;

	// Slot 0 is the dummy and is never handed out by getOrCreate.
	std::vector<VulkanTexture> m_Textures;
	std::unordered_map<LR_GUID, uint32_t> m_IndexOf;

	// Rebuilt whenever m_Textures grows. It holds POINTERS INTO m_Textures, so
	// it must be refreshed after any push_back that may have reallocated -- which
	// is why rebuildBound() exists rather than the pointers being cached at
	// insert time.
	std::vector<const VulkanTexture*> m_Bound;

	void rebuildBound();
};

}
