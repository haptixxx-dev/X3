# Welcome to X3 Engine

X3 is a lightweight, modular game engine written in C. It leverages the power of **SDL3** for cross-platform windowing and rendering, and **Flecs** for a high-performance Entity Component System (ECS).

## Features

- **Core**: Minimalist C11 codebase.
- **ECS**: Built on Flecs for data-oriented design.
- **Rendering**: SDL3-based 2D rendering (extensible).
- **Input**: Unified input handling.

## Quick Start

### Prerequisites

- CMake 3.16+
- C Compiler (GCC, Clang, MSVC)

### Building

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

### Running the Example

```bash
./simple_game
```
