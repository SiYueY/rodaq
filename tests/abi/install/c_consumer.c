#include <rodaq/plugin/abi/common.h>
#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/abi/canonical.h>
#include <rodaq/plugin/abi/config.h>
#include <rodaq/plugin/abi/plugin.h>
#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/plugin/abi/service.h>
#include <rodaq/plugin/abi/storage.h>

int main(void) { return RODAQ_PLUGIN_ABI_VERSION == UINT32_C(1) ? 0 : 1; }
