---
description: Build shaders and the X3 engine application
---

1. Install dependencies (if not already installed)
// turbo
```bash
brew install shaderc spirv-cross cmake
```

2. Compile Shaders
// turbo
```bash
./compile_shaders.sh
```

3. Build the Project
// turbo
```bash
rm -rf build && mkdir build && cd build && cmake .. && cmake --build .
```

4. Run the Game
```bash
./build/simple_game
```
