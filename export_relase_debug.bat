@REM Remove existing package from cache to ensure a fresh build
call conan remove -c "libxvc/*"

@REM Build and create Debug package
@REM add options to build depended library: --build=spdlog* --build=fmt*
call conan create . -pr:a default -s build_type=Debug -s compiler.runtime_type=Debug -b missing

@REM Build and create Release package
@REM add options to build depended library: --build=spdlog* --build=fmt*
call conan create . -pr:a default -s build_type=Release -s compiler.runtime_type=Release -b missing

echo.
echo Verifying packages in Conan cache:
call conan list libxvc/0.1.2:*
echo.
echo Build complete! Both Debug and Release packages are now in the Conan cache.