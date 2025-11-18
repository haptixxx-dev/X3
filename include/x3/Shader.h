#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>

namespace x3 {

class Shader {
public:
    static SDL_GPUShader* Load(SDL_GPUDevice* device, const std::string& path, int numSamplers, int numUniformBuffers, int numStorageBuffers, int numStorageTextures) {
        // Determine format based on backend
        SDL_GPUShaderFormat format = SDL_GetGPUShaderFormats(device);
        std::string fullPath = path;

        SDL_GPUShaderFormat actualFormat = SDL_GPU_SHADERFORMAT_INVALID;
        if (format & SDL_GPU_SHADERFORMAT_SPIRV) {
            fullPath += ".spv";
            actualFormat = SDL_GPU_SHADERFORMAT_SPIRV;
        } else if (format & SDL_GPU_SHADERFORMAT_DXIL) {
            fullPath += ".dxil";
            actualFormat = SDL_GPU_SHADERFORMAT_DXIL;
        } else if (format & SDL_GPU_SHADERFORMAT_MSL) {
            fullPath += ".msl";
            actualFormat = SDL_GPU_SHADERFORMAT_MSL;
        } else {
            std::cerr << "Unknown shader format supported by device!" << std::endl;
            return nullptr;
        }

        std::ifstream file(fullPath, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open shader file: " << fullPath << std::endl;
            return nullptr;
        }

        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize + 1); // +1 for null terminator
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        buffer[fileSize] = '\0'; // Null terminate
        file.close();

        SDL_GPUShaderCreateInfo createInfo;
        createInfo.code_size = fileSize; // Exclude null terminator from size, but buffer has it
        createInfo.code = (const Uint8*)buffer.data();
        
        // Entry point is usually "main" for SPIR-V, but "main0" for MSL from spirv-cross
        createInfo.entrypoint = "main0"; 
        
        createInfo.format = actualFormat;
        createInfo.stage = (path.find(".vert") != std::string::npos) ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
        createInfo.num_samplers = numSamplers;
        createInfo.num_uniform_buffers = numUniformBuffers;
        createInfo.num_storage_buffers = numStorageBuffers;
        createInfo.num_storage_textures = numStorageTextures;

        return SDL_CreateGPUShader(device, &createInfo);
    }
};

} // namespace x3
