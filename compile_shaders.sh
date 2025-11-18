#!/bin/bash

# Check for tools
if ! command -v glslc &> /dev/null; then
    echo "Error: glslc (Shaderc) could not be found."
    echo "Please install it: brew install shaderc"
    exit 1
fi

if ! command -v spirv-cross &> /dev/null; then
    echo "Error: spirv-cross could not be found."
    echo "Please install it: brew install spirv-cross"
    exit 1
fi

mkdir -p src/shaders/compiled

echo "Compiling Shaders..."

# Compile Vertex Shaders
for shader in src/shaders/*.vert.hlsl; do
    filename=$(basename -- "$shader")
    name="${filename%.*}"
    name="${name%.*}"
    
    echo "Compiling $shader..."
    
    # HLSL -> SPIR-V (using glslc)
    glslc -fshader-stage=vertex -o "src/shaders/compiled/${name}.vert.spv" "$shader"
    
    # SPIR-V -> MSL (Metal)
    spirv-cross --msl "src/shaders/compiled/${name}.vert.spv" --output "src/shaders/compiled/${name}.vert.msl"
done

# Compile Pixel Shaders
for shader in src/shaders/*.frag.hlsl; do
    filename=$(basename -- "$shader")
    name="${filename%.*}"
    name="${name%.*}"
    
    echo "Compiling $shader..."
    
    # HLSL -> SPIR-V (using glslc)
    glslc -fshader-stage=fragment -o "src/shaders/compiled/${name}.frag.spv" "$shader"
    
    # SPIR-V -> MSL (Metal)
    spirv-cross --msl "src/shaders/compiled/${name}.frag.spv" --output "src/shaders/compiled/${name}.frag.msl"
done

echo "Shader compilation complete!"
