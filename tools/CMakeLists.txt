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

# Golden-image regression tests for the renderer. Console app, so verify.sh's
# GUI smoke test never picks it up. It DOES need a display -- the window is
# created unmapped rather than the context being surfaceless.
add_executable(X3RenderTest)
target_sources(X3RenderTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/render-test/main.cpp)
target_link_libraries(X3RenderTest PRIVATE X3Engine)
target_compile_definitions(X3RenderTest PRIVATE X3_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(X3RenderTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
)

# Assertions over the renderer's pure arithmetic. Needs NO GPU and NO display,
# which makes it the only gate in this repo that can run anywhere -- and the only
# one that checks what the code is supposed to do rather than what it did last
# time. A shadow cascade that is subtly wrong produces an entirely plausible
# picture, so a golden image cannot catch it.
add_executable(X3MathTest)
target_sources(X3MathTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/math-test/main.cpp)
target_link_libraries(X3MathTest PRIVATE X3Engine)
set_target_properties(X3MathTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
)

# Phase 9's mesh cooker: source model -> .x3mesh (geometry + prebuilt BVH).
# Console app, so verify.sh's GUI smoke test never picks it up. Needs no GPU and
# no display -- `X3AssetCook --self-test` round-trips synthetic data and needs no
# asset either, which makes it runnable on any checkout alongside X3MathTest.
# The format itself lives in the engine library (X3/src/Project/Assets/AssetCook.*,
# picked up by that target's source glob) rather than here, because the runtime
# is what will eventually read these files -- a format that only the cooker can
# parse is a format that drifts from the loader.
add_executable(X3AssetCook)
target_sources(X3AssetCook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cook/main.cpp)
target_link_libraries(X3AssetCook PRIVATE X3Engine)
set_target_properties(X3AssetCook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
)

# Phase 12: assertions over the MaterialX/OpenPBR importer's reduction. Needs no
# GPU, no display and NO ASSET FILES -- every .mtlx document it parses is a
# string literal in the test, so there is no fixture that can go stale while the
# assertions keep passing against the wrong document. The importer itself lives
# in the engine library (X3/src/Project/Assets/MaterialXImport.*, picked up by
# that target's source glob) because the editor will import materials too; only
# the gate lives here.
add_executable(X3MtlxTest)
target_sources(X3MtlxTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mtlx-test/main.cpp)
target_link_libraries(X3MtlxTest PRIVATE X3Engine)
set_target_properties(X3MtlxTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
)
