#CMakeLists for forked ImPlot
cmake_minimum_required(VERSION 3.16)
project(implot)

add_library(implot)
file(GLOB_RECURSE IMPLOT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(implot PRIVATE "${IMPLOT_SOURCES}")

target_include_directories(implot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")


target_link_libraries(implot PRIVATE imgui)