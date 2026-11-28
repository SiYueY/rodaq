# Out-of-tree official plugin build verification against installed RoDAQ prefix (IC-A02 / IC-A04).
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

# 1. Build official MCAP storage plugin out-of-tree against installed RoDAQ::plugin.
if(RODAQ_BUILD_MCAP_STORAGE)
    set(mcap_build "${work_dir}/mcap-build")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${RODAQ_SOURCE_DIR}/plugins/storage/mcap" -B "${mcap_build}"
            -DCMAKE_PREFIX_PATH=${prefix_path}
            -DCMAKE_CXX_COMPILER=${RODAQ_CXX_COMPILER}
            -DCMAKE_C_COMPILER=${RODAQ_C_COMPILER}
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Out-of-tree MCAP configure failed (${rc})")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${mcap_build}" --parallel 2
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Out-of-tree MCAP build failed (${rc})")
    endif()
    message(STATUS "Out-of-tree MCAP plugin build against installed RoDAQ prefix PASSED")
endif()

# 2. Build official ROS 2 acquisition plugin out-of-tree against installed RoDAQ::plugin.
if(RODAQ_BUILD_ROS2_ACQUISITION)
    set(ros2_build "${work_dir}/ros2-build")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${RODAQ_SOURCE_DIR}/plugins/acquisition/ros2" -B "${ros2_build}"
            -DCMAKE_PREFIX_PATH=${prefix_path}
            -DCMAKE_CXX_COMPILER=${RODAQ_CXX_COMPILER}
            -DCMAKE_C_COMPILER=${RODAQ_C_COMPILER}
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Out-of-tree ROS 2 configure failed (${rc})")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${ros2_build}" --parallel 2
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Out-of-tree ROS 2 build failed (${rc})")
    endif()
    message(STATUS "Out-of-tree ROS 2 plugin build against installed RoDAQ prefix PASSED")
endif()
