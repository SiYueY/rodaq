#include <rodaq/c/plugin.h>

extern "C" int rodaq_loader_dependency_anchor();

namespace {
// Keep DT_NEEDED without relying on --no-as-needed or calling any lifecycle hook.
const int kDependency = rodaq_loader_dependency_anchor();
}  // namespace

#if OWN_VERSION
extern "C" uint32_t rodaq_plugin_abi_version() {
    return kDependency == 1 ? RODAQ_PLUGIN_ABI_VERSION : 0;
}
#endif
