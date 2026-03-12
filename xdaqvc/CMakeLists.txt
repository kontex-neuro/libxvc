add_library(xvc STATIC)

set(sources
    xvc.cc
    camera.cc
    port_pool.cc
    ws_client.cc
    server.cc
    validator.cc
)
set(headers
    xvc.h
    camera.h
    port_pool.h
    ws_client.h
    server.h
    validator.h
    common.h
)

target_sources(xvc
    PRIVATE
        "${sources}"
    PUBLIC 
        FILE_SET "public_headers"
        TYPE "HEADERS"
        FILES "${headers}"
)
target_include_directories(xvc
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
set_target_properties(xvc PROPERTIES 
    VERSION "${libxvc_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    POSITION_INDEPENDENT_CODE ON
)

target_compile_features(xvc PUBLIC cxx_std_23)
target_compile_options(xvc
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
    if(MSVC)
        message(STATUS "MSVC Debug not set ASAN options")
    else()
        target_compile_options(libxvc PUBLIC -fsanitize=address,undefined)
        target_link_options(libxvc PUBLIC -fsanitize=address,undefined)
    endif()
endif()

target_link_libraries(xvc 
    PUBLIC
        Boost::boost
        # TODO:
        xdaqmetadata::xdaqmetadata
    PRIVATE
        cpr::cpr
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator
        PkgConfig::gstreamer
)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "default install path" FORCE)
endif()

install(
    TARGETS xvc
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

add_library(updater STATIC)

target_sources(updater
    PRIVATE
        updater.cc
    PUBLIC 
        FILE_SET "public_headers"
        TYPE "HEADERS"
        FILES updater.h
)
target_compile_features(updater PUBLIC cxx_std_23)
target_compile_options(updater
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)
target_link_libraries(updater 
    PRIVATE
        cpr::cpr
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
)