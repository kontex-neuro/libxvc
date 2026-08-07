add_executable(tvcli tvcli.cc)
target_link_libraries(tvcli 
    PRIVATE
        xvc
        xdaqmetadata::xdaqmetadata
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