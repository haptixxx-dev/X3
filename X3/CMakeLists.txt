project(X3Engine VERSION 0.0.1 LANGUAGES CXX)

# ---- Third-party libraries ----
# Disable unnecessary GLFW components and add libraries
# set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
# set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(ONLY_LIBS ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/stb_image)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/stb_truetype)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/glm)

# Use assimp's bundled minizip. System minizip installs unzip.h under a
# minizip/ subdirectory on some distros, but assimp includes it unqualified.
set(ASSIMP_BUILD_MINIZIP ON CACHE BOOL "" FORCE)
# Newer compilers emit warnings assimp's own -Werror was never tuned for.
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/assimp)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/spdlog)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/entt)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/yaml-cpp)

# Vulkan libraries
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/VulkanMemoryAllocator)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/vk-bootstrap)

# Jolt Physics configuration
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS ON CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(OBJECT_LAYER_BITS 16 CACHE STRING "" FORCE)
# Jolt's own GPU compute backends are unused here. Leaving JPH_USE_VK on makes
# Jolt compile its test HLSL shader with dxc, which we neither ship nor need.
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/JoltPhysics/Build)

# ---- Engine library target ----
add_library(X3Engine STATIC)
set_target_properties(X3Engine PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)
target_include_directories(X3Engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(X3Engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/entt/include)
target_include_directories(X3Engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/JoltPhysics)
file(GLOB_RECURSE X3_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# Filter out the OpenGL backend (deleted in a following commit)
list(FILTER X3_SOURCES EXCLUDE REGEX ".*/Platform/OpenGL/.*")

target_sources(X3Engine PRIVATE ${X3_SOURCES})

# Optionally, set up precompiled header directly on the target (CMake ≥3.16)
target_precompile_headers(X3Engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lrpch.h")  # see CMake docs:contentReference[oaicite:9]{index=9}

target_link_libraries(X3Engine PUBLIC glm spdlog::spdlog entt yaml-cpp Jolt)
target_link_libraries(X3Engine PUBLIC assimp stb_image stb_truetype)

# Vulkan + windowing
find_package(Vulkan REQUIRED)
target_link_libraries(X3Engine PUBLIC Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
# GLFW is the windowing layer for Vulkan too
find_package(glfw3 REQUIRED)
target_link_libraries(X3Engine PUBLIC glfw)

# Compile GLSL shaders to SPIR-V
find_program(GLSLC_EXECUTABLE NAMES glslc HINTS Vulkan::glslc REQUIRED)
file(GLOB_RECURSE GLSL_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/*.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/*.frag"
)

foreach(GLSL ${GLSL_SOURCE_FILES})
    get_filename_component(FILE_NAME ${GLSL} NAME)
    set(SPIRV "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/${FILE_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${GLSLC_EXECUTABLE} ${GLSL} -o ${SPIRV}
        DEPENDS ${GLSL}
        COMMENT "Compiling ${FILE_NAME} to SPIR-V"
    )
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach()

add_custom_target(
    Shaders
    DEPENDS ${SPIRV_BINARY_FILES}
    COMMENT "Building all shaders"
)
add_dependencies(X3Engine Shaders) 

# Suppress array-bounds warning only for assimp
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(assimp PRIVATE -Wno-array-bounds)
endif()


# DEFINES
target_compile_definitions(X3Engine PUBLIC
    $<$<BOOL:${BUILD_INSTALL}>:BUILD_INSTALL> # if ON -> #define BUILD_INSTALL
)

target_compile_definitions(X3Engine PUBLIC
    $<$<BOOL:${BUILD_INSTALL}>:CMAKE_ENGINE_RESOURCES_PATH="./engine_res/">
    $<$<NOT:$<BOOL:${BUILD_INSTALL}>>:CMAKE_ENGINE_RESOURCES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/res/">
)
if(WIN32)
    target_compile_definitions(X3Engine PUBLIC LR_PLATFORM_WINDOWS)
endif()

# --target install (copy res near the executable)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/res/
        DESTINATION engine_res)