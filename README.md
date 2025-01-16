# XDAQ Video Capture Library

## Third-party

* CMake ([New BSD License](https://github.com/Kitware/CMake/blob/master/Copyright.txt))
* Ninja ([Apache License 2.0](https://github.com/ninja-build/ninja/blob/master/COPYING))
* fmt ([MIT License](https://github.com/fmtlib/fmt/blob/master/LICENSE))
* spdlog ([MIT License](https://github.com/gabime/spdlog/blob/v1.x/LICENSE))
* boost ([Boost Software License 1.0](https://github.com/boostorg/boost/blob/master/LICENSE_1_0.txt))
* nlohmann/json ([MIT License](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT))
* cpr ([MIT License](https://github.com/libcpr/cpr/blob/master/LICENSE))
* GStreamer ([LGPL-2.1](https://github.com/GStreamer/gstreamer/blob/main/LICENSE))
* googletest ([BSD 3-Clause](https://github.com/google/googletest/blob/main/LICENSE))

## Build instructions
    conan install . -b missing -pr:a <profile> -s build_type=Release
    cmake -S . -B build/Release --preset conan-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release
    cmake --build build/Release --preset conan-release
    
## Export as conan package to local cache
    conan export-pkg . -pr:a <profile> -s build_type=Release

## Run updater tests
    conan install . -b missing -pr:a <profile> -s build_type=Release -o build_testing=True
    cmake -S . -B build/Release --preset conan-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
    cmake --build build/Release --preset conan-release --target xvc_updater_tests
    cd ./build/Release/test
    ctest --output-on-failure
