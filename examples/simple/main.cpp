#include "x3/Engine.h"
#include <iostream>

class FirstPersonScene : public x3::Scene {
public:
    void Init(x3::Engine* engine) override {
        std::cout << "First Person Scene Initialized" << std::endl;
        
        engine->GetInput().MapAction("QUIT", SDL_SCANCODE_ESCAPE);
        engine->GetInput().MapAction("FORWARD", SDL_SCANCODE_W);
        engine->GetInput().MapAction("BACKWARD", SDL_SCANCODE_S);
        engine->GetInput().MapAction("LEFT", SDL_SCANCODE_A);
        engine->GetInput().MapAction("RIGHT", SDL_SCANCODE_D);
        engine->GetInput().MapAction("UP", SDL_SCANCODE_SPACE);
        engine->GetInput().MapAction("DOWN", SDL_SCANCODE_LSHIFT);
        engine->GetInput().MapAction("LOOK_LEFT", SDL_SCANCODE_LEFT);
        engine->GetInput().MapAction("LOOK_RIGHT", SDL_SCANCODE_RIGHT);
        engine->GetInput().MapAction("LOOK_UP", SDL_SCANCODE_UP);
        engine->GetInput().MapAction("LOOK_DOWN", SDL_SCANCODE_DOWN);

        camera = std::make_unique<x3::Camera>(x3::Vector3(0, 1, 5));
        engine->SetCamera(camera.get());

        // Load a GLTF model from file
        // cubeModel = engine->GetResources().GetModel("models/cube.gltf");
        
        // Or create a simple cube manually if no GLTF file available
        cubeModel = new x3::Model();
        // Cube vertices with proper face normals
        cubeModel->vertices = {
            // Front face (z = 1)
            {{-1, -1,  1}, {0, 0, 1}}, {{ 1, -1,  1}, {0, 0, 1}}, {{ 1,  1,  1}, {0, 0, 1}}, {{-1,  1,  1}, {0, 0, 1}},
            // Right face (x = 1)
            {{ 1, -1,  1}, {1, 0, 0}}, {{ 1, -1, -1}, {1, 0, 0}}, {{ 1,  1, -1}, {1, 0, 0}}, {{ 1,  1,  1}, {1, 0, 0}},
            // Back face (z = -1)
            {{ 1, -1, -1}, {0, 0, -1}}, {{-1, -1, -1}, {0, 0, -1}}, {{-1,  1, -1}, {0, 0, -1}}, {{ 1,  1, -1}, {0, 0, -1}},
            // Left face (x = -1)
            {{-1, -1, -1}, {-1, 0, 0}}, {{-1, -1,  1}, {-1, 0, 0}}, {{-1,  1,  1}, {-1, 0, 0}}, {{-1,  1, -1}, {-1, 0, 0}},
            // Top face (y = 1)
            {{-1,  1,  1}, {0, 1, 0}}, {{ 1,  1,  1}, {0, 1, 0}}, {{ 1,  1, -1}, {0, 1, 0}}, {{-1,  1, -1}, {0, 1, 0}},
            // Bottom face (y = -1)
            {{-1, -1, -1}, {0, -1, 0}}, {{ 1, -1, -1}, {0, -1, 0}}, {{ 1, -1,  1}, {0, -1, 0}}, {{-1, -1,  1}, {0, -1, 0}}
        };
        cubeModel->indices = {
            0, 1, 2, 2, 3, 0,       // Front
            4, 5, 6, 6, 7, 4,       // Right
            8, 9, 10, 10, 11, 8,    // Back
            12, 13, 14, 14, 15, 12, // Left
            16, 17, 18, 18, 19, 16, // Top
            20, 21, 22, 22, 23, 20  // Bottom
        };
        
        // Upload to GPU
        cubeModel->UploadToGPU(engine->GetDevice());
    }

    void Update(x3::Engine* engine, float deltaTime) override {
        if (engine->GetInput().IsActionJustPressed("QUIT")) {
            engine->Shutdown();
        }

        if (engine->GetInput().IsActionPressed("FORWARD")) camera->ProcessKeyboard(0, deltaTime);
        if (engine->GetInput().IsActionPressed("BACKWARD")) camera->ProcessKeyboard(1, deltaTime);
        if (engine->GetInput().IsActionPressed("LEFT")) camera->ProcessKeyboard(2, deltaTime);
        if (engine->GetInput().IsActionPressed("RIGHT")) camera->ProcessKeyboard(3, deltaTime);
        if (engine->GetInput().IsActionPressed("UP")) camera->position.y += camera->movementSpeed * deltaTime;
        if (engine->GetInput().IsActionPressed("DOWN")) camera->position.y -= camera->movementSpeed * deltaTime;

        float sensitivity = 100.0f;
        if (engine->GetInput().IsActionPressed("LOOK_LEFT")) camera->ProcessMouseMovement(-sensitivity * deltaTime, 0);
        if (engine->GetInput().IsActionPressed("LOOK_RIGHT")) camera->ProcessMouseMovement(sensitivity * deltaTime, 0);
        if (engine->GetInput().IsActionPressed("LOOK_UP")) camera->ProcessMouseMovement(0, sensitivity * deltaTime);
        if (engine->GetInput().IsActionPressed("LOOK_DOWN")) camera->ProcessMouseMovement(0, -sensitivity * deltaTime);
    }

    void Render(x3::Engine* engine, SDL_GPUCommandBuffer* cmdBuffer, SDL_GPURenderPass* renderPass) override {
        engine->RenderModel(cmdBuffer, renderPass, cubeModel, {0, 0, 0}, {1, 1, 1}, {1, 1, 1});
    }

    void Unload(x3::Engine* engine) override {
        delete cubeModel;
    }

private:
    std::unique_ptr<x3::Camera> camera;
    x3::Model* cubeModel = nullptr;
};

int main(int argc, char* argv[]) {
    x3::Engine engine("X3 Engine GPU Demo", 800, 600);
    
    if (engine.Init()) {
        engine.GetScenes().PushScene(std::make_unique<FirstPersonScene>(), &engine);
        engine.Run();
    }

    return 0;
}
