#include "x3/Engine.h"
#include "x3/Shader.h"
#include <iostream>

namespace x3 {

Engine::Engine(const std::string& title, int width, int height)
    : m_title(title), m_width(width), m_height(height), m_running(false) {
}

Engine::~Engine() {
    Shutdown();
}

bool Engine::Init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    m_window = SDL_CreateWindow(m_title.c_str(), m_width, m_height, SDL_WINDOW_RESIZABLE);
    if (!m_window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        return false;
    }

    m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!m_device) {
        std::cerr << "SDL_CreateGPUDevice failed: " << SDL_GetError() << std::endl;
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(m_device, m_window)) {
        std::cerr << "SDL_ClaimWindowForGPUDevice failed: " << SDL_GetError() << std::endl;
        return false;
    }

    m_input = std::make_unique<InputSystem>();
    m_input->Init();

    m_resources = std::make_unique<ResourceManager>(m_device);
    m_scenes = std::make_unique<SceneManager>();

    // Create Graphics Pipeline
    SDL_GPUShader* vertShader = Shader::Load(m_device, "src/shaders/compiled/Basic.vert", 0, 1, 0, 0);
    SDL_GPUShader* fragShader = Shader::Load(m_device, "src/shaders/compiled/Basic.frag", 0, 0, 0, 0);

    if (!vertShader || !fragShader) {
        std::cerr << "Failed to load shaders! Make sure they are compiled." << std::endl;
        return false;
    }

    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.vertex_shader = vertShader;
        pipelineInfo.fragment_shader = fragShader;
        // Vertex Input State
        SDL_GPUVertexBufferDescription vertexBufferDesc = {};
        vertexBufferDesc.slot = 0;
        vertexBufferDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vertexBufferDesc.pitch = sizeof(Vector3);

        SDL_GPUVertexAttribute vertexAttr = {};
        vertexAttr.buffer_slot = 0;
        vertexAttr.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        vertexAttr.location = 0;
        vertexAttr.offset = 0;

        pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
        pipelineInfo.vertex_input_state.vertex_buffer_descriptions = &vertexBufferDesc;
        pipelineInfo.vertex_input_state.num_vertex_attributes = 1;
        pipelineInfo.vertex_input_state.vertex_attributes = &vertexAttr;

        // Primitive State
        pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        // Target Info
        SDL_GPUColorTargetDescription colorTargetDesc = {};
        colorTargetDesc.format = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);

        pipelineInfo.target_info.num_color_targets = 1;
        pipelineInfo.target_info.color_target_descriptions = &colorTargetDesc;
        pipelineInfo.target_info.has_depth_stencil_target = false;
        // pipelineInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;

        pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

        m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
        
        SDL_ReleaseGPUShader(m_device, vertShader);
        SDL_ReleaseGPUShader(m_device, fragShader);

    m_running = true;
    return true;
}

void Engine::Run() {
    Uint64 lastTime = SDL_GetTicksNS();

    while (m_running) {
        // Input
        m_input->Update();
        
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                m_running = false;
            }
        }

        if (m_input->IsActionJustPressed("QUIT")) {
            m_running = false;
        }

        // Update
        m_scenes->Update(this, 0.016f); // Fixed time step for now

        // Render
        Render();
    }
}

void Engine::ProcessEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            m_running = false;
        }
    }
    m_input->Update();
}

void Engine::Update(float deltaTime) {
    m_scenes->Update(this, deltaTime);
}

void Engine::Render() {
    SDL_GPUCommandBuffer* cmdBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (!cmdBuffer) {
        return;
    }

    SDL_GPUTexture* swapchainTexture;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmdBuffer, m_window, &swapchainTexture, nullptr, nullptr)) {
        SDL_CancelGPUCommandBuffer(cmdBuffer);
        return;
    }

    if (swapchainTexture) {
        SDL_GPUColorTargetInfo colorTargetInfo = {};
        colorTargetInfo.texture = swapchainTexture;
        colorTargetInfo.clear_color = { 0.1f, 0.1f, 0.1f, 1.0f };
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(cmdBuffer, &colorTargetInfo, 1, nullptr);
        
        if (renderPass) {
            SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);
            
            // Render Scene
            if (m_scenes) {
                m_scenes->Render(this, renderPass);
            }

            SDL_EndGPURenderPass(renderPass);
        }
    }

    SDL_SubmitGPUCommandBuffer(cmdBuffer);
}

void Engine::Shutdown() {
    if (m_pipeline) SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
    if (m_device) SDL_DestroyGPUDevice(m_device);
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
}

void Engine::SetCamera(Camera* camera) {
    m_camera = camera;
}

void Engine::RenderModel(SDL_GPURenderPass* renderPass, Model* model, const Vector3& position, const Vector3& scale, Vector3 color) {
    if (!model || !model->vertexBuffer || !model->indexBuffer || !m_camera) return;

    SDL_GPUBufferBinding vertexBinding = { model->vertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
    
    SDL_GPUBufferBinding indexBinding = { model->indexBuffer, 0 };
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_DrawGPUIndexedPrimitives(renderPass, model->indices.size(), 1, 0, 0, 0);
}

} // namespace x3
