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

namespace X3 
{

	void Renderer::Init() {
		// fixed size from start
		m_CameraUBO = IUniformBuffer::Create(80, 0, BufferUsageType::DYNAMIC_DRAW);
		m_SettingsUBO = IUniformBuffer::Create(64, 1, BufferUsageType::DYNAMIC_DRAW); // increased to 64 to accommodate lightCount
		
		// Denoise UBO: 3 mat4 (192) + 2 vec4 (32) + 12 floats/ints (48) = 272 bytes, round to 288
		m_DenoiseUBO = IUniformBuffer::Create(288, 2, BufferUsageType::DYNAMIC_DRAW);

		// Load the default shader (path tracing)
		m_CurrentShader = GetOrLoadShader(ShaderType::PATH_TRACING);
		if (!m_CurrentShader) {
			LOG_ENGINE_CRITICAL("Unable to equip compute shader!");
			return;
		}
		m_CurrentShader->Bind();
	}

	std::shared_ptr<IComputeShader> Renderer::GetOrLoadShader(ShaderType type) {
		// Check if shader is already cached and valid
		auto it = m_ShaderCache.find(type);
		if (it != m_ShaderCache.end() && it->second && it->second->GetID() != 0) {
			return it->second;
		}

		// Load the shader
		auto pathIt = m_ShaderPaths.find(type);
		if (pathIt == m_ShaderPaths.end()) {
			LOG_ENGINE_ERROR("Shader path not found for shader type: {}", static_cast<int>(type));
			return nullptr;
		}

		auto shader = IComputeShader::Create(pathIt->second.string(), glm::uvec3(1));
		if (!shader || !shader->IsValid()) {
			LOG_ENGINE_ERROR("Failed to create or compile shader: {}", pathIt->second.string());
			return nullptr;  // Don't cache invalid shaders
		}
		

		// Cache it
		m_ShaderCache[type] = shader;
		return shader;
	}

	std::shared_ptr<IImage2D> Renderer::Render(const Scene* scene, const AssetPool* assetPool,
		const glm::mat4* editorCameraTransform, float editorCameraFOV) {
		auto t = m_Profiler->timer("Renderer::Render()");

		const auto pScene = Parse(scene, assetPool, editorCameraTransform, editorCameraFOV);
		if (!pScene) { // Most likely scene missing camera
			return nullptr;
		}
		SetupGPUResources(pScene, scene, assetPool);
		Draw();

		// Apply denoising if enabled (only for path tracing)
		std::shared_ptr<IImage2D> outputFrame = m_Frames[m_WriteFrameIndex];
		if (m_RenderSettings.enableDenoise && m_RenderSettings.shaderType == ShaderType::PATH_TRACING) {
			DenoisePass(pScene);
			outputFrame = m_DenoisedOutput;
			
			// Update history for next frame (swap current denoised -> history)
			std::swap(m_HistoryColor, m_DenoisedOutput);
			std::swap(m_HistoryGBuffer, m_GBuffer);
			m_PrevViewProjMatrix = pScene->ViewProjMatrix;
			m_PrevCameraPos = glm::vec3(pScene->CameraTransform[3]);
			m_Cache.denoiseFrameCount++;
		} else {
			m_Cache.denoiseFrameCount = 0; // Reset when denoise is disabled
		}

		// Double-buffering is only used when explicitly enabled (e.g., during runtime/play mode)
		// AND when not accumulating (accumulation requires reading from the same buffer)
		// AND only for path tracing (other shaders don't benefit from it)
		bool canDoubleBuffer = m_RenderSettings.useDoubleBuffering
			&& !m_RenderSettings.accumulate
			&& m_RenderSettings.shaderType == ShaderType::PATH_TRACING;

		if (!canDoubleBuffer) {
			m_WasDoubleBuffering = false;
			// Single buffer mode - return the frame we just processed
			return outputFrame;
		}

		// Handle transition into double-buffer mode
		// On the first frame, the "other" buffer is stale, so return current buffer
		// but still swap so that next frame writes to the other buffer
		if (!m_WasDoubleBuffering) {
			m_WasDoubleBuffering = true;
			// Swap for next frame so frame 2 writes to buffer 1 while we can return buffer 0
			m_WriteFrameIndex = 1 - m_WriteFrameIndex;
			return outputFrame;
		}

		// Double-buffer swap: return the frame written LAST frame (guaranteed complete)
		// while compute shader writes to the current frame
		int readFrameIndex = 1 - m_WriteFrameIndex;
		bool usingDenoise = m_RenderSettings.enableDenoise && m_RenderSettings.shaderType == ShaderType::PATH_TRACING;
		auto result = (usingDenoise && m_HistoryColor) ? m_HistoryColor : m_Frames[readFrameIndex];

		// Swap for next frame
		m_WriteFrameIndex = readFrameIndex;

		return result;
	}

	std::shared_ptr<const Renderer::ParsedScene> Renderer::Parse(const Scene* scene, const AssetPool* assetPool,
		const glm::mat4* editorCameraTransform, float editorCameraFOV) const {
		if (scene == nullptr) {
			LOG_ENGINE_WARN("Parse: scene is nullptr");
			return nullptr;
		}

		auto t = m_Profiler->timer("Renderer::Parse()");
		auto pScene = std::make_shared<Renderer::ParsedScene>();

		// Use editor camera if provided, otherwise use scene's main camera)
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

		// Compute View-Projection matrix for denoising reprojection
		// View matrix is inverse of camera transform
		glm::mat4 viewMatrix = glm::inverse(pScene->CameraTransform);
		// Simple projection matrix (matches the ray generation in PathTracing.comp)
		float aspect = static_cast<float>(m_RenderSettings.resolution.x) / static_cast<float>(m_RenderSettings.resolution.y);
		float fov = 2.0f * atan(1.0f / pScene->CameraFocalLength);
		glm::mat4 projMatrix = glm::perspective(fov, aspect, 0.1f, 1000.0f);
		pScene->ViewProjMatrix = projMatrix * viewMatrix;
		
		// SKYBOX
		pScene->skyboxGUID = scene->skyboxGuid;

		// LIGHTS
		auto lightView = scene->GetRegistry()->view<TransformComponent, LightComponent>();
		pScene->LightBuffer.reserve(lightView.size_hint());
		for (auto entity : lightView) {
			EntityHandle e(entity, scene->GetRegistry());
			const auto& transform = e.GetComponent<TransformComponent>();
			const auto& light = e.GetComponent<LightComponent>();

			LightData lightData;
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
				Material mat;
				mat.emission = materialComponent.emission;
				mat.color = materialComponent.color;
				mat.pbrParams = glm::vec4(materialComponent.metallic, materialComponent.roughness, materialComponent.ao, 0.0f);
				pScene->MaterialBuffer.emplace_back(mat);
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

		// Update frame buffers if resolution changed (double-buffered)
		if (m_RenderSettings.resolution != m_Cache.Resolution) {
			m_Frames[0] = IImage2D::Create(nullptr, m_RenderSettings.resolution.x, m_RenderSettings.resolution.y, 0, Image2DType::LR_READ_WRITE);
			m_Frames[1] = IImage2D::Create(nullptr, m_RenderSettings.resolution.x, m_RenderSettings.resolution.y, 0, Image2DType::LR_READ_WRITE);
			
			// Clear denoise buffers - they'll be recreated if needed
			m_GBuffer = nullptr;
			m_HistoryColor = nullptr;
			m_HistoryGBuffer = nullptr;
			m_DenoisedOutput = nullptr;
			m_AtrousPingPong = nullptr;
			m_Cache.denoiseFrameCount = 0;
			
			m_Cache.Resolution = m_RenderSettings.resolution;
		}

		// Bind the current write target to image unit 0
		// LOG_EDITOR_INFO(m_WriteFrameIndex);
		m_Frames[m_WriteFrameIndex]->ChangeImageUnit(0);

		// Only create and bind G-buffer for path tracing with denoise enabled
		if (m_RenderSettings.enableDenoise && m_RenderSettings.shaderType == ShaderType::PATH_TRACING) {
			if (!m_GBuffer) {
				m_GBuffer = IImage2D::Create(nullptr, m_RenderSettings.resolution.x, m_RenderSettings.resolution.y, 2, Image2DType::LR_READ_WRITE);
				m_HistoryColor = IImage2D::Create(nullptr, m_RenderSettings.resolution.x, m_RenderSettings.resolution.y, 2, Image2DType::LR_READ_WRITE);
				m_HistoryGBuffer = IImage2D::Create(nullptr, m_RenderSettings.resolution.x, m_RenderSettings.resolution.y, 3, Image2DType::LR_READ_WRITE);
				m_DenoisedOutput = IImage2D::Create(nullptr, m_RenderSettings.resolution.x, m_RenderSettings.resolution.y, 4, Image2DType::LR_READ_WRITE);
				m_AtrousPingPong = IImage2D::Create(nullptr, m_RenderSettings.resolution.x, m_RenderSettings.resolution.y, 5, Image2DType::LR_READ_WRITE);
				m_Cache.denoiseFrameCount = 0;
			}
			m_GBuffer->ChangeImageUnit(2);
		}

		// increment acumulation
		m_Cache.AccumulatedFrames = (m_RenderSettings.accumulate) ? (m_Cache.AccumulatedFrames + 1) : 0;

		// UBOs

		// SETTINGS
		uint32_t entityCount = pScene->MeshEntityLookupTable.size();
		uint32_t lightCount = pScene->LightBuffer.size();
		m_SettingsUBO->Bind();
		m_SettingsUBO->AddData(0, sizeof(uint32_t), &m_RenderSettings.raysPerPixel);
		m_SettingsUBO->AddData(4, sizeof(uint32_t), &m_RenderSettings.bouncesPerRay);
		m_SettingsUBO->AddData(8, sizeof(uint32_t), &m_Cache.AccumulatedFrames);
		m_SettingsUBO->AddData(12, sizeof(uint32_t), &entityCount);
		m_SettingsUBO->AddData(16, sizeof(uint32_t), &m_RenderSettings.debugMode);
		m_SettingsUBO->AddData(20, sizeof(uint32_t), &m_RenderSettings.aabbHeatmapCutoff);
		m_SettingsUBO->AddData(24, sizeof(uint32_t), &m_RenderSettings.triangleHeatmapCutoff);
		m_SettingsUBO->AddData(28, sizeof(uint32_t), &lightCount);
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

		// SSBOs - UPDATED EVERY FRAME (reuse buffers when size unchanged)

		{
			// EntityLookupTable - BINDING POINT 0
			uint32_t count = pScene->MeshEntityLookupTable.size();
			uint32_t sizeBytes = sizeof(MeshEntityHandle) * count;
			if (count != m_Cache.entityLookupSize || !m_MeshEntityLookupSSBO) {
				m_MeshEntityLookupSSBO = IShaderStorageBuffer::Create(sizeBytes, 0, BufferUsageType::DYNAMIC_DRAW);
				m_Cache.entityLookupSize = count;
			}
			m_MeshEntityLookupSSBO->Bind();
			m_MeshEntityLookupSSBO->AddData(0, sizeBytes, pScene->MeshEntityLookupTable.data());
			m_MeshEntityLookupSSBO->Unbind();
		}
		{
			// Transforms - BINDING POINT 1
			uint32_t count = pScene->TransformBuffer.size();
			uint32_t sizeBytes = sizeof(glm::mat4) * count;
			if (count != m_Cache.transformSize || !m_TransformSSBO) {
				m_TransformSSBO = IShaderStorageBuffer::Create(sizeBytes, 1, BufferUsageType::DYNAMIC_DRAW);
				m_Cache.transformSize = count;
			}
			m_TransformSSBO->Bind();
			m_TransformSSBO->AddData(0, sizeBytes, pScene->TransformBuffer.data());
			m_TransformSSBO->Unbind();
		}
		{
			// Materials - BINDING POINT 2
			uint32_t count = pScene->MaterialBuffer.size();
			uint32_t sizeBytes = sizeof(Material) * count;
			if (count != m_Cache.materialSize || !m_MaterialSSBO) {
				m_MaterialSSBO = IShaderStorageBuffer::Create(sizeBytes, 2, BufferUsageType::DYNAMIC_DRAW);
				m_Cache.materialSize = count;
			}
			m_MaterialSSBO->Bind();
			m_MaterialSSBO->AddData(0, sizeBytes, pScene->MaterialBuffer.data());
			m_MaterialSSBO->Unbind();
		}
		{
			// Lights - BINDING POINT 6
			uint32_t count = pScene->LightBuffer.size();
			if (count > 0) {
				uint32_t sizeBytes = sizeof(LightData) * count;
				if (count != m_Cache.lightSize || !m_LightSSBO) {
					m_LightSSBO = IShaderStorageBuffer::Create(sizeBytes, 6, BufferUsageType::DYNAMIC_DRAW);
					m_Cache.lightSize = count;
				}
				m_LightSSBO->Bind();
				m_LightSSBO->AddData(0, sizeBytes, pScene->LightBuffer.data());
				m_LightSSBO->Unbind();
			}
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

		// UV Buffer - BINDING POINT 7
		static uint32_t prevUVBuffVersion = 0;
		{
			uint32_t currUVBuffVersion = assetPool->GetUpdateVersion(AssetPool::AssetType::UVBuffer);
			if (prevUVBuffVersion != currUVBuffVersion && !assetPool->UVBuffer.empty()) {
				prevUVBuffVersion = currUVBuffVersion;

				uint32_t uvBuffer_sizeBytes = sizeof(glm::vec2) * assetPool->UVBuffer.size();
				m_UVBufferSSBO = IShaderStorageBuffer::Create(uvBuffer_sizeBytes, 7, BufferUsageType::STATIC_DRAW);
				m_UVBufferSSBO->Bind();
				m_UVBufferSSBO->AddData(0, uvBuffer_sizeBytes, assetPool->UVBuffer.data());
				m_UVBufferSSBO->Unbind();
			}
		}

		return true;
	}

	void Renderer::Draw() {
		auto t = m_Profiler->timer("Renderer::Draw()");

		// Switch shader if needed
		auto desiredShader = GetOrLoadShader(m_RenderSettings.shaderType);
		if (!desiredShader) {
			LOG_ENGINE_ERROR("Failed to load shader for type: {}. Falling back to previous shader.",
				static_cast<int>(m_RenderSettings.shaderType));
			// Keep using current shader if available
			if (!m_CurrentShader || !m_CurrentShader->IsValid()) {
				LOG_ENGINE_ERROR("No valid shader available for rendering");
				return;
			}
		} else if (desiredShader != m_CurrentShader) {
			m_CurrentShader = desiredShader;
			m_Cache.AccumulatedFrames = 0; // Reset accumulation when switching shaders
			m_WasDoubleBuffering = false;  // Reset double buffering state to avoid stale frames
		}

		if (!m_CurrentShader || !m_CurrentShader->IsValid()) {
			LOG_ENGINE_ERROR("No valid shader available for rendering");
			return;
		}

		m_CurrentShader->Bind();
		m_CurrentShader->setWorkGroupSizes(glm::uvec3(
			(m_RenderSettings.resolution.x + 7) / 8,
			(m_RenderSettings.resolution.y + 3) / 4,
			1
		  ));
		m_CurrentShader->Dispatch();
	}

	void Renderer::DenoisePass(std::shared_ptr<const ParsedScene> pScene) {
		auto t = m_Profiler->timer("Renderer::DenoisePass()");

		auto denoiseShader = GetOrLoadShader(ShaderType::DENOISE);
		if (!denoiseShader) {
			LOG_ENGINE_ERROR("Failed to load denoise shader");
			return;
		}

		// Compute shared UBO data
		glm::mat4 prevViewProjInv = glm::inverse(m_PrevViewProjMatrix);
		glm::vec4 cameraPos = glm::vec4(glm::vec3(pScene->CameraTransform[3]), 0.0f);
		glm::vec4 prevCameraPos = glm::vec4(m_PrevCameraPos, 0.0f);

		// UBO Layout:
		// mat4 CurrentViewProj (0), mat4 PrevViewProjInv (64), mat4 PrevViewProj (128)
		// vec4 CameraPos (192), vec4 PrevCameraPos (208)
		// float TemporalAlpha (224), float SigmaColor (228), float SigmaNormal (232), float SigmaDepth (236)
		// int FilterRadius (240), int FrameCount (244), float MotionScale (248), float VarianceClipGamma (252)
		// int DenoiseQuality (256), int AtrousStepSize (260), int AtrousPassIndex (264), int _padding (268)

		m_DenoiseUBO->Bind();
		m_DenoiseUBO->AddData(0, sizeof(glm::mat4), &pScene->ViewProjMatrix);
		m_DenoiseUBO->AddData(64, sizeof(glm::mat4), &prevViewProjInv);
		m_DenoiseUBO->AddData(128, sizeof(glm::mat4), &m_PrevViewProjMatrix);
		m_DenoiseUBO->AddData(192, sizeof(glm::vec4), &cameraPos);
		m_DenoiseUBO->AddData(208, sizeof(glm::vec4), &prevCameraPos);
		m_DenoiseUBO->AddData(224, sizeof(float), &m_RenderSettings.denoiseTemporalAlpha);
		m_DenoiseUBO->AddData(228, sizeof(float), &m_RenderSettings.denoiseSigmaColor);
		m_DenoiseUBO->AddData(232, sizeof(float), &m_RenderSettings.denoiseSigmaNormal);
		m_DenoiseUBO->AddData(236, sizeof(float), &m_RenderSettings.denoiseSigmaDepth);
		m_DenoiseUBO->AddData(240, sizeof(int), &m_RenderSettings.denoiseFilterRadius);
		m_DenoiseUBO->AddData(244, sizeof(uint32_t), &m_Cache.denoiseFrameCount);
		m_DenoiseUBO->AddData(248, sizeof(float), &m_RenderSettings.denoiseMotionScale);
		m_DenoiseUBO->AddData(252, sizeof(float), &m_RenderSettings.denoiseVarianceClipGamma);
		m_DenoiseUBO->AddData(256, sizeof(int), &m_RenderSettings.denoiseQuality);

		glm::uvec3 workGroups(
			(m_RenderSettings.resolution.x + 7) / 8,
			(m_RenderSettings.resolution.y + 7) / 8,
			1
		);

		denoiseShader->Bind();
		denoiseShader->setWorkGroupSizes(workGroups);

		// === PASS 0: Temporal Accumulation ===
		{
			int passIndex = 0;
			int stepSize = 1;
			m_DenoiseUBO->AddData(260, sizeof(int), &stepSize);
			m_DenoiseUBO->AddData(264, sizeof(int), &passIndex);

			// Bind images for temporal pass
			m_Frames[m_WriteFrameIndex]->ChangeImageUnit(0);  // noisy input
			m_GBuffer->ChangeImageUnit(1);                     // G-buffer
			m_HistoryColor->ChangeImageUnit(2);                // history color
			m_HistoryGBuffer->ChangeImageUnit(3);              // history G-buffer
			m_DenoisedOutput->ChangeImageUnit(4);              // output

			denoiseShader->Dispatch();
		}

		// === PASSES 1+: À-Trous Wavelet Filter (only if quality == 1) ===
		if (m_RenderSettings.denoiseQuality == 1) {
			int numPasses = std::clamp(m_RenderSettings.denoiseAtrousPasses, 1, 5);

			// Use ping-pong between DenoisedOutput and AtrousPingPong
			std::shared_ptr<IImage2D>* readBuffer = &m_DenoisedOutput;
			std::shared_ptr<IImage2D>* writeBuffer = &m_AtrousPingPong;

			for (int pass = 0; pass < numPasses; pass++) {
				int passIndex = pass + 1;
				int stepSize = 1 << pass;  // 1, 2, 4, 8, 16

				m_DenoiseUBO->AddData(260, sizeof(int), &stepSize);
				m_DenoiseUBO->AddData(264, sizeof(int), &passIndex);

				// Bind ping-pong buffers
				// For à-trous passes, we read from previous output via binding 0 (noisyInput)
				(*readBuffer)->ChangeImageUnit(0);   // read from previous pass
				m_GBuffer->ChangeImageUnit(1);        // G-buffer stays the same
				// bindings 2,3 not used in à-trous passes but keep them bound
				m_HistoryColor->ChangeImageUnit(2);
				m_HistoryGBuffer->ChangeImageUnit(3);
				(*writeBuffer)->ChangeImageUnit(4);  // write to output

				denoiseShader->Dispatch();

				// Swap buffers for next iteration
				std::swap(readBuffer, writeBuffer);
			}

			// After all passes, ensure final result is in m_DenoisedOutput
			// If we did an odd number of passes, result is in m_AtrousPingPong
			if (numPasses % 2 == 1) {
				std::swap(m_DenoisedOutput, m_AtrousPingPong);
			}
		}

		m_DenoiseUBO->Unbind();
	}
}