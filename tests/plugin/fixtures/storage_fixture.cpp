#include "fixture_support.hpp"

#include <rodaq/plugin/abi/plugin.h>

#include <cstdint>

#ifndef RODAQ_FIXTURE_PLUGIN_ID
#define RODAQ_FIXTURE_PLUGIN_ID "rodaq.storage.fixture"
#endif

#ifndef RODAQ_FIXTURE_PROTOCOL_VERSION
#define RODAQ_FIXTURE_PROTOCOL_VERSION RODAQ_STORAGE_PROTOCOL_VERSION
#endif

#ifndef RODAQ_FIXTURE_PROTOCOL_STRUCT_SIZE
#define RODAQ_FIXTURE_PROTOCOL_STRUCT_SIZE sizeof(ExtendedStoragePlugin)
#endif

#ifndef RODAQ_FIXTURE_CREATE_FUNCTION
#define RODAQ_FIXTURE_CREATE_FUNCTION rodaq::test::fixture_create
#endif

namespace {

constexpr char kPluginId[] = RODAQ_FIXTURE_PLUGIN_ID;
constexpr char kPluginName[] = "Storage Fixture";
constexpr char kPluginVersion[] = "2.1.0-rc.1+build.4";
constexpr char kPluginDescription[] = "";

const rodaq_plugin_descriptor descriptor = {
    RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE,
    0,
    {kPluginId, sizeof(kPluginId) - 1},
    {kPluginName, sizeof(kPluginName) - 1},
    {kPluginVersion, sizeof(kPluginVersion) - 1},
    {kPluginDescription, sizeof(kPluginDescription) - 1},
};

struct ExtendedStoragePlugin {
    rodaq_storage_plugin base;
    std::uint64_t appendable_tail;
};

const ExtendedStoragePlugin storage_plugin = {
    {
        RODAQ_FIXTURE_PROTOCOL_VERSION,
        RODAQ_FIXTURE_PROTOCOL_STRUCT_SIZE,
        &descriptor,
        RODAQ_FIXTURE_CREATE_FUNCTION,
        rodaq::test::fixture_destroy,
        rodaq::test::fixture_storage_bind,
        rodaq::test::fixture_storage_configure,
        rodaq::test::fixture_storage_open,
        rodaq::test::fixture_storage_submit,
        rodaq::test::fixture_storage_flush,
        rodaq::test::fixture_storage_close,
        rodaq::test::fixture_query_interface,
    },
    0,
};

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT const std::uint32_t rodaq_plugin_abi_version =
    RODAQ_PLUGIN_ABI_VERSION;

extern "C" RODAQ_PLUGIN_EXPORT const rodaq_storage_plugin* rodaq_get_storage_plugin(void) noexcept {
    rodaq::test::fixture_record_event("entrypoint");
    return &storage_plugin.base;
}
