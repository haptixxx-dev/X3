project(X3Runtime LANGUAGES CXX)

add_executable(X3Runtime)

target_compile_features(X3Runtime PRIVATE cxx_std_20)
file(GLOB_RECURSE RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(X3Runtime PRIVATE ${RUNTIME_SOURCES})
target_include_directories(X3Runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(X3Runtime PRIVATE ${CMAKE_SOURCE_DIR}/X3/src)
target_link_libraries(X3Runtime PUBLIC X3Engine)

# compile into the same config folder as the editor
set_target_properties(X3Runtime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/runtime
    #RUNTIME_OUTPUT_DIRECTORY C:/Dev/X3-Exports/tst123
)

install(TARGETS X3Runtime # executable
    RUNTIME DESTINATION runtime
)

if(MSVC)
    target_compile_definitions(X3Runtime PUBLIC _CRT_SECURE_NO_WARNINGS)
    if(NOT ENABLE_CONSOLE)
        set_target_properties(X3Runtime PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    endif()
endif()

