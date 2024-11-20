set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.81.0 REQUIRED COMPONENTS program_options)

add_executable(xvc_tool xvc_tool.cc)
target_link_libraries(xvc_tool 
    PRIVATE
        Boost::program_options
        fmt::fmt
        spdlog::spdlog
        PkgConfig::gstreamer
        libxvc
)
target_compile_features(xvc_tool PRIVATE cxx_std_20)
