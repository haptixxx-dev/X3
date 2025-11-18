#include "x3/Resource.h"
#include <iostream>
#include "../libs/cgltf/cgltf.h"

namespace x3 {

void Model::UploadToGPU(SDL_GPUDevice* device) {
    if (vertices.empty() || indices.empty()) return;

    // Create Vertex Buffer
    SDL_GPUBufferCreateInfo vboInfo = {};
    vboInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vboInfo.size = vertices.size() * sizeof(Vector3);
    vertexBuffer = SDL_CreateGPUBuffer(device, &vboInfo);

    // Create Index Buffer
    SDL_GPUBufferCreateInfo iboInfo = {};
    iboInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    iboInfo.size = indices.size() * sizeof(int);
    indexBuffer = SDL_CreateGPUBuffer(device, &iboInfo);

    // Upload Data (Using Transfer Buffer is proper way, but for simplicity we map if possible or use transfer)
    // SDL3 GPU requires a transfer buffer to upload data to device-local buffers.
    
    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = vboInfo.size + iboInfo.size;
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

    Uint8* map = (Uint8*)SDL_MapGPUTransferBuffer(device, transferBuffer, false);
    if (map) {
        memcpy(map, vertices.data(), vboInfo.size);
        memcpy(map + vboInfo.size, indices.data(), iboInfo.size);
        SDL_UnmapGPUTransferBuffer(device, transferBuffer);
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTransferBufferLocation vLoc = { transferBuffer, 0 };
    SDL_GPUBufferRegion vRegion = { vertexBuffer, 0, vboInfo.size };
    SDL_UploadToGPUBuffer(copyPass, &vLoc, &vRegion, false);

    SDL_GPUTransferBufferLocation iLoc = { transferBuffer, (Uint32)vboInfo.size };
    SDL_GPUBufferRegion iRegion = { indexBuffer, 0, iboInfo.size };
    SDL_UploadToGPUBuffer(copyPass, &iLoc, &iRegion, false);

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(cmd);
    
    // Release transfer buffer (should wait for fence in production, but SDL3 handles this internally usually or we just leak it for this simple demo until next frame)
    SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
}

ResourceManager::ResourceManager(SDL_GPUDevice* device) : m_device(device) {
}

ResourceManager::~ResourceManager() {
    // Cleanup textures/models
    for (auto& [path, model] : m_models) {
        if (model->vertexBuffer) SDL_ReleaseGPUBuffer(m_device, model->vertexBuffer);
        if (model->indexBuffer) SDL_ReleaseGPUBuffer(m_device, model->indexBuffer);
        delete model;
    }
    m_models.clear();
}

SDL_Texture* ResourceManager::GetTexture(const std::string& path) {
    // Placeholder: Texture loading needs to be rewritten for SDL_GPUTexture
    return nullptr; 
}

Model* ResourceManager::GetModel(const std::string& path) {
    auto it = m_models.find(path);
    if (it != m_models.end()) {
        return it->second;
    }

    // ... (Existing cgltf loading code) ...
    // Re-implementing briefly for context
    
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path.c_str(), &data);

    if (result != cgltf_result_success) {
        std::cerr << "Failed to parse GLTF: " << path << std::endl;
        return nullptr;
    }

    result = cgltf_load_buffers(&options, data, path.c_str());
    if (result != cgltf_result_success) {
        cgltf_free(data);
        return nullptr;
    }

    Model* model = new Model();

    // ... (Existing mesh parsing loop) ...
    for (size_t i = 0; i < data->meshes_count; ++i) {
        cgltf_mesh* mesh = &data->meshes[i];
        for (size_t j = 0; j < mesh->primitives_count; ++j) {
            cgltf_primitive* primitive = &mesh->primitives[j];
            // Indices
            if (primitive->indices) {
                size_t indexCount = primitive->indices->count;
                size_t baseIndex = model->vertices.size();
                for (size_t k = 0; k < indexCount; ++k) {
                    cgltf_size index = cgltf_accessor_read_index(primitive->indices, k);
                    model->indices.push_back(baseIndex + index);
                }
            }
            // Attributes
            for (size_t k = 0; k < primitive->attributes_count; ++k) {
                cgltf_attribute* attr = &primitive->attributes[k];
                if (attr->type == cgltf_attribute_type_position) {
                    cgltf_accessor* accessor = attr->data;
                    for (size_t l = 0; l < accessor->count; ++l) {
                        float v[3];
                        cgltf_accessor_read_float(accessor, l, v, 3);
                        model->vertices.push_back(Vector3(v[0], v[1], v[2]));
                    }
                }
            }
        }
    }

    cgltf_free(data);
    
    // Upload to GPU immediately
    model->UploadToGPU(m_device);

    m_models[path] = model;
    return model;
}

} // namespace x3
