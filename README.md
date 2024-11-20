# XDAQ Video Capture Library

### Build instructions
    conan install . -b missing -pr:a <profile> -s build_type=Release
    cmake -S . -B build/Release --preset conan-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release
    cmake --build build/Release --preset conan-release
    
### Export as conan package to local cache
    conan export-pkg . -pr:a <profile> -s build_type=Release