# XDAQ Video Capture Library

### Build instructions

    conan install . -b missing -pr:a default -s build_type=Release
    cmake -S . -B build/Release --preset conan-release -G "Ninja" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<path\to\xdaqvc\libxvc>
    cmake --build build/Release --preset conan-release --target install