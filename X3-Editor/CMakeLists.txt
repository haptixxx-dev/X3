project(X3Editor VERSION 1.1.0 LANGUAGES CXX)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/implot)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui-docking)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/icon-fonts-headers)

add_executable(X3Editor)
file(GLOB_RECURSE EDITOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(X3Editor PRIVATE ${EDITOR_SOURCES})
target_include_directories(X3Editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(X3Editor PUBLIC imgui implot iconFontsHeaders X3Engine)

# Define paths to editor resources
target_compile_definitions(X3Editor PUBLIC
    $<$<BOOL:${BUILD_INSTALL}>:CMAKE_EDITOR_RESOURCES_PATH="./editor_res/">
    $<$<NOT:$<BOOL:${BUILD_INSTALL}>>:CMAKE_EDITOR_RESOURCES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/res/">
)

# Ensure Runtime gets built
add_dependencies(X3Editor X3Runtime)

set_target_properties(X3Editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
)

# Install Step
install(TARGETS X3Editor # executable
    RUNTIME DESTINATION .
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/res/ # resources
        DESTINATION editor_res)



# Compiler definitions / flags
if(MSVC)
    if (NOT ENABLE_CONSOLE)
        # Build as a GUI app (no console)
        set_target_properties(X3Editor PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    endif()
endif()

