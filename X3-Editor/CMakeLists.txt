project(X3-Editor VERSION 1.1.0 LANGUAGES CXX)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/implot)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui-docking)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/icon-fonts-headers)

add_executable(X3-Editor)
file(GLOB_RECURSE EDITOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(X3-Editor PRIVATE ${EDITOR_SOURCES})
target_include_directories(X3-Editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(X3-Editor PUBLIC imgui implot iconFontsHeaders LauraEngine)

# Define paths to editor resources
target_compile_definitions(X3-Editor PUBLIC
    $<$<BOOL:${BUILD_INSTALL}>:CMAKE_EDITOR_RESOURCES_PATH="./editor_res/">
    $<$<NOT:$<BOOL:${BUILD_INSTALL}>>:CMAKE_EDITOR_RESOURCES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/res/">
)

# Ensure Runtime gets built
add_dependencies(X3-Editor LauraRuntime)

set_target_properties(X3-Editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
)

# Install Step
install(TARGETS X3-Editor # executable
    RUNTIME DESTINATION .
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/res/ # resources
        DESTINATION editor_res)



# Compiler definitions / flags
if(MSVC)
    if (NOT ENABLE_CONSOLE)
        # Build as a GUI app (no console)
        set_target_properties(X3-Editor PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    endif()
endif()

