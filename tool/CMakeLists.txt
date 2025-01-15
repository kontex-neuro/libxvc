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
target_compile_options(xvc_tool
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        # $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
        # $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)



# Add the new update tool
add_executable(xvc_update_tool xvc_update_tool.cc)
target_link_libraries(xvc_update_tool
PRIVATE
Boost::program_options
fmt::fmt
spdlog::spdlog
libxvc
)
target_compile_features(xvc_update_tool PRIVATE cxx_std_20)
target_compile_options(xvc_update_tool
PRIVATE
$<$<CXX_COMPILER_ID:MSVC>:/W4>
$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
# Include directories
target_include_directories(xvc_update_tool
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)