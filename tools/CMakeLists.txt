# Developer tools. Built by default so they cannot rot: a tool that only
# compiles when someone remembers to switch it on is a tool that stops
# compiling. X3FixtureGen is a console app - verify.sh's binary discovery
# looks for X3Editor*/X3Runtime* only, so it is never smoke-run as a GUI.

add_executable(X3FixtureGen)
target_sources(X3FixtureGen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fixture-gen/main.cpp)
target_link_libraries(X3FixtureGen PRIVATE X3Engine)

# Default asset and output locations live in the source tree.
target_compile_definitions(X3FixtureGen PRIVATE X3_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set_target_properties(X3FixtureGen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
)
