@echo off
setlocal

:: Builds and runs the local test suite (ADR 0009: tests are a local developer tool, not a
:: CI gate). Tier 1 and Tier 2 tests run here with no network and no device.
::
:: Tier 3 tests are tagged [.integration] and are NOT run by this script. To exercise the
:: device protocol on the bench:
::
::   set XVC_TEST_DEVICE_HOST=<device-ip>
::   build\tests\test_update_device.exe "[.integration]"
::
:: The destructive transfer case additionally requires XVC_TEST_DEVICE_ARTIFACT to be set to
:: a manifest.json path; it installs a package and restarts the device.

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
if errorlevel 1 goto :failed

:: Set PKG_CONFIG_PATH to GStreamer installation
@REM set PKG_CONFIG_PATH=C:\gstreamer\1.0\msvc_x86_64\lib\pkgconfig

conan install . -b missing -pr:a default -s build_type=Release
if errorlevel 1 goto :failed

:: The option is BUILD_TESTS. This previously passed BUILD_TESTING, which CMake silently
:: ignored, so the tests were never built and the script appeared to succeed.
cmake --preset conan-release -DBUILD_TESTS=ON
if errorlevel 1 goto :failed

:: Build every registered test target. The old script named xvc_updater_tests, a target
:: removed in 72314ca, so this step had been failing outright.
cmake --build build
if errorlevel 1 goto :failed

pushd build
ctest --output-on-failure
set TEST_RESULT=%errorlevel%
popd

if not "%TEST_RESULT%"=="0" goto :failed

echo.
echo All tests passed.
exit /b 0

:failed
echo.
echo BUILD OR TESTS FAILED.
exit /b 1
