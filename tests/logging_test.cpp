#include <cassert>

#include <rodaq/logging.hpp>

int main() {
    using namespace rodaq::logging;

    assert(level() == Level::Info);

    const auto invalid_level_before_initialize = set_level(static_cast<Level>(255));
    assert(!invalid_level_before_initialize.ok());
    assert(invalid_level_before_initialize.status().code() == rodaq::StatusCode::InvalidArgument);

    const auto set_level_before_initialize = set_level(Level::Debug);
    assert(!set_level_before_initialize.ok());
    assert(set_level_before_initialize.status().code() == rodaq::StatusCode::InvalidState);

    const auto flush_before_initialize = flush();
    assert(!flush_before_initialize.ok());
    assert(flush_before_initialize.status().code() == rodaq::StatusCode::InvalidState);

    Config invalid_no_sinks;
    invalid_no_sinks.console = false;
    invalid_no_sinks.rotating_file_path.clear();
    const auto invalid_sinks_result = initialize(invalid_no_sinks);
    assert(!invalid_sinks_result.ok());
    assert(invalid_sinks_result.status().code() == rodaq::StatusCode::InvalidArgument);

    Config invalid_queue_config;
    invalid_queue_config.async = true;
    invalid_queue_config.async_queue_size = 0;
    const auto invalid_queue_result = initialize(invalid_queue_config);
    assert(!invalid_queue_result.ok());
    assert(invalid_queue_result.status().code() == rodaq::StatusCode::InvalidArgument);

    Config config;
    config.async = false;
    config.console = true;

    const auto initialize_result = initialize(config);
    assert(initialize_result.ok());

    const auto duplicate_initialize_result = initialize(config);
    assert(!duplicate_initialize_result.ok());
    assert(duplicate_initialize_result.status().code() == rodaq::StatusCode::Conflict);

    const auto invalid_level_result = set_level(static_cast<Level>(255));
    assert(!invalid_level_result.ok());
    assert(invalid_level_result.status().code() == rodaq::StatusCode::InvalidArgument);

    const auto set_level_result = set_level(Level::Debug);
    assert(set_level_result.ok());
    assert(level() == Level::Debug);

    RODAQ_LOG_INFO("logging smoke test value={}", 42);
    RODAQ_LOG_WARNING("logging warning smoke test");

    log(Level::Info, SourceLocation{__FILE__, __LINE__, __func__}, "broken {", 1);

    const auto flush_result = flush();
    assert(flush_result.ok());

    const auto shutdown_result = shutdown();
    assert(shutdown_result.ok());

    assert(level() == Level::Info);

    const auto set_level_after_shutdown = set_level(Level::Debug);
    assert(!set_level_after_shutdown.ok());
    assert(set_level_after_shutdown.status().code() == rodaq::StatusCode::InvalidState);

    const auto flush_after_shutdown = flush();
    assert(!flush_after_shutdown.ok());
    assert(flush_after_shutdown.status().code() == rodaq::StatusCode::InvalidState);

    const auto repeated_shutdown_result = shutdown();
    assert(repeated_shutdown_result.ok());

    return 0;
}
