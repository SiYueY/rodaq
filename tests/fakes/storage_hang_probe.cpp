#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>
#include <rodaq/storage/interface.hpp>

#include <chrono>
#include <string_view>
#include <utility>

// A disposable process, never a background thread in the test runner. The
// parent observes a real backend-entry byte, then supervises this hung call.
int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    namespace plugin = rodaq::plugin;
    namespace storage = rodaq::storage;
    const auto deadline = plugin::Deadline::after(std::chrono::seconds(2)).value();
    plugin::PluginManagerOptions options;
    options.search_paths = {argv[1]};
    options.max_loaded_plugins = 1;
    options.max_instances = 1;
    options.max_config_size = 1024;
    options.shutdown_timeout = std::chrono::seconds(1);
    auto manager = plugin::PluginManager::create(std::move(options));
    if (!manager.ok()) {
        return 3;
    }
    auto library = manager.value().load("rodaq.fake.storage", "1.0.0");
    plugin::ServiceRegistry services;
    if (!library.ok() || !services.freeze().ok()) {
        return 4;
    }
    auto instance = library.value().create_instance(services, R"({"mode":"hang_write"})");
    if (!instance.ok()) {
        return 5;
    }
    auto handle = instance.value().query<storage::Interface>();
    const storage::PrepareConfig limits{
        {sizeof(storage::PrepareConfig)}, 64, 256, 4, 4, 100, 4096, 65536, 1, 32, 7, 100, 1000};
    if (!handle.ok() || !handle.value().prepare(limits, deadline).ok()) {
        return 6;
    }
    storage::StreamDescriptor descriptor{};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.stream_id = 1;
    descriptor.descriptor_id = 1;
    descriptor.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    descriptor.channel_name = {"/fake", 5};
    descriptor.message_encoding = {"json", 4};
    const std::string_view path(argv[2]);
    const storage::SegmentSpec spec{
        {sizeof(spec)}, 1, {path.data(), static_cast<std::uint32_t>(path.size())}};
    if (!handle.value().prepare_descriptor(descriptor, deadline).ok() ||
        !handle.value().open_segment(spec, deadline).ok()) {
        return 7;
    }
    storage::Record record{};
    record.header.struct_size = sizeof(record);
    record.stream_id = 1;
    record.descriptor_id = 1;
    record.acceptance_sequence = 1;
    record.system_boot_id = 7;
    record.clock_epoch_id = 3;
    record.receive_monotonic = 200;
    (void)handle.value().write_batch({&record, 1}, deadline);
    return 99;  // A hung Native call must never report a fabricated return.
}
