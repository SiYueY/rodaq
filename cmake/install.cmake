include_guard(GLOBAL)

include(GNUInstallDirs)

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

# M0-A intentionally exports no linkable target or CMake package. The package
# configuration and production library target are established once their
# ownership and SONAME are frozen.
