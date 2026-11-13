#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>
#include <rodaq/plugins/mcap_storage/interface.hpp>

#include <mcap/reader.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

int main() {
    using namespace std::chrono_literals;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::ServiceRegistry;
    namespace mcap_api = rodaq::plugins::mcap_storage;

    const auto output = std::filesystem::temp_directory_path() / "rodaq-mcap-reference-test.mcap";
    std::error_code error;
    std::filesystem::remove(output, error);

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 1s;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());

    auto library_result = manager.load("com.rodaq.reference.mcap", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.freeze().ok());
    auto invalid_config = library.create_instance(services, R"({"compression":"invalid"})");
    RODAQ_TEST_CHECK(!invalid_config.ok());
    RODAQ_TEST_CHECK(invalid_config.status().code() == rodaq::StatusCode::InvalidArgument);
    const std::string config =
        R"({"chunk_size":1024,"compression":"zstd","compression_level":"fast","crc":false,"message_index":true,"summary":true})";
    auto instance_result = library.create_instance(services, config);
    RODAQ_TEST_CHECK(instance_result.ok());
    auto instance = std::move(instance_result).value();

    auto operation_result = instance.query<mcap_api::StorageInterface>();
    RODAQ_TEST_CHECK(operation_result.ok());
    auto operation = std::move(operation_result).value();
    RODAQ_TEST_CHECK(mcap_api::open(operation, output.string()).ok());
    auto channel = mcap_api::register_channel(
        operation, "/rodaq_reference", "std_msgs/msg/Int32", "ros2msg", "int32 data\n", "cdr");
    RODAQ_TEST_CHECK(channel.ok());
    auto unknown_channel =
        mcap_api::write(operation, channel.value() + 1u, 123456789, rodaq_bytes_t{nullptr, 0u});
    RODAQ_TEST_CHECK(!unknown_channel.ok());
    RODAQ_TEST_CHECK(unknown_channel.status().code() == rodaq::StatusCode::NotFound);

    std::array<std::uint8_t, 64u * 1024u> cdr{};
    cdr[4] = 0x2au;
    RODAQ_TEST_CHECK(mcap_api::write(
                         operation, channel.value(), 123456789,
                         rodaq_bytes_t{cdr.data(), static_cast<std::uint32_t>(cdr.size())})
                         .ok());
    RODAQ_TEST_CHECK(mcap_api::close(operation).ok());
    RODAQ_TEST_CHECK(instance.close(Deadline::after(2s)).ok());
    RODAQ_TEST_CHECK(manager.shutdown(Deadline::after(2s)).ok());

    mcap::McapReader reader;
    RODAQ_TEST_CHECK(reader.open(output.string()).code == mcap::StatusCode::Success);
    RODAQ_TEST_CHECK(
        reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).code ==
        mcap::StatusCode::Success);
    std::size_t message_count = 0u;
    for (const auto& message : reader.readMessages()) {
        (void)message;
        ++message_count;
    }
    RODAQ_TEST_CHECK(message_count == 1u);
    RODAQ_TEST_CHECK(reader.header().has_value());
    RODAQ_TEST_CHECK(reader.header()->profile == "ros2");
    RODAQ_TEST_CHECK(!reader.chunkIndexes().empty());
    RODAQ_TEST_CHECK(reader.chunkIndexes().front().compression == "zstd");
    reader.close();
    return 0;
}
