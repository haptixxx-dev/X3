#pragma once

#include "lrpch.h"
#include "Renderer/RenderSettings.h"
#include "Core/GUID.h"
#include "EngineCfg.h"

#include "Renderer/GpuTypes.h"
#include "Renderer/RenderGraph.h"
#include "Renderer/TextureTable.h"
#include "Project/Assets/MaterialDesc.h"

#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanComputePipeline.h"
#include "Platform/Vulkan/VulkanDescriptors.h"
#include "Platform/Vulkan/VulkanGraphicsPipeline.h"
#include "Platform/Vulkan/VulkanImage.h"

// Forward declarations to reduce compilation dependencies
namespace X3 {
    class Scene;
    class AssetManager;
    class Profiler;
    class VulkanContext;
    struct AssetPool;
}

namespace X3
{

	class Renderer {
	private:
		// The three descriptor sets every compute shader in this engine declares.
		// Set 0 is images, set 1 uniforms, set 2 storage buffers, matching
		// res/shaders/Bindings.slang. The TABLE ITSELF is generated from Slang
		// reflection at build time (Renderer/Generated/DescriptorTables.h); only
		// the count is stated here, because it sizes a std::array member.
		static constexpr uint32_t kSetCount = 3;

		struct Cache {
			glm::uvec2 Resolution{0};
			uint32_t AccumulatedFrames = 0;
			LR_GUID prevSkyboxGuid = LR_GUID::INVALID;

			// Asset-pool versions, so the device-local buffers are re-uploaded only
			// when their source actually changed. These were function-local statics
			// in the old SetupGPUResources, which meant two Renderers would have
			// shared them.
			uint32_t triPositionVersion = 0;
			uint32_t nodeBufferVersion  = 0;
			uint32_t bvhPrimIndexVersion = 0;
			uint32_t triRefVersion      = 0;
			uint32_t vertexVersion      = 0;
			uint32_t textureVersion     = 0;
			bool     assetBuffersUploaded = false;
		};

		// MeshEntityHandle and LightData used to be declared here, as PRIVATE
		// NESTED types -- which is exactly why no static_assert could ever be
		// written against their layout. They now live in Renderer/GpuTypes.h with
		// the rest of the GPU mirror, as Gpu::MeshEntityHandle and Gpu::LightData.

		// std140 - 272 bytes. Mirrored by CameraUBO in res/shaders/GpuTypes.slang.
		//
		// transform + focalLength are all the path tracer ever needed. The
		// rasterizer needs view and proj, which are not derivable from a focal
		// length: the projection also encodes aspect and the depth range, and the
		// view matrix is transform's inverse, which a vertex shader should not be
		// computing per vertex.
		struct CameraUBOData {
			glm::mat4 transform;
			glm::mat4 view;
			glm::mat4 proj;
			glm::mat4 viewProj;
			float     focalLength;
			float     nearPlane;
			float     farPlane;
			float     _pad;
		};

		// Asserted for the same reason everything in GpuTypes.h is: a silent
		// layout drift here does not fail loudly, it shades with a transposed or
		// offset matrix and produces a picture that is merely wrong. This struct
		// went from 80 B to 272 B in Phase 7 with nothing checking it.
		//
		// No alignment asserts, matching GpuTypes.h -- this project's glm is
		// packed_highp, so alignof(mat4) is 4 and asserting the std140 alignment
		// would fail on a layout that is nonetheless correct.
		static_assert(sizeof(CameraUBOData) == 272);
		static_assert(offsetof(CameraUBOData, transform)   ==   0);
		static_assert(offsetof(CameraUBOData, view)        ==  64);
		static_assert(offsetof(CameraUBOData, proj)        == 128);
		static_assert(offsetof(CameraUBOData, viewProj)    == 192);
		static_assert(offsetof(CameraUBOData, focalLength) == 256);
		static_assert(offsetof(CameraUBOData, nearPlane)   == 260);
		static_assert(offsetof(CameraUBOData, farPlane)    == 264);

		// std140 - 32 bytes. Mirrored by SettingsUBO in res/shaders/GpuTypes.slang.
		struct SettingsUBOData {
			uint32_t raysPerPixel;
			uint32_t bouncesPerRay;
			uint32_t accumulatedFrames;
			uint32_t entityCount;
			uint32_t debugMode;
			uint32_t aabbHeatmapCutoff;
			uint32_t triangleHeatmapCutoff;
			uint32_t lightCount;
		};

		struct ParsedScene {
			std::vector<Gpu::MeshEntityHandle> MeshEntityLookupTable; // only renderable entities in the scene

			// TriPositionBuffer, TriRefBuffer, VertexBuffer, NodeBuffer and
			// BvhPrimIndexBuffer are stored in the AssetPool.
			//
			// The material list is FLATTENED AND VARIABLE-STRIDE: entity i's
			// materials occupy [materialBase, materialBase + materialSlotCount),
			// so it is no longer one entry per entity.
			//
			// AUTHORING form, not runtime form. Parse() is const and runs before
			// any frame exists, but resolving a texture GUID to a table index may
			// have to UPLOAD that texture, which needs a FrameContext. So Parse
			// collects MaterialDescs and SetupGPUResources converts them through
			// TextureTable::resolve().
			std::vector<MaterialDesc> MaterialDescs;
			std::vector<glm::mat4> TransformBuffer;
			std::vector<Gpu::LightData> LightBuffer;

			bool hasValidCamera = false;
			float CameraFocalLength = 0;
			glm::mat4 CameraTransform{};

			LR_GUID skyboxGUID = LR_GUID::INVALID;
		};

	public:
		Renderer(std::shared_ptr<Profiler> profiler)
			: m_Profiler(profiler) {
		};
		~Renderer() = default;

		inline void applySettings(RenderSettings renderSettings) { m_RenderSettings = renderSettings; }
		inline void ResetAccumulation() { m_Cache.AccumulatedFrames = 0; }

		// Creates the compute pipelines and their descriptor set rings. Runs out of
		// frame, from RenderLayer::onAttach.
		void Init();

		// Destroys everything this class owns. MUST run after vkDeviceWaitIdle,
		// which is what RenderLayer::onDetach guarantees -- pipelines and descriptor
		// set layouts are destroyed INLINE rather than deferred (see
		// VulkanComputePipeline.h), so a pending command buffer that bound one is
		// undefined behaviour rather than a caught error.
		void Shutdown();

		// Returns a NON-OWNING pointer to the image this frame was rendered into,
		// or nullptr if there is no camera. The Renderer owns the images for its
		// whole life and recreate() keeps the object address, so the pointer stays
		// valid across frames -- but its CONTENTS belong to the frame it was
		// returned for. Compare generation() before reusing anything derived from
		// it, which is what the editor's ImGui descriptor cache does.
		VulkanImage* Render(const FrameContext& frame,
			const Scene* scene, const AssetPool* resourcePool,
			const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f);

	private:
		std::shared_ptr<const ParsedScene> Parse(const Scene* scene, const AssetPool* resourcePool,
			const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f) const;
		bool SetupGPUResources(const FrameContext& frame, std::shared_ptr<const ParsedScene> pScene,
			const Scene* scene, const AssetPool* resourcePool);
		// Takes the parsed scene, not just a count: the depth prepass and every
		// raster pass after it need the per-entity triangle ranges to issue draws.
		void Draw(const FrameContext& frame, std::shared_ptr<const ParsedScene> pScene);

		// Creates the pipeline and its kSetCount descriptor set rings together, and
		// they are destroyed together in Shutdown(). That pairing is load-bearing:
		// every ring holds a raw pointer into its pipeline's layout vector.
		VulkanComputePipeline* GetOrLoadShader(ShaderType type);

		// Index of the image slot this frame writes. Accumulation pins it to 0 --
		// see the comment on the definition.
		uint32_t writeSlot(const FrameContext& frame) const;

		std::shared_ptr<Profiler> m_Profiler;
		VulkanContext* m_Ctx = nullptr;

		std::unordered_map<ShaderType, VulkanComputePipeline> m_Pipelines;
		std::unordered_map<ShaderType, std::array<VulkanDescriptorSetRing, kSetCount>> m_SetRings;
		ShaderType m_CurrentShaderType = ShaderType::PATH_TRACING;

		// One image per frame slot. The resolution is tracked PER SLOT, not once:
		// a slot may only be recreated on the frame that owns it, because the other
		// slot may still be referenced by a command buffer in flight.
		std::array<VulkanImage, FRAMES_IN_FLIGHT> m_Frames;
		std::array<glm::uvec2, FRAMES_IN_FLIGHT>  m_FrameResolutions{};

		VulkanTexture m_SkyboxTexture;

		// THE BSDF ENERGY LUT. Baked once, on the first frame that has a
		// FrameContext, then read by every shading pass for the rest of the
		// process. Content depends only on the BSDF code, so nothing invalidates
		// it at runtime -- a shader edit rebuilds the binary and restarts the
		// engine, which re-bakes.
		//
		// 32x32 is plenty: the functions it tabulates are smooth in both
		// roughness and NdotV, and the bilinear fetch in Bsdf.slang covers the
		// rest. It is a storage image rather than a sampled texture so one
		// binding serves both the bake and the reads.
		static constexpr uint32_t kBsdfLutSize = 64;
		VulkanImage m_BsdfLut;
		bool        m_BsdfLutBaked = false;

		// The material texture array bound at set 0 binding 2, plus the
		// MaterialDesc -> Gpu::Material conversion that fills in its indices.
		TextureTable m_TextureTable;

		// Lives for the Renderer's whole life, not per frame: its transient POOL
		// must survive across frames, or every transient image would be
		// reallocated every frame. Only the DECLARATIONS are rebuilt each frame,
		// by begin().
		RenderGraph m_Graph;
		// This frame's handle for the render target, so the pass body can reach it
		// through RgResources rather than closing over the image directly -- which
		// is what makes a pass body unable to touch a resource it did not declare.
		RgHandle m_TargetHandle = RgHandle::Invalid;
		RgHandle m_BsdfLutHandle = RgHandle::Invalid;

		// Written in full every frame, so they are rings: one slot per frame,
		// written only for the slot whose fence beginFrame() has waited.
		VulkanRingBuffer m_CameraUBO, m_SettingsUBO;
		VulkanRingBuffer m_MeshEntityLookupSSBO, m_MaterialSSBO, m_TransformSSBO, m_LightSSBO;
		// The second-tier material blocks. A ring like the others because it is
		// rebuilt in full every frame alongside m_MaterialSSBO -- and usually
		// empty, since only materials with a coat, sheen or anisotropy get one.
		VulkanRingBuffer m_MaterialExtSSBO;

		// Written only when the asset pool's version changes, so they are plain
		// device-local buffers uploaded through the frame's staging arena. A ring
		// would be wrong here: a growing ring discards every slot, so a buffer
		// written only on change would read garbage on the frame after a growth.
		VulkanBuffer m_TriPositionSSBO, m_NodeBufferSSBO, m_BvhPrimIndexSSBO;
		VulkanBuffer m_TriRefSSBO, m_VertexSSBO;

		// Flat triangle list for the rasterizer: three global vertex indices per
		// triangle, derived from TriRefBuffer at upload time. It carries
		// VK_BUFFER_USAGE_INDEX_BUFFER_BIT as well as storage usage, because
		// vkCmdBindIndexBuffer needs the former and the vertex shader reads it
		// through the latter -- the shader indexes it rather than relying on
		// SV_VertexID alone, so a draw's firstIndex is a plain offset.
		VulkanBuffer m_MeshIndexSSBO;

		// ---- Phase 7: the rasterizer -----------------------------------------
		// The engine had no graphics pipeline of any kind before this.
		static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
		VulkanGraphicsPipeline m_DepthPrepassPipeline;
		std::array<VulkanDescriptorSetRing, kSetCount> m_DepthPrepassRings;
		RgHandle m_DepthHandle = RgHandle::Invalid;
		VulkanImage m_DepthImage;
		glm::uvec2  m_DepthResolution{ 0 };

		/// Creates the raster pipelines. Separate from GetOrLoadShader because
		/// those are keyed by ShaderType, which is a user-facing choice of
		/// renderer; these are not selectable.
		bool EnsureRasterPipelines();

		/// One draw per entity into the currently-open rendering block.
		void DrawGeometry(const FrameContext& frame, const VulkanGraphicsPipeline& pipeline,
		                  std::span<const VkDescriptorSet> sets,
		                  const ParsedScene& pScene, VkExtent2D extent);

		Cache m_Cache;
		RenderSettings m_RenderSettings;
		std::unordered_map<ShaderType, std::filesystem::path> m_ShaderPaths = {
			{ShaderType::PATH_TRACING, EngineCfg::RESOURCES_PATH / "shaders" / "PathTracing.slang"},
			{ShaderType::PHONG, EngineCfg::RESOURCES_PATH / "shaders" / "Phong.slang"},
			{ShaderType::PBR, EngineCfg::RESOURCES_PATH / "shaders" / "PBR.slang"},
			{ShaderType::FURNACE_TEST, EngineCfg::RESOURCES_PATH / "shaders" / "FurnaceTest.slang"},
			{ShaderType::BSDF_LUT_BAKE, EngineCfg::RESOURCES_PATH / "shaders" / "BsdfLutBake.slang"}
		};
	};
}
