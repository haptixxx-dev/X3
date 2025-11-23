#pragma once

#include "Core/application.h"
#include "EngineCfg.h"

// returns new heap allocated polymorphic application
extern X3::Application* X3::CreateApplication(const std::filesystem::path& exeDir);

int main(int argc, char** argv) {
    std::filesystem::path exePath = std::filesystem::weakly_canonical(argv[0]);
    std::filesystem::path exeDir = exePath.parent_path();

    X3::EngineCfg::Init(exeDir); // init EngineCfg::RESOURCES_PATH

    X3::Application* app = X3::CreateApplication(exeDir);
    app->run();
    delete app;
}
