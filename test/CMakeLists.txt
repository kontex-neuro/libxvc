project(xvc_tests)

add_executable(xvc_updater_tests
    updater_test_base.h
    updater_test.cc
)

target_link_libraries(xvc_updater_tests
    PRIVATE
        libxvc          # This already includes most dependencies
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(xvc_updater_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

add_test(NAME xvc_updater_tests COMMAND xvc_updater_tests)

target_compile_features(xvc_updater_tests PRIVATE cxx_std_20)

# Optional: Add compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xvc_updater_tests PRIVATE -Wall -Wextra -Wpedantic)
endif() 