cmake_minimum_required(VERSION 3.16)
project(iconFontsHeaders)

add_library(iconFontsHeaders INTERFACE)


target_include_directories(iconFontsHeaders INTERFACE include "${CMAKE_CURRENT_SOURCE_DIR}/")