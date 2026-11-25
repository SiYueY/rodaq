find_package(Drogon 1.9.14 REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# Server-private library: links ONLY public rodaq::daq + Drogon (no private src/ headers).
add_library(rodaq_server_lib STATIC
    src/server_config.cpp
    src/security/auth.cpp
    src/service_call_executor.cpp
    src/download/download_session.cpp
    src/api/dto.cpp
    src/server_app.cpp)

target_include_directories(rodaq_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rodaq_server_lib
    PUBLIC
        rodaq::daq
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto)
rodaq_enable_project_warnings(rodaq_server_lib)

add_executable(rodaq src/main.cpp)
target_include_directories(rodaq PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rodaq PRIVATE rodaq_server_lib rodaq::daq Drogon::Drogon)
rodaq_enable_project_warnings(rodaq)

install(TARGETS rodaq RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/api/openapi.yaml DESTINATION ${CMAKE_INSTALL_DATADIR}/rodaq/api)
if(EXISTS "${CMAKE_SOURCE_DIR}/web/dist/index.html")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/web/dist/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rodaq/web)
endif()

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_executable(rodaq_server_test
        tests/server_contract_test.cpp)
    target_link_libraries(rodaq_server_test PRIVATE rodaq_server_lib GTest::gtest_main)
    target_compile_definitions(rodaq_server_test PRIVATE
        RODAQ_OPENAPI_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/api/openapi.yaml"
        RODAQ_S5_ACQUISITION_PACKAGE="${CMAKE_BINARY_DIR}/rodaq-plugins/rodaq.fake.acquisition/1.0.0"
        RODAQ_S5_STORAGE_PACKAGE="${CMAKE_BINARY_DIR}/rodaq-plugins/rodaq.fake.storage/1.0.0"
        RODAQ_TASK_CONFIG_FIXTURE="${CMAKE_SOURCE_DIR}/tests/task/fixtures/continuous.json")
    rodaq_enable_project_warnings(rodaq_server_test)
    gtest_discover_tests(rodaq_server_test)

    if(RODAQ_BUILD_BEHAVIORAL_FAKES)
        add_executable(rodaq_server_e2e_test
            tests/server_e2e_test.cpp)
        target_include_directories(rodaq_server_e2e_test PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/tests/task)
        target_link_libraries(rodaq_server_e2e_test PRIVATE rodaq_server_lib GTest::gtest_main)
        add_dependencies(rodaq_server_e2e_test rodaq_fake_acquisition rodaq_fake_storage)
        target_compile_definitions(rodaq_server_e2e_test PRIVATE
            RODAQ_S5_ACQUISITION_PACKAGE="${CMAKE_BINARY_DIR}/plugins/fake_acquisition/rodaq-plugins/rodaq.fake.acquisition/1.0.0"
            RODAQ_S5_STORAGE_PACKAGE="${CMAKE_BINARY_DIR}/plugins/fake_storage/rodaq-plugins/rodaq.fake.storage/1.0.0"
            RODAQ_TASK_CONFIG_FIXTURE="${CMAKE_SOURCE_DIR}/tests/task/fixtures/continuous.json")
        rodaq_enable_project_warnings(rodaq_server_e2e_test)
        gtest_discover_tests(rodaq_server_e2e_test)
    endif()
endif()
