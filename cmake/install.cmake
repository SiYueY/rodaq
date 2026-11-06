include_guard(GLOBAL)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(
    DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    FILES "${RODAQ_THIRD_PARTY_ROOT}/expected-lite/include/nonstd/expected.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nonstd"
)

install(
    FILES "${RODAQ_THIRD_PARTY_ROOT}/expected-lite/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rodaq/licenses/expected-lite"
)

install(
    FILES
        "${RODAQ_THIRD_PARTY_ROOT}/hdr_histogram/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rodaq/licenses/HdrHistogram_c"
)
install(
    FILES
        "${RODAQ_THIRD_PARTY_ROOT}/spdlog/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rodaq/licenses/spdlog"
)
install(
    FILES
        "${RODAQ_THIRD_PARTY_ROOT}/fmt/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rodaq/licenses/fmt"
)
install(
    FILES
        "${RODAQ_THIRD_PARTY_ROOT}/yaml-cpp/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rodaq/licenses/yaml-cpp"
)
install(
    FILES "${CMAKE_BINARY_DIR}/rodaq-dependency-inventory.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rodaq/sbom"
)

install(TARGETS
    rodaq_headers
    rodaq_public_api
    rodaq_expected_lite
    rodaq_api_support
    rodaq
    rodaq_sdk
    yaml-cpp
    EXPORT RoDAQTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(EXPORT RoDAQTargets
    NAMESPACE rodaq::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rodaq"
)

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/RoDAQConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/RoDAQConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rodaq"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/RoDAQConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/RoDAQConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/RoDAQConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rodaq"
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/cmake/rodaq_plugin.cmake"
        "${PROJECT_SOURCE_DIR}/cmake/rodaq_plugin_metadata.hpp.in"
        "${PROJECT_SOURCE_DIR}/cmake/check_plugin_symbols.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rodaq"
)
