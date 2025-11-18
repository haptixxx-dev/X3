#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <memory>

#include "x3/Input.h"
#include "x3/Resource.h"
#include "x3/Scene.h"
#include "x3/Math.h"
#include "x3/Camera.h"

namespace x3 {

class Engine {
public:
    Engine(const std::string& title, int width, int height);
    ~Engine();

    bool Init();
    void Run();
    void Shutdown();

    SDL_GPUDevice* GetDevice() const { return m_device; }
    SDL_Window* GetWindow() const { return m_window; }
    
    InputSystem& GetInput() { return *m_input; }
    ResourceManager& GetResources() { return *m_resources; }
    SceneManager& GetScenes() { return *m_scenes; }

    // 3D Rendering
    void SetCamera(Camera* camera);
    void RenderModel(SDL_GPURenderPass* renderPass, Model* model, const Vector3& position, const Vector3& scale, Vector3 color);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    void ProcessEvents();
    void Update(float deltaTime);
    void Render();

    std::string m_title;
    int m_width, m_height;
    bool m_running;

    SDL_Window* m_window = nullptr;
    SDL_GPUDevice* m_device = nullptr;

    std::unique_ptr<InputSystem> m_input;
    std::unique_ptr<ResourceManager> m_resources;
    std::unique_ptr<SceneManager> m_scenes;

    Camera* m_camera = nullptr;
    
    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
};

} // namespace x3
