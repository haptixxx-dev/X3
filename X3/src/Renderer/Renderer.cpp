#include "Renderer/Renderer.h"
#include <glm/gtc/matrix_access.hpp>
#include "Project/Scene/Scene.h"
#include "Project/Assets/AssetManager.h"
#include "Renderer/IComputeShader.h"
#include "Renderer/ITexture2D.h"
#include "Renderer/IImage2D.h"
#include "Renderer/IUniformBuffer.h"
#include "Renderer/IShaderStorageBuffer.h"
#include "Core/Profiler.h"

namespace Laura 
{

	void Renderer::Init() {
		// fixed size from start
		m_CameraUBO = IUniformBuffer::Create(80, 0, BufferUsageType::DYNAMIC_DRAW);
		m_SettingsUBO = IUniformBuffer::Create(48, 1, BufferUsageType::DYNAMIC_DRAW);

		// work group sizes set in Draw() before shader->dispatch() 
		m_Shader = IComputeShader::Create(m_ComputeShaderPath.string(), glm::uvec3(1)); 
		if (!m_Shader) {
			LOG_ENGINE_CRITICAL("Unable to equip compute shader!");
			return;
		}
		m_Shader->Bind();
	}

	std::shared_ptr<IImage2D> Renderer::Render(const Scene* scene, const AssetPool* assetPool) {
		auto t = m_Profiler->timer("Renderer::Render()");

		const auto pScene = Parse(scene, assetPool);
		if (!pScene) { // Most likely scene missing camera
			return nullptr;
		}
		SetupGPUResources(pScene, scene, assetPool);
		Draw();
		return m_Frame;
	}

	std::shared_ptr<const Renderer::ParsedScene> Renderer::Parse(const Scene* scene, const AssetPool* assetPool) const {
		if (scene == nullptr) {
			return nullptr;
		}

		auto t = m_Profiler->timer("Renderer::Parse()");
		auto pScene = std::make_shared<Renderer::ParsedScene>();
	
		// MAIN CAMERA
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

		if (!pScene->hasValidCamera) {
			return nullptr;
		}
		
		// SKYBOX
		pScene->skyboxGUID = scene->skyboxGuid;
		
		// ENTITY HANDLES, TRANSFORMS & MATERIALS
		auto renderableView = scene->GetRegistry()->view<TransformComponent, MeshComponent>();
		pScene->MeshEntityLookupTable.reserve(renderableView.size_hint());
		pScene->TransformBuffer.reserve(renderableView.size_hint());
		pScene->MaterialBuffer.reserve(renderableView.size_hint());

		for (auto entity : renderableView) {
			EntityHandle e(entity, scene->GetRegistry());
			LR_GUID& guid = e.GetComponent<MeshComponent>().guid;
			std::shared_ptr<MeshMetadata> metadata = assetPool->find<MeshMetadata>(guid);
			if (!metadata) {
				continue;
			}
			
			// transform guaranteed by the view
			pScene->TransformBuffer.emplace_back(e.GetComponent<TransformComponent>().GetMatrix());

			// material not guaranteed
			if (e.HasComponent<MaterialComponent>()) {
				MaterialComponent& materialComponent = e.GetComponent<MaterialComponent>();
				pScene->MaterialBuffer.emplace_back(materialComponent.emission, materialComponent.color);
			} else {
				pScene->MaterialBuffer.emplace_back(); // default constructed material
			}

			pScene->MeshEntityLookupTable.emplace_back (
				metadata->firstTriIdx,
				metadata->TriCount,
				metadata->firstNodeIdx,
				metadata->nodeCount,
				pScene->TransformBuffer.size() - 1,
				pScene->MaterialBuffer.size() - 1
			);
		}
		return pScene;
	}

	// returns false if error occured, else true
	// assumes a valid pScene
	bool Renderer::SetupGPUResources(std::shared_ptr<const ParsedScene> pScene, const Scene* scene, const AssetPool* assetPool) {
		m_Profiler->timer("Renderer::SetupGPUResources()");

		// Update frame buffer if changed
		if (m_RenderSettings.resolution != m_Cache.Resolution) {
			m_Frame = IImage2D::Create(nullptr, m_RenderSettings.resolution.x, m_RenderSettings.resolution.y, 0, Image2DType::LR_READ_WRITE);
			m_Cache.Resolution = m_RenderSettings.resolution;
		}

		// increment acumulation
		m_Cache.AccumulatedFrames = (m_RenderSettings.accumulate) ? (m_Cache.AccumulatedFrames + 1) : 0;

		// UBOs

		// SETTINGS
		uint32_t entityCount = pScene->MeshEntityLookupTable.size();
		m_SettingsUBO->Bind();
		m_SettingsUBO->AddData(0, sizeof(uint32_t), &m_RenderSettings.raysPerPixel);
		m_SettingsUBO->AddData(4, sizeof(uint32_t), &m_RenderSettings.bouncesPerRay);
		m_SettingsUBO->AddData(8, sizeof(uint32_t), &m_Cache.AccumulatedFrames);
		m_SettingsUBO->AddData(12, sizeof(uint32_t), &entityCount);
		m_SettingsUBO->AddData(16, sizeof(uint32_t), &m_RenderSettings.debugMode);
		m_SettingsUBO->AddData(20, sizeof(uint32_t), &m_RenderSettings.aabbHeatmapCutoff);
		m_SettingsUBO->AddData(24, sizeof(uint32_t), &m_RenderSettings.triangleHeatmapCutoff);
		m_SettingsUBO->Unbind();

		// CAMERA
		m_CameraUBO->Bind();
		m_CameraUBO->AddData(0, sizeof(glm::mat4), &pScene->CameraTransform);
		m_CameraUBO->AddData(64, sizeof(float), &pScene->CameraFocalLength);
		m_CameraUBO->Unbind();


		// Update SKYBOX texture if guid changed 
		if (scene && scene->skyboxGuid != m_Cache.prevSkyboxGuid) {
			m_Cache.prevSkyboxGuid = scene->skyboxGuid;
			auto metadata = assetPool->find<TextureMetadata>(pScene->skyboxGUID);
			if (metadata) {
				const uint32_t SKYBOX_TEXTURE_UNIT = 1;
				const unsigned char* data = &assetPool->TextureBuffer[metadata->texStartIdx];
				m_SkyboxTexture = ITexture2D::Create(data, metadata->width, metadata->height, SKYBOX_TEXTURE_UNIT);
			}
			else {
				m_SkyboxTexture = nullptr;
			}
		}

		// SSBOs - UPDATED EVERY FRAME 

		{
			// EntityLookupTable - BINDING POINT 0
			uint32_t sizeBytes = sizeof(MeshEntityHandle) * pScene->MeshEntityLookupTable.size();
			m_MeshEntityLookupSSBO = IShaderStorageBuffer::Create(sizeBytes, 0, BufferUsageType::DYNAMIC_DRAW);
			m_MeshEntityLookupSSBO->Bind();
			m_MeshEntityLookupSSBO->AddData(0, sizeBytes, pScene->MeshEntityLookupTable.data());
			m_MeshEntityLookupSSBO->Unbind();
		}
		{
			// Transforms - BINDING POINT 1
			uint32_t sizeBytes = sizeof(glm::mat4) * pScene->TransformBuffer.size();
			m_TransformSSBO = IShaderStorageBuffer::Create(sizeBytes, 1, BufferUsageType::DYNAMIC_DRAW);
			m_TransformSSBO->Bind();
			m_TransformSSBO->AddData(0, sizeBytes, pScene->TransformBuffer.data());
			m_TransformSSBO->Unbind();
		}
		{
			// Materials - BINDING POINT 2
			uint32_t sizeBytes = sizeof(Material) * pScene->MaterialBuffer.size();
			m_MaterialSSBO = IShaderStorageBuffer::Create(sizeBytes, 2, BufferUsageType::DYNAMIC_DRAW);
			m_MaterialSSBO->Bind();
			m_MaterialSSBO->AddData(0, sizeBytes, pScene->MaterialBuffer.data());
			m_MaterialSSBO->Unbind();
		}

		// SSBOs - UPDATED ON CHANGE 

		static uint32_t prevMeshBuffVersion = 0;
		static uint32_t prevNodeBuffVersion = 0;
		static uint32_t prevIndexBuffVersion = 0;
		static uint32_t prevSkyboxTextureVersion = 0;

		// Mesh Buffer - BINDING POINT 3
		{
    		uint32_t currMeshBuffVersion = assetPool->GetUpdateVersion(AssetPool::AssetType::MeshBuffer);
    		if (prevMeshBuffVersion != currMeshBuffVersion) {
        		prevMeshBuffVersion = currMeshBuffVersion;

        		uint32_t meshBuffer_sizeBytes = sizeof(Triangle) * assetPool->MeshBuffer.size();
        		m_MeshBufferSSBO = IShaderStorageBuffer::Create(meshBuffer_sizeBytes, 3, BufferUsageType::STATIC_DRAW);
        		m_MeshBufferSSBO->Bind();
        		m_MeshBufferSSBO->AddData(0, meshBuffer_sizeBytes, assetPool->MeshBuffer.data());
        		m_MeshBufferSSBO->Unbind();
    		}
		}

		// Node Buffer - BINDING POINT 4
		{
    		uint32_t currNodeBuffVersion = assetPool->GetUpdateVersion(AssetPool::AssetType::NodeBuffer);
    		if (prevNodeBuffVersion != currNodeBuffVersion) {
        		prevNodeBuffVersion = currNodeBuffVersion;

        		uint32_t nodeBuffer_sizeBytes = sizeof(BVHAccel::Node) * assetPool->NodeBuffer.size();
        		m_NodeBufferSSBO = IShaderStorageBuffer::Create(nodeBuffer_sizeBytes, 4, BufferUsageType::STATIC_DRAW);
        		m_NodeBufferSSBO->Bind();
        		m_NodeBufferSSBO->AddData(0, nodeBuffer_sizeBytes, assetPool->NodeBuffer.data());
        		m_NodeBufferSSBO->Unbind();
    		}
		}

		// Index Buffer - BINDING POINT 5 
		{
    		uint32_t currIndexBuffVersion = assetPool->GetUpdateVersion(AssetPool::AssetType::IndexBuffer);
    		if (prevIndexBuffVersion != currIndexBuffVersion) {
        		prevIndexBuffVersion = currIndexBuffVersion;

        		uint32_t indexBuffer_sizeBytes = sizeof(uint32_t) * assetPool->IndexBuffer.size();
        		m_IndexBufferSSBO = IShaderStorageBuffer::Create(indexBuffer_sizeBytes, 5, BufferUsageType::STATIC_DRAW);
        		m_IndexBufferSSBO->Bind();
        		m_IndexBufferSSBO->AddData(0, indexBuffer_sizeBytes, assetPool->IndexBuffer.data());
        		m_IndexBufferSSBO->Unbind();
    		}
		}

		return true;
	}

	void Renderer::Draw() {
		auto t = m_Profiler->timer("Renderer::Draw()");
		m_Shader->Bind();
		m_Shader->setWorkGroupSizes(glm::uvec3(
			(m_RenderSettings.resolution.x + 7) / 8,
			(m_RenderSettings.resolution.y + 3) / 4,
			1
		  ));
		m_Shader->Dispatch();
	}
}