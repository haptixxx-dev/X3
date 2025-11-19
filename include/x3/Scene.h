#pragma once

#include <vector>
#include <memory>
#include <SDL3/SDL.h>

namespace x3 {

class Engine;

class Scene {
public:
    virtual ~Scene() = default;
    virtual void Init(Engine* engine) {}
    virtual void Update(Engine* engine, float deltaTime) {}
    virtual void Render(Engine* engine, SDL_GPUCommandBuffer* cmdBuffer, SDL_GPURenderPass* renderPass) {} // Added renderPass
    virtual void Unload(Engine* engine) {}
};

class SceneManager {
public:
    void PushScene(std::unique_ptr<Scene> scene, Engine* engine);
    void PopScene(Engine* engine);
    void Update(Engine* engine, float deltaTime);
    void Render(Engine* engine, SDL_GPUCommandBuffer* cmdBuffer, SDL_GPURenderPass* renderPass); // Added renderPass

private:
    std::vector<std::unique_ptr<Scene>> m_scenes;
};

} // namespace x3
