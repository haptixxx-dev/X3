#pragma once

#include "lrpch.h"
#include "Renderer/RenderSettings.h"
#include "Core/GUID.h"
#include "EngineCfg.h"

#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanComputePipeline.h"
#include "Platform/Vulkan/VulkanDescriptors.h"
#include "Platform/Vulkan/VulkanImage.h"

// Forward declarations to reduce compilation dependencies
namespace X3 {
    class Scene;
    class AssetManager;
    class Profiler;
    class VulkanContext;
    struct Material;
    struct AssetPool;
}

namespace X3
{

	class Renderer {
	private:
		// The three descriptor sets every compute shader in this engine declares.
		// Set 0 is images, set 1 uniforms, set 2 storage buffers -- matching the
		// SET(n) macros in res/shaders/*.comp. Until Phase 3's Slang reflection
		// generates this table, it is hand-synced with the GLSL and a mismatch is
		// caught by DescriptorWriter::flush()'s completeness assert.
		static constexpr uint32_t kSetCount = 3;

		struct Cache {
			glm::uvec2 Resolution{0};
			uint32_t AccumulatedFrames = 0;
			LR_GUID prevSkyboxGuid = LR_GUID::INVALID;

			// Asset-pool versions, so the device-local buffers are re-uploaded only
			// when their source actually changed. These were function-local statics
			// in the old SetupGPUResources, which meant two Renderers would have
			// shared them.
			uint32_t meshBufferVersion  = 0;
			uint32_t nodeBufferVersion  = 0;
			uint32_t indexBufferVersion = 0;
			bool     assetBuffersUploaded = false;
		};

		// Under the std430 - 24 bytes
		struct MeshEntityHandle {
			uint32_t FirstTriIdx = 0;
			uint32_t TriCount = 0;
			uint32_t FirstNodeIdx = 0;
			uint32_t NodeCount = 0;
			uint32_t TransformIdx = 0;
			uint32_t MaterialIdx = 0;

			MeshEntityHandle(uint32_t firstTriIdx, uint32_t triCount,
							 uint32_t firstNodeIdx, uint32_t nodeCount,
							 uint32_t transformIdx, uint32_t materialIdx)
				: FirstTriIdx(firstTriIdx), TriCount(triCount),
				  FirstNodeIdx(firstNodeIdx), NodeCount(nodeCount),
				  TransformIdx(transformIdx), MaterialIdx(materialIdx) {}
		};

		// std430 - 64 bytes
		struct LightData {
			glm::vec4 position;    // xyz: position (for point/spot), w: type (0=directional, 1=point, 2=spot)
			glm::vec4 direction;   // xyz: direction (for directional/spot), w: intensity
			glm::vec4 color;       // xyz: color, w: range
			glm::vec4 params;      // x: attenuation, y: innerConeAngle, z: outerConeAngle, w: padding
		};

		// std140 - 80 bytes. Mirrored by CameraUBO in res/shaders/*.comp.
		struct CameraUBOData {
			glm::mat4 transform;
			float     focalLength;
			float     _pad[3];
		};

		// std140 - 32 bytes. Mirrored by SettingsUBO in res/shaders/*.comp.
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
			std::vector<MeshEntityHandle> MeshEntityLookupTable; // only renderable entities in the scene

			// MeshBuffer, NodeBuffer & IndexBuffer are stored in the AssetPool
			std::vector<Material> MaterialBuffer;
			std::vector<glm::mat4> TransformBuffer;
			std::vector<LightData> LightBuffer;

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
		void Draw(const FrameContext& frame, uint32_t entityCount);

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

		// Written in full every frame, so they are rings: one slot per frame,
		// written only for the slot whose fence beginFrame() has waited.
		VulkanRingBuffer m_CameraUBO, m_SettingsUBO;
		VulkanRingBuffer m_MeshEntityLookupSSBO, m_MaterialSSBO, m_TransformSSBO, m_LightSSBO;

		// Written only when the asset pool's version changes, so they are plain
		// device-local buffers uploaded through the frame's staging arena. A ring
		// would be wrong here: a growing ring discards every slot, so a buffer
		// written only on change would read garbage on the frame after a growth.
		VulkanBuffer m_MeshBufferSSBO, m_NodeBufferSSBO, m_IndexBufferSSBO;

		Cache m_Cache;
		RenderSettings m_RenderSettings;
		std::unordered_map<ShaderType, std::filesystem::path> m_ShaderPaths = {
			{ShaderType::PATH_TRACING, EngineCfg::RESOURCES_PATH / "shaders" / "PathTracing.comp"},
			{ShaderType::PHONG, EngineCfg::RESOURCES_PATH / "shaders" / "Phong.comp"},
			{ShaderType::PBR, EngineCfg::RESOURCES_PATH / "shaders" / "PBR.comp"}
		};
	};
}
