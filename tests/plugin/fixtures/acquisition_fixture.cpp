#include "fixture_support.hpp"

#include <rodaq/plugin/abi/plugin.h>

#include <cstdint>

#ifndef RODAQ_FIXTURE_PLUGIN_ID
#define RODAQ_FIXTURE_PLUGIN_ID "rodaq.acquisition.fixture"
#endif

#ifndef RODAQ_FIXTURE_PLUGIN_NAME
#define RODAQ_FIXTURE_PLUGIN_NAME "Acquisition Fixture"
#endif

#ifndef RODAQ_FIXTURE_PLUGIN_VERSION
#define RODAQ_FIXTURE_PLUGIN_VERSION "1.0.0"
#endif

#ifndef RODAQ_FIXTURE_PROTOCOL_VERSION
#define RODAQ_FIXTURE_PROTOCOL_VERSION RODAQ_ACQUISITION_PROTOCOL_VERSION
#endif

#ifndef RODAQ_FIXTURE_PROTOCOL_STRUCT_SIZE
#define RODAQ_FIXTURE_PROTOCOL_STRUCT_SIZE RODAQ_ACQUISITION_PLUGIN_MIN_STRUCT_SIZE
#endif

#ifndef RODAQ_FIXTURE_DESCRIPTOR_STRUCT_SIZE
#define RODAQ_FIXTURE_DESCRIPTOR_STRUCT_SIZE RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE
#endif

#ifndef RODAQ_FIXTURE_DEFINE_MARKER
#define RODAQ_FIXTURE_DEFINE_MARKER 1
#endif

#ifndef RODAQ_FIXTURE_BOOT_ABI_VERSION
#define RODAQ_FIXTURE_BOOT_ABI_VERSION RODAQ_PLUGIN_ABI_VERSION
#endif

#ifndef RODAQ_FIXTURE_DEFINE_FAMILY
#define RODAQ_FIXTURE_DEFINE_FAMILY 1
#endif

#ifndef RODAQ_FIXTURE_DUAL_FAMILY
#define RODAQ_FIXTURE_DUAL_FAMILY 0
#endif

#ifndef RODAQ_FIXTURE_DESCRIPTOR_NULL
#define RODAQ_FIXTURE_DESCRIPTOR_NULL 0
#endif

#ifndef RODAQ_FIXTURE_CREATE_FUNCTION
#define RODAQ_FIXTURE_CREATE_FUNCTION rodaq::test::fixture_create
#endif

namespace {

constexpr char kPluginId[] = RODAQ_FIXTURE_PLUGIN_ID;
[[maybe_unused]] constexpr char kPluginName[] = RODAQ_FIXTURE_PLUGIN_NAME;
constexpr char kPluginVersion[] = RODAQ_FIXTURE_PLUGIN_VERSION;
constexpr char kPluginDescription[] = "";

#if defined(RODAQ_FIXTURE_INVALID_UTF8_NAME)
constexpr char kInvalidName[] = {'b', 'a', 'd', static_cast<char>(0xFF)};

[[maybe_unused]] const rodaq_plugin_descriptor descriptor = {
    RODAQ_FIXTURE_DESCRIPTOR_STRUCT_SIZE,
    0,
    {kPluginId, sizeof(kPluginId) - 1},
    {kInvalidName, sizeof(kInvalidName)},
    {kPluginVersion, sizeof(kPluginVersion) - 1},
    {kPluginDescription, sizeof(kPluginDescription) - 1},
};
#else
[[maybe_unused]] const rodaq_plugin_descriptor descriptor = {
    RODAQ_FIXTURE_DESCRIPTOR_STRUCT_SIZE,
    0,
    {kPluginId, sizeof(kPluginId) - 1},
    {kPluginName, sizeof(kPluginName) - 1},
    {kPluginVersion, sizeof(kPluginVersion) - 1},
    {kPluginDescription, sizeof(kPluginDescription) - 1},
};
#endif

const rodaq_plugin_descriptor* descriptor_pointer() noexcept {
#if RODAQ_FIXTURE_DESCRIPTOR_NULL
    return nullptr;
#else
    return &descriptor;
#endif
}

struct ExtendedAcquisitionPlugin {
    rodaq_acquisition_plugin base;
    std::uint64_t appendable_tail;
};

const ExtendedAcquisitionPlugin acquisition_plugin = {
    {
        RODAQ_FIXTURE_PROTOCOL_VERSION,
        RODAQ_FIXTURE_PROTOCOL_STRUCT_SIZE,
        descriptor_pointer(),
        RODAQ_FIXTURE_CREATE_FUNCTION,
        rodaq::test::fixture_destroy,
        rodaq::test::fixture_acquisition_bind,
        rodaq::test::fixture_acquisition_configure,
        rodaq::test::fixture_acquisition_start,
        rodaq::test::fixture_acquisition_stop,
        rodaq::test::fixture_acquisition_query_interface,
    },
    0,
};

#if RODAQ_FIXTURE_DUAL_FAMILY
const rodaq_plugin_descriptor storage_descriptor = {
    RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE,
    0,
    RODAQ_FIXTURE_VIEW("rodaq.storage.secondary_fixture"),
    RODAQ_FIXTURE_VIEW("Secondary Storage Fixture"),
    RODAQ_FIXTURE_VIEW("1.0.0"),
    RODAQ_FIXTURE_VIEW(""),
};

struct ExtendedStoragePlugin {
    rodaq_storage_plugin base;
    std::uint64_t appendable_tail;
};

const ExtendedStoragePlugin storage_plugin = {
    {
        RODAQ_STORAGE_PROTOCOL_VERSION,
        sizeof(rodaq_storage_plugin),
        &storage_descriptor,
        rodaq::test::fixture_create,
        rodaq::test::fixture_destroy,
        rodaq::test::fixture_storage_bind,
        rodaq::test::fixture_storage_configure,
        rodaq::test::fixture_storage_open,
        rodaq::test::fixture_storage_submit,
        rodaq::test::fixture_storage_flush,
        rodaq::test::fixture_storage_close,
        rodaq::test::fixture_storage_query_interface,
    },
    0,
};
#endif

}  // namespace

#if RODAQ_FIXTURE_DEFINE_MARKER
extern "C" RODAQ_PLUGIN_EXPORT const std::uint32_t rodaq_plugin_abi_version =
    RODAQ_FIXTURE_BOOT_ABI_VERSION;
#endif

#if RODAQ_FIXTURE_DEFINE_FAMILY
extern "C" RODAQ_PLUGIN_EXPORT const rodaq_acquisition_plugin* rodaq_get_acquisition_plugin(
    void) noexcept {
    rodaq::test::fixture_record_event("entrypoint");
    return &acquisition_plugin.base;
}
#endif

#if RODAQ_FIXTURE_DUAL_FAMILY
extern "C" RODAQ_PLUGIN_EXPORT const rodaq_storage_plugin* rodaq_get_storage_plugin(void) noexcept {
    rodaq::test::fixture_record_event("storage-entrypoint");
    return &storage_plugin.base;
}
#endif
