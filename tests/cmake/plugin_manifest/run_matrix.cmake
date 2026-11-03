set(work "${CMAKE_CURRENT_BINARY_DIR}/plugin-manifests")
file(MAKE_DIRECTORY "${work}")

function(check_manifest name expected body)
    set(path "${work}/${name}.json")
    file(WRITE "${path}" "${body}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DRODAQ_HELPER=${RODAQ_HELPER}"
            "-DRODAQ_MANIFEST=${path}"
            "-DRODAQ_FAMILY=acquisition"
            -P "${CMAKE_CURRENT_LIST_DIR}/validate_one.cmake"
        RESULT_VARIABLE result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(expected STREQUAL "PASS" AND NOT result EQUAL 0)
        message(FATAL_ERROR "Expected manifest PASS: ${name}")
    elseif(expected STREQUAL "FAIL" AND result EQUAL 0)
        message(FATAL_ERROR "Expected manifest FAIL: ${name}")
    endif()
endfunction()

check_manifest(valid "PASS"
    "{\"id\":\"vendor.plugin-2_test\",\"name\":\"Plugin\",\"version\":\"1.0.0-alpha.1+build.5\",\"family\":\"acquisition\"}")
check_manifest(invalid_id_case "FAIL"
    "{\"id\":\"Rodaq.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(invalid_id_segment "FAIL"
    "{\"id\":\"bad..plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(invalid_version_zero "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"01.0.0\",\"family\":\"acquisition\"}")
check_manifest(invalid_prerelease "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0-alpha.01\",\"family\":\"acquisition\"}")
check_manifest(missing_name "FAIL"
    "{\"id\":\"vendor.plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(wrong_family "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"storage\"}")
