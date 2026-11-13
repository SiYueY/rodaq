foreach(required IN ITEMS
        RODAQ_MANIFEST_OUTPUT
        RODAQ_ARTIFACT_PATH
        RODAQ_PLUGIN_ID
        RODAQ_PLUGIN_NAME
        RODAQ_PLUGIN_VERSION
        RODAQ_PLUGIN_ARCH)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing required manifest variable: ${required}")
    endif()
endforeach()

if(NOT EXISTS "${RODAQ_ARTIFACT_PATH}")
    message(FATAL_ERROR "Plugin artifact does not exist: ${RODAQ_ARTIFACT_PATH}")
endif()
if(NOT RODAQ_PLUGIN_ID MATCHES "^[a-z0-9._-]+$")
    message(FATAL_ERROR "Plugin id is invalid: ${RODAQ_PLUGIN_ID}")
endif()
if(NOT RODAQ_PLUGIN_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "Plugin version is not SemVer: ${RODAQ_PLUGIN_VERSION}")
endif()
if(NOT RODAQ_PLUGIN_ARCH MATCHES "^(amd64|arm64)$")
    message(FATAL_ERROR "Plugin architecture is invalid: ${RODAQ_PLUGIN_ARCH}")
endif()

file(SHA256 "${RODAQ_ARTIFACT_PATH}" artifact_sha256)

function(rodaq_json_escape input output)
    string(REPLACE "\\" "\\\\" escaped "${input}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\t" "\\t" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    foreach(code RANGE 1 31)
        string(ASCII ${code} character)
        string(TOUPPER "${code}" code_text)
        if(code LESS 16)
            set(hex_digit "0${code_text}")
        else()
            math(EXPR hex_value "${code}" OUTPUT_FORMAT HEXADECIMAL)
            string(SUBSTRING "${hex_value}" 2 -1 hex_digit)
            string(TOUPPER "${hex_digit}" hex_digit)
        endif()
        string(REPLACE "${character}" "\\u00${hex_digit}" escaped "${escaped}")
    endforeach()
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

rodaq_json_escape("${RODAQ_PLUGIN_ID}" plugin_id)
rodaq_json_escape("${RODAQ_PLUGIN_NAME}" plugin_name)
rodaq_json_escape("${RODAQ_PLUGIN_VERSION}" plugin_version)

get_filename_component(artifact_name "${RODAQ_ARTIFACT_PATH}" NAME)
rodaq_json_escape("lib/${artifact_name}" artifact_relative_path)

set(manifest_content
"{
  \"format_version\": 1,
  \"plugin\": {
    \"id\": \"${plugin_id}\",
    \"name\": \"${plugin_name}\",
    \"version\": \"${plugin_version}\",
    \"abi\": {
      \"major\": 1,
      \"minor\": 0
    }
  },
  \"platform\": {
    \"os\": \"linux\",
    \"arch\": \"${RODAQ_PLUGIN_ARCH}\"
  },
  \"artifact\": {
    \"path\": \"${artifact_relative_path}\",
    \"sha256\": \"${artifact_sha256}\"
  }
}
")

get_filename_component(manifest_directory "${RODAQ_MANIFEST_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${manifest_directory}")
file(WRITE "${RODAQ_MANIFEST_OUTPUT}" "${manifest_content}")
