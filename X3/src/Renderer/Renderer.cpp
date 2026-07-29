#include "Renderer/Renderer.h"
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include "Project/Scene/Scene.h"
#include "Project/Assets/AssetManager.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Core/Profiler.h"

// GENERATED from the Slang reflection output at build time -- see
// scripts/gen_descriptor_tables.py. Replaces the table that used to be
// hand-written at the top of this file and matched to the shader by comment.
#include "Renderer/Generated/DescriptorTables.h"
#include "Renderer/ShadowCascades.h"

namespace X3
{

	namespace {
		// Matches [numthreads(8,4,1)] in res/shaders/*.slang. These are LOCAL
		// sizes; dispatch() takes GROUP COUNTS, and the division below is what
		// converts one into the other. The old code stored the group counts in a
		// member named m_WorkGroupSizes, so the name said local size and every
		// call site meant groups.
		constexpr uint32_t kLocalSizeX = 8;
		constexpr uint32_t kLocalSizeY = 4;

		// Matches [numthreads(64,1,1)] in ClusterBuild.slang and LightCull.slang.
		// The cluster grid is one-dimensional to the dispatch: the x/y/z split is
		// decoded from the flat index inside the shader.
		constexpr uint32_t kClusterLocalSize = 64;
	}

	// The material texture table's size is declared in Bindings.slang and
	// reflected out into the generated header. If the C++ constant that fills the
	// array disagrees, sampledImageArray writes the wrong number of descriptors,
	// so make the disagreement a compile error rather than a runtime one.
	static_assert(Generated::kU_MaterialTexturesCount == MAX_MATERIAL_TEXTURES,
	              "MAX_MATERIAL_TEXTURES disagrees with X3_MAX_MATERIAL_TEXTURES in "
	              "res/shaders/GpuTypes.slang");

	void Renderer::Init() {
		m_Ctx = VulkanContext::Get();
		if (!m_Ctx) {
			LOG_ENGINE_CRITICAL("Renderer::Init with no Vulkan context");
			return;
		}

		// THE RINGS ARE CONSTRUCTED HERE, NOT LEFT TO ensureCapacity().
		// BufferKind is fixed at construction and is what selects both the usage
		// flag (UNIFORM_BUFFER vs STORAGE_BUFFER) and the offset alignment the
		// stride is rounded to. A default-constructed ring is Storage, so leaving
		// the UBOs to be allocated by their first ensureCapacity() gave them
		// storage usage and storage alignment -- VUID-VkWriteDescriptorSet-
		// descriptorType-00330 and -00327 respectively, every frame. The initial
		// sizes are just a starting point; ensureCapacity() grows them.
		m_CameraUBO   = VulkanRingBuffer(*m_Ctx, BufferKind::Uniform, sizeof(CameraUBOData),   "CameraUBO");
		m_SettingsUBO = VulkanRingBuffer(*m_Ctx, BufferKind::Uniform, sizeof(SettingsUBOData), "SettingsUBO");

		m_MeshEntityLookupSSBO = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "EntityLookupSSBO");
		m_TransformSSBO        = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "TransformSSBO");
		m_PrevTransformSSBO    = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "PrevTransformSSBO");
		m_ShadowUBO            = VulkanRingBuffer(*m_Ctx, BufferKind::Uniform, sizeof(ShadowUBOData), "ShadowUBO");
		m_MaterialSSBO         = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "MaterialSSBO");
		m_LightSSBO            = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "LightSSBO");
		m_MaterialExtSSBO      = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "MaterialExtSSBO");

		// INDEX usage as well as storage: vkCmdBindIndexBuffer needs the former
		// and the vertex shader reads the same allocation through the latter.
		// VulkanBuffer's extraUsage parameter exists for exactly this.
		m_MeshIndexSSBO = VulkanBuffer(*m_Ctx, BufferKind::Storage, 0, "MeshIndexBuffer",
		                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		// THE CLUSTER GRID IS FIXED SIZE, so unlike every other device-local
		// buffer here these are allocated at their final size in Init() rather
		// than grown by ensureCapacity() in a frame. Nothing about the scene
		// changes them: the grid depends on the projection, not on how many
		// lights or meshes exist.
		m_ClusterAABBSSBO = VulkanBuffer(*m_Ctx, BufferKind::Storage,
			sizeof(Gpu::ClusterAABB) * Gpu::CLUSTER_COUNT, "ClusterAABBBuffer");
		m_ClusterLightGridSSBO = VulkanBuffer(*m_Ctx, BufferKind::Storage,
			sizeof(uint32_t) * Gpu::CLUSTER_COUNT, "ClusterLightGrid");
		m_ClusterLightIndexSSBO = VulkanBuffer(*m_Ctx, BufferKind::Storage,
			sizeof(uint32_t) * Gpu::CLUSTER_COUNT * Gpu::MAX_LIGHTS_PER_CLUSTER,
			"ClusterLightIndices");

		// The images and the device-local buffers stay unallocated: both need a
		// FrameContext (recreate() and ensureCapacity() take one), and Init() runs
		// out of frame.

		// Slot 0's dummy is created here, out of frame, using VulkanTexture's
		// blocking constructor -- which is legal only outside a frame.
		m_TextureTable.init(*m_Ctx);

		m_Graph = RenderGraph(*m_Ctx);

		// Load the default shader (path tracing).
		m_CurrentShaderType = ShaderType::PATH_TRACING;
		if (!GetOrLoadShader(m_CurrentShaderType)) {
			LOG_ENGINE_CRITICAL("Unable to equip compute shader!");
		}
	}

	void Renderer::Shutdown() {
		// Runs from RenderLayer::onDetach, which is after vkDeviceWaitIdle. Order
		// matters: the rings hold pointers into their pipeline's layout vector, so
		// they must die first.
		m_SetRings.clear();
		m_Pipelines.clear();

		for (VulkanImage& image : m_Frames)
			image = VulkanImage{};
		m_SkyboxTexture = VulkanTexture{};
		m_BsdfLut = VulkanImage{};
		m_BsdfLutBaked = false;
		m_TextureTable.shutdown();
		// After vkDeviceWaitIdle, like everything else here -- the pooled
		// transients route through the deferred-destroy queue, but the wait is
		// what makes that safe.
		m_Graph.shutdown();

		m_CameraUBO            = VulkanRingBuffer{};
		m_SettingsUBO          = VulkanRingBuffer{};
		m_MeshEntityLookupSSBO = VulkanRingBuffer{};
		m_MaterialSSBO         = VulkanRingBuffer{};
		m_TransformSSBO        = VulkanRingBuffer{};
		m_LightSSBO            = VulkanRingBuffer{};
		m_MaterialExtSSBO      = VulkanRingBuffer{};

		m_TriPositionSSBO  = VulkanBuffer{};
		m_NodeBufferSSBO   = VulkanBuffer{};
		m_BvhPrimIndexSSBO = VulkanBuffer{};
		m_TriRefSSBO       = VulkanBuffer{};
		m_VertexSSBO       = VulkanBuffer{};
		m_MeshIndexSSBO    = VulkanBuffer{};

		m_DummyStorageImage     = VulkanImage{};
		m_TaaHistory            = {};
		m_TaaRings              = {};
		m_BloomA                = VulkanImage{};
		m_BloomB                = VulkanImage{};

		m_ClusterAABBSSBO       = VulkanBuffer{};
		m_ClusterLightGridSSBO  = VulkanBuffer{};
		m_ClusterLightIndexSSBO = VulkanBuffer{};

		m_DepthPrepassRings = {};
		m_DepthPrepassPipeline = VulkanGraphicsPipeline{};
		m_ForwardOpaqueRings = {};
		m_ForwardOpaquePipeline = VulkanGraphicsPipeline{};
		m_BloomRings = {};
		m_ShadowDepthRings = {};
		m_ShadowDepthPipeline = VulkanGraphicsPipeline{};
		m_ShadowAtlas = VulkanImage{};
		m_ShadowUBO = VulkanRingBuffer{};
		m_ForwardTransparentRings = {};
		m_ForwardTransparentPipeline = VulkanGraphicsPipeline{};
		m_VelocityRings = {};
		m_VelocityPipeline = VulkanGraphicsPipeline{};
		m_VelocityImage = VulkanImage{};
		m_PrevTransformSSBO = VulkanRingBuffer{};
		m_PrevTransforms.clear();
	}

	uint32_t Renderer::writeSlot(const FrameContext& frame) const {
		// ACCUMULATION PINS THE SLOT TO 0, and that is not an oversight.
		// PathTracing.slang does a read-modify-write on the same image:
		// the accumulator IS the previous frame's result. Alternating slots would
		// give each slot every other sample and the viewport would flicker between
		// two different partial accumulations.
		//
		// Writing one image every frame is legal here because the write is GPU-side
		// only -- there is no CPU write to synchronise, and the read-modify-write is
		// ordered by the barrier Draw() records before the dispatch. The
		// per-frame-slot rule exists for CPU writes to memory the GPU may still be
		// reading, which this is not.
		return m_RenderSettings.accumulate ? 0u : frame.index();
	}

	VulkanComputePipeline* Renderer::GetOrLoadShader(ShaderType type) {
		auto it = m_Pipelines.find(type);
		if (it != m_Pipelines.end())
			return it->second.valid() ? &it->second : nullptr;

		auto pathIt = m_ShaderPaths.find(type);
		if (pathIt == m_ShaderPaths.end()) {
			LOG_ENGINE_ERROR("Shader path not found for shader type: {}", static_cast<int>(type));
			return nullptr;
		}

		ComputePipelineDesc desc;
		// ".spv" is appended HERE, by the caller. ComputePipelineDesc::spirvPath is
		// a full path with extension; the old loadShaderFromFile appended it itself
		// and that hid which of the two names on disk was being opened.
		desc.spirvPath  = pathIt->second.string() + ".spv";
		desc.entryPoint = "main";
		desc.setLayouts = Generated::kComputeSetLayouts;
		// SIXTEEN BYTES FOR EVERY COMPUTE PIPELINE, whether its shader declares a
		// push constant or not. Only Bloom.slang uses one -- four passes selected
		// by an index rather than four near-identical shaders, each of which
		// would need its own descriptor rings and its own entry in the generated
		// table for no gain. A declared range a shader never reads costs nothing;
		// making the size depend on the ShaderType would mean this function
		// carrying a table of exceptions.
		desc.pushConstantSize = 16;
		desc.debugName  = "ComputeShader";

		// Pipeline and rings are created together and destroyed together -- every
		// ring holds a raw pointer into this pipeline's layout vector, so the two
		// lifetimes cannot be separated. std::unordered_map never moves its
		// elements on rehash, which is what makes the pointer stable.
		auto [pipeIt, inserted] = m_Pipelines.try_emplace(type, *m_Ctx, desc);
		VulkanComputePipeline& pipeline = pipeIt->second;
		if (!pipeline.valid()) {
			LOG_ENGINE_ERROR("Failed to create compute pipeline: {}", desc.spirvPath.string());
			return nullptr;   // left in the map so the failure is not retried every frame
		}

		auto& rings = m_SetRings[type];
		for (uint32_t set = 0; set < kSetCount; ++set)
			rings[set] = VulkanDescriptorSetRing(*m_Ctx, pipeline.setLayout(set));

		return &pipeline;
	}

	bool Renderer::EnsureRasterPipelines() {
		if (m_DepthPrepassPipeline.valid())
			return true;

		GraphicsPipelineDesc desc;
		desc.vertexSpirv =
			(EngineCfg::RESOURCES_PATH / "shaders" / "DepthPrepass.slang").string() + ".spv";
		// NO FRAGMENT STAGE. See DepthPrepass.slang: one that writes nothing is
		// slower than none at all.
		desc.setLayouts  = Generated::kComputeSetLayouts;
		desc.pushConstantSize = sizeof(uint32_t) * 3;
		desc.depthFormat = kDepthFormat;
		desc.depthTest   = true;
		desc.depthWrite  = true;
		desc.debugName   = "DepthPrepass";

		m_DepthPrepassPipeline = VulkanGraphicsPipeline(*m_Ctx, desc);
		if (!m_DepthPrepassPipeline.valid()) {
			LOG_ENGINE_ERROR("Failed to create the depth prepass pipeline");
			return false;
		}

		for (uint32_t set = 0; set < kSetCount; ++set)
			m_DepthPrepassRings[set] = VulkanDescriptorSetRing(*m_Ctx, m_DepthPrepassPipeline.setLayout(set));

		// ---- Forward opaque --------------------------------------------------
		GraphicsPipelineDesc fwd;
		fwd.vertexSpirv =
			(EngineCfg::RESOURCES_PATH / "shaders" / "ForwardOpaque.slang").string() + ".spv";
		fwd.fragmentSpirv  = fwd.vertexSpirv;   // one module, both stages
		fwd.vertexEntry    = "vertexMain";
		fwd.fragmentEntry  = "fragmentMain";
		fwd.setLayouts     = Generated::kComputeSetLayouts;
		fwd.pushConstantSize = sizeof(uint32_t) * 3;
		fwd.colorFormats   = { kColorFormat };
		fwd.depthFormat    = kDepthFormat;
		// EQUAL and NO DEPTH WRITE. The prepass already laid the nearest surface
		// down, so this shades each pixel exactly once regardless of overdraw --
		// which is the entire reason the prepass exists. Writing depth again
		// would be redundant work against a buffer that already holds the answer.
		fwd.depthTest      = true;
		fwd.depthWrite     = false;
		fwd.depthCompare   = VK_COMPARE_OP_EQUAL;
		fwd.debugName      = "ForwardOpaque";

		m_ForwardOpaquePipeline = VulkanGraphicsPipeline(*m_Ctx, fwd);
		if (!m_ForwardOpaquePipeline.valid()) {
			LOG_ENGINE_ERROR("Failed to create the forward opaque pipeline");
			return false;
		}
		for (uint32_t set = 0; set < kSetCount; ++set)
			m_ForwardOpaqueRings[set] = VulkanDescriptorSetRing(*m_Ctx, m_ForwardOpaquePipeline.setLayout(set));

		// ---- Forward transparent ---------------------------------------------
		// THE SAME SHADER, different pipeline state. Blending on, and the depth
		// test is GREATER rather than EQUAL because a transparent surface is not
		// in the prepass -- there is no equal depth for it to match. Depth write
		// stays off so transparent surfaces do not occlude each other; the
		// back-to-front sort is what orders them.
		GraphicsPipelineDesc trans = fwd;
		trans.depthCompare = VK_COMPARE_OP_GREATER;
		trans.blendEnable  = true;
		// Both sides visible: a pane seen from behind is a real thing to draw,
		// and the reference has no notion of back-face culling for a surface a
		// ray passes through.
		trans.cullMode     = VK_CULL_MODE_NONE;
		trans.debugName    = "ForwardTransparent";

		m_ForwardTransparentPipeline = VulkanGraphicsPipeline(*m_Ctx, trans);
		if (!m_ForwardTransparentPipeline.valid()) {
			LOG_ENGINE_ERROR("Failed to create the forward transparent pipeline");
			return false;
		}
		for (uint32_t set = 0; set < kSetCount; ++set)
			m_ForwardTransparentRings[set] =
				VulkanDescriptorSetRing(*m_Ctx, m_ForwardTransparentPipeline.setLayout(set));

		// ---- Shadow depth ----------------------------------------------------
		// Depth-only from the light, one draw list rendered four times into four
		// scissored sub-rects of the atlas.
		GraphicsPipelineDesc sh;
		sh.vertexSpirv =
			(EngineCfg::RESOURCES_PATH / "shaders" / "ShadowDepth.slang").string() + ".spv";
		sh.setLayouts        = Generated::kComputeSetLayouts;
		sh.pushConstantSize  = sizeof(uint32_t) * 3;
		sh.depthFormat       = kDepthFormat;
		sh.depthTest         = true;
		sh.depthWrite        = true;
		sh.depthCompare      = VK_COMPARE_OP_GREATER;   // reverse-Z, as everywhere
		// FRONT faces culled, not back. Rendering only back faces moves the
		// recorded depth to the far side of each caster, which pushes the whole
		// shadow test away from the receiving surface and removes most acne
		// before any bias is applied. It costs correctness for open geometry --
		// a single-sided wall casts no shadow -- which is why the bias below is
		// still real rather than nominal.
		sh.cullMode          = VK_CULL_MODE_FRONT_BIT;
		sh.depthBias         = true;
		// NEGATIVE, because the projection is reverse-Z: pushing a fragment
		// further from the light means DECREASING its depth. Values copied from
		// forward-Z references push the wrong way and roughly double the acne.
		sh.depthBiasConstant = -1.25f;
		sh.depthBiasSlope    = -2.75f;
		sh.debugName         = "ShadowDepth";

		m_ShadowDepthPipeline = VulkanGraphicsPipeline(*m_Ctx, sh);
		if (!m_ShadowDepthPipeline.valid()) {
			LOG_ENGINE_ERROR("Failed to create the shadow depth pipeline");
			return false;
		}
		for (uint32_t set = 0; set < kSetCount; ++set)
			m_ShadowDepthRings[set] = VulkanDescriptorSetRing(*m_Ctx, m_ShadowDepthPipeline.setLayout(set));

		// ---- Velocity --------------------------------------------------------
		// Same geometry, same depth rule as the forward pass -- velocity has to
		// describe the surface the prepass decided was visible, not some other one.
		GraphicsPipelineDesc vel = fwd;
		vel.vertexSpirv =
			(EngineCfg::RESOURCES_PATH / "shaders" / "Velocity.slang").string() + ".spv";
		vel.fragmentSpirv = vel.vertexSpirv;
		vel.colorFormats  = { kVelocityFormat };
		vel.debugName     = "Velocity";

		m_VelocityPipeline = VulkanGraphicsPipeline(*m_Ctx, vel);
		if (!m_VelocityPipeline.valid()) {
			LOG_ENGINE_ERROR("Failed to create the velocity pipeline");
			return false;
		}
		for (uint32_t set = 0; set < kSetCount; ++set)
			m_VelocityRings[set] = VulkanDescriptorSetRing(*m_Ctx, m_VelocityPipeline.setLayout(set));

		return true;
	}

	std::array<VkDescriptorSet, Renderer::kSetCount> Renderer::WriteCommonSets(
		const FrameContext& frame, const RgResources& res,
		std::array<VulkanDescriptorSetRing, kSetCount>& rings,
		bool targetIsAttachment, bool velocityIsAttachment, bool shadowIsAttachment) {
		VulkanContext& ctx = frame.context();

		// ONE DescriptorWriter per (set, frame), flushed before the first bind of
		// that set. ring.get(frame) is the only way to name a set, so a set the
		// GPU may still be reading is unnameable -- which is the fix for
		// VUID-vkUpdateDescriptorSets-None-03047.
		{
			DescriptorWriter w(ctx, rings[0], frame);
			w.storageImage(0, targetIsAttachment ? m_DummyStorageImage
			                                     : res.image(m_TargetHandle))
			 .sampledImage(1, m_SkyboxTexture.valid() ? m_SkyboxTexture : ctx.dummyTexture())
			 // EVERY element, every frame. There is no PARTIALLY_BOUND, so an
			 // element that was never written is undefined behaviour on access,
			 // not a validation error -- TextureTable fills unused slots with its
			 // dummy for exactly this reason.
			 .sampledImageArray(2, m_TextureTable.descriptors())
			 .storageImage(3, res.image(m_BsdfLutHandle))
			 .storageImage(7, res.image(m_BloomAHandle))
			 .storageImage(8, res.image(m_BloomBHandle))
			 // READ one, WRITE the other. Swapped per frame by m_TaaWriteSlot.
			 .storageImage(9,  res.image(m_TaaHistoryHandle[1 - m_TaaWriteSlot]))
			 .storageImage(10, res.image(m_TaaHistoryHandle[m_TaaWriteSlot]))
			 // Sampled, not storage: D32_SFLOAT is not a storage-image format.
			 // The layout is explicit because an attachment being read is not in
			 // GENERAL.
			 .sampledImage(4, res.image(m_DepthHandle),
			               ctx.getSampler(SamplerDesc{}),
			               VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
			if (velocityIsAttachment)
				w.sampledImage(5, ctx.dummyTexture());
			else
				w.sampledImage(5, res.image(m_VelocityHandle),
				               ctx.getSampler(SamplerDesc{}),
				               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			// The shadow atlas. CLAMP_TO_BORDER with a FLOAT border, and the
			// border value is the subtle part: under reverse-Z 0 is the FAR plane,
			// so an opaque-black border reads as "the nearest occluder is
			// infinitely far" and a sample outside a cascade comes out LIT.
			// FLOAT_OPAQUE_WHITE is depth 1 -- an occluder pressed against the
			// light -- and would black out everything past the cascade edge. It
			// must also be a FLOAT border rather than the INT default: an INT
			// border colour on a float-format image is undefined behaviour, and
			// this is the first sampler in the engine that actually reads its
			// border.
			SamplerDesc shadowSampler{};
			shadowSampler.filter      = VK_FILTER_LINEAR;
			shadowSampler.addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			shadowSampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
			if (shadowIsAttachment)
				w.sampledImage(6, ctx.dummyTexture());
			else
				w.sampledImage(6, res.image(m_ShadowAtlasHandle),
				               ctx.getSampler(shadowSampler),
				               VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
			w.flush();
		}
		{
			DescriptorWriter w(ctx, rings[1], frame);
			w.uniformBuffer(0, m_CameraUBO, frame)
			 .uniformBuffer(1, m_SettingsUBO, frame)
			 .uniformBuffer(2, m_ShadowUBO, frame)
			 .flush();
		}
		{
			// Every binding, every frame -- flush() asserts completeness. An empty
			// scene still writes its bindings, falling back to the context's dummy
			// storage buffer for the device-local buffers that have nothing in
			// them yet.
			const VulkanBuffer& dummy = ctx.dummyStorageBuffer();
			DescriptorWriter w(ctx, rings[2], frame);
			w.storageBuffer(0, m_MeshEntityLookupSSBO, frame)
			 .storageBuffer(1, m_TransformSSBO, frame)
			 .storageBuffer(2, m_MaterialSSBO, frame)
			 .storageBuffer(3, m_TriPositionSSBO.valid()  ? m_TriPositionSSBO  : dummy)
			 .storageBuffer(4, m_NodeBufferSSBO.valid()   ? m_NodeBufferSSBO   : dummy)
			 .storageBuffer(5, m_BvhPrimIndexSSBO.valid() ? m_BvhPrimIndexSSBO : dummy)
			 .storageBuffer(6, m_LightSSBO, frame)
			 .storageBuffer(7, m_TriRefSSBO.valid()       ? m_TriRefSSBO       : dummy)
			 .storageBuffer(8, m_VertexSSBO.valid()       ? m_VertexSSBO       : dummy)
			 // Usually empty -- only materials with a coat, sheen or anisotropy
			 // get an entry -- and still written every frame, because flush()
			 // asserts every binding was written exactly once. No shader reads it
			 // unless a material's flags.y names an index into it.
			 .storageBuffer(9, m_MaterialExtSSBO, frame)
			 .storageBuffer(10, m_MeshIndexSSBO.valid()   ? m_MeshIndexSSBO    : dummy)
			 // Fixed size and allocated in Init(), so unlike the buffers above
			 // these are never invalid once the context exists.
			 .storageBuffer(11, m_ClusterAABBSSBO)
			 .storageBuffer(12, m_ClusterLightGridSSBO)
			 .storageBuffer(13, m_ClusterLightIndexSSBO)
			 .storageBuffer(14, m_PrevTransformSSBO, frame)
			 .flush();
		}

		return { rings[0].get(frame), rings[1].get(frame), rings[2].get(frame) };
	}

	void Renderer::DrawGeometry(const FrameContext& frame, const VulkanGraphicsPipeline& pipeline,
	                            std::span<const VkDescriptorSet> sets,
	                            std::span<const ParsedScene::DrawRange> draws, VkExtent2D extent) {
		pipeline.bind(frame, sets, extent);

		// ONE INDEX BUFFER FOR THE WHOLE SCENE. Every mesh's triangles live in one
		// flat array, so binding happens once and each entity is a range within
		// it. That is the same layout the BVH traversal already relies on.
		vkCmdBindIndexBuffer(frame.cmd(), m_MeshIndexSSBO.handle(), 0, VK_INDEX_TYPE_UINT32);

		for (const ParsedScene::DrawRange& d : draws) {
			if (d.triCount == 0) continue;

			// Entity index and material slot are the ONLY per-draw state.
			// Everything else the shader needs -- transform, material, vertices --
			// it looks up from bound buffers, which is what keeps this a push
			// constant rather than a descriptor set rebind per object.
			const uint32_t push[3] = { d.entityIndex, d.materialSlot, 0u };
			pipeline.pushConstants(frame, push, sizeof(push));

			vkCmdDrawIndexed(frame.cmd(),
			                 d.triCount * 3,       // indices
			                 1,                    // instances
			                 d.firstTriIdx * 3,    // firstIndex, into the flat list
			                 0,                    // vertexOffset: indices are global
			                 0);                   // firstInstance
		}
	}

	void Renderer::DrawShadowCascade(const FrameContext& frame,
	                                 std::span<const VkDescriptorSet> sets,
	                                 const ParsedScene& pScene, uint32_t cascade) {
		// THE SCISSOR IS THE LOAD-BEARING HALF, not the viewport. The render
		// graph opens its rendering block with renderArea covering the WHOLE
		// atlas, and a viewport transform does not clip -- it only maps NDC to
		// pixels. Without a matching scissor, a cascade's geometry that falls
		// outside its own tile would still be rasterized into its neighbour.
		const uint32_t r = Gpu::SHADOW_MAP_RESOLUTION;
		const VkRect2D tile{ VkOffset2D{ int32_t(cascade * r), 0 }, VkExtent2D{ r, r } };
		m_ShadowDepthPipeline.bind(frame, sets, tile);

		vkCmdBindIndexBuffer(frame.cmd(), m_MeshIndexSSBO.handle(), 0, VK_INDEX_TYPE_UINT32);

		// OPAQUE CASTERS ONLY. A transparent surface writing into the shadow map
		// would cast a solid shadow, which is the same limitation the traced path
		// has -- IsInShadow never resolves a material -- so the two still agree.
		// Fixing it means fixing both together.
		for (const ParsedScene::DrawRange& d : pScene.DrawList) {
			if (d.triCount == 0) continue;
			const uint32_t push[3] = { d.entityIndex, d.materialSlot, cascade };
			m_ShadowDepthPipeline.pushConstants(frame, push, sizeof(push));
			vkCmdDrawIndexed(frame.cmd(), d.triCount * 3, 1, d.firstTriIdx * 3, 0, 0);
		}
	}

	VulkanImage* Renderer::Render(const FrameContext& frame,
		const Scene* scene, const AssetPool* assetPool,
		const glm::mat4* editorCameraTransform, float editorCameraFOV) {
		auto t = m_Profiler->timer("Renderer::Render()");

		if (!m_Ctx)
			return nullptr;

		// A DIFFERENT SCENE INVALIDATES THE ACCUMULATOR. Accumulation pins the
		// write slot to 0 and does a read-modify-write, so the first frame of a
		// new scene otherwise blends into whatever the last scene left there --
		// at weight 1/(N+1), which for a well-accumulated previous image means
		// the new scene barely registers.
		//
		// This was not theoretical. The render-test harness renders scenarios
		// back to back in one process, and every accumulating scenario was
		// inheriting the previous scenario's final image: `lights-pathtracing`
		// changed when `materials-pbr` did, and its golden differed depending on
		// whether it ran alone or in the suite. Golden-image tests whose result
		// depends on what ran before them are not reproducible, whatever they
		// otherwise measure.
		if (scene != m_LastScene) {
			m_LastScene = scene;
			m_Cache.AccumulatedFrames = 0;
			m_TaaHistoryValid = false;
			m_JitterIndex = 0;
		}

		const auto pScene = Parse(scene, assetPool, editorCameraTransform, editorCameraFOV);
		if (!pScene) { // Most likely scene missing camera
			return nullptr;
		}
		if (!SetupGPUResources(frame, pScene, scene, assetPool))
			return nullptr;

		Draw(frame, pScene);

		// The image just written. There is no double-buffer swap any more: the old
		// one returned the OTHER slot, which the current frame's command buffer had
		// recorded no barrier for, so the consumer sampled an image whose last write
		// was unsynchronised with this frame's reads. Per-frame slots plus the
		// post-dispatch barrier in Draw() give the same pipelining without that.
		return &m_Frames[writeSlot(frame)];
	}

	std::shared_ptr<const Renderer::ParsedScene> Renderer::Parse(const Scene* scene, const AssetPool* assetPool,
		const glm::mat4* editorCameraTransform, float editorCameraFOV) const {
		if (scene == nullptr) {
			LOG_ENGINE_WARN("Parse: scene is nullptr");
			return nullptr;
		}

		auto t = m_Profiler->timer("Renderer::Parse()");
		auto pScene = std::make_shared<Renderer::ParsedScene>();

		// CAMERA (use editor camera if provided, otherwise use scene's main camera)
		if (editorCameraTransform != nullptr) {
			// Use editor camera
			pScene->hasValidCamera = true;
			pScene->CameraTransform = *editorCameraTransform;
			pScene->CameraFocalLength = 1.0f / tan(glm::radians(editorCameraFOV) / 2.0f);
		} else {
			// Use scene's main camera
			auto cameraView = scene->GetRegistry()->view<TransformComponent, CameraComponent>();
			for (auto entity : cameraView) {
				EntityHandle e(entity, scene->GetRegistry());
				if (!e.GetComponent<CameraComponent>().isMain) {
					continue;
				}
				pScene->hasValidCamera = true;
				pScene->CameraTransform = e.GetComponent<TransformComponent>().GetMatrix();
				pScene->CameraFocalLength = e.GetComponent<CameraComponent>().GetFocalLength();
				break;
			}
		}

		if (!pScene->hasValidCamera) {
			LOG_ENGINE_WARN("Parse: no valid camera (editorCam={}, sceneCams={})",
				editorCameraTransform != nullptr,
				scene->GetRegistry()->view<TransformComponent, CameraComponent>().size_hint());
			return nullptr;
		}

		// SKYBOX
		pScene->skyboxGUID = scene->skyboxGuid;

		// LIGHTS
		auto lightView = scene->GetRegistry()->view<TransformComponent, LightComponent>();
		pScene->LightBuffer.reserve(lightView.size_hint());
		for (auto entity : lightView) {
			EntityHandle e(entity, scene->GetRegistry());
			const auto& transform = e.GetComponent<TransformComponent>();
			const auto& light = e.GetComponent<LightComponent>();

			Gpu::LightData lightData;
			glm::vec3 position = transform.GetTranslation();
			glm::vec3 forward = glm::normalize(glm::mat3(transform.GetMatrix()) * glm::vec3(0, 0, 1));

			lightData.position = glm::vec4(position, static_cast<float>(light.type));
			lightData.direction = glm::vec4(forward, light.intensity);
			lightData.color = glm::vec4(light.color, light.range);
			// params.w was the struct's only free lane. Soft shadows take it, in
			// RADIANS like the cone angles beside it -- the shader should never
			// be converting units.
			lightData.params = glm::vec4(light.attenuation,
			                             glm::radians(light.innerConeAngle),
			                             glm::radians(light.outerConeAngle),
			                             glm::radians(light.softnessDegrees));

			pScene->LightBuffer.push_back(lightData);
		}

		// ENTITY HANDLES, TRANSFORMS & MATERIALS
		auto renderableView = scene->GetRegistry()->view<TransformComponent, MeshComponent>();
		pScene->MeshEntityLookupTable.reserve(renderableView.size_hint());
		pScene->TransformBuffer.reserve(renderableView.size_hint());
		// An under-estimate now that each entity contributes one entry PER
		// MATERIAL SLOT rather than exactly one. Doubling just trims reallocation.
		pScene->MaterialDescs.reserve(renderableView.size_hint() * 2);

		for (auto entity : renderableView) {
			EntityHandle e(entity, scene->GetRegistry());
			LR_GUID& guid = e.GetComponent<MeshComponent>().guid;
			std::shared_ptr<MeshMetadata> metadata = assetPool->find<MeshMetadata>(guid);
			if (!metadata) {
				LOG_ENGINE_WARN("Parse: entity references mesh GUID {} which is not in the asset pool; skipping",
					(uint64_t)guid);
				continue;
			}

			// transform guaranteed by the view
			pScene->TransformBuffer.emplace_back(e.GetComponent<TransformComponent>().GetMatrix());

			// MATERIALS ARE PER-SUBMESH NOW. Every entity contributes
			// materialSlotCount consecutive entries starting at materialBase,
			// rather than exactly one. A mesh with no slots at all still gets one
			// so the shader's `min(slot, slotCount - 1)` clamp never underflows.
			const uint32_t slotCount = std::max(1u, metadata->materialSlotCount);
			const uint32_t base = static_cast<uint32_t>(pScene->MaterialDescs.size());

			const MaterialComponent* materialComponent =
				e.HasComponent<MaterialComponent>() ? &e.GetComponent<MaterialComponent>() : nullptr;

			for (uint32_t slot = 0; slot < slotCount; ++slot) {
				const MaterialDesc* imported =
					slot < metadata->importedMaterials.size() ? &metadata->importedMaterials[slot] : nullptr;

				// Precedence: the entity's override for this slot, then the
				// material the model file shipped, then the default. Both real
				// sources are MaterialDesc and go through ONE conversion, so an
				// override and an import cannot disagree about what a field means.
				if (materialComponent && slot < materialComponent->slots.size()) {
					MaterialDesc desc = materialComponent->slots[slot];

					// TEXTURES ARE INHERITED WHEN THE OVERRIDE NAMES NONE.
					//
					// An override that specifies no maps at all is not saying
					// "this material is untextured" -- it is a scene authored
					// before materials could reference textures at all, or a slot
					// whose scalars were tweaked in the inspector. Letting the
					// override win outright means every pre-Phase-2 scene silently
					// discards its model's embedded maps, which is what the
					// committed fixture did: four textures decoded into the pool
					// and not one of them ever bound.
					//
					// Scalars still come from the override. Only the texture slots
					// fall back, and only when ALL FOUR are unset -- a partially
					// textured override is a deliberate statement and is left
					// alone. Revisit when Phase 13's material editor can actually
					// assign and clear a texture; until then there is no way for a
					// user to mean "no texture" on purpose.
					const bool overrideNamesNoTextures =
						desc.baseColorTex  == LR_GUID::INVALID &&
						desc.normalTex     == LR_GUID::INVALID &&
						desc.metalRoughTex == LR_GUID::INVALID &&
						desc.emissiveTex   == LR_GUID::INVALID;

					if (overrideNamesNoTextures && imported) {
						desc.baseColorTex  = imported->baseColorTex;
						desc.normalTex     = imported->normalTex;
						desc.metalRoughTex = imported->metalRoughTex;
						desc.emissiveTex   = imported->emissiveTex;
					}

					pScene->MaterialDescs.push_back(desc);
				}
				else if (imported) {
					pScene->MaterialDescs.push_back(*imported);
				}
				else {
					pScene->MaterialDescs.emplace_back();
				}
			}

			const uint32_t entityIndex =
				static_cast<uint32_t>(pScene->MeshEntityLookupTable.size());

			pScene->MeshEntityLookupTable.push_back(Gpu::MeshEntityHandle{
				metadata->firstTriIdx,
				metadata->TriCount,
				metadata->firstNodeIdx,
				metadata->nodeCount,
				static_cast<uint32_t>(pScene->TransformBuffer.size() - 1),
				base,
				slotCount,
				metadata->firstVertexIdx
			});

			// THE RASTER DRAW LIST. SubmeshInfo::firstTriIdx is MESH-LOCAL, so it
			// is rebased onto the pool here -- the same rebase MeshEntityHandle
			// already carries, done once rather than in the shader.
			//
			// A mesh with no submesh table (the generated primitives, which have
			// one material and never populate it) falls back to one draw covering
			// the whole mesh. Skipping it instead would silently stop drawing
			// every primitive in the scene.
			// OPAQUE OR TRANSPARENT, decided here from the authoring material,
			// because the depth prepass must not draw the transparent ones -- see
			// ParsedScene::TransparentDrawList.
			const glm::vec3 origin = glm::vec3(pScene->TransformBuffer.back()[3]);
			const float sortDepth = glm::length(origin - glm::vec3(pScene->CameraTransform[3]));

			auto emit = [&](uint32_t firstTri, uint32_t triCount, uint32_t slot) {
				if (triCount == 0) return;
				const size_t descIdx = base + std::min<size_t>(slot, slotCount ? slotCount - 1 : 0);
				const bool transparent = descIdx < pScene->MaterialDescs.size()
				                      && pScene->MaterialDescs[descIdx].color.a < 1.0f;
				(transparent ? pScene->TransparentDrawList : pScene->DrawList)
					.push_back({ entityIndex, firstTri, triCount, slot, sortDepth });
			};

			if (metadata->submeshes.empty()) {
				emit(metadata->firstTriIdx, metadata->TriCount, 0u);
			} else {
				for (const SubmeshInfo& sm : metadata->submeshes)
					emit(metadata->firstTriIdx + sm.firstTriIdx, sm.triCount, sm.materialSlot);
			}
		}

		// BACK TO FRONT. Alpha blending is not commutative, so this ordering is
		// part of the answer rather than an optimisation.
		//
		// PER OBJECT, NOT PER TRIANGLE, and that is a known limitation rather
		// than an oversight: two interpenetrating transparent meshes, or a single
		// concave one, still composite wrongly. Per-triangle sorting means
		// rebuilding an index buffer every frame, and the honest fix is
		// order-independent transparency, which is not Phase 7.
		std::sort(pScene->TransparentDrawList.begin(), pScene->TransparentDrawList.end(),
		          [](const ParsedScene::DrawRange& a, const ParsedScene::DrawRange& b) {
			          return a.sortDepth > b.sortDepth;
		          });
		return pScene;
	}

	// returns false if error occured, else true
	// assumes a valid pScene
	bool Renderer::SetupGPUResources(const FrameContext& frame, std::shared_ptr<const ParsedScene> pScene,
		const Scene* scene, const AssetPool* assetPool) {
		auto t = m_Profiler->timer("Renderer::SetupGPUResources()");

		VulkanContext& ctx = frame.context();

		// --- The frame image --------------------------------------------------
		// Recreated ONLY for the slot this frame owns. The other slot may still be
		// referenced by a command buffer in flight, and recreate() defers its old
		// handles precisely because of that -- but its NEW allocation would then be
		// written by a dispatch the other frame never barriered.
		const uint32_t slot = writeSlot(frame);
		if (!m_Frames[slot].valid() || m_FrameResolutions[slot] != m_RenderSettings.resolution) {
			ImageDesc desc;
			desc.width     = m_RenderSettings.resolution.x;
			desc.height    = m_RenderSettings.resolution.y;
			desc.format    = VK_FORMAT_R32G32B32A32_SFLOAT;   // matches `rgba32f` in the shader
			desc.usage     = VK_IMAGE_USAGE_STORAGE_BIT
			               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT   // runtime blit to the swapchain
			               | VK_IMAGE_USAGE_SAMPLED_BIT        // editor ImGui viewport
			               // Phase 7c: the forward pass RENDERS into this rather
			               // than imageStore-ing to it, so it needs to be usable
			               // as a colour attachment as well as a storage image.
			               // The two are mutually exclusive per pass -- an
			               // attachment is in COLOR_ATTACHMENT_OPTIMAL and a
			               // storage image in GENERAL -- which is why the forward
			               // pass binds m_DummyStorageImage at set 0 binding 0.
			               | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			desc.debugName = "RenderTarget";
			m_Frames[slot].recreate(frame, desc);
			m_FrameResolutions[slot] = m_RenderSettings.resolution;

			// A fresh allocation has no accumulated samples in it.
			m_Cache.AccumulatedFrames = 0;
		}
		// --- The BSDF energy LUT ----------------------------------------------
		// Allocated on the first frame, because VulkanImage::recreate needs a
		// FrameContext and Init() runs out of frame. The bake dispatch itself is
		// a render graph pass added in Draw().
		if (!m_BsdfLut.valid()) {
			ImageDesc lut;
			lut.width     = kBsdfLutSize;
			lut.height    = kBsdfLutSize;
			lut.format    = VK_FORMAT_R32G32B32A32_SFLOAT;
			lut.usage     = VK_IMAGE_USAGE_STORAGE_BIT;
			lut.debugName = "BsdfEnergyLut";
			m_BsdfLut.recreate(frame, lut);
			m_BsdfLutBaked = false;
		}

		// --- The dummy storage image ------------------------------------------
		// EXISTS BECAUSE ONE DESCRIPTOR TABLE SERVES EVERY PIPELINE. Set 0
		// binding 0 is a writable storage image and flush() asserts it is written,
		// so a raster pass that RENDERS INTO the target cannot also bind the
		// target there: an attachment is in COLOR_ATTACHMENT_OPTIMAL and a storage
		// image must be in GENERAL, and no image is in two layouts at once.
		//
		// 1x1 and never read. It only has to be a legal, correctly-laid-out thing
		// to point a descriptor at.
		if (!m_DummyStorageImage.valid()) {
			ImageDesc dummy;
			dummy.width     = 1;
			dummy.height    = 1;
			dummy.format    = VK_FORMAT_R32G32B32A32_SFLOAT;   // matches `rgba32f`
			dummy.usage     = VK_IMAGE_USAGE_STORAGE_BIT;
			dummy.debugName = "DummyStorageImage";
			m_DummyStorageImage.recreate(frame, dummy);
			// Once, here. Nothing else ever touches it, so it stays in GENERAL for
			// the life of the renderer and needs no graph declaration.
			m_DummyStorageImage.transition(frame, VK_IMAGE_LAYOUT_GENERAL,
			                               VK_ACCESS_SHADER_WRITE_BIT,
			                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		}

		// --- The velocity buffer ----------------------------------------------
		// Allocated with the render target, since it must match its extent.
		if (!m_VelocityImage.valid() || m_VelocityResolution != m_RenderSettings.resolution) {
			ImageDesc vel;
			vel.width     = m_RenderSettings.resolution.x;
			vel.height    = m_RenderSettings.resolution.y;
			vel.format    = kVelocityFormat;
			vel.usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			              | VK_IMAGE_USAGE_SAMPLED_BIT      // Phase 11's TAA
			              | VK_IMAGE_USAGE_STORAGE_BIT;     // debug views
			vel.debugName = "SceneVelocity";
			m_VelocityImage.recreate(frame, vel);
			m_VelocityResolution = m_RenderSettings.resolution;
		}

		// --- The TAA history --------------------------------------------------
		if (!m_TaaHistory[0].valid() || m_TaaResolution != m_RenderSettings.resolution) {
			ImageDesc h;
			h.width  = m_RenderSettings.resolution.x;
			h.height = m_RenderSettings.resolution.y;
			h.format = kBloomFormat;   // RGBA16F, same reasoning
			h.usage  = VK_IMAGE_USAGE_STORAGE_BIT;
			h.debugName = "TaaHistoryA";
			m_TaaHistory[0].recreate(frame, h);
			h.debugName = "TaaHistoryB";
			m_TaaHistory[1].recreate(frame, h);
			m_TaaResolution = m_RenderSettings.resolution;
			// A fresh allocation holds whatever the allocator returned.
			m_TaaHistoryValid = false;
		}

		// --- The bloom ping-pong ----------------------------------------------
		{
			const glm::uvec2 half{ glm::max(m_RenderSettings.resolution.x / 2u, 1u),
			                       glm::max(m_RenderSettings.resolution.y / 2u, 1u) };
			if (!m_BloomA.valid() || m_BloomResolution != half) {
				ImageDesc b;
				b.width  = half.x;
				b.height = half.y;
				b.format = kBloomFormat;
				b.usage  = VK_IMAGE_USAGE_STORAGE_BIT;
				b.debugName = "BloomA";
				m_BloomA.recreate(frame, b);
				b.debugName = "BloomB";
				m_BloomB.recreate(frame, b);
				m_BloomResolution = half;
			}
		}

		// --- The shadow atlas -------------------------------------------------
		// FIXED SIZE, unlike every other image here: it depends on the cascade
		// count and the shadow resolution, not on the viewport. Resizing the
		// editor window must not change where shadows fall.
		if (!m_ShadowAtlas.valid()) {
			ImageDesc atlas;
			atlas.width     = Gpu::SHADOW_MAP_RESOLUTION * Gpu::SHADOW_CASCADES;
			atlas.height    = Gpu::SHADOW_MAP_RESOLUTION;
			atlas.format    = kDepthFormat;
			atlas.usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
			                | VK_IMAGE_USAGE_SAMPLED_BIT;
			atlas.debugName = "ShadowAtlas";
			m_ShadowAtlas.recreate(frame, atlas);
		}

		// --- The depth buffer -------------------------------------------------
		// Recreated with the render target, since it must match its extent.
		if (!m_DepthImage.valid() || m_DepthResolution != m_RenderSettings.resolution) {
			ImageDesc depth;
			depth.width     = m_RenderSettings.resolution.x;
			depth.height    = m_RenderSettings.resolution.y;
			depth.format    = kDepthFormat;
			depth.usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
			                | VK_IMAGE_USAGE_SAMPLED_BIT;   // Phase 7 will read it
			depth.debugName = "SceneDepth";
			m_DepthImage.recreate(frame, depth);
			m_DepthResolution = m_RenderSettings.resolution;
		}

		m_Cache.Resolution = m_RenderSettings.resolution;

		// THE COUNT USED BY THIS FRAME, and it is advanced AFTER the settings are
		// built rather than before.
		//
		// PathTracing.slang blends with weight 1/(N+1) against what is already in
		// the image. Incrementing first made the very first accumulated frame
		// N=1, so it blended half-and-half with whatever the slot happened to
		// hold instead of replacing it -- and 1/25th of that stale content
		// survives a 24-frame accumulation, because the surviving fraction is
		// prod(1 - 1/(i+1)) over i = 1..24.
		//
		// In the editor that is a faint ghost of the previous view. In the render
		// tests it made every accumulating scenario depend on the scenario before
		// it: `lights-pathtracing` differed by 20 levels between running alone
		// and running in the suite, which is not a property a golden-image test
		// is allowed to have.
		const uint32_t accumulatedFrames =
			m_RenderSettings.accumulate ? m_Cache.AccumulatedFrames : 0;

		// --- Uniform rings ----------------------------------------------------
		const uint32_t entityCount = static_cast<uint32_t>(pScene->MeshEntityLookupTable.size());
		const uint32_t lightCount  = static_cast<uint32_t>(pScene->LightBuffer.size());

		SettingsUBOData settings{};
		settings.raysPerPixel          = m_RenderSettings.raysPerPixel;
		settings.bouncesPerRay         = m_RenderSettings.bouncesPerRay;
		settings.accumulatedFrames     = accumulatedFrames;
		settings.entityCount           = entityCount;
		settings.debugMode             = m_RenderSettings.debugMode;
		settings.aabbHeatmapCutoff     = m_RenderSettings.aabbHeatmapCutoff;
		settings.triangleHeatmapCutoff = m_RenderSettings.triangleHeatmapCutoff;
		settings.lightCount            = lightCount;
		settings.taaHistoryValid       = m_TaaHistoryValid ? 1u : 0u;

		CameraUBOData camera{};
		camera.transform   = pScene->CameraTransform;
		camera.focalLength = pScene->CameraFocalLength;
		camera.nearPlane   = 0.05f;
		camera.farPlane    = 1000.0f;

		// REVERSE-Z, and it is not a preference. A standard [0,1] depth buffer
		// spends almost all of its float precision in the first few percent of the
		// view distance, because floating point is dense near zero and the
		// projection is already hyperbolic -- the two compound. Mapping near to 1
		// and far to 0 makes them cancel, which is what makes a 1000-unit far
		// plane usable at all. The depth test is therefore GREATER, and the depth
		// buffer clears to 0.
		//
		// The path tracer is unaffected: it generates rays from `transform` and
		// `focalLength` and never sees a projection matrix.
		{
			const float aspect = float(m_RenderSettings.resolution.x)
			                   / float(glm::max(m_RenderSettings.resolution.y, 1u));
			// focalLength is 1/tan(fov/2) where fov is HORIZONTAL, and the
			// horizontal part is the whole subtlety. CameraComponent's own comment
			// says it: "half of the screen width is 1". MakeCameraRay agrees --
			// it divides BOTH ray axes by dims.x, so the vertical extent is
			// height/width, not 1.
			//
			// glm's perspective takes a VERTICAL fov, so passing 2*atan(1/f)
			// straight in silently widens the frustum by the aspect ratio in both
			// axes. It does not look broken -- it looks like a scene framed
			// slightly differently -- which is exactly why it needs to be checked
			// against the reference rather than by eye.
			//
			// tan(fovY/2) = (height/width) / focalLength gives proj[0][0] ==
			// focalLength and proj[1][1] == aspect * focalLength, which is what
			// makes the raster frustum identical to the ray generator's.
			const float fovY = 2.0f * std::atan(
				1.0f / (aspect * glm::max(camera.focalLength, 1e-4f)));

			// Three things are deliberate here and each one produced an empty
			// depth buffer when it was wrong. All three fail the same silent way:
			// the pass runs, the draws are issued, validation stays clean, and
			// nothing reaches the depth buffer -- which is indistinguishable from
			// a pass that never ran.
			//
			// _ZO, NOT plain glm::perspective. GLM_FORCE_DEPTH_ZERO_TO_ONE is not
			// defined project-wide, so glm::perspective builds OpenGL clip space
			// with z in [-1,1]. Vulkan's clip volume is z in [0,1], so every
			// vertex lands outside it. Naming the _ZO variant explicitly is what
			// stops a build flag someone changes later from reintroducing that.
			//
			// LH, NOT RH, and this is a property of THIS ENGINE'S CAMERA rather
			// than a convention worth arguing about. Trace.slang's MakeCameraRay
			// generates rays along float3(x, y, focalLength) -- +Z IS FORWARD
			// here. glm's RH variants assume -Z forward, so they yield w = -viewZ,
			// which is negative for everything actually in front of the camera.
			// Every visible vertex is then clipped and the only geometry that
			// survives is what is BEHIND the camera. If the ray generation
			// convention ever changes, this changes with it.
			//
			// far and near are SWAPPED, which is what makes it reverse-Z.
			glm::mat4 proj = glm::perspectiveLH_ZO(fovY, aspect, camera.farPlane, camera.nearPlane);

			// NO Y-FLIP, and that is a consequence of the LH projection above.
			//
			// The usual `proj[1][1] *= -1` exists because glm's RH projections put
			// +Y up while Vulkan's NDC puts +Y down. Going left-handed already
			// inverts the Y row relative to the RH form, so negating it again puts
			// the picture back upside down -- the depth prepass rendered a
			// perfect, vertically mirrored image of the path-traced reference
			// until this came out.
			//
			// Ground truth for this is the golden pair: `depth-prepass` and
			// `lights-pathtracing` frame the same scene, so a Y disagreement is
			// visible by putting them side by side in the contact sheet.

			camera.view     = glm::inverse(pScene->CameraTransform);
			camera.proj     = proj;
			camera.viewProj = proj * camera.view;

			// ---- TAA SUB-PIXEL JITTER ----------------------------------------
			// Halton(2,3), which is the standard choice because it fills a pixel
			// far more evenly than a random sequence at the small sample counts
			// TAA converges over -- a random offset clusters, and clusters are
			// what leave residual aliasing after the blend.
			//
			// The jitter goes into a SEPARATE matrix. `viewProj` above stays
			// unjittered because the velocity pass projects with it: a motion
			// vector must describe where the SURFACE moved, and folding the
			// sub-pixel offset into it makes TAA reproject by the jitter as well
			// and the whole image swims.
			glm::vec2 jitter(0.0f);
			if (m_RenderSettings.taaEnabled) {
				auto halton = [](uint32_t i, uint32_t base) {
					float f = 1.0f, r = 0.0f;
					while (i > 0) { f /= float(base); r += f * float(i % base); i /= base; }
					return r;
				};
				// +1 so index 0 is not the degenerate (0,0) sample, which would
				// make the first frame unjittered and the sequence lopsided.
				const uint32_t n = (m_JitterIndex % 16u) + 1u;
				jitter = glm::vec2(halton(n, 2) - 0.5f, halton(n, 3) - 0.5f);
				// Pixels to NDC. The factor of two is because NDC spans [-1,1]
				// across `resolution` pixels, not [0,1].
				jitter.x *= 2.0f / float(glm::max(m_RenderSettings.resolution.x, 1u));
				jitter.y *= 2.0f / float(glm::max(m_RenderSettings.resolution.y, 1u));
			}
			glm::mat4 jitteredProj = proj;
			jitteredProj[2][0] += jitter.x;
			jitteredProj[2][1] += jitter.y;
			camera.viewProjJittered = jitteredProj * camera.view;
			camera.jitter = glm::vec4(jitter, m_PrevJitter);
			m_PrevJitter = jitter;
			++m_JitterIndex;

			// LAST FRAME'S, or this frame's on the very first frame -- a velocity
			// of zero is the honest answer when there is no previous frame, and
			// an identity matrix here would instead report the entire screen
			// moving at once.
			camera.prevViewProj = m_Cache.havePrevViewProj ? m_Cache.prevViewProj
			                                               : camera.viewProj;
			m_Cache.prevViewProj     = camera.viewProj;
			m_Cache.havePrevViewProj = true;
		}

		m_Cache.AccumulatedFrames = m_RenderSettings.accumulate ? accumulatedFrames + 1 : 0;

		// --- Shadow cascades --------------------------------------------------
		// THE FIRST DIRECTIONAL LIGHT GETS CASCADES and nothing else does. There
		// is no "primary light" concept anywhere in the scene format, so first-
		// found is the rule; every other light keeps tracing, which is correct
		// rather than merely cheap -- a traced shadow needs no map, no cascade
		// fit and no bias tuning.
		ShadowUBOData shadow{};
		shadow.shadowLightIndex = Gpu::NO_SHADOW_LIGHT;
		shadow.depthBiasScale   = kShadowNormalOffsetTexels;
		for (uint32_t i = 0; i < lightCount; ++i) {
			if (static_cast<uint32_t>(pScene->LightBuffer[i].position.w) != 0u) continue;
			shadow.shadowLightIndex = i;

			const ShadowCascades cascades = ComputeShadowCascades(
				pScene->CameraTransform, camera.focalLength,
				float(m_RenderSettings.resolution.x)
					/ float(glm::max(m_RenderSettings.resolution.y, 1u)),
				camera.nearPlane, kShadowFarPlane,
				glm::vec3(pScene->LightBuffer[i].direction),
				Gpu::SHADOW_MAP_RESOLUTION);

			for (uint32_t c = 0; c < Gpu::SHADOW_CASCADES; ++c) {
				shadow.viewProj[c]      = cascades.viewProj[c];
				shadow.splitDepth[c]    = cascades.splitDepth[c];
				shadow.texelWorldSize[c]= cascades.texelWorldSize[c];
			}
			break;
		}
		m_ShadowUBO.ensureCapacity(frame, sizeof(ShadowUBOData));
		m_ShadowUBO.writeStruct(frame, shadow);
		m_ShadowCastersPresent = (shadow.shadowLightIndex != Gpu::NO_SHADOW_LIGHT);

		m_SettingsUBO.ensureCapacity(frame, sizeof(SettingsUBOData));
		m_SettingsUBO.writeStruct(frame, settings);
		m_CameraUBO.ensureCapacity(frame, sizeof(CameraUBOData));
		m_CameraUBO.writeStruct(frame, camera);

		// --- Skybox -----------------------------------------------------------
		// Reloaded only when the GUID changes. An absent skybox leaves
		// m_SkyboxTexture invalid and Draw() writes ctx.dummyTexture() instead --
		// which is what closes VUID-vkCmdDispatch-None-08114. The old code left
		// binding 1 of set 0 unwritten forever and the shader sampled an undefined
		// descriptor.
		if (scene && scene->skyboxGuid != m_Cache.prevSkyboxGuid) {
			m_Cache.prevSkyboxGuid = scene->skyboxGuid;
			m_SkyboxTexture = VulkanTexture{};

			// The skybox reads from the per-asset pixel store now, not from an
			// offset into one flat buffer. It stays at set 0 binding 1 and out of
			// the material texture table: it is not a material texture.
			auto metadata = assetPool->find<TextureMetadata>(pScene->skyboxGUID);
			auto pixelsIt = assetPool->Textures.find(pScene->skyboxGUID);
			const bool havePixels = pixelsIt != assetPool->Textures.end()
			                        && !pixelsIt->second.data.empty();

			if (metadata && havePixels) {
				TextureDesc desc;
				desc.width     = metadata->width;
				desc.height    = metadata->height;
				desc.format    = VK_FORMAT_R8G8B8A8_SRGB;
				desc.mipLevels = 1;
				desc.debugName = "Skybox";
				// THE IN-FRAME CONSTRUCTOR: staged into the frame's arena and
				// recorded into frame.cmd(). No vkQueueWaitIdle, which is what the
				// old VulkanTexture2D did on every skybox change.
				m_SkyboxTexture = VulkanTexture(ctx, frame, desc, pixelsIt->second.data.data());
			}
		}

		// --- Per-frame storage rings ------------------------------------------
		// Written in full every frame, so a ring is correct: a growth discards every
		// slot, and the slot the GPU reads is always the one just written.
		{
			const VkDeviceSize bytes = sizeof(Gpu::MeshEntityHandle) * pScene->MeshEntityLookupTable.size();
			m_MeshEntityLookupSSBO.ensureCapacity(frame, bytes);
			m_MeshEntityLookupSSBO.write(frame, pScene->MeshEntityLookupTable.data(), bytes);
		}
		{
			const VkDeviceSize bytes = sizeof(glm::mat4) * pScene->TransformBuffer.size();
			m_TransformSSBO.ensureCapacity(frame, bytes);
			m_TransformSSBO.write(frame, pScene->TransformBuffer.data(), bytes);

			// AND THE PREVIOUS FRAME'S, for velocity.
			//
			// A CHANGED ENTITY COUNT INVALIDATES THE WHOLE THING, because
			// transform index i does not necessarily refer to the same object it
			// referred to last frame -- Parse rebuilds the table from the scene
			// every frame, and adding or removing an entity shifts everything
			// after it. Falling back to the current transforms reports zero
			// motion, which is wrong for one frame; using the stale table would
			// report an object teleporting across the screen, which TAA would
			// smear for several.
			if (m_PrevTransforms.size() != pScene->TransformBuffer.size())
				m_PrevTransforms = pScene->TransformBuffer;

			m_PrevTransformSSBO.ensureCapacity(frame, bytes);
			m_PrevTransformSSBO.write(frame, m_PrevTransforms.data(), bytes);
			m_PrevTransforms = pScene->TransformBuffer;
		}
		{
			// AUTHORING -> RUNTIME happens here, not in Parse(), because
			// resolving a texture GUID may have to upload that texture and an
			// upload needs the frame. The table caches by GUID, so a material
			// referencing an already-resolved texture costs a hash lookup.
			std::vector<Gpu::Material> gpuMaterials;
			std::vector<Gpu::MaterialExt> gpuMaterialExts;
			gpuMaterials.reserve(pScene->MaterialDescs.size());
			for (const MaterialDesc& desc : pScene->MaterialDescs)
				gpuMaterials.push_back(m_TextureTable.resolve(frame, *assetPool, desc, gpuMaterialExts));

			const VkDeviceSize bytes = sizeof(Gpu::Material) * gpuMaterials.size();
			m_MaterialSSBO.ensureCapacity(frame, bytes);
			m_MaterialSSBO.write(frame, gpuMaterials.data(), bytes);

			// USUALLY EMPTY, and still written every frame. The binding must be
			// written whether or not anything uses it -- flush() asserts
			// completeness -- and kMinBufferSize keeps a zero-length allocation
			// legal (VUID-VkDescriptorBufferInfo-range-00341). No shader reads it
			// unless a material's flags.y names an index into it.
			const VkDeviceSize extBytes = sizeof(Gpu::MaterialExt) * gpuMaterialExts.size();
			m_MaterialExtSSBO.ensureCapacity(frame, extBytes);
			m_MaterialExtSSBO.write(frame, gpuMaterialExts.data(), extBytes);
		}
		{
			// An empty light list still allocates and still gets written: the
			// binding must be written every frame, and kMinBufferSize keeps the
			// descriptor legal (VUID-VkDescriptorBufferInfo-range-00341).
			const VkDeviceSize bytes = sizeof(Gpu::LightData) * pScene->LightBuffer.size();
			m_LightSSBO.ensureCapacity(frame, bytes);
			m_LightSSBO.write(frame, pScene->LightBuffer.data(), bytes);
		}

		// --- Device-local asset buffers, uploaded on change --------------------
		// The version counters live in m_Cache now. They were function-local
		// statics, so two Renderers in one process would have shared them and the
		// second would never have uploaded anything.
		const uint32_t triPosVersion   = assetPool->GetUpdateVersion(AssetPool::AssetType::TriPositionBuffer);
		const uint32_t nodeVersion     = assetPool->GetUpdateVersion(AssetPool::AssetType::NodeBuffer);
		const uint32_t primIdxVersion  = assetPool->GetUpdateVersion(AssetPool::AssetType::BvhPrimIndexBuffer);
		const uint32_t triRefVersion   = assetPool->GetUpdateVersion(AssetPool::AssetType::TriRefBuffer);
		const uint32_t vertexVersion   = assetPool->GetUpdateVersion(AssetPool::AssetType::VertexBuffer);

		if (!m_Cache.assetBuffersUploaded || m_Cache.triPositionVersion != triPosVersion) {
			m_Cache.triPositionVersion = triPosVersion;
			const VkDeviceSize bytes = sizeof(Gpu::TrianglePositions) * assetPool->TriPositionBuffer.size();
			m_TriPositionSSBO.ensureCapacity(frame, bytes);
			m_TriPositionSSBO.upload(frame, assetPool->TriPositionBuffer.data(), bytes);
		}
		if (!m_Cache.assetBuffersUploaded || m_Cache.nodeBufferVersion != nodeVersion) {
			m_Cache.nodeBufferVersion = nodeVersion;
			const VkDeviceSize bytes = sizeof(BVHAccel::Node) * assetPool->NodeBuffer.size();
			m_NodeBufferSSBO.ensureCapacity(frame, bytes);
			m_NodeBufferSSBO.upload(frame, assetPool->NodeBuffer.data(), bytes);
		}
		if (!m_Cache.assetBuffersUploaded || m_Cache.bvhPrimIndexVersion != primIdxVersion) {
			m_Cache.bvhPrimIndexVersion = primIdxVersion;
			const VkDeviceSize bytes = sizeof(uint32_t) * assetPool->BvhPrimIndexBuffer.size();
			m_BvhPrimIndexSSBO.ensureCapacity(frame, bytes);
			m_BvhPrimIndexSSBO.upload(frame, assetPool->BvhPrimIndexBuffer.data(), bytes);
		}
		if (!m_Cache.assetBuffersUploaded || m_Cache.triRefVersion != triRefVersion) {
			m_Cache.triRefVersion = triRefVersion;
			const VkDeviceSize bytes = sizeof(Gpu::TriRef) * assetPool->TriRefBuffer.size();
			m_TriRefSSBO.ensureCapacity(frame, bytes);
			m_TriRefSSBO.upload(frame, assetPool->TriRefBuffer.data(), bytes);

			// THE RASTERIZER'S INDEX BUFFER, derived here rather than stored in
			// the asset pool. It is pure redundancy CPU-side -- three uints per
			// triangle that TriRefBuffer already holds -- and only the GPU needs
			// it, so keeping it out of the pool keeps the cook step of Phase 9
			// from having to produce and version a second copy.
			std::vector<uint32_t> indices;
			indices.reserve(assetPool->TriRefBuffer.size() * 3);
			for (const Gpu::TriRef& t : assetPool->TriRefBuffer) {
				indices.push_back(t.i0);
				indices.push_back(t.i1);
				indices.push_back(t.i2);
			}
			const VkDeviceSize indexBytes = sizeof(uint32_t) * indices.size();
			m_MeshIndexSSBO.ensureCapacity(frame, indexBytes);
			m_MeshIndexSSBO.upload(frame, indices.data(), indexBytes);
		}
		if (!m_Cache.assetBuffersUploaded || m_Cache.vertexVersion != vertexVersion) {
			m_Cache.vertexVersion = vertexVersion;
			const VkDeviceSize bytes = sizeof(Gpu::Vertex) * assetPool->VertexBuffer.size();
			m_VertexSSBO.ensureCapacity(frame, bytes);
			m_VertexSSBO.upload(frame, assetPool->VertexBuffer.data(), bytes);
		}
		// A re-import replaces the pixel data behind every GUID, so the table's
		// cached uploads are stale. Dropping them here rather than letting them
		// accumulate is what keeps a repeatedly-reloaded project from filling all
		// MAX_MATERIAL_TEXTURES slots with dead entries.
		const uint32_t textureVersion = assetPool->GetUpdateVersion(AssetPool::AssetType::Textures);
		if (m_Cache.assetBuffersUploaded && m_Cache.textureVersion != textureVersion)
			m_TextureTable.invalidate();
		m_Cache.textureVersion = textureVersion;

		m_Cache.assetBuffersUploaded = true;

		return true;
	}

	void Renderer::Draw(const FrameContext& frame, std::shared_ptr<const ParsedScene> pScene) {
		auto t = m_Profiler->timer("Renderer::Draw()");

		// Switch shader if needed.
		//
		// FORWARD IS ACCEPTED WITHOUT LOADING ANYTHING. It is the one ShaderType
		// with no compute pipeline behind it, so gating the switch on
		// GetOrLoadShader succeeding silently refuses to select it -- the setting
		// changes, the mode does not, and the frame renders with whatever was
		// selected before. That produced a convincing wrong answer: the "forward"
		// render tests came out as the PATH TRACER at one sample per pixel, which
		// is a correct picture of the right scene covered in Monte Carlo noise,
		// and reads as a shading bug in the new pass rather than as the new pass
		// never having run.
		if (m_RenderSettings.shaderType != m_CurrentShaderType) {
			if (m_RenderSettings.shaderType == ShaderType::FORWARD
			    || GetOrLoadShader(m_RenderSettings.shaderType)) {
				m_CurrentShaderType = m_RenderSettings.shaderType;
				// NO ACCUMULATION RESET HERE. applySettings already did it, at the
				// moment the settings arrived; doing it again here does it a FRAME
				// LATE, because Draw() runs after SetupGPUResources has already
				// built this frame's settings from the counter.
				//
				// The late reset made the accumulated-frame sequence 0,0,1,2,...
				// instead of 0,1,2,3,..., and since the path tracer seeds its RNG
				// from that counter, it changed the noise -- but only when the
				// shader had actually changed, which in the render-test suite
				// meant only when some other scenario had run first. Same commit,
				// same scene, two different images depending on the command line.
			}
		}

		// FORWARD HAS NO COMPUTE PIPELINE. It is the one ShaderType that
		// rasterizes, so there is nothing in m_ShaderPaths for it and asking for
		// one would log an error and abandon the frame.
		VulkanComputePipeline* pipeline = nullptr;
		if (m_CurrentShaderType != ShaderType::FORWARD) {
			pipeline = GetOrLoadShader(m_CurrentShaderType);
			if (!pipeline) {
				LOG_ENGINE_ERROR("No valid shader available for rendering");
				return;
			}
		}

		VulkanContext& ctx   = frame.context();
		VulkanImage&   image = m_Frames[writeSlot(frame)];

		// ---- THE RENDER GRAPH ------------------------------------------------
		// One pass today. That is the point of building it now rather than at
		// Phase 7: the graph is exercised by real work from the moment it exists,
		// so when Forward+ adds fifteen more passes they are added to something
		// that already demonstrably derives the right barriers, rather than to
		// scaffolding that has never run.
		//
		// The two barriers below used to be written by hand here, and their
		// reasoning is now expressed as declarations instead:
		//   readWrite(ComputeReadWrite)  ->  the pre-dispatch barrier. The shader
		//       does a read-modify-write on the accumulator, so this must record
		//       EVEN WHEN THE LAYOUT IS UNCHANGED; transition() would elide it,
		//       which is why RgUsage carries a distinct read-write intent.
		//   read(FragmentRead) + read(TransferRead)  ->  the post-dispatch
		//       barrier. The editor samples this image from ImGui's fragment
		//       shader and the runtime blits it, and Draw() cannot know which, so
		//       one barrier must cover both consumers. Declaring two readers in
		//       one pass is how that stays one barrier: two chained barriers whose
		//       second source is the first's DESTINATION access would order the
		//       stages without making the compute write visible to the second
		//       consumer.
		m_Graph.begin();
		const RgHandle target = m_Graph.importImage("RenderTarget", &image);
		const RgHandle lut    = m_Graph.importImage("BsdfEnergyLut", &m_BsdfLut);
		const RgHandle depth  = m_Graph.importImage("SceneDepth", &m_DepthImage);
		m_BsdfLutHandle = lut;
		m_DepthHandle   = depth;

		// The cluster grid. Imported so the graph derives the two barriers between
		// ClusterBuild, LightCull and the pass that reads the result -- a write
		// followed by a read of the same storage buffer in the same submission
		// needs one, and nothing else here would emit it.
		const RgHandle velocity = m_Graph.importImage("SceneVelocity", &m_VelocityImage);
		m_VelocityHandle = velocity;

		const RgHandle shadowAtlas = m_Graph.importImage("ShadowAtlas", &m_ShadowAtlas);
		m_ShadowAtlasHandle = shadowAtlas;

		const RgHandle bloomA = m_Graph.importImage("BloomA", &m_BloomA);
		const RgHandle bloomB = m_Graph.importImage("BloomB", &m_BloomB);
		const RgHandle taaHistoryA = m_Graph.importImage("TaaHistoryA", &m_TaaHistory[0]);
		const RgHandle taaHistoryB = m_Graph.importImage("TaaHistoryB", &m_TaaHistory[1]);
		m_TaaHistoryHandle = { taaHistoryA, taaHistoryB };
		m_BloomAHandle = bloomA;
		m_BloomBHandle = bloomB;

		const RgHandle clusterAabb  = m_Graph.importBuffer("ClusterAABBBuffer", &m_ClusterAABBSSBO);
		const RgHandle clusterGrid  = m_Graph.importBuffer("ClusterLightGrid", &m_ClusterLightGridSSBO);
		const RgHandle clusterIndex = m_Graph.importBuffer("ClusterLightIndices", &m_ClusterLightIndexSSBO);

		// ---- BSDF LUT BAKE, ONCE -------------------------------------------
		// Declared as a real graph pass rather than a special case before the
		// frame, which is what makes the write-then-read hazard the graph's
		// problem instead of a hand-written barrier. The shading pass below
		// declares the same resource as a ComputeRead, so the graph emits the
		// barrier between them; on every later frame the bake pass is simply
		// absent and the LUT is read with no barrier at all.
		if (!m_BsdfLutBaked) {
			if (VulkanComputePipeline* bakePipeline = GetOrLoadShader(ShaderType::BSDF_LUT_BAKE)) {
				auto& bakeRings = m_SetRings[ShaderType::BSDF_LUT_BAKE];
				m_Graph.addPass("BsdfLutBake")
					.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
					.readWrite(bloomA, RgUsage::ComputeReadWrite)
					.readWrite(bloomB, RgUsage::ComputeReadWrite)
					.read(shadowAtlas, RgUsage::DepthRead)
					.read(velocity, RgUsage::SampledRead)
					.write(lut, RgUsage::ComputeWrite)
					.read(depth, RgUsage::DepthRead)
					// The bake shader never touches the render target, but one
					// descriptor table serves every pipeline, so binding 0 of set
					// 0 is still a writable storage image that must be bound and
					// must therefore be in GENERAL. Declaring it is the honest
					// description of what this pass binds.
					.write(target, RgUsage::ComputeWrite)
					.execute([this, bakePipeline, &bakeRings](const FrameContext& f, const RgResources& res) {
						const std::array<VkDescriptorSet, kSetCount> sets =
							WriteCommonSets(f, res, bakeRings);
						bakePipeline->dispatch(f, sets,
							(kBsdfLutSize + kLocalSizeX - 1) / kLocalSizeX,
							(kBsdfLutSize + kLocalSizeY - 1) / kLocalSizeY,
							1);
					});
				m_BsdfLutBaked = true;
			}
		}

		// ---- DEPTH PREPASS ---------------------------------------------------
		// The first rasterized geometry in this engine. It lays depth down so the
		// forward pass can shade each pixel once, which is what mitigates
		// Forward+'s overdraw weakness.
		//
		// Runs only when the pipeline built. A missing DepthPrepass.slang.spv is a
		// content error the editor must survive, exactly as a missing compute
		// shader is.
		if (EnsureRasterPipelines() && !pScene->MeshEntityLookupTable.empty()) {
			m_Graph.addPass("DepthPrepass")
				.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
				.readWrite(bloomA, RgUsage::ComputeReadWrite)
				.readWrite(bloomB, RgUsage::ComputeReadWrite)
				.read(shadowAtlas, RgUsage::DepthRead)
				.read(velocity, RgUsage::SampledRead)
				// Clear to ZERO, not one: the projection is reverse-Z, so the far
				// plane is 0 and the depth test is GREATER. Clearing to 1 rejects
				// every fragment and draws nothing.
				.depthAttachment(depth, RgLoadOp::Clear, 0.0f)
				// ONE DESCRIPTOR TABLE SERVES EVERY PIPELINE, so this pass binds
				// the render target and the BSDF LUT even though its shader reads
				// neither -- and therefore has to declare them. Binding them
				// without declaring them is what the graph's own assert caught
				// here, for the second time; the invariant is doing its job.
				.write(target, RgUsage::ComputeWrite)
				.read(lut, RgUsage::ComputeRead)
				.execute([this, pScene](const FrameContext& f, const RgResources& res) {
					const std::array<VkDescriptorSet, kSetCount> sets =
						WriteCommonSets(f, res, m_DepthPrepassRings);
					// THE OPAQUE LIST ONLY. A transparent surface writing depth here
					// would make everything behind it fail the opaque pass's EQUAL
					// test and vanish outright -- the transparent object then reads
					// as an opaque hole in the scene.
					DrawGeometry(f, m_DepthPrepassPipeline, sets, pScene->DrawList,
					             VkExtent2D{ m_RenderSettings.resolution.x,
					                         m_RenderSettings.resolution.y });
				});
		}

		// ---- SHADOW CASCADES -------------------------------------------------
		// Runs in EVERY mode, not only Forward+, for the same reason the depth
		// prepass does: the debug view lives in the compute shading pass, so an
		// atlas only filled in raster mode could not be looked at.
		//
		// ONE PASS, FOUR SCISSORED DRAWS. Four separate passes would each want
		// their own attachment and the graph cannot name a sub-rect of one; one
		// pass that clears the whole atlas and then scissors per cascade needs
		// nothing the graph does not already do.
		if (m_ShadowDepthPipeline.valid() && m_ShadowCastersPresent && !pScene->DrawList.empty()) {
			m_Graph.addPass("ShadowDepth")
				.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
				.readWrite(bloomA, RgUsage::ComputeReadWrite)
				.readWrite(bloomB, RgUsage::ComputeReadWrite)
				// Clear to ZERO: reverse-Z, so 0 is the far plane and the depth
				// test is GREATER. Clearing to 1 rejects every caster and the
				// atlas stays empty -- which looks exactly like a pass that never
				// ran, and every surface comes out fully lit.
				.depthAttachment(shadowAtlas, RgLoadOp::Clear, 0.0f)
				.write(target, RgUsage::ComputeWrite)
				.read(lut, RgUsage::ComputeRead)
				.read(depth, RgUsage::DepthRead)
				.read(velocity, RgUsage::SampledRead)
				.execute([this, pScene](const FrameContext& f, const RgResources& res) {
					// shadowIsAttachment: the atlas is in DEPTH_ATTACHMENT_OPTIMAL
					// here, so set 0 binding 6 gets the dummy texture rather than
					// the atlas itself.
					const std::array<VkDescriptorSet, kSetCount> sets =
						WriteCommonSets(f, res, m_ShadowDepthRings,
						                /*targetIsAttachment=*/false,
						                /*velocityIsAttachment=*/false,
						                /*shadowIsAttachment=*/true);
					for (uint32_t c = 0; c < Gpu::SHADOW_CASCADES; ++c)
						DrawShadowCascade(f, sets, *pScene, c);
				});
		}

		// ---- CLUSTERED LIGHT CULLING ----------------------------------------
		// Two dispatches, one thread per cluster each, ahead of any shading.
		//
		// SPLIT INTO TWO PASSES rather than one that does both. The AABBs depend
		// only on the projection and the light assignment depends on the lights,
		// so keeping them separate is what will let the build be skipped when the
		// projection has not changed. It is not skipped yet -- 3456 threads of
		// arithmetic is not worth caching before the forward pass exists to show
		// whether it matters -- but the split is where the decision goes.
		if (VulkanComputePipeline* buildPipe = GetOrLoadShader(ShaderType::CLUSTER_BUILD)) {
			auto& buildRings = m_SetRings[ShaderType::CLUSTER_BUILD];
			m_Graph.addPass("ClusterBuild")
				.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
				.readWrite(bloomA, RgUsage::ComputeReadWrite)
				.readWrite(bloomB, RgUsage::ComputeReadWrite)
				.read(shadowAtlas, RgUsage::DepthRead)
				.read(velocity, RgUsage::SampledRead)
				.write(clusterAabb, RgUsage::ComputeWrite)
				// One descriptor table serves every pipeline, so this pass binds
				// the render target, the LUT and the depth buffer without reading
				// any of them -- and therefore has to declare them.
				.write(target, RgUsage::ComputeWrite)
				.read(lut, RgUsage::ComputeRead)
				.read(depth, RgUsage::DepthRead)
				.execute([this, buildPipe, &buildRings](const FrameContext& f, const RgResources& res) {
					const std::array<VkDescriptorSet, kSetCount> sets =
						WriteCommonSets(f, res, buildRings);
					buildPipe->dispatch(f, sets,
						(Gpu::CLUSTER_COUNT + kClusterLocalSize - 1) / kClusterLocalSize, 1, 1);
				});
		}

		if (VulkanComputePipeline* cullPipe = GetOrLoadShader(ShaderType::LIGHT_CULL)) {
			auto& cullRings = m_SetRings[ShaderType::LIGHT_CULL];
			m_Graph.addPass("LightCull")
				.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
				.readWrite(bloomA, RgUsage::ComputeReadWrite)
				.readWrite(bloomB, RgUsage::ComputeReadWrite)
				.read(shadowAtlas, RgUsage::DepthRead)
				.read(velocity, RgUsage::SampledRead)
				.read(clusterAabb, RgUsage::ComputeRead)
				.write(clusterGrid, RgUsage::ComputeWrite)
				.write(clusterIndex, RgUsage::ComputeWrite)
				.write(target, RgUsage::ComputeWrite)
				.read(lut, RgUsage::ComputeRead)
				.read(depth, RgUsage::DepthRead)
				.execute([this, cullPipe, &cullRings](const FrameContext& f, const RgResources& res) {
					const std::array<VkDescriptorSet, kSetCount> sets =
						WriteCommonSets(f, res, cullRings);
					cullPipe->dispatch(f, sets,
						(Gpu::CLUSTER_COUNT + kClusterLocalSize - 1) / kClusterLocalSize, 1, 1);
				});
		}

		// VELOCITY, before the shading pass rather than after it. Order does
		// not matter to correctness -- they write different attachments --
		// but running it first means a later pass could consume it in the
		// same frame without the graph having to reorder anything.
		if (m_VelocityPipeline.valid() && !pScene->DrawList.empty()) {
			m_Graph.addPass("Velocity")
				.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
				.readWrite(bloomA, RgUsage::ComputeReadWrite)
				.readWrite(bloomB, RgUsage::ComputeReadWrite)
				.read(shadowAtlas, RgUsage::DepthRead)
				// CLEAR, not load. A pixel with no geometry has no motion,
				// and zero is the value every consumer expects there.
				.colorAttachment(velocity, RgLoadOp::Clear, glm::vec4(0.0f))
				.depthAttachment(depth, RgLoadOp::Load)
				.write(target, RgUsage::ComputeWrite)
				.read(lut, RgUsage::ComputeRead)
				.execute([this, pScene](const FrameContext& f, const RgResources& res) {
					const std::array<VkDescriptorSet, kSetCount> sets =
						WriteCommonSets(f, res, m_VelocityRings,
						                /*targetIsAttachment=*/false,
						                /*velocityIsAttachment=*/true);
					DrawGeometry(f, m_VelocityPipeline, sets, pScene->DrawList,
					             VkExtent2D{ m_RenderSettings.resolution.x,
					                         m_RenderSettings.resolution.y });
				});
		}

		// ---- SHADING ---------------------------------------------------------
		// EITHER the compute path OR the raster path, never both -- they write the
		// same image. FORWARD is the real renderer; the compute shaders are the
		// reference it is validated against.
		const bool forwardMode = (m_CurrentShaderType == ShaderType::FORWARD);

		if (forwardMode) {
			// The background. The forward pass produces fragments only where
			// geometry is, so without this every missed pixel keeps the clear --
			// and the gate on this phase is that the whole frame agrees with the
			// reference, not just the parts with something in them.
			if (VulkanComputePipeline* skyPipe = GetOrLoadShader(ShaderType::SKYBOX_FILL)) {
				auto& skyRings = m_SetRings[ShaderType::SKYBOX_FILL];
				m_Graph.addPass("SkyboxFill")
					.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
					.readWrite(bloomA, RgUsage::ComputeReadWrite)
					.readWrite(bloomB, RgUsage::ComputeReadWrite)
					.read(shadowAtlas, RgUsage::DepthRead)
					.read(velocity, RgUsage::SampledRead)
					.write(target, RgUsage::ComputeWrite)
					.read(lut, RgUsage::ComputeRead)
					.read(depth, RgUsage::DepthRead)
					.execute([this, skyPipe, &skyRings](const FrameContext& f, const RgResources& res) {
						const std::array<VkDescriptorSet, kSetCount> sets =
							WriteCommonSets(f, res, skyRings);
						skyPipe->dispatch(f, sets,
							(m_RenderSettings.resolution.x + kLocalSizeX - 1) / kLocalSizeX,
							(m_RenderSettings.resolution.y + kLocalSizeY - 1) / kLocalSizeY,
							1);
					});
			}

			if (m_ForwardOpaquePipeline.valid() && !pScene->MeshEntityLookupTable.empty()) {
				m_Graph.addPass("ForwardOpaque")
					.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
					.readWrite(bloomA, RgUsage::ComputeReadWrite)
					.readWrite(bloomB, RgUsage::ComputeReadWrite)
					.read(shadowAtlas, RgUsage::DepthRead)
					.read(velocity, RgUsage::SampledRead)
					// LOAD, not clear: the skybox fill above is the background,
					// and clearing here would erase it.
					.colorAttachment(target, RgLoadOp::Load)
					// LOAD as well -- the prepass's depth is the whole point, and
					// the EQUAL test has nothing to compare against without it.
					.depthAttachment(depth, RgLoadOp::Load)
					.read(lut, RgUsage::ComputeRead)
					.read(clusterGrid, RgUsage::ComputeRead)
					.read(clusterIndex, RgUsage::ComputeRead)
					.execute([this, pScene](const FrameContext& f, const RgResources& res) {
						// targetIsAttachment: the target is in
						// COLOR_ATTACHMENT_OPTIMAL here, so set 0 binding 0 gets
						// the dummy storage image instead of it.
						const std::array<VkDescriptorSet, kSetCount> sets =
							WriteCommonSets(f, res, m_ForwardOpaqueRings, /*targetIsAttachment=*/true);
						DrawGeometry(f, m_ForwardOpaquePipeline, sets, pScene->DrawList,
						             VkExtent2D{ m_RenderSettings.resolution.x,
						                         m_RenderSettings.resolution.y });
					});
			}
			// ---- TRANSPARENT, after the opaque pass and back to front ---------
			if (m_ForwardTransparentPipeline.valid() && !pScene->TransparentDrawList.empty()) {
				m_Graph.addPass("ForwardTransparent")
					.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
					.readWrite(bloomA, RgUsage::ComputeReadWrite)
					.readWrite(bloomB, RgUsage::ComputeReadWrite)
					.read(shadowAtlas, RgUsage::DepthRead)
					.colorAttachment(target, RgLoadOp::Load)
					// LOAD and never written: the pipeline has depth write off, so
					// this tests against the opaque depth without disturbing it.
					.depthAttachment(depth, RgLoadOp::Load)
					.read(lut, RgUsage::ComputeRead)
					.read(velocity, RgUsage::SampledRead)
					.read(clusterGrid, RgUsage::ComputeRead)
					.read(clusterIndex, RgUsage::ComputeRead)
					.execute([this, pScene](const FrameContext& f, const RgResources& res) {
						const std::array<VkDescriptorSet, kSetCount> sets =
							WriteCommonSets(f, res, m_ForwardTransparentRings,
							                /*targetIsAttachment=*/true);
						DrawGeometry(f, m_ForwardTransparentPipeline, sets,
						             pScene->TransparentDrawList,
						             VkExtent2D{ m_RenderSettings.resolution.x,
						                         m_RenderSettings.resolution.y });
					});
			}
			// ---- BLOOM, BEFORE THE TONEMAP -----------------------------------
			// Bloom is light spilling in the lens, so it is added to the LIGHT.
			// Compositing it after the tonemap adds a fixed brightness whatever
			// the source radiance was, which is why post-tonemap bloom reads as
			// fog rather than as glare.
			if (m_RenderSettings.bloomEnabled) {
				if (VulkanComputePipeline* bloomPipe = GetOrLoadShader(ShaderType::BLOOM)) {
					// Created here rather than in GetOrLoadShader, which knows
					// nothing about how many passes a shader is used by.
					if (!m_BloomRings[0][0].valid()) {
						for (auto& perPass : m_BloomRings)
							for (uint32_t set = 0; set < kSetCount; ++set)
								perPass[set] = VulkanDescriptorSetRing(*m_Ctx, bloomPipe->setLayout(set));
					}
					// FOUR PASSES, ONE PIPELINE, selected by push constant.
					// Separate shaders would each need their own descriptor rings
					// and their own entry in the generated table for no gain --
					// they read and write the same three images.
					struct BloomStep { const char* name; uint32_t pass; bool halfRes; };
					static constexpr BloomStep kSteps[] = {
						{ "BloomExtract",   0, true  },
						{ "BloomBlurH",     1, true  },
						{ "BloomBlurV",     2, true  },
						{ "BloomComposite", 3, false },
					};
					for (uint32_t si = 0; si < 4; ++si) {
						const BloomStep& step = kSteps[si];
						auto& bloomRings = m_BloomRings[si];
						m_Graph.addPass(step.name)
							.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
							.readWrite(target, RgUsage::ComputeReadWrite)
							.readWrite(bloomA, RgUsage::ComputeReadWrite)
							.readWrite(bloomB, RgUsage::ComputeReadWrite)
							.read(lut, RgUsage::ComputeRead)
							.read(depth, RgUsage::DepthRead)
							.read(velocity, RgUsage::SampledRead)
							.read(shadowAtlas, RgUsage::DepthRead)
							.execute([this, bloomPipe, &bloomRings, step]
							         (const FrameContext& f, const RgResources& res) {
								const std::array<VkDescriptorSet, kSetCount> sets =
									WriteCommonSets(f, res, bloomRings);
								struct { uint32_t pass; float threshold, intensity, pad; } push{
									step.pass, m_RenderSettings.bloomThreshold,
									m_RenderSettings.bloomIntensity, 0.0f };
								bloomPipe->pushConstants(f, &push, sizeof(push));
								const uint32_t w = step.halfRes
									? glm::max(m_RenderSettings.resolution.x / 2u, 1u)
									: m_RenderSettings.resolution.x;
								const uint32_t h = step.halfRes
									? glm::max(m_RenderSettings.resolution.y / 2u, 1u)
									: m_RenderSettings.resolution.y;
								bloomPipe->dispatch(f, sets,
									(w + kLocalSizeX - 1) / kLocalSizeX,
									(h + kLocalSizeY - 1) / kLocalSizeY, 1);
							});
					}
				}
			}

			// ---- TONEMAP ------------------------------------------------------
			// Every raster pass above wrote LINEAR radiance so that the
			// transparent pass could composite correctly. This is where it
			// becomes displayable, and it must run after the last thing that
			// writes colour.
			if (VulkanComputePipeline* tonePipe = GetOrLoadShader(ShaderType::TONEMAP)) {
				auto& toneRings = m_SetRings[ShaderType::TONEMAP];
				m_Graph.addPass("Tonemap")
					.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
					.readWrite(bloomA, RgUsage::ComputeReadWrite)
					.readWrite(bloomB, RgUsage::ComputeReadWrite)
					.read(shadowAtlas, RgUsage::DepthRead)
					.readWrite(target, RgUsage::ComputeReadWrite)
					.read(lut, RgUsage::ComputeRead)
					.read(depth, RgUsage::DepthRead)
					.read(velocity, RgUsage::SampledRead)
					.execute([this, tonePipe, &toneRings](const FrameContext& f, const RgResources& res) {
						const std::array<VkDescriptorSet, kSetCount> sets =
							WriteCommonSets(f, res, toneRings);
						tonePipe->dispatch(f, sets,
							(m_RenderSettings.resolution.x + kLocalSizeX - 1) / kLocalSizeX,
							(m_RenderSettings.resolution.y + kLocalSizeY - 1) / kLocalSizeY,
							1);
					});
			}

			// ---- TAA RESOLVE, after the tonemap -------------------------------
			// Display-referred, deliberately: neighbourhood clamping in HDR lets
			// one very bright neighbour widen the box until it stops rejecting
			// anything, so the clamp silently stops working exactly where
			// ghosting is most visible.
			if (m_RenderSettings.taaEnabled) {
				if (VulkanComputePipeline* taaPipe = GetOrLoadShader(ShaderType::TAA)) {
					if (!m_TaaRings[0][0].valid())
						for (auto& perPass : m_TaaRings)
							for (uint32_t set = 0; set < kSetCount; ++set)
								perPass[set] = VulkanDescriptorSetRing(*m_Ctx, taaPipe->setLayout(set));

					for (uint32_t tp = 0; tp < 2; ++tp)
					m_Graph.addPass(tp == 0 ? "TaaResolve" : "TaaCopyBack")
						.readWrite(target, RgUsage::ComputeReadWrite)
						.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
						.readWrite(bloomA, RgUsage::ComputeReadWrite)
						.readWrite(bloomB, RgUsage::ComputeReadWrite)
						.read(lut, RgUsage::ComputeRead)
						.read(depth, RgUsage::DepthRead)
						.read(velocity, RgUsage::SampledRead)
						.read(shadowAtlas, RgUsage::DepthRead)
						.execute([this, taaPipe, tp](const FrameContext& f, const RgResources& res) {
							const std::array<VkDescriptorSet, kSetCount> sets =
								WriteCommonSets(f, res, m_TaaRings[tp]);
							const struct { uint32_t pass, a, b, c; } push{ tp, 0, 0, 0 };
							taaPipe->pushConstants(f, &push, sizeof(push));
							taaPipe->dispatch(f, sets,
								(m_RenderSettings.resolution.x + kLocalSizeX - 1) / kLocalSizeX,
								(m_RenderSettings.resolution.y + kLocalSizeY - 1) / kLocalSizeY,
								1);
						});
					// Valid from the NEXT frame: this pass is what fills it. The
					// slot flips so the image just written becomes next frame's
					// read source.
					m_TaaHistoryValid = true;
					m_TaaWriteSlot = 1u - m_TaaWriteSlot;
				}
			}


		}
		else m_Graph.addPass("Trace")
			.readWrite(taaHistoryA, RgUsage::ComputeReadWrite)
			.readWrite(taaHistoryB, RgUsage::ComputeReadWrite)
			.readWrite(bloomA, RgUsage::ComputeReadWrite)
			.readWrite(bloomB, RgUsage::ComputeReadWrite)
			.read(shadowAtlas, RgUsage::DepthRead)
			.read(velocity, RgUsage::SampledRead)
			.read(lut, RgUsage::ComputeRead)
			// debugModes 4 and 5 read the grid. Declared unconditionally rather
			// than only when a debug mode is on, because the declarations are what
			// the graph derives barriers from and a barrier that appears only in
			// debug builds is a hazard that only shows up in release.
			.read(clusterGrid, RgUsage::ComputeRead)
			.read(clusterIndex, RgUsage::ComputeRead)
			// Read so debugMode 3 can show it, and declared so the graph emits
			// the DEPTH_ATTACHMENT -> DEPTH_READ_ONLY transition after the
			// prepass wrote it.
			.read(depth, RgUsage::DepthRead)
			.readWrite(target, RgUsage::ComputeReadWrite)
			.execute([this, pipeline](const FrameContext& f, const RgResources& res) {
				const std::array<VkDescriptorSet, kSetCount> sets =
					WriteCommonSets(f, res, m_SetRings[m_CurrentShaderType]);

				// GROUP COUNTS, derived from the local sizes declared in the shader.
				pipeline->dispatch(f, sets,
					(m_RenderSettings.resolution.x + kLocalSizeX - 1) / kLocalSizeX,
					(m_RenderSettings.resolution.y + kLocalSizeY - 1) / kLocalSizeY,
					1);
			});

		// The consumers. An empty body -- this pass exists to DECLARE that the
		// image is read afterwards, which is what makes the graph emit the
		// post-dispatch barrier. Both readers sit in one pass so it stays one
		// barrier covering both.
		m_Graph.addPass("Present")
			.read(target, RgUsage::FragmentRead)
			.read(target, RgUsage::TransferRead);

		m_TargetHandle = target;
		m_Graph.compile(frame);
		m_Graph.execute(frame);

		// The dump is not free (it builds a string), so it is behind the same
		// debug toggle the heatmaps use rather than running every frame.
		if (m_RenderSettings.dumpRenderGraph) {
			LOG_ENGINE_INFO("\n{}", m_Graph.dump());
		}
	}
}
