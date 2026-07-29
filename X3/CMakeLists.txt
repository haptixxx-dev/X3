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

# enkiTS -- the engine's task scheduler. Its own examples and tests are build
# noise for us.
set(ENKITS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_C_INTERFACE OFF CACHE BOOL "" FORCE)
set(ENKITS_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/enkiTS)

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

target_sources(X3Engine PRIVATE ${X3_SOURCES})

# Optionally, set up precompiled header directly on the target (CMake ≥3.16)
target_precompile_headers(X3Engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lrpch.h")  # see CMake docs:contentReference[oaicite:9]{index=9}

target_link_libraries(X3Engine PUBLIC glm spdlog::spdlog entt yaml-cpp Jolt enkiTS)
target_link_libraries(X3Engine PUBLIC assimp stb_image stb_truetype)

# Vulkan + windowing
find_package(Vulkan REQUIRED)
target_link_libraries(X3Engine PUBLIC Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator vk-bootstrap::vk-bootstrap)
# GLFW is the windowing layer for Vulkan too
find_package(glfw3 REQUIRED)
target_link_libraries(X3Engine PUBLIC glfw)

# ---- Shaders: Slang -> SPIR-V, plus reflection -> generated descriptor tables --
#
# slangc is NOT packaged on most distros, and on Arch the `slang` package is
# S-Lang, an unrelated interpreted language. scripts/fetch-slang.sh downloads a
# PINNED release into libs/slang (gitignored -- it is ~200 MB extracted, which
# does not belong in the repo). A slangc already on PATH is used if present.
set(SLANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/slang")
find_program(SLANGC_EXECUTABLE
    NAMES slangc
    HINTS "${SLANG_DIR}/bin"
    NO_CMAKE_SYSTEM_PATH
)
if(NOT SLANGC_EXECUTABLE)
    find_program(SLANGC_EXECUTABLE NAMES slangc)
endif()
if(NOT SLANGC_EXECUTABLE)
    message(FATAL_ERROR
        "slangc not found. Run scripts/fetch-slang.sh to download the pinned "
        "release into X3/libs/slang, or put slangc on PATH. Note that a system "
        "package named 'slang' is probably S-Lang, which is a different project.")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res/shaders")

# ENTRY POINTS ONLY. GpuTypes.slang, Bindings.slang and Trace.slang are modules
# -- imported, never compiled standalone -- so they are dependencies rather than
# targets. This is an explicit list rather than a glob precisely because that
# distinction is not visible in the filename.
# Compute entry points, and the raster ones. They are separate lists because the
# stage is a compile flag, not something slangc infers from the file.
set(SLANG_ENTRY_POINTS PathTracing PBR Phong FurnaceTest BsdfLutBake)
set(SLANG_VERTEX_ENTRY_POINTS DepthPrepass)
file(GLOB SLANG_MODULES "${SHADER_DIR}/*.slang")

# -matrix-layout-column-major is LOAD-BEARING, not a style choice. Slang follows
# HLSL conventions and would otherwise store matrices row-major, while every
# glm::mat4 this engine uploads is column-major. The mismatch produces no
# compile error and no validation message -- transforms simply come out
# transposed, which reads as a camera bug.
# The stage is chosen per entry point below; everything else is common.
# -matrix-layout-column-major is LOAD-BEARING for all of them.

set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer/Generated")
set(REFLECTION_DIR "${CMAKE_CURRENT_BINARY_DIR}/shader-reflection")
file(MAKE_DIRECTORY ${REFLECTION_DIR})

foreach(NAME ${SLANG_ENTRY_POINTS} ${SLANG_VERTEX_ENTRY_POINTS})
    set(SRC "${SHADER_DIR}/${NAME}.slang")
    set(SPIRV "${SHADER_DIR}/${NAME}.slang.spv")
    set(REFL "${REFLECTION_DIR}/${NAME}.json")

    set(STAGE compute)
    if(${NAME} IN_LIST SLANG_VERTEX_ENTRY_POINTS)
        set(STAGE vertex)
    endif()

    add_custom_command(
        OUTPUT ${SPIRV} ${REFL}
        COMMAND ${SLANGC_EXECUTABLE} ${SRC} -target spirv -entry main -stage ${STAGE}
                -matrix-layout-column-major -I ${SHADER_DIR}
                -o ${SPIRV} -reflection-json ${REFL}
        # Every module, not just this entry point: a change to Trace.slang must
        # rebuild all three, and they no longer share a preprocessor include
        # graph CMake could scan.
        DEPENDS ${SRC} ${SLANG_MODULES}
        COMMENT "Compiling ${NAME}.slang to SPIR-V"
    )
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
    list(APPEND REFLECTION_FILES ${REFL})
endforeach()

# The descriptor table Renderer.cpp used to hand-maintain. Generated from the
# reflection above, so the C++ cannot disagree with the shader. The script
# rewrites the file only when its content changes, so an unrelated shader edit
# does not retrigger a full rebuild of everything that includes it.
set(DESCRIPTOR_TABLE_HEADER "${GENERATED_DIR}/DescriptorTables.h")
add_custom_command(
    OUTPUT ${DESCRIPTOR_TABLE_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/gen_descriptor_tables.py
            --out ${DESCRIPTOR_TABLE_HEADER} ${REFLECTION_FILES}
    DEPENDS ${REFLECTION_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/gen_descriptor_tables.py
    COMMENT "Generating descriptor tables from Slang reflection"
)

add_custom_target(
    Shaders
    DEPENDS ${SPIRV_BINARY_FILES} ${DESCRIPTOR_TABLE_HEADER}
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