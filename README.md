# Thor Vision Video Capture Library

## Build instructions

1. Get the source code and go to the project directory
```console
git clone https://github.com/kontex-neuro/libxvc.git
cd libxvc
```

2. Create python virtual environment `.venv` in the project directory and activate it
    #### Windows
    ```console
    py -m venv .venv
    .venv\Scripts\activate
    ```
    #### Linux/macOS
    ```bash
    python -m venv .venv
    source .venv/bin/activate
    ```

3. Install build tools in `.venv` via pip
```console
pip install conan ninja
```

4. Install dependencies via Conan
```console
conan install . -b missing -pr:a <profile> -s build_type=Release
```

5. Generate the build files with CMake
```console
cmake -S . -B build/Release --preset conan-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release
```

6. Build the project
```console
cmake --build build/Release --preset conan-release
```

7. Export as conan package to local cache
```console
conan export-pkg . -pr:a <profile> -s build_type=Release
```

## Run updater tests

1. Install dependencies with option `build_testing` enabled 
```console
conan install . -b missing -pr:a <profile> -s build_type=Release -o build_testing=True
```

2. Generate the build files with CMake
```console
cmake -S . -B build/Release --preset conan-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

3. Build the project
```console
cmake --build build/Release --preset conan-release
```

4. Run tests
```console
cd ./build/Release/test
ctest --output-on-failure
```

## Examples (coming soon)

## Third-party

* boost ([Boost Software License 1.0](https://github.com/boostorg/boost/blob/master/LICENSE_1_0.txt))
* CMake ([New BSD License](https://github.com/Kitware/CMake/blob/master/Copyright.txt))
* cpr ([MIT License](https://github.com/libcpr/cpr/blob/master/LICENSE))
* fmt ([MIT License](https://github.com/fmtlib/fmt/blob/master/LICENSE))
* GStreamer ([LGPL-2.1](https://github.com/GStreamer/gstreamer/blob/main/LICENSE))
* GoogleTest ([BSD 3-Clause](https://github.com/google/googletest/blob/main/LICENSE))
* Ninja ([Apache License 2.0](https://github.com/ninja-build/ninja/blob/master/COPYING))
* nlohmann/json ([MIT License](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT))
* spdlog ([MIT License](https://github.com/gabime/spdlog/blob/v1.x/LICENSE))