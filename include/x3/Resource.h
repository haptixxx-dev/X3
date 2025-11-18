#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "x3/Math.h"

namespace x3 {

struct Model {
    std::vector<Vector3> vertices;
    std::vector<int> indices;

    // GPU Resources
    SDL_GPUBuffer* vertexBuffer = nullptr;
    SDL_GPUBuffer* indexBuffer = nullptr;

    void UploadToGPU(SDL_GPUDevice* device);
};

class ResourceManager {
public:
    ResourceManager(SDL_GPUDevice* device); // Changed from Renderer to Device
    ~ResourceManager();

    SDL_Texture* GetTexture(const std::string& path); // Need to update texture loading for GPU too eventually
    Model* GetModel(const std::string& path);

private:
    SDL_GPUDevice* m_device;
    std::unordered_map<std::string, SDL_Texture*> m_textures; // Still SDL_Texture for now (2D fallback)
    std::unordered_map<std::string, Model*> m_models;
};

} // namespace x3
