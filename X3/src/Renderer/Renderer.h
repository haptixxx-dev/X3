#pragma once

#include "lrpch.h"
#include "Renderer/RenderSettings.h"
#include "Core/GUID.h"
#include "EngineCfg.h"

// Forward declarations to reduce compilation dependencies
namespace X3 {
    class Scene;
    class AssetManager;
    class VulkanComputeShader;
    class VulkanTexture2D;
    class VulkanImage2D;
    class VulkanUniformBuffer;
    class VulkanShaderStorageBuffer;
    class Profiler;
    struct Material;
    struct AssetPool;
}

namespace X3 
{
	
	class Renderer {
	private:

		struct Cache {
			glm::uvec2 Resolution{0};
			uint32_t AccumulatedFrames = 0;
			LR_GUID prevSkyboxGuid = LR_GUID::INVALID;

			// SSBO size cache (to avoid recreating buffers every frame)
			uint32_t entityLookupSize = 0;
			uint32_t transformSize = 0;
			uint32_t materialSize = 0;
			uint32_t lightSize = 0;
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

		void Init();
		std::shared_ptr<VulkanImage2D> Render(const Scene* scene, const AssetPool* resourcePool,
			const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f);

	private:
		std::shared_ptr<const ParsedScene> Parse(const Scene* scene, const AssetPool* resourcePool,
			const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f) const;
		bool SetupGPUResources(std::shared_ptr<const ParsedScene> pScene, const Scene* scene, const AssetPool* resourcePool);
		void Draw(); // Draws directly to m_Frame
		std::shared_ptr<VulkanComputeShader> GetOrLoadShader(ShaderType type);


		std::shared_ptr<Profiler> m_Profiler;

		std::shared_ptr<VulkanComputeShader> m_CurrentShader;
		std::unordered_map<ShaderType, std::shared_ptr<VulkanComputeShader>> m_ShaderCache;

		// Double-buffered frame output to prevent GPU stalls
		// One frame is written by compute shader while the other is read by ImGui
		std::shared_ptr<VulkanImage2D> m_Frames[2];
		int m_WriteFrameIndex = 0;  // Index of frame currently being written to
		bool m_WasDoubleBuffering = false;  // Track mode transitions

		std::shared_ptr<VulkanTexture2D> m_SkyboxTexture;
		std::shared_ptr<VulkanUniformBuffer> m_CameraUBO, m_SettingsUBO;
		std::shared_ptr<VulkanShaderStorageBuffer> m_MeshEntityLookupSSBO, m_MeshBufferSSBO, m_NodeBufferSSBO, m_IndexBufferSSBO, m_MaterialSSBO, m_TransformSSBO, m_LightSSBO;

		Cache m_Cache;
		RenderSettings m_RenderSettings;
		std::unordered_map<ShaderType, std::filesystem::path> m_ShaderPaths = {
			{ShaderType::PATH_TRACING, EngineCfg::RESOURCES_PATH / "shaders" / "PathTracing.comp"},
			{ShaderType::PHONG, EngineCfg::RESOURCES_PATH / "shaders" / "Phong.comp"},
			{ShaderType::PBR, EngineCfg::RESOURCES_PATH / "shaders" / "PBR.comp"}
		};
	};
}
