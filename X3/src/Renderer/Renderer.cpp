#include "Renderer/Renderer.h"
#include <glm/gtc/matrix_access.hpp>
#include "Project/Scene/Scene.h"
#include "Project/Assets/AssetManager.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Core/Profiler.h"

// GENERATED from the Slang reflection output at build time -- see
// scripts/gen_descriptor_tables.py. Replaces the table that used to be
// hand-written at the top of this file and matched to the shader by comment.
#include "Renderer/Generated/DescriptorTables.h"

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
		m_MaterialSSBO         = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "MaterialSSBO");
		m_LightSSBO            = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "LightSSBO");
		m_MaterialExtSSBO      = VulkanRingBuffer(*m_Ctx, BufferKind::Storage, 0, "MaterialExtSSBO");

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

	VulkanImage* Renderer::Render(const FrameContext& frame,
		const Scene* scene, const AssetPool* assetPool,
		const glm::mat4* editorCameraTransform, float editorCameraFOV) {
		auto t = m_Profiler->timer("Renderer::Render()");

		if (!m_Ctx)
			return nullptr;

		const auto pScene = Parse(scene, assetPool, editorCameraTransform, editorCameraFOV);
		if (!pScene) { // Most likely scene missing camera
			return nullptr;
		}
		if (!SetupGPUResources(frame, pScene, scene, assetPool))
			return nullptr;

		Draw(frame, static_cast<uint32_t>(pScene->MeshEntityLookupTable.size()));

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
			lightData.params = glm::vec4(light.attenuation, glm::radians(light.innerConeAngle), glm::radians(light.outerConeAngle), 0.0f);

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
		}
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
			               | VK_IMAGE_USAGE_SAMPLED_BIT;       // editor ImGui viewport
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

		m_Cache.Resolution = m_RenderSettings.resolution;

		// increment acumulation
		m_Cache.AccumulatedFrames = (m_RenderSettings.accumulate) ? (m_Cache.AccumulatedFrames + 1) : 0;

		// --- Uniform rings ----------------------------------------------------
		const uint32_t entityCount = static_cast<uint32_t>(pScene->MeshEntityLookupTable.size());
		const uint32_t lightCount  = static_cast<uint32_t>(pScene->LightBuffer.size());

		SettingsUBOData settings{};
		settings.raysPerPixel          = m_RenderSettings.raysPerPixel;
		settings.bouncesPerRay         = m_RenderSettings.bouncesPerRay;
		settings.accumulatedFrames     = m_Cache.AccumulatedFrames;
		settings.entityCount           = entityCount;
		settings.debugMode             = m_RenderSettings.debugMode;
		settings.aabbHeatmapCutoff     = m_RenderSettings.aabbHeatmapCutoff;
		settings.triangleHeatmapCutoff = m_RenderSettings.triangleHeatmapCutoff;
		settings.lightCount            = lightCount;

		CameraUBOData camera{};
		camera.transform   = pScene->CameraTransform;
		camera.focalLength = pScene->CameraFocalLength;

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

	void Renderer::Draw(const FrameContext& frame, uint32_t /*entityCount*/) {
		auto t = m_Profiler->timer("Renderer::Draw()");

		// Switch shader if needed
		if (m_RenderSettings.shaderType != m_CurrentShaderType) {
			if (GetOrLoadShader(m_RenderSettings.shaderType)) {
				m_CurrentShaderType = m_RenderSettings.shaderType;
				m_Cache.AccumulatedFrames = 0; // Reset accumulation when switching shaders
			}
		}

		VulkanComputePipeline* pipeline = GetOrLoadShader(m_CurrentShaderType);
		if (!pipeline) {
			LOG_ENGINE_ERROR("No valid shader available for rendering");
			return;
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
		m_BsdfLutHandle = lut;

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
					.write(lut, RgUsage::ComputeWrite)
					// The bake shader never touches the render target, but one
					// descriptor table serves every pipeline, so binding 0 of set
					// 0 is still a writable storage image that must be bound and
					// must therefore be in GENERAL. Declaring it is the honest
					// description of what this pass binds.
					.write(target, RgUsage::ComputeWrite)
					.execute([this, bakePipeline, &ctx, &bakeRings](const FrameContext& f, const RgResources& res) {
						// The bake reads nothing but writes the LUT, yet every
						// binding in the layout must still be written -- one
						// table serves every pipeline, so the unused ones get
						// the context's dummies.
						const VulkanBuffer& dummy = ctx.dummyStorageBuffer();
						{
							DescriptorWriter w(ctx, bakeRings[0], f);
							w.storageImage(0, res.image(m_TargetHandle))
							 .sampledImage(1, m_SkyboxTexture.valid() ? m_SkyboxTexture : ctx.dummyTexture())
							 .sampledImageArray(2, m_TextureTable.descriptors())
							 .storageImage(3, res.image(m_BsdfLutHandle))
							 .flush();
						}
						{
							DescriptorWriter w(ctx, bakeRings[1], f);
							w.uniformBuffer(0, m_CameraUBO, f)
							 .uniformBuffer(1, m_SettingsUBO, f)
							 .flush();
						}
						{
							DescriptorWriter w(ctx, bakeRings[2], f);
							w.storageBuffer(0, m_MeshEntityLookupSSBO, f)
							 .storageBuffer(1, m_TransformSSBO, f)
							 .storageBuffer(2, m_MaterialSSBO, f)
							 .storageBuffer(3, m_TriPositionSSBO.valid()  ? m_TriPositionSSBO  : dummy)
							 .storageBuffer(4, m_NodeBufferSSBO.valid()   ? m_NodeBufferSSBO   : dummy)
							 .storageBuffer(5, m_BvhPrimIndexSSBO.valid() ? m_BvhPrimIndexSSBO : dummy)
							 .storageBuffer(6, m_LightSSBO, f)
							 .storageBuffer(7, m_TriRefSSBO.valid()       ? m_TriRefSSBO       : dummy)
							 .storageBuffer(8, m_VertexSSBO.valid()       ? m_VertexSSBO       : dummy)
							 .storageBuffer(9, m_MaterialExtSSBO, f)
							 .flush();
						}

						const std::array<VkDescriptorSet, kSetCount> sets = {
							bakeRings[0].get(f), bakeRings[1].get(f), bakeRings[2].get(f)
						};
						bakePipeline->dispatch(f, sets,
							(kBsdfLutSize + kLocalSizeX - 1) / kLocalSizeX,
							(kBsdfLutSize + kLocalSizeY - 1) / kLocalSizeY,
							1);
					});
				m_BsdfLutBaked = true;
			}
		}

		m_Graph.addPass("Trace")
			.read(lut, RgUsage::ComputeRead)
			.readWrite(target, RgUsage::ComputeReadWrite)
			.execute([this, pipeline, &ctx](const FrameContext& f, const RgResources& res) {
				VulkanImage& img = res.image(m_TargetHandle);
				auto& rings = m_SetRings[m_CurrentShaderType];

				// ONE DescriptorWriter per (set, frame), flushed before the first
				// bind of that set. ring.get(frame) is the only way to name a set,
				// so a set the GPU may still be reading is unnameable -- which is
				// the fix for VUID-vkUpdateDescriptorSets-None-03047.
				{
					DescriptorWriter w(ctx, rings[0], f);
					w.storageImage(0, img)
					 .sampledImage(1, m_SkyboxTexture.valid() ? m_SkyboxTexture : ctx.dummyTexture())
					 // EVERY element, every frame. There is no PARTIALLY_BOUND, so
					 // an element that was never written is undefined behaviour on
					 // access, not a validation error -- TextureTable fills unused
					 // slots with its dummy for exactly this reason.
					 .sampledImageArray(2, m_TextureTable.descriptors())
					 .storageImage(3, res.image(m_BsdfLutHandle))
					 .flush();
				}
				{
					DescriptorWriter w(ctx, rings[1], f);
					w.uniformBuffer(0, m_CameraUBO, f)
					 .uniformBuffer(1, m_SettingsUBO, f)
					 .flush();
				}
				{
					// Every binding, every frame -- flush() asserts completeness. An
					// empty scene still writes its bindings, falling back to the
					// context's dummy storage buffer for the device-local buffers
					// that have nothing in them yet.
					const VulkanBuffer& dummy = ctx.dummyStorageBuffer();
					DescriptorWriter w(ctx, rings[2], f);
					w.storageBuffer(0, m_MeshEntityLookupSSBO, f)
					 .storageBuffer(1, m_TransformSSBO, f)
					 .storageBuffer(2, m_MaterialSSBO, f)
					 .storageBuffer(3, m_TriPositionSSBO.valid()  ? m_TriPositionSSBO  : dummy)
					 .storageBuffer(4, m_NodeBufferSSBO.valid()   ? m_NodeBufferSSBO   : dummy)
					 .storageBuffer(5, m_BvhPrimIndexSSBO.valid() ? m_BvhPrimIndexSSBO : dummy)
					 .storageBuffer(6, m_LightSSBO, f)
					 .storageBuffer(7, m_TriRefSSBO.valid()       ? m_TriRefSSBO       : dummy)
					 .storageBuffer(8, m_VertexSSBO.valid()       ? m_VertexSSBO       : dummy)
					 // Usually empty -- only materials with a coat, sheen or
					 // anisotropy get an entry -- and still written every frame,
					 // because flush() asserts every binding was written exactly
					 // once. No shader reads it unless a material's flags.y names
					 // an index into it.
					 .storageBuffer(9, m_MaterialExtSSBO, f)
					 .flush();
				}

				const std::array<VkDescriptorSet, kSetCount> sets = {
					rings[0].get(f), rings[1].get(f), rings[2].get(f)
				};

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
