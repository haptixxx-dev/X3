project(X3-Runtime LANGUAGES CXX)

add_executable(X3-Runtime)

target_compile_features(X3-Runtime PRIVATE cxx_std_20)
file(GLOB_RECURSE RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(X3-Runtime PRIVATE ${RUNTIME_SOURCES})
target_include_directories(X3-Runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(X3-Runtime PUBLIC X3-Engine)

# compile into the same config folder as the editor
set_target_properties(X3-Runtime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/runtime
    #RUNTIME_OUTPUT_DIRECTORY C:/Dev/X3-Exports/tst123
)

install(TARGETS X3-Runtime # executable
    RUNTIME DESTINATION runtime
)

if(MSVC)
    target_compile_definitions(X3-Runtime PUBLIC _CRT_SECURE_NO_WARNINGS)
    if(NOT ENABLE_CONSOLE)
        set_target_properties(X3-Runtime PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    endif()
endif()

