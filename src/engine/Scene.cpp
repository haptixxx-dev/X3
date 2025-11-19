#include "x3/Scene.h"

namespace x3 {

void SceneManager::PushScene(std::unique_ptr<Scene> scene, Engine* engine) {
    scene->Init(engine);
    m_scenes.push_back(std::move(scene));
}

void SceneManager::PopScene(Engine* engine) {
    if (!m_scenes.empty()) {
        m_scenes.back()->Unload(engine);
        m_scenes.pop_back();
    }
}

void SceneManager::Update(Engine* engine, float deltaTime) {
    if (!m_scenes.empty()) {
        m_scenes.back()->Update(engine, deltaTime);
    }
}

void SceneManager::Render(Engine* engine, SDL_GPUCommandBuffer* cmdBuffer, SDL_GPURenderPass* renderPass) {
    if (!m_scenes.empty()) {
        m_scenes.back()->Render(engine, cmdBuffer, renderPass);
    }
}

} // namespace x3
