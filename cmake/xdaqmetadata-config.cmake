add_library(xdaq::xdaqmetadata SHARED IMPORTED)

target_include_directories(xdaq::xdaqmetadata INTERFACE
    ${PROJECT_SOURCE_DIR}/xvc/xdaqmetadata
)

if(APPLE)
    set_target_properties(xdaq::xdaqmetadata PROPERTIES
        IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/xvc/xdaqmetadata/lib/xdaqmetadata.dylib
    )
elseif(WIN32)
    set_target_properties(xdaq::xdaqmetadata PROPERTIES
        IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/xvc/xdaqmetadata/bin/xdaqmetadata.dll
        IMPORTED_IMPLIB ${PROJECT_SOURCE_DIR}/xvc/xdaqmetadata/lib/xdaqmetadata.lib
    )
else()
    set_target_properties(xdaq::xdaqmetadata PROPERTIES
        IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/xvc/xdaqmetadata/bin/xdaqmetadata.so
        IMPORTED_NO_SONAME TRUE
    )
endif()

set(xdaqmetadata_LIBRARIES xdaq::xdaqmetadata)
set(xdaqmetadata_FOUND TRUE)