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
    config.console = false;
    config.rotating_file_path = path.string();
    config.rotating_max_bytes = 2U * 1024U * 1024U;
    config.rotating_max_files = 2;
    config.async = true;
    config.async_queue_size = 128;

    const auto initialize_result = initialize(config);
    assert(initialize_result.ok());

    for (int index = 0; index < 500; ++index) {
        RODAQ_LOG_INFO("async-message index={}", index);
    }

    const auto mid_flush = flush();
    assert(mid_flush.ok());

    RODAQ_LOG_INFO("async-file-marker");

    const auto flush_result = flush();
    assert(flush_result.ok());

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
