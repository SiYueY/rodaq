#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <rodaq/logging.hpp>

int main() {
    using namespace rodaq::logging;

    const auto path = std::filesystem::temp_directory_path() / "rodaq_logging_async_test.log";
    std::error_code error;
    std::filesystem::remove(path, error);

    Config config;
    config.name = "rodaq-async-test";
    config.sinks = {FileSinkConfig{SinkOptions{}, path.string(), true}};
    config.async.enabled = true;
    config.async.queue_size = 128;
    config.async.overflow_policy = OverflowPolicy::DiscardNew;

    const auto initialize_result = initialize(config);
    assert(initialize_result.ok());

    const auto initialized_stats = stats();
    assert(initialized_stats.initialized);
    assert(initialized_stats.asynchronous);
    assert(initialized_stats.queue_capacity == 128);

    RODAQ_LOG_INFO("async-file-marker");

    for (int index = 0; index < 5000; ++index) {
        RODAQ_LOG_INFO("async-message index={}", index);
    }

    const auto shutdown_result = shutdown();
    assert(shutdown_result.ok());

    std::ifstream stream(path);
    assert(stream.is_open());

    const std::string content{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{},
    };
    assert(content.find("async-file-marker") != std::string::npos);

    stream.close();
    std::filesystem::remove(path, error);

    return 0;
}
