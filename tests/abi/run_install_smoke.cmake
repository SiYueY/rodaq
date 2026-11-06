file(REMOVE_RECURSE "${RODAQ_TEST_INSTALL_PREFIX}")
file(MAKE_DIRECTORY "${RODAQ_TEST_OBJECT_DIR}")

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        --install
        "${RODAQ_TEST_BINARY_DIR}"
        --prefix
        "${RODAQ_TEST_INSTALL_PREFIX}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)

if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "RoDAQ install failed.\n${install_stdout}\n${install_stderr}")
endif()

set(include_dir "${RODAQ_TEST_INSTALL_PREFIX}/include")
set(plugin_abi_dir "${include_dir}/rodaq/plugin/abi")
set(legacy_abi_dir "${include_dir}/rodaq/abi")
set(expected_license
    "${RODAQ_TEST_INSTALL_PREFIX}/share/rodaq/licenses/expected-lite/LICENSE.txt")
set(runtime_header "${include_dir}/rodaq/daq.hpp")
set(runtime_config_header "${include_dir}/rodaq/daq_config.hpp")
set(dependency_inventory
    "${RODAQ_TEST_INSTALL_PREFIX}/share/rodaq/sbom/rodaq-dependency-inventory.json")
set(exported_targets
    "${RODAQ_TEST_INSTALL_PREFIX}/lib/cmake/rodaq/RoDAQTargets.cmake")

foreach(header_name common canonical service config runtime acquisition storage plugin)
    if(NOT EXISTS "${plugin_abi_dir}/${header_name}.h")
        message(FATAL_ERROR
            "Installed Plugin ABI header is missing: ${header_name}.h")
    endif()
endforeach()

foreach(sdk_header
        abi_support.hpp
        config_view.hpp
        service_registry.hpp
        services.hpp
        plugin.hpp
        acquisition_plugin.hpp
        storage_plugin.hpp)
    if(NOT EXISTS "${include_dir}/rodaq/plugin/${sdk_header}")
        message(FATAL_ERROR "Installed SDK header is missing: ${sdk_header}")
    endif()
endforeach()

if(EXISTS "${legacy_abi_dir}")
    message(FATAL_ERROR "Legacy installed ABI directory still exists: ${legacy_abi_dir}")
endif()

if(NOT EXISTS "${expected_license}")
    message(FATAL_ERROR "Installed expected-lite license is missing")
endif()

foreach(installed_file
        "${runtime_header}"
        "${runtime_config_header}"
        "${dependency_inventory}"
        "${RODAQ_TEST_INSTALL_PREFIX}/share/rodaq/licenses/HdrHistogram_c/LICENSE.txt"
        "${RODAQ_TEST_INSTALL_PREFIX}/share/rodaq/licenses/spdlog/LICENSE"
        "${RODAQ_TEST_INSTALL_PREFIX}/share/rodaq/licenses/fmt/LICENSE"
        "${RODAQ_TEST_INSTALL_PREFIX}/share/rodaq/licenses/yaml-cpp/LICENSE")
    if(NOT EXISTS "${installed_file}")
        message(FATAL_ERROR "Installed DAQ/SBOM file is missing: ${installed_file}")
    endif()
endforeach()

file(READ "${exported_targets}" exported_targets_text)
if(NOT exported_targets_text MATCHES "add_library\\(rodaq::daq SHARED IMPORTED\\)")
    message(FATAL_ERROR "RoDAQTargets does not export the DAQ shared target")
endif()
if(NOT exported_targets_text MATCHES "add_library\\(rodaq::sdk STATIC IMPORTED\\)")
    message(FATAL_ERROR "RoDAQTargets does not export the SDK static target")
endif()
foreach(forbidden_target
        "rodaq_headers"
        "rodaq_public_api"
        "rodaq_api_support"
        "rodaq_expected_lite"
        "yaml-cpp"
        "rodaq_plugin"
        "rodaq_objects"
        "rodaq_testing")
    if(exported_targets_text MATCHES "add_library\\([^\\n]*${forbidden_target}")
        message(FATAL_ERROR
            "Internal or third-party target escaped RoDAQTargets: ${forbidden_target}")
    endif()
endforeach()

find_program(RODAQ_READELF_EXECUTABLE readelf)
if(RODAQ_READELF_EXECUTABLE)
    set(installed_runtime
        "${RODAQ_TEST_INSTALL_PREFIX}/lib/librodaq.so")
    execute_process(
        COMMAND "${RODAQ_READELF_EXECUTABLE}" -d "${installed_runtime}"
        RESULT_VARIABLE readelf_result
        OUTPUT_VARIABLE readelf_output
        ERROR_VARIABLE readelf_error
    )
    if(NOT readelf_result EQUAL 0)
        message(FATAL_ERROR "readelf failed for installed DAQ library: ${readelf_error}")
    endif()
    foreach(private_dependency
            "libfmt" "libspdlog" "libyaml-cpp" "libhdr_histogram")
        if(readelf_output MATCHES "${private_dependency}")
            message(FATAL_ERROR
                "Private DAQ dependency escaped as DT_NEEDED: ${private_dependency}")
        endif()
    endforeach()
endif()

find_program(RODAQ_NM_EXECUTABLE nm)
if(RODAQ_NM_EXECUTABLE)
    execute_process(
        COMMAND "${RODAQ_NM_EXECUTABLE}" -D -C --defined-only
                "${RODAQ_TEST_INSTALL_PREFIX}/lib/librodaq.so"
        RESULT_VARIABLE nm_result
        OUTPUT_VARIABLE nm_output
        ERROR_VARIABLE nm_error
    )
    if(NOT nm_result EQUAL 0)
        message(FATAL_ERROR "nm failed for installed DAQ library: ${nm_error}")
    endif()
    if(nm_output MATCHES "fmt::|spdlog::|yaml::|hdr_")
        message(FATAL_ERROR "Private implementation symbols escaped the DAQ DSO")
    endif()
    foreach(internal_symbol
            "rodaq::DAQ::Impl"
            "rodaq::ActivationState"
            "rodaq::AdmissionGate"
            "rodaq::PayloadPool"
            "rodaq::Registry"
            "rodaq::StoragePipeline")
        if(nm_output MATCHES "${internal_symbol}")
            message(FATAL_ERROR
                "Internal implementation symbol escaped the DAQ DSO: ${internal_symbol}")
        endif()
    endforeach()
    foreach(public_symbol
            "rodaq::DAQ::start"
            "rodaq::DAQ::stop"
            "rodaq::DAQ::lifecycle_snapshot"
            "rodaq::DAQ::stats"
            "rodaq::Status::ok"
            "rodaq::ConfigTree::root"
            "rodaq::ConfigNodeRef::find"
            "rodaq::load_yaml_config"
            "rodaq::load_yaml_config_file")
        if(NOT nm_output MATCHES "${public_symbol}")
            message(FATAL_ERROR
                "Expected DAQ application symbol is missing: ${public_symbol}")
        endif()
    endforeach()

    execute_process(
        COMMAND "${RODAQ_NM_EXECUTABLE}" -u -C
                "${RODAQ_TEST_INSTALL_PREFIX}/lib/librodaq_sdk.a"
        RESULT_VARIABLE sdk_nm_result
        OUTPUT_VARIABLE sdk_undefined_output
        ERROR_VARIABLE sdk_nm_error
    )
    if(NOT sdk_nm_result EQUAL 0)
        message(FATAL_ERROR "nm failed for installed SDK archive: ${sdk_nm_error}")
    endif()
    if(sdk_undefined_output MATCHES "YAML::|yaml-cpp|load_yaml_config|rodaq::DAQ|spdlog::|fmt::")
        message(FATAL_ERROR
            "Installed SDK archive depends on DAQ, YAML, fmt, or spdlog")
    endif()
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_C_COMPILER}"
        -std=c11
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        "-I${include_dir}"
        -c
        "${RODAQ_TEST_C_SOURCE}"
        -o
        "${RODAQ_TEST_OBJECT_DIR}/c_consumer.o"
    RESULT_VARIABLE c_result
    OUTPUT_VARIABLE c_stdout
    ERROR_VARIABLE c_stderr
)

if(NOT c_result EQUAL 0)
    message(FATAL_ERROR
        "Installed C ABI consumer failed to compile.\n${c_stdout}\n${c_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CXX_COMPILER}"
        -std=c++17
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        "-I${include_dir}"
        -c
        "${RODAQ_TEST_CPP_SOURCE}"
        -o
        "${RODAQ_TEST_OBJECT_DIR}/cpp_consumer.o"
    RESULT_VARIABLE cpp_result
    OUTPUT_VARIABLE cpp_stdout
    ERROR_VARIABLE cpp_stderr
)

if(NOT cpp_result EQUAL 0)
    message(FATAL_ERROR
        "Installed C++ ABI consumer failed to compile.\n${cpp_stdout}\n${cpp_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CXX_COMPILER}"
        -std=c++17
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        "-I${include_dir}"
        -c
        "${RODAQ_TEST_RESULT_SOURCE}"
        -o
        "${RODAQ_TEST_OBJECT_DIR}/result_consumer.o"
    RESULT_VARIABLE result_result
    OUTPUT_VARIABLE result_stdout
    ERROR_VARIABLE result_stderr
)

if(NOT result_result EQUAL 0)
    message(FATAL_ERROR
        "Installed Result consumer failed to compile.\n"
        "${result_stdout}\n${result_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        -S
        "${RODAQ_TEST_SDK_PLUGIN_SOURCE_DIR}"
        -B
        "${RODAQ_TEST_OBJECT_DIR}/sdk-plugin-consumer"
        "-DCMAKE_PREFIX_PATH=${RODAQ_TEST_INSTALL_PREFIX}"
        "-DCMAKE_CXX_FLAGS=${RODAQ_TEST_CONSUMER_SANITIZER_FLAGS}"
        "-DCMAKE_SHARED_LINKER_FLAGS=${RODAQ_TEST_CONSUMER_SANITIZER_FLAGS}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed SDK Plugin consumer failed to configure.\n"
        "${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        --build
        "${RODAQ_TEST_OBJECT_DIR}/sdk-plugin-consumer"
    RESULT_VARIABLE sdk_build_result
    OUTPUT_VARIABLE sdk_build_stdout
    ERROR_VARIABLE sdk_build_stderr
)
if(NOT sdk_build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed SDK Plugin consumer failed to build.\n"
        "${sdk_build_stdout}\n${sdk_build_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        -S
        "${RODAQ_TEST_DAQ_CONSUMER_SOURCE_DIR}"
        -B
        "${RODAQ_TEST_OBJECT_DIR}/daq-consumer"
        "-DCMAKE_PREFIX_PATH=${RODAQ_TEST_INSTALL_PREFIX}"
        "-DCMAKE_CXX_FLAGS=${RODAQ_TEST_CONSUMER_SANITIZER_FLAGS}"
    RESULT_VARIABLE runtime_configure_result
    OUTPUT_VARIABLE runtime_configure_stdout
    ERROR_VARIABLE runtime_configure_stderr
)
if(NOT runtime_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed DAQ consumer failed to configure.\n"
        "${runtime_configure_stdout}\n${runtime_configure_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        --build
        "${RODAQ_TEST_OBJECT_DIR}/daq-consumer"
    RESULT_VARIABLE runtime_build_result
    OUTPUT_VARIABLE runtime_build_stdout
    ERROR_VARIABLE runtime_build_stderr
)
if(NOT runtime_build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed DAQ consumer failed to build.\n"
        "${runtime_build_stdout}\n${runtime_build_stderr}")
endif()

execute_process(
    COMMAND "${RODAQ_TEST_OBJECT_DIR}/daq-consumer/rodaq_consumer"
    RESULT_VARIABLE runtime_run_result
    OUTPUT_VARIABLE runtime_run_stdout
    ERROR_VARIABLE runtime_run_stderr
)
if(NOT runtime_run_result EQUAL 0)
    message(FATAL_ERROR
        "Installed DAQ consumer failed to run.\n"
        "${runtime_run_stdout}\n${runtime_run_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        -S
        "${RODAQ_TEST_COMBINED_CONSUMER_SOURCE_DIR}"
        -B
        "${RODAQ_TEST_OBJECT_DIR}/combined-consumer"
        "-DCMAKE_PREFIX_PATH=${RODAQ_TEST_INSTALL_PREFIX}"
        "-DCMAKE_CXX_FLAGS=${RODAQ_TEST_CONSUMER_SANITIZER_FLAGS}"
    RESULT_VARIABLE combined_configure_result
    OUTPUT_VARIABLE combined_configure_stdout
    ERROR_VARIABLE combined_configure_stderr
)
if(NOT combined_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed combined DAQ and SDK consumer failed to configure.\n"
        "${combined_configure_stdout}\n${combined_configure_stderr}")
endif()

execute_process(
    COMMAND
        "${RODAQ_TEST_CMAKE_COMMAND}"
        --build
        "${RODAQ_TEST_OBJECT_DIR}/combined-consumer"
    RESULT_VARIABLE combined_build_result
    OUTPUT_VARIABLE combined_build_stdout
    ERROR_VARIABLE combined_build_stderr
)
if(NOT combined_build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed combined DAQ and SDK consumer failed to build.\n"
        "${combined_build_stdout}\n${combined_build_stderr}")
endif()

execute_process(
    COMMAND "${RODAQ_TEST_OBJECT_DIR}/combined-consumer/rodaq_combined_consumer"
    RESULT_VARIABLE combined_run_result
    OUTPUT_VARIABLE combined_run_stdout
    ERROR_VARIABLE combined_run_stderr
)
if(NOT combined_run_result EQUAL 0)
    message(FATAL_ERROR
        "Installed combined DAQ and SDK consumer failed to run.\n"
        "${combined_run_stdout}\n${combined_run_stderr}")
endif()
