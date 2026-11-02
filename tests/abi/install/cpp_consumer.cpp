#include <rodaq/abi/abi.h>
#include <rodaq/abi/acquisition.h>
#include <rodaq/abi/canonical.h>
#include <rodaq/abi/config.h>
#include <rodaq/abi/plugin.h>
#include <rodaq/abi/runtime.h>
#include <rodaq/abi/service.h>
#include <rodaq/abi/storage.h>

int main() { return RODAQ_PLUGIN_ABI_VERSION == UINT32_C(1) ? 0 : 1; }
