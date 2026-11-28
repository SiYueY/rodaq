function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Package contract command failed: ${ARGV}")
    endif()
endfunction()

set(root "${RODAQ_BUILD_DIR}/package-contract")
file(REMOVE_RECURSE "${root}")
run_checked("${CMAKE_COMMAND}" --install "${RODAQ_BUILD_DIR}" --prefix "${root}/original")
# Relocation detects source/build-tree and fixed install-prefix dependencies.
file(RENAME "${root}/original" "${root}/relocated")
function(consumer_case name mode disable_ament expect_ros expect_success)
    set(build "${root}/${name}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${RODAQ_SOURCE_DIR}/tests/package_consumer"
        -B "${build}" "-DCMAKE_PREFIX_PATH=${root}/relocated"
        "-DRODAQ_SANITIZER=${RODAQ_SANITIZER}"
        "-DCMAKE_CXX_COMPILER=${RODAQ_CXX_COMPILER}"
        "-DRODAQ_COMPONENT_MODE=${mode}"
        "-DCMAKE_DISABLE_FIND_PACKAGE_ament_index_cpp=${disable_ament}"
        "-DRODAQ_EXPECT_ROS2=${expect_ros}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(expect_success)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${name} configure failed: ${output}
${errors}")
        endif()
        # Build/link each distinct successful SDK surface; optional absent is
        # identical to base and does not need another executable build.
        if(name STREQUAL "base_without_ros" OR expect_ros)
            run_checked("${CMAKE_COMMAND}" --build "${build}" --parallel 2)
            run_checked("${build}/consumer")
            if(expect_ros)
                run_checked("${build}/ros2_consumer")
            endif()
        endif()
    elseif(rc EQUAL 0)
        message(FATAL_ERROR "${name}: required unavailable component unexpectedly succeeded")
    elseif(NOT errors MATCHES "RoDAQ_FOUND")
        message(FATAL_ERROR "${name}: unrelated configure failure: ${output}
${errors}")
    endif()
    message(STATUS "Component contract ${name}: PASS")
endfunction()

consumer_case(base_without_ros base TRUE FALSE TRUE)
consumer_case(base_with_environment base FALSE FALSE TRUE)
consumer_case(required_missing_dependency required TRUE FALSE FALSE)
consumer_case(optional_missing_dependency optional TRUE FALSE TRUE)
consumer_case(unknown_required unknown TRUE FALSE FALSE)
if(RODAQ_BUILD_ROS2_ACQUISITION)
    consumer_case(required_available required FALSE TRUE TRUE)
else()
    consumer_case(required_not_installed required FALSE FALSE FALSE)
endif()
