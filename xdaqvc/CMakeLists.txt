target_sources(xvc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/xvc.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/camera.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/port_pool.cc
        # ${CMAKE_CURRENT_SOURCE_DIR}/stream.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/ws_client.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/server.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/validator.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/update/artifact.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/update/manifest.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/port_pool.h
        ${CMAKE_CURRENT_SOURCE_DIR}/validator.h
    PUBLIC
        # The install rule sends this file set to <includedir>/xdaqvc, and headers are placed
        # relative to BASE_DIRS. Basing on this directory therefore keeps the existing flat
        # headers at xdaqvc/<name>.h while nesting the new ones at xdaqvc/update/<name>.h
        # (ADR 0010). Getting this wrong only surfaces when a consumer builds against the
        # installed package, never during local development.
        FILE_SET "public_headers"
        TYPE "HEADERS"
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/xvc.h
            ${CMAKE_CURRENT_SOURCE_DIR}/camera.h
            ${CMAKE_CURRENT_SOURCE_DIR}/server.h
            # ${CMAKE_CURRENT_SOURCE_DIR}/stream.h
            ${CMAKE_CURRENT_SOURCE_DIR}/common.h
            ${CMAKE_CURRENT_SOURCE_DIR}/ws_client.h
            ${CMAKE_CURRENT_SOURCE_DIR}/update/update.h
            ${CMAKE_CURRENT_SOURCE_DIR}/update/types.h
            ${CMAKE_CURRENT_SOURCE_DIR}/update/artifact.h
            ${CMAKE_CURRENT_SOURCE_DIR}/update/manifest.h
)
set_target_properties(xvc 
    PROPERTIES 
        # TODO
        INSTALL_RPATH "@executable_path/../Frameworks;@loader_path/../Frameworks"
        # VERSION "${libxvc_VERSION}"
        # SOVERSION "${PROJECT_VERSION_MAJOR}"
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
)

target_compile_features(xvc PUBLIC cxx_std_23)
target_compile_options(xvc
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
)

target_link_libraries(xvc 
    PUBLIC
        Boost::boost
        PkgConfig::gstreamer
        nlohmann_json::nlohmann_json
    PRIVATE
        cpr::cpr
        spdlog::spdlog
        nlohmann_json_schema_validator
        # SHA-256 via platform crypto (ADR 0001). bcrypt is an import lib from the Windows
        # SDK -- nothing is redistributed. macOS needs no entry: CommonCrypto lives inside
        # libSystem.dylib, which every Mach-O binary already links.
        $<$<PLATFORM_ID:Windows>:bcrypt>
)

# if(BUILD_SHARED_LIBS)
#     target_compile_definitions(xvc PRIVATE "xvc_EXPORTS")
# endif()