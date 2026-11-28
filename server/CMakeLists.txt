find_package(Drogon 1.9.14 REQUIRED CONFIG)

# Private server helper library (not installed, not exported).
# Links ONLY public rodaq::daq + Drogon + boundary JSON/Crypto; no Core private headers.
add_library(rodaq_server_lib STATIC
    src/api/json_codec.cpp
    src/api/error_response.cpp
    src/api/dto.cpp
    src/config.cpp
    src/security/auth.cpp
    src/service_call_executor.cpp
    src/download/download_session.cpp
    src/download/download_registry.cpp
    src/static_assets.cpp
    src/server_app.cpp
)
target_include_directories(rodaq_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${PROJECT_SOURCE_DIR}/internal)
target_link_libraries(rodaq_server_lib PUBLIC
    rodaq::daq
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    Threads::Threads
)
rodaq_enable_project_warnings(rodaq_server_lib)

add_executable(rodaq src/main.cpp)
target_link_libraries(rodaq PRIVATE rodaq_server_lib)
rodaq_enable_project_warnings(rodaq)

install(TARGETS rodaq RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES api/openapi.yaml DESTINATION ${CMAKE_INSTALL_DATADIR}/rodaq/api)

set(RODAQ_WEB_DIST_DIR "" CACHE PATH "Optional prebuilt web/dist directory to stage into share/rodaq/web")
if(RODAQ_WEB_DIST_DIR)
    install(DIRECTORY "${RODAQ_WEB_DIST_DIR}/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rodaq/web)
endif()

if(BUILD_TESTING)
    find_package(GTest 1.11 REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    include(GoogleTest)
    add_executable(rodaq_server_unit_test tests/server_unit_test.cpp)
    target_link_libraries(rodaq_server_unit_test PRIVATE rodaq_server_lib GTest::gtest_main)
    rodaq_enable_project_warnings(rodaq_server_unit_test)
    gtest_discover_tests(rodaq_server_unit_test PROPERTIES TIMEOUT 30)

    add_test(
        NAME rodaq_server_openapi_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/openapi_conformance_test.py
                ${CMAKE_CURRENT_SOURCE_DIR}/api/openapi.yaml
                ${PROJECT_SOURCE_DIR}/schemas/task_config.schema.json
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_tests_properties(rodaq_server_openapi_test PROPERTIES TIMEOUT 30)

    if(RODAQ_BUILD_BEHAVIORAL_FAKES)
        add_executable(rodaq_server_http_test tests/server_http_test.cpp)
        target_link_libraries(rodaq_server_http_test PRIVATE rodaq_server_lib GTest::gtest_main)
        target_compile_definitions(rodaq_server_http_test PRIVATE
            RODAQ_SERVER_STORAGE_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_storage/rodaq-plugins"
            RODAQ_SERVER_ACQUISITION_PACKAGE="${PROJECT_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins"
            RODAQ_SERVER_OPENAPI_PATH="${CMAKE_CURRENT_SOURCE_DIR}/api/openapi.yaml"
            RODAQ_SERVER_BINARY="$<TARGET_FILE:rodaq>")
        add_dependencies(rodaq_server_http_test rodaq rodaq_fake_storage rodaq_fake_acquisition)
        rodaq_enable_project_warnings(rodaq_server_http_test)
        gtest_discover_tests(rodaq_server_http_test PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endif()
