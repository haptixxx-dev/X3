#pragma once

#include "lrpch.h"
#include "Renderer/RenderSettings.h"
#include "Renderer/IRendererAPI.h"
#include "Core/GUID.h"
#include "EngineCfg.h"

// Forward declarations to reduce compilation dependencies
namespace Laura {
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

namespace Laura 
{
	
	class Renderer {
	private:

		struct Cache {
			glm::uvec2 Resolution{0};
			uint32_t AccumulatedFrames = 0;
			LR_GUID prevSkyboxGuid = LR_GUID::INVALID;
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

		struct ParsedScene {
			std::vector<MeshEntityHandle> MeshEntityLookupTable; // only renderable entities in the scene

			// MeshBuffer, NodeBuffer & IndexBuffer are stored in the AssetPool
			std::vector<Material> MaterialBuffer;
			std::vector<glm::mat4> TransformBuffer;

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

		inline static IRendererAPI::API GetAPI() { return IRendererAPI::GetAPI(); } // getter
		inline static void SetAPI(IRendererAPI::API api) { IRendererAPI::SetAPI(api); } // setter
		inline void applySettings(RenderSettings renderSettings) { m_RenderSettings = renderSettings; }

		void Init();
		std::shared_ptr<IImage2D> Render(const Scene* scene, const AssetPool* resourcePool);

	private:
		std::shared_ptr<const ParsedScene> Parse(const Scene* scene, const AssetPool* resourcePool) const;
		bool SetupGPUResources(std::shared_ptr<const ParsedScene> pScene, const Scene* scene, const AssetPool* resourcePool);
		void Draw(); // Draws directly to m_Frame


		std::shared_ptr<Profiler> m_Profiler;

		std::shared_ptr<IComputeShader> m_Shader;
		std::shared_ptr<IImage2D> m_Frame;
		std::shared_ptr<ITexture2D> m_SkyboxTexture;
		std::shared_ptr<IUniformBuffer> m_CameraUBO, m_SettingsUBO;
		std::shared_ptr<IShaderStorageBuffer> m_MeshEntityLookupSSBO, m_MeshBufferSSBO, m_NodeBufferSSBO, m_IndexBufferSSBO, m_MaterialSSBO, m_TransformSSBO;
		
		Cache m_Cache;
		RenderSettings m_RenderSettings;
		std::filesystem::path m_ComputeShaderPath = EngineCfg::RESOURCES_PATH / "Shaders" / "PathTracing.comp";
	};
}