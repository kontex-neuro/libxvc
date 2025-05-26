add_executable(xvc_updater_tests)

target_sources(xvc_updater_tests
    PRIVATE
        updater_test_base.h
        updater_test.cc
)
target_link_libraries(xvc_updater_tests
    PRIVATE
        libxvc
        gtest::gtest
        openssl::openssl
)
target_include_directories(xvc_updater_tests
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

add_test(
    NAME xvc_updater_tests 
    COMMAND xvc_updater_tests
)
target_compile_features(xvc_updater_tests PRIVATE cxx_std_20)
target_compile_options(xvc_updater_tests
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)

# add_executable(test_port_pool)

# target_sources(test_port_pool
#     PRIVATE
#         test_port_pool.cc
# )
# target_link_libraries(test_port_pool
#     PRIVATE
#         Catch2::Catch2
#         libxvc
# )
# target_compile_features(test_port_pool PRIVATE cxx_std_20)
# target_compile_options(test_port_pool
#     PRIVATE
#         $<$<CXX_COMPILER_ID:MSVC>:/W4>
#         $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
# )