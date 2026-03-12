add_executable(tvcli tvcli.cc)
target_link_libraries(tvcli 
    PRIVATE
        xvc
        CLI11::CLI11
        PkgConfig::gstreamer-app
        PkgConfig::gstreamer-video
)
target_compile_features(tvcli PRIVATE cxx_std_23)
target_compile_options(tvcli
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)

add_executable(xvc_update_tool xvc_update_tool.cc)
target_link_libraries(xvc_update_tool
    PRIVATE
        updater
        CLI11::CLI11
        spdlog::spdlog
)
target_compile_features(xvc_update_tool PRIVATE cxx_std_23)
target_compile_options(xvc_update_tool
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)