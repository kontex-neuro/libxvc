cmake_minimum_required(VERSION 3.25)

project(libxvc
    LANGUAGES CXX
    VERSION 0.0.1
    DESCRIPTION "XDAQ Video Capture Library"
    HOMEPAGE_URL "https://kontex.io/" 
)

add_library(libxvc STATIC)

set(xvc_sources
    xvc/src/xvc.cc
    xvc/src/camera.cc
    xvc/src/port_pool.cc
)
set(xvc_headers
    xvc/include/xvc.h
    xvc/include/camera.h
    xvc/include/port_pool.h
)

target_sources(libxvc
    PRIVATE 
        ${xvc_sources} 
        ${xvc_headers}
)
target_include_directories(libxvc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/xvc/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/xvc/src
)

set_target_properties(libxvc PROPERTIES 
    VERSION 0.0.1 
    SOVERSION 0
    PUBLIC_HEADER "${xvc_headers}"
    # POSITION_INDEPENDENT_CODE ON
)

# set(Boost_USE_STATIC_LIBS ON)
# find_package(Boost 1.81.0 REQUIRED COMPONENTS program_options)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(cpr REQUIRED)
find_package(xdaqmetadata REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
# pkg_search_module(gstreamer-codecparsers REQUIRED IMPORTED_TARGET gstreamer-codecparsers-1.0>=1.4)

target_link_libraries(libxvc PUBLIC
    fmt::fmt
    cpr::cpr
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    PkgConfig::gstreamer
    # PkgConfig::gstreamer-video
    # PkgConfig::gstreamer-codecparsers
    xdaq::xdaqmetadata
)
target_compile_features(libxvc PUBLIC cxx_std_20)

install(TARGETS libxvc
    # EXPORT libxvcExports
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# install(EXPORT libxvcExports
#     FILE libxvc-config.cmake
#     NAMESPACE xdaq::
#     DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libxvc
# )