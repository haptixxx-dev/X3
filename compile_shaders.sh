#!/bin/bash

# Detect OS and set compiler
if [[ "$OSTYPE" == "darwin"* ]]; then
    COMPILER="glslc"
    INSTALL_MSG="Please install it: brew install shaderc"
else
    COMPILER="dxc"
    INSTALL_MSG="Please install it:\n  Ubuntu/Debian: sudo apt install spirv-tools\n  Or build from: https://github.com/microsoft/DirectXShaderCompiler"
fi

# Check for shader compiler
if ! command -v $COMPILER &> /dev/null; then
    echo "Error: $COMPILER could not be found."
    echo -e "$INSTALL_MSG"
    exit 1
fi

# Check for spirv-cross
if ! command -v spirv-cross &> /dev/null; then
    echo "Error: spirv-cross could not be found."
    if [[ "$OSTYPE" == "darwin"* ]]; then
        echo "Please install it: brew install spirv-cross"
    else
        echo "Please install it: sudo apt install spirv-cross"
    fi
    exit 1
fi

mkdir -p src/shaders/compiled
echo "Compiling Shaders with $COMPILER..."

# Compile Vertex Shaders
for shader in src/shaders/*.vert.hlsl; do
    [ -f "$shader" ] || continue
    
    filename=$(basename -- "$shader")
    name="${filename%.*}"
    name="${name%.*}"
    
    echo "Compiling $shader..."
    
    # HLSL -> SPIR-V
    if [[ "$COMPILER" == "glslc" ]]; then
        glslc -fshader-stage=vertex -o "src/shaders/compiled/${name}.vert.spv" "$shader"
    else
        dxc -spirv -T vs_6_0 -E main -Fo "src/shaders/compiled/${name}.vert.spv" "$shader"
    fi
    
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to compile $shader"
        exit 1
    fi
    
    # SPIR-V -> MSL (Metal)
    spirv-cross --msl "src/shaders/compiled/${name}.vert.spv" --output "src/shaders/compiled/${name}.vert.msl"
done

# Compile Pixel/Fragment Shaders
for shader in src/shaders/*.frag.hlsl; do
    [ -f "$shader" ] || continue
    
    filename=$(basename -- "$shader")
    name="${filename%.*}"
    name="${name%.*}"
    
    echo "Compiling $shader..."
    
    # HLSL -> SPIR-V
    if [[ "$COMPILER" == "glslc" ]]; then
        glslc -fshader-stage=fragment -o "src/shaders/compiled/${name}.frag.spv" "$shader"
    else
        dxc -spirv -T ps_6_0 -E main -Fo "src/shaders/compiled/${name}.frag.spv" "$shader"
    fi
    
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
    [ -f "$spv" ] || continue
    
    if command -v spirv-val &> /dev/null; then
        spirv-val "$spv"
        if [ $? -ne 0 ]; then
            echo "ERROR: Invalid SPIR-V in $spv"
            exit 1
        fi
    fi
done

echo "Shader compilation complete!"