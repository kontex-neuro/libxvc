target_sources(xvc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/xvc.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/camera.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/port_pool.cc
        # ${CMAKE_CURRENT_SOURCE_DIR}/stream.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/ws_client.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/server.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/validator.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/port_pool.h
        ${CMAKE_CURRENT_SOURCE_DIR}/validator.h
    PUBLIC 
        FILE_SET "public_headers"
        TYPE "HEADERS"
        FILES 
            ${CMAKE_CURRENT_SOURCE_DIR}/xvc.h
            ${CMAKE_CURRENT_SOURCE_DIR}/camera.h
            ${CMAKE_CURRENT_SOURCE_DIR}/server.h
            # ${CMAKE_CURRENT_SOURCE_DIR}/stream.h
            ${CMAKE_CURRENT_SOURCE_DIR}/common.h
            ${CMAKE_CURRENT_SOURCE_DIR}/ws_client.h
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
)

# if(BUILD_SHARED_LIBS)
#     target_compile_definitions(xvc PRIVATE "xvc_EXPORTS")
# endif()