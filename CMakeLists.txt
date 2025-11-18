cmake_minimum_required(VERSION 3.20)
project(X3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL3 Submodule
add_subdirectory(src/libs/SDL EXCLUDE_FROM_ALL)

# Engine Library
include_directories(include)
include_directories(src/libs/SDL/include)
include_directories(src/libs/cgltf) # Add cgltf include

file(GLOB_RECURSE ENGINE_SOURCES "src/engine/*.cpp" "src/libs/cgltf/*.cpp")

add_library(x3 ${ENGINE_SOURCES})
target_link_libraries(x3 PRIVATE SDL3::SDL3)
target_include_directories(x3 PUBLIC include)

# Example Game
add_executable(simple_game examples/simple/main.cpp)
target_link_libraries(simple_game PRIVATE x3)
