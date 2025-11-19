./compile_shaders.sh
echo "Shaders compiled."
rm -rf build
mkdir build
cd build
cmake ..
cmake --build . --parallel $(nproc)
echo "Build complete."