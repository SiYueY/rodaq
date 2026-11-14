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
run_checked("${CMAKE_COMMAND}" -S "${RODAQ_SOURCE_DIR}/tests/package_consumer"
    -B "${root}/consumer" "-DCMAKE_PREFIX_PATH=${root}/relocated"
    "-DRODAQ_SANITIZER=${RODAQ_SANITIZER}"
    "-DCMAKE_CXX_COMPILER=${RODAQ_CXX_COMPILER}")
run_checked("${CMAKE_COMMAND}" --build "${root}/consumer")
run_checked("${root}/consumer/consumer")
