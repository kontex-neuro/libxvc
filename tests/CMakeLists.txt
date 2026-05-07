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