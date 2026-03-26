cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCUDA_ROOT=/usr/local/cuda
cmake --build build -j