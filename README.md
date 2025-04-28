# Thor Vision Video Capture Library

## Build instructions

1. Get the source code and go to the project directory
```sh
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
    ```sh
    python -m venv .venv
    source .venv/bin/activate
    ```

3. Install build tools in `.venv` via pip
```sh
pip install cmake conan ninja
```

4. Install dependencies via Conan
```sh
conan install . -b missing -pr:a <profile> -s build_type=Release
```

5. Generate the build files with CMake
```sh
cmake -S . -B build/Release --preset conan-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release
```

6. Build the project
```sh
cmake --build build/Release --preset conan-release
```

7. Export as conan package to local cache
```sh
conan export-pkg . -pr:a <profile> -s build_type=Release
```

## Run updater tests

1. Install dependencies with option `build_testing` enabled 
```sh
conan install . -b missing -pr:a <profile> -s build_type=Release -o build_testing=True
```

2. Generate the build files with CMake
```sh
cmake -S . -B build/Release --preset conan-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

3. Build the project
```sh
cmake --build build/Release --preset conan-release
```

4. Run tests
```sh
cd ./build/Release/test
ctest --output-on-failure
```

## Examples

### Check CLI optinos
```sh
./build/Release/tool/tvcli --help
```

Example output:
```sh
Thor Vision CLI 


tvcli [OPTIONS] [SUBCOMMAND]


OPTIONS:
  -h,     --help              Print this help message and exit 

SUBCOMMANDS:
  stream                      Stream camera 
  list                        List cameras 
  logs                        Show server logs
```

### List available cameras
```sh
./build/Release/tool/tvcli list
```

Example output:
```sh
Discovered Cameras:

Camera ID    : 0
Name         : DF100
Capabilities :
  - image/jpeg,width=1280,height=720,framerate=30/1
  - image/jpeg,width=1280,height=720,framerate=25/1
  - image/jpeg,width=1280,height=720,framerate=20/1
  - image/jpeg,width=1280,height=720,framerate=15/1
  - image/jpeg,width=1280,height=720,framerate=10/1
  - image/jpeg,width=1280,height=720,framerate=5/1
  - image/jpeg,width=800,height=600,framerate=30/1
  - image/jpeg,width=800,height=600,framerate=25/1
  - image/jpeg,width=800,height=600,framerate=20/1
  - image/jpeg,width=800,height=600,framerate=15/1
  - image/jpeg,width=800,height=600,framerate=10/1
  - image/jpeg,width=800,height=600,framerate=5/1
  - image/jpeg,width=640,height=480,framerate=30/1
  - image/jpeg,width=640,height=480,framerate=25/1
  - image/jpeg,width=640,height=480,framerate=20/1
  - image/jpeg,width=640,height=480,framerate=15/1
  - image/jpeg,width=640,height=480,framerate=10/1
  - image/jpeg,width=640,height=480,framerate=5/1
  - video/x-raw,format=YUY2,width=1280,height=720,framerate=10/1
  - video/x-raw,format=YUY2,width=1280,height=720,framerate=5/1
  - video/x-raw,format=YUY2,width=800,height=600,framerate=15/1
  - video/x-raw,format=YUY2,width=800,height=600,framerate=10/1
  - video/x-raw,format=YUY2,width=800,height=600,framerate=5/1
  - video/x-raw,format=YUY2,width=640,height=480,framerate=30/1
  - video/x-raw,format=YUY2,width=640,height=480,framerate=25/1
  - video/x-raw,format=YUY2,width=640,height=480,framerate=20/1
  - video/x-raw,format=YUY2,width=640,height=480,framerate=15/1
  - video/x-raw,format=YUY2,width=640,height=480,framerate=10/1
  - video/x-raw,format=YUY2,width=640,height=480,framerate=5/1
```

### Stream camera 
Stream from a camera with device ID `0`, using the capability `"image/jpeg,width=1280,height=720,framerate=30/1"`, with codec set to `jpeg`. 
Enable recording with `--record`, split the recording with `-s`, and limit the split files with `--max-size-time` (1 minute) and `--max-files` (4).

```sh
./build/Release/tool/tvcli stream --id 0 --cap "image/jpeg,width=1280,height=720,framerate=30/1" --codec "jpeg" --record -s --max-size-time 1 --max-files 4
```

Example output:
```sh
[2025-04-28 13:19:51.524] [info] Received buffer: size=2764800, pts=238181123, width=1280, height=720, fpga_timestamp=163344901787, rhythm_timestamp=0, ttl_in=0, ttl_out=0, spi_perf_counter=136144539, reserved=0
```

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