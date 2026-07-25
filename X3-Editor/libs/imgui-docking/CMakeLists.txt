cmake_minimum_required(VERSION 3.16)
project(imgui)

add_library(imgui)
file(GLOB_RECURSE IMGUI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# main.cpp is ImGui's own demo application. It defines main() and calls the
# OpenGL backend directly, so it breaks the link once a backend is excluded.
list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

# imgui_impl_opengl3.cpp is vendored upstream but never built - Vulkan-only engine.
list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*imgui_impl_opengl3\\.cpp$")

target_sources(imgui PRIVATE "${IMGUI_SOURCES}")

target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Vulkan REQUIRED)
target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan)