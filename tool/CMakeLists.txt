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

# Common settings for all test executables
function(add_xvc_test name)
    add_executable(${name} ${name}.cc)
    target_include_directories(${name} 
        PRIVATE 
            ${CMAKE_SOURCE_DIR}
            ${CONAN_INCLUDE_DIRS}
    )
    target_link_libraries(${name} 
        PRIVATE 
            libxvc
            fmt::fmt
            spdlog::spdlog
            PkgConfig::gstreamer
            cpr::cpr
            nlohmann_json::nlohmann_json
    )
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_options(${name}
        PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
    )
endfunction()

# Add test executables
add_xvc_test(test_handshake)
add_xvc_test(test_prepare_transfer)
add_xvc_test(test_download_verify)
add_xvc_test(test_transfer_file)