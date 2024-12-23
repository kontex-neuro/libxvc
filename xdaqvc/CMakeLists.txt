add_library(libxvc STATIC)

set(XVC_SOURCES
    xvc.cc
    camera.cc
    port_pool.cc
    ws_client.cc
)
set(XVC_HEADERS
    xvc.h
    camera.h
    port_pool.h
    ws_client.h
)

target_sources(libxvc
    PRIVATE
        "${XVC_SOURCES}"
    PUBLIC 
        FILE_SET "public_headers"
        TYPE "HEADERS"
        FILES "${XVC_HEADERS}"
)
target_include_directories(libxvc
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
set_target_properties(libxvc PROPERTIES 
    VERSION "${libxvc_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    POSITION_INDEPENDENT_CODE ON
)

target_compile_features(libxvc PUBLIC cxx_std_20)
target_compile_options(libxvc
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        # $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
        # $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)
target_link_libraries(libxvc 
    PUBLIC
        fmt::fmt
        cpr::cpr
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        PkgConfig::gstreamer
        xdaqmetadata::xdaqmetadata
        Boost::boost
)

install(
    TARGETS libxvc
    EXPORT libxvc-targets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    FILE_SET "public_headers" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/xdaqvc"
)
install(
    EXPORT libxvc-targets
    FILE libxvc-targets.cmake
    NAMESPACE libxvc::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libxvc"
)
export(
    EXPORT libxvc-targets 
    FILE libxvc-config.cmake
    NAMESPACE libxvc::
)