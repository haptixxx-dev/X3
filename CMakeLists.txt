cmake_minimum_required(VERSION 4.1)

# if BUILD_INSTALL = ON -> LauraEngine target defines #define BUILD_INSTALL
# and propagates the macro to Runtime & Editor
option(BUILD_INSTALL "Use installed paths instead of source-tree paths" ON)


# Console option for applications
option(ENABLE_CONSOLE "Enable console window for applications" ON)


# decide where to ship if --target install
if(NOT DEFINED CMAKE_INSTALL_PREFIX) # if install path not specified
    # use build/install/<config>
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install path")
endif()

message(WARNING "CMAKE_INSTALL_PREFIX = ${CMAKE_INSTALL_PREFIX}")

project(Laura VERSION 1.1.0 LANGUAGES CXX)

# Global C++ settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable multithreaded compilation
if(MSVC)
    # Enable parallel compilation in MSBuild
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
endif()

add_subdirectory(X3)         # Engine library (X3)
add_subdirectory(X3-Runtime)  # Runtime executable (X3-Runtime)
add_subdirectory(X3-Editor)   # Editor executable (X3-Editor)