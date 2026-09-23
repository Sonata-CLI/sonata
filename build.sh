cmake -S . -B build
cmake --build build --parallel
if [ -f "./build/sn" ]; then
    chmod +x "./build/sn"
fi