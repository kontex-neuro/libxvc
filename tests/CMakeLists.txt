# ADR 0009: tests are a local developer tool, not a CI gate. catch_discover_tests is what
# makes `ctest` find anything at all -- nothing was registered before, so ctest discovered
# zero tests regardless of how many executables existed.
include(Catch)

add_executable(test_update_sha256 test_update_sha256.cc)
target_link_libraries(test_update_sha256
    PRIVATE
        xvc
        Catch2::Catch2WithMain
)
target_compile_features(test_update_sha256 PRIVATE cxx_std_23)
target_compile_options(test_update_sha256
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
catch_discover_tests(test_update_sha256)

add_executable(test_update_manifest test_update_manifest.cc)
target_link_libraries(test_update_manifest
    PRIVATE
        xvc
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
)
target_compile_features(test_update_manifest PRIVATE cxx_std_23)
# Resolves the fixtures regardless of the directory the test binary is launched from.
target_compile_definitions(test_update_manifest
    PRIVATE XVC_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
target_compile_options(test_update_manifest
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
catch_discover_tests(test_update_manifest)

add_executable(test_update_plan test_update_plan.cc)
target_link_libraries(test_update_plan
    PRIVATE
        xvc
        Catch2::Catch2WithMain
)
target_compile_features(test_update_plan PRIVATE cxx_std_23)
target_compile_options(test_update_plan
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
catch_discover_tests(test_update_plan)

add_executable(test_update_artifact test_update_artifact.cc)
target_link_libraries(test_update_artifact
    PRIVATE
        xvc
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
)
target_compile_features(test_update_artifact PRIVATE cxx_std_23)
target_compile_options(test_update_artifact
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
catch_discover_tests(test_update_artifact)

add_executable(test_update_device test_update_device.cc)
target_link_libraries(test_update_device
    PRIVATE
        xvc
        Catch2::Catch2WithMain
)
target_compile_features(test_update_device PRIVATE cxx_std_23)
target_compile_options(test_update_device
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
catch_discover_tests(test_update_device)

add_executable(test_port_pool test_port_pool.cc)
target_link_libraries(test_port_pool
    PRIVATE
        xvc
        Catch2::Catch2WithMain
)
target_compile_features(test_port_pool PRIVATE cxx_std_23)
target_compile_options(test_port_pool
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
catch_discover_tests(test_port_pool)

# NOT registered with CTest on purpose: test_ws_client connects to a hardcoded device at
# 192.168.177.100 and blocks waiting for camera events, so registering it would make `ctest`
# hang on any checkout without that hardware on the network. This is the coupling ADR 0009
# identifies as the reason the previous test suite went unrun. It still builds, and can be
# invoked directly on the bench.
add_executable(test_ws_client test_ws_client.cc)
target_link_libraries(test_ws_client
    PRIVATE
        xvc
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
)
target_compile_features(test_ws_client PRIVATE cxx_std_23)
target_compile_options(test_ws_client
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)

add_executable(test_camera test_camera.cc)
target_link_libraries(test_camera
    PRIVATE
        xvc
        Catch2::Catch2WithMain
)
target_compile_features(test_camera PRIVATE cxx_std_23)
target_compile_options(test_camera
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
catch_discover_tests(test_camera)