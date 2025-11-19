#!/bin/bash

# Check for DXC (DirectX Shader Compiler)
if ! command -v dxc &> /dev/null; then
    echo "Error: dxc (DirectX Shader Compiler) could not be found."
    echo "Please install it:"
    echo "  Ubuntu/Debian: sudo apt install spirv-tools"
    echo "  Or build from: https://github.com/microsoft/DirectXShaderCompiler"
    exit 1
fi

if ! command -v spirv-cross &> /dev/null; then
    echo "Error: spirv-cross could not be found."
    echo "Please install it: sudo apt install spirv-cross"
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
    
    # HLSL -> SPIR-V (using DXC)
    dxc -spirv -T vs_6_0 -E main -Fo "src/shaders/compiled/${name}.vert.spv" "$shader"
    
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to compile $shader"
        exit 1
    fi
    
    # SPIR-V -> MSL (Metal)
    spirv-cross --msl "src/shaders/compiled/${name}.vert.spv" --output "src/shaders/compiled/${name}.vert.msl"
done

# Compile Pixel/Fragment Shaders
for shader in src/shaders/*.frag.hlsl; do
    filename=$(basename -- "$shader")
    name="${filename%.*}"
    name="${name%.*}"
    
    echo "Compiling $shader..."
    
    # HLSL -> SPIR-V (using DXC)
    dxc -spirv -T ps_6_0 -E main -Fo "src/shaders/compiled/${name}.frag.spv" "$shader"
    
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to compile $shader"
        exit 1
    fi
    
    # SPIR-V -> MSL (Metal)
    spirv-cross --msl "src/shaders/compiled/${name}.frag.spv" --output "src/shaders/compiled/${name}.frag.msl"
done

# Validate SPIR-V
echo "Validating SPIR-V..."
for spv in src/shaders/compiled/*.spv; do
    if command -v spirv-val &> /dev/null; then
        spirv-val "$spv"
        if [ $? -ne 0 ]; then
            echo "ERROR: Invalid SPIR-V in $spv"
            exit 1
        fi
    fi
done

echo "Shader compilation complete!"