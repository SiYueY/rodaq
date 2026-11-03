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
check_manifest(valid_core "PASS"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(valid_prerelease "PASS"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0-alpha.1\",\"family\":\"acquisition\"}")
check_manifest(valid_build_metadata "PASS"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0+build.5\",\"family\":\"acquisition\"}")
check_manifest(valid_build_metadata_hyphen "PASS"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0+build-01\",\"family\":\"acquisition\"}")
check_manifest(valid_prerelease_build_hyphen "PASS"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0-alpha+build-01\",\"family\":\"acquisition\"}")
check_manifest(missing_id "FAIL"
    "{\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(invalid_id_case "FAIL"
    "{\"id\":\"Rodaq.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(invalid_id_segment "FAIL"
    "{\"id\":\"bad..plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(invalid_id_path "FAIL"
    "{\"id\":\"bad.plugin/path\",\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(missing_version "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"family\":\"acquisition\"}")
check_manifest(invalid_version_zero "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"01.0.0\",\"family\":\"acquisition\"}")
check_manifest(invalid_version_short "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0\",\"family\":\"acquisition\"}")
check_manifest(invalid_empty_prerelease "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0-\",\"family\":\"acquisition\"}")
check_manifest(invalid_prerelease "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0-alpha.01\",\"family\":\"acquisition\"}")
check_manifest(invalid_prerelease_empty_identifier "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0-alpha..1\",\"family\":\"acquisition\"}")
check_manifest(invalid_empty_build "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0+\",\"family\":\"acquisition\"}")
check_manifest(invalid_trailing_build_plus "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0-alpha+\",\"family\":\"acquisition\"}")
check_manifest(missing_name "FAIL"
    "{\"id\":\"vendor.plugin\",\"version\":\"1.0.0\",\"family\":\"acquisition\"}")
check_manifest(missing_family "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0\"}")
check_manifest(wrong_family "FAIL"
    "{\"id\":\"vendor.plugin\",\"name\":\"Plugin\",\"version\":\"1.0.0\",\"family\":\"storage\"}")
