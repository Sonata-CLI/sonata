cmake -S . -B build
cmake --build build
if [ -f "./build/sn" ]; then
    chmod +x "./build/sn"
fi