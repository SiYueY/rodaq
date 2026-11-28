# True out-of-tree official plugin build verification against installed RoDAQ prefix (CR-P1-02 / IC-A02).
# Copies each plugin directory into an isolated temp directory with NO parent repo tree
# so any hidden relative source dependency (e.g. ../../support/...) fails immediately.
if(NOT DEFINED RODAQ_BUILD_DIR OR NOT DEFINED RODAQ_SOURCE_DIR)
    message(FATAL_ERROR "RODAQ_BUILD_DIR and RODAQ_SOURCE_DIR are required")
endif()

set(work_dir "${RODAQ_BUILD_DIR}/oot-plugins-contract")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")

set(install_prefix "${work_dir}/install")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${RODAQ_BUILD_DIR}" --prefix "${install_prefix}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${rc})")
endif()

set(prefix_path "${install_prefix}")
if(DEFINED ENV{CMAKE_PREFIX_PATH} AND NOT "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "")
    set(prefix_path "${install_prefix};$ENV{CMAKE_PREFIX_PATH}")
endif()

set(sanitizer_args "")
if(RODAQ_SANITIZER STREQUAL "address-undefined")
    list(APPEND sanitizer_args
        "-DCMAKE_C_FLAGS=-fsanitize=address,undefined"
        "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined"
        "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined")
elseif(RODAQ_SANITIZER STREQUAL "thread")
    list(APPEND sanitizer_args
        "-DCMAKE_C_FLAGS=-fsanitize=thread"
        "-DCMAKE_CXX_FLAGS=-fsanitize=thread"
        "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread")
endif()

# 1. Build copied official MCAP storage plugin out-of-tree against installed RoDAQ::plugin.
if(RODAQ_BUILD_MCAP_STORAGE)
    set(mcap_src "${work_dir}/isolated-mcap-src")
    set(mcap_build "${work_dir}/mcap-build")
    file(COPY "${RODAQ_SOURCE_DIR}/plugins/storage/mcap/" DESTINATION "${mcap_src}")
    file(COPY "${RODAQ_SOURCE_DIR}/internal/strict_json.hpp" DESTINATION "${mcap_src}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${mcap_src}" -B "${mcap_build}"
            -DCMAKE_PREFIX_PATH=${prefix_path}
            -DRODAQ_MCAP_CPP_INCLUDE_DIR=${RODAQ_SOURCE_DIR}/third_party/mcap-cpp/cpp/mcap/include
            -DCMAKE_CXX_COMPILER=${RODAQ_CXX_COMPILER}
            -DCMAKE_C_COMPILER=${RODAQ_C_COMPILER}
            ${sanitizer_args}
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Isolated out-of-tree MCAP configure failed (${rc})")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${mcap_build}" --parallel 2
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Isolated out-of-tree MCAP build failed (${rc})")
    endif()
    message(STATUS "Isolated out-of-tree MCAP plugin build against installed RoDAQ prefix PASSED")
endif()

# 2. Build copied official ROS 2 acquisition plugin out-of-tree against installed RoDAQ::plugin + RoDAQ::ros2_support.
if(RODAQ_BUILD_ROS2_ACQUISITION)
    set(ros2_src "${work_dir}/isolated-ros2-src")
    set(ros2_build "${work_dir}/ros2-build")
    file(COPY "${RODAQ_SOURCE_DIR}/plugins/acquisition/ros2/" DESTINATION "${ros2_src}")
    file(COPY "${RODAQ_SOURCE_DIR}/internal/strict_json.hpp" DESTINATION "${ros2_src}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${ros2_src}" -B "${ros2_build}"
            -DCMAKE_PREFIX_PATH=${prefix_path}
            -DCMAKE_CXX_COMPILER=${RODAQ_CXX_COMPILER}
            -DCMAKE_C_COMPILER=${RODAQ_C_COMPILER}
            ${sanitizer_args}
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Isolated out-of-tree ROS 2 configure failed (${rc})")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${ros2_build}" --parallel 2
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Isolated out-of-tree ROS 2 build failed (${rc})")
    endif()
    message(STATUS "Isolated out-of-tree ROS 2 plugin build against installed RoDAQ prefix PASSED")
endif()
