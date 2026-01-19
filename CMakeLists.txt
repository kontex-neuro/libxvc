cmake_minimum_required(VERSION 3.25)

set(libxvc_VERSION 0.2.1)

set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum OS X deployment version" FORCE)

project(libxvc
    LANGUAGES CXX
    VERSION "${libxvc_VERSION}"
    DESCRIPTION "Thor Vision Video Capture Library"
    HOMEPAGE_URL "https://github.com/kontex-neuro/libxvc"
)

if(APPLE)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(Boost_USE_STATIC_LIBS ON)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(cpr REQUIRED)
find_package(xdaqmetadata REQUIRED)
find_package(Boost 1.81.0 REQUIRED)
find_package(CLI11 REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
pkg_search_module(gstreamer-video REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.4)

add_subdirectory(xdaqvc)
add_subdirectory(tool)

if(BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED)
    find_package(Catch2 REQUIRED)
    include(CTest)
    add_subdirectory(test)
endif()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libxvc-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libxvc-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libxvc"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libxvc-config-version.cmake"
    VERSION "${libxvc_VERSION}"
    COMPATIBILITY AnyNewerVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/libxvc-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libxvc-config-version.cmake"
    DESTINATION 
        "${CMAKE_INSTALL_LIBDIR}/cmake/libxvc"
)