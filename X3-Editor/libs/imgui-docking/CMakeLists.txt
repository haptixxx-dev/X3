cmake_minimum_required(VERSION 3.16)
project(imgui)

add_library(imgui)
file(GLOB_RECURSE IMGUI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(imgui PRIVATE "${IMGUI_SOURCES}")

target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Vulkan REQUIRED)
target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan) # assumes glfw linked