#include "fixture_support.hpp"

#include <rodaq/plugin/abi/plugin.h>

#include <cstdint>

#ifndef RODAQ_PROVIDER_DEFINE_MARKER
#define RODAQ_PROVIDER_DEFINE_MARKER 1
#endif

#ifndef RODAQ_PROVIDER_DEFINE_FAMILY
#define RODAQ_PROVIDER_DEFINE_FAMILY 1
#endif

namespace {

const rodaq_plugin_descriptor descriptor = {
    RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE,
    0,
    RODAQ_FIXTURE_VIEW("rodaq.acquisition.provider"),
    RODAQ_FIXTURE_VIEW("Symbol Provider"),
    RODAQ_FIXTURE_VIEW("1.0.0"),
    RODAQ_FIXTURE_VIEW(""),
};

const rodaq_acquisition_plugin plugin = {
    RODAQ_ACQUISITION_PROTOCOL_VERSION,
    RODAQ_ACQUISITION_PLUGIN_MIN_STRUCT_SIZE,
    &descriptor,
    rodaq::test::fixture_create,
    rodaq::test::fixture_destroy,
    rodaq::test::fixture_acquisition_bind,
    rodaq::test::fixture_acquisition_configure,
    rodaq::test::fixture_acquisition_start,
    rodaq::test::fixture_acquisition_stop,
    rodaq::test::fixture_query_interface,
};

}  // namespace

#if RODAQ_PROVIDER_DEFINE_MARKER
extern "C" RODAQ_PLUGIN_EXPORT const std::uint32_t rodaq_plugin_abi_version =
    RODAQ_PLUGIN_ABI_VERSION;
#endif

#if RODAQ_PROVIDER_DEFINE_FAMILY
extern "C" RODAQ_PLUGIN_EXPORT const rodaq_acquisition_plugin* rodaq_get_acquisition_plugin(
    void) noexcept {
    return &plugin;
}
#endif
