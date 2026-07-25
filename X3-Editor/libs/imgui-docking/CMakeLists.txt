cmake_minimum_required(VERSION 3.16)
project(imgui)

add_library(imgui)
file(GLOB_RECURSE IMGUI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# main.cpp is ImGui's own demo application. It defines main() and calls the
# OpenGL backend directly, so it breaks the link once a backend is excluded.
list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

# Select the imgui backend matching the active graphics API
if(X3_GRAPHICS_API STREQUAL "Vulkan")
    list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*imgui_impl_opengl3\\.cpp$")
else()
    list(FILTER IMGUI_SOURCES EXCLUDE REGEX ".*imgui_impl_vulkan\\.cpp$")
endif()

target_sources(imgui PRIVATE "${IMGUI_SOURCES}")

target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(imgui PRIVATE glfw) # assumes glfw linked

if(X3_GRAPHICS_API STREQUAL "Vulkan")
    find_package(Vulkan REQUIRED)
    target_link_libraries(imgui PRIVATE Vulkan::Vulkan)
endif()