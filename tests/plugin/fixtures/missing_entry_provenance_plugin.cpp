#include <rodaq/plugin/c/abi.h>

#include <cstdint>

extern "C" int rodaq_test_provenance_dependency_marker(void);

namespace {

[[maybe_unused]] volatile int dependency_marker = rodaq_test_provenance_dependency_marker();

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void) {
    return RODAQ_PLUGIN_ABI_VERSION(1u, 0u);
}
