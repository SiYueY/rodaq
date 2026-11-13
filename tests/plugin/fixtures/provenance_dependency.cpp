#include <rodaq/plugin/c/abi.h>

#include <cstdint>

namespace {

const rodaq_plugin_entry_t kDependencyEntry{};

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL int rodaq_test_provenance_dependency_marker(void) {
    return 1;
}

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void) {
    return &kDependencyEntry;
}
