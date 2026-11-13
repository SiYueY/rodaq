if(NOT DEFINED RODAQ_SOURCE_DIR)
    message(FATAL_ERROR "RODAQ_SOURCE_DIR is required")
endif()

set(public_headers
    "${RODAQ_SOURCE_DIR}/include/rodaq/plugin/interface.hpp"
    "${RODAQ_SOURCE_DIR}/include/rodaq/plugin/plugin.hpp"
    "${RODAQ_SOURCE_DIR}/include/rodaq/plugin/service_registry.hpp")

foreach(header IN LISTS public_headers)
    file(READ "${header}" content)
    if(content MATCHES "ManagedOperationAccess|ManagedInterfaceReference|OperationBuilder")
        message(FATAL_ERROR "Internal Plugin SDK type leaked into ${header}")
    endif()
endforeach()

file(READ "${RODAQ_SOURCE_DIR}/include/rodaq/plugin/export.hpp" export_content)
if(export_content MATCHES "namespace rodaq::plugin::sdk")
    message(FATAL_ERROR "Legacy public sdk namespace is still present")
endif()
