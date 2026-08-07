cmake_minimum_required(VERSION 3.25)

set(libxvc_VERSION 0.4.0)

project(libxvc
    LANGUAGES CXX
    VERSION "${libxvc_VERSION}"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(Boost_USE_STATIC_LIBS ON)
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()
if(APPLE)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
endif()
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "default install path" FORCE)
endif()

option(BUILD_SHARED_LIBS "Build as shared library" OFF)
option(BUILD_TESTS "Build tests" OFF)
option(BUILD_TOOLS "Build tools" OFF)
# option(BUILD_PYTHON_BINDINGS "Build python bindings" ON)
# option(BUILD_PYTHON_STUBS "Build python stubs" OFF)
option(USE_ASAN "Enable AddressSanitizer (ASan)" OFF)
option(USE_UBSAN "Enable UndefinedBehaviorSanitizer (UBSan)" OFF)
option(USE_TSAN "Enable ThreadSanitizer (TSan)" OFF)

if(USE_ASAN AND USE_TSAN)
    message(
        FATAL_ERROR
        "AddressSanitizer (ASan) and ThreadSanitizer (TSan) are mutually exclusive and cannot be enabled at the same time."
    )
endif()

set(SANITIZER_COMPILE_FLAGS "")
set(SANITIZER_LINK_FLAGS "")

if(USE_ASAN)
    if(WIN32 AND MSVC)
        list(APPEND SANITIZER_COMPILE_FLAGS /fsanitize=address)
        list(APPEND SANITIZER_LINK_FLAGS /fsanitize=address)
    else()
        list(APPEND SANITIZER_COMPILE_FLAGS -fsanitize=address)
        list(APPEND SANITIZER_LINK_FLAGS -fsanitize=address)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND SANITIZER_LINK_FLAGS -lpthread)
        endif()
    endif()
endif()

if(USE_UBSAN)
    if(WIN32 AND MSVC)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            list(APPEND SANITIZER_COMPILE_FLAGS -fsanitize=undefined)
            list(APPEND SANITIZER_LINK_FLAGS -fsanitize=undefined)
        else()
            message(
                WARNING
                "UndefinedBehaviorSanitizer (UBSan) is not directly supported via a simple flag in MSVC."
            )
        endif()
    else()
        list(APPEND SANITIZER_COMPILE_FLAGS -fsanitize=undefined)
        list(APPEND SANITIZER_LINK_FLAGS -fsanitize=undefined)
    endif()
endif()

if(USE_TSAN)
    if(WIN32 AND MSVC)
        message(
            FATAL_ERROR
            "ThreadSanitizer (TSan) is not supported by the MSVC compiler. Please use Clang or GCC."
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "ThreadSanitizer (TSan) is not supported on macOS.")
    else()
        list(APPEND SANITIZER_COMPILE_FLAGS -fsanitize=thread)
        list(APPEND SANITIZER_LINK_FLAGS -fsanitize=thread)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND SANITIZER_LINK_FLAGS -lpthread)
        endif()
    endif()
endif()

add_library(xvc)

target_compile_options(xvc PUBLIC ${SANITIZER_COMPILE_FLAGS})
target_link_options(xvc PUBLIC ${SANITIZER_LINK_FLAGS})

find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(nlohmann_json_schema_validator REQUIRED)
find_package(cpr REQUIRED)
find_package(Boost 1.81.0 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)

add_subdirectory(xdaqvc)

if(BUILD_TESTS)
    enable_testing()
    find_package(Catch2 REQUIRED)
    add_subdirectory(tests)
endif()

if(BUILD_TOOLS)
    find_package(xdaqmetadata REQUIRED)
    find_package(CLI11 REQUIRED)
    pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
    pkg_search_module(gstreamer-video REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.4)
    add_subdirectory(tools)
endif()

# if(BUILD_PYTHON_BINDINGS)
#     find_package(Python 3.9 
#         REQUIRED COMPONENTS Interpreter Development.Module
#         # OPTIONAL_COMPONENTS Development.SABIModule
#     )
#     find_package(nanobind CONFIG REQUIRED)
#     add_subdirectory(python/src)
# endif()

include(GNUInstallDirs)

install(
    TARGETS xvc
    EXPORT libxvc-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    FILE_SET "public_headers" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xdaqvc
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

include(CMakePackageConfigHelpers)

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
