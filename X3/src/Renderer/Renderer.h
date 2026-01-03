#pragma once

#include "lrpch.h"
#include "Renderer/RenderSettings.h"
#include "Renderer/IRendererAPI.h"
#include "Core/GUID.h"
#include "EngineCfg.h"

// Forward declarations to reduce compilation dependencies
namespace X3 {
    class Scene;
    class AssetManager;
    class IComputeShader;
    class ITexture2D;
    class IImage2D;
    class IUniformBuffer;
    class IShaderStorageBuffer;
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

			// Denoise frame counter
			uint32_t denoiseFrameCount = 0;
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
			glm::mat4 ViewProjMatrix{};  // For denoising reprojection

			LR_GUID skyboxGUID = LR_GUID::INVALID;
		};

	public:
		Renderer(std::shared_ptr<Profiler> profiler)
			: m_Profiler(profiler) {
		};
		~Renderer() = default;

		inline static IRendererAPI::API GetAPI() { return IRendererAPI::GetAPI(); } // getter
		inline static void SetAPI(IRendererAPI::API api) { IRendererAPI::SetAPI(api); } // setter
		inline void applySettings(RenderSettings renderSettings) { m_RenderSettings = renderSettings; }
		inline void ResetAccumulation() { m_Cache.AccumulatedFrames = 0; }

		void Init();
		std::shared_ptr<IImage2D> Render(const Scene* scene, const AssetPool* resourcePool,
			const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f);

	private:
		std::shared_ptr<const ParsedScene> Parse(const Scene* scene, const AssetPool* resourcePool,
			const glm::mat4* editorCameraTransform = nullptr, float editorCameraFOV = 90.0f) const;
		bool SetupGPUResources(std::shared_ptr<const ParsedScene> pScene, const Scene* scene, const AssetPool* resourcePool);
		void Draw(); // Draws directly to m_Frame
		void DenoisePass(std::shared_ptr<const ParsedScene> pScene); // Apply denoising
		std::shared_ptr<IComputeShader> GetOrLoadShader(ShaderType type);


		std::shared_ptr<Profiler> m_Profiler;

		std::shared_ptr<IComputeShader> m_CurrentShader;
		std::unordered_map<ShaderType, std::shared_ptr<IComputeShader>> m_ShaderCache;

		// Double-buffered frame output to prevent GPU stalls
		// One frame is written by compute shader while the other is read by ImGui
		std::shared_ptr<IImage2D> m_Frames[2];
		int m_WriteFrameIndex = 0;  // Index of frame currently being written to
		bool m_WasDoubleBuffering = false;  // Track mode transitions

		std::shared_ptr<ITexture2D> m_SkyboxTexture;
		std::shared_ptr<IUniformBuffer> m_CameraUBO, m_SettingsUBO;
		std::shared_ptr<IShaderStorageBuffer> m_MeshEntityLookupSSBO, m_MeshBufferSSBO, m_NodeBufferSSBO, m_IndexBufferSSBO, m_MaterialSSBO, m_TransformSSBO, m_LightSSBO, m_UVBufferSSBO;

		// Denoising resources
		std::shared_ptr<IImage2D> m_GBuffer;              // Current frame G-buffer (normal + depth)
		std::shared_ptr<IImage2D> m_HistoryColor;         // Previous denoised frame
		std::shared_ptr<IImage2D> m_HistoryGBuffer;       // Previous frame G-buffer
		std::shared_ptr<IImage2D> m_DenoisedOutput;       // Current frame denoised output
		std::shared_ptr<IImage2D> m_AtrousPingPong;       // Ping-pong buffer for à-trous passes
		std::shared_ptr<IUniformBuffer> m_DenoiseUBO;
		glm::mat4 m_PrevViewProjMatrix{1.0f};
		glm::vec3 m_PrevCameraPos{0.0f};

		Cache m_Cache;
		RenderSettings m_RenderSettings;
		std::unordered_map<ShaderType, std::filesystem::path> m_ShaderPaths = {
			{ShaderType::PATH_TRACING, EngineCfg::RESOURCES_PATH / "shaders" / "PathTracing.comp"},
			{ShaderType::PHONG, EngineCfg::RESOURCES_PATH / "shaders" / "Phong.comp"},
			{ShaderType::PBR, EngineCfg::RESOURCES_PATH / "shaders" / "PBR.comp"},
			{ShaderType::DENOISE, EngineCfg::RESOURCES_PATH / "shaders" / "Denoise.comp"}
		};
	};
}
