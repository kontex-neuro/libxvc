add_executable(tvcli tvcli.cc)
target_link_libraries(tvcli 
    PRIVATE
        libxvc
        CLI11::CLI11
        PkgConfig::gstreamer-app
        PkgConfig::gstreamer-video
)
target_compile_features(tvcli PRIVATE cxx_std_20)
target_compile_options(tvcli
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        # $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
        # $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(xvc_update_tool xvc_update_tool.cc)
target_link_libraries(xvc_update_tool
    PRIVATE
        libxvc
        CLI11::CLI11
        openssl::openssl
)
target_compile_features(xvc_update_tool PRIVATE cxx_std_20)
target_compile_options(xvc_update_tool
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)