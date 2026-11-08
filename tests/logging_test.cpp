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

    const auto zero_backtrace_before_initialize = enable_backtrace(0);
    assert(!zero_backtrace_before_initialize.ok());
    assert(zero_backtrace_before_initialize.status().code() == rodaq::StatusCode::InvalidArgument);

    const auto oversized_backtrace_before_initialize = enable_backtrace(65537);
    assert(!oversized_backtrace_before_initialize.ok());
    assert(
        oversized_backtrace_before_initialize.status().code() ==
        rodaq::StatusCode::InvalidArgument);

    const auto enable_before_initialize = enable_backtrace(32);
    assert(!enable_before_initialize.ok());
    assert(enable_before_initialize.status().code() == rodaq::StatusCode::InvalidState);

    const auto disable_before_initialize = disable_backtrace();
    assert(!disable_before_initialize.ok());
    assert(disable_before_initialize.status().code() == rodaq::StatusCode::InvalidState);

    const auto dump_before_initialize = dump_backtrace();
    assert(!dump_before_initialize.ok());
    assert(dump_before_initialize.status().code() == rodaq::StatusCode::InvalidState);

    const auto flush_before_initialize = request_flush();
    assert(!flush_before_initialize.ok());
    assert(flush_before_initialize.status().code() == rodaq::StatusCode::InvalidState);

    Config invalid_config;
    invalid_config.sinks.clear();
    const auto invalid_config_result = initialize(invalid_config);
    assert(!invalid_config_result.ok());
    assert(invalid_config_result.status().code() == rodaq::StatusCode::InvalidArgument);

    Config invalid_queue_config;
    invalid_queue_config.async.enabled = true;
    invalid_queue_config.async.queue_size = 0;
    const auto invalid_queue_result = initialize(invalid_queue_config);
    assert(!invalid_queue_result.ok());
    assert(invalid_queue_result.status().code() == rodaq::StatusCode::InvalidArgument);

    Config config;
    config.async.enabled = false;

    const auto initialize_result = initialize(config);
    assert(initialize_result.ok());

    const auto duplicate_initialize_result = initialize(config);
    assert(!duplicate_initialize_result.ok());
    assert(duplicate_initialize_result.status().code() == rodaq::StatusCode::Conflict);

    const auto initialized_stats = stats();
    assert(initialized_stats.initialized);
    assert(!initialized_stats.asynchronous);

    const auto invalid_level_result = set_level(static_cast<Level>(255));
    assert(!invalid_level_result.ok());
    assert(invalid_level_result.status().code() == rodaq::StatusCode::InvalidArgument);

    const auto set_level_result = set_level(Level::Debug);
    assert(set_level_result.ok());
    assert(level() == Level::Debug);

    const auto enable_backtrace_result = enable_backtrace(32);
    assert(enable_backtrace_result.ok());

    const auto dump_backtrace_result = dump_backtrace();
    assert(dump_backtrace_result.ok());

    const auto disable_backtrace_result = disable_backtrace();
    assert(disable_backtrace_result.ok());

    const auto repeated_disable_result = disable_backtrace();
    assert(repeated_disable_result.ok());

    RODAQ_LOG_INFO("logging smoke test value={}", 42);

    log(Level::Info, SourceLocation{__FILE__, __LINE__, __func__}, "broken {", 1);

    const auto flush_result = request_flush();
    assert(flush_result.ok());

    const auto shutdown_result = shutdown();
    assert(shutdown_result.ok());

    const auto stopped_stats = stats();
    assert(!stopped_stats.initialized);
    assert(level() == Level::Info);

    const auto set_level_after_shutdown = set_level(Level::Debug);
    assert(!set_level_after_shutdown.ok());
    assert(set_level_after_shutdown.status().code() == rodaq::StatusCode::InvalidState);

    const auto enable_after_shutdown = enable_backtrace(32);
    assert(!enable_after_shutdown.ok());
    assert(enable_after_shutdown.status().code() == rodaq::StatusCode::InvalidState);

    const auto disable_after_shutdown = disable_backtrace();
    assert(!disable_after_shutdown.ok());
    assert(disable_after_shutdown.status().code() == rodaq::StatusCode::InvalidState);

    const auto dump_after_shutdown = dump_backtrace();
    assert(!dump_after_shutdown.ok());
    assert(dump_after_shutdown.status().code() == rodaq::StatusCode::InvalidState);

    const auto flush_after_shutdown = request_flush();
    assert(!flush_after_shutdown.ok());
    assert(flush_after_shutdown.status().code() == rodaq::StatusCode::InvalidState);

    const auto repeated_shutdown_result = shutdown();
    assert(repeated_shutdown_result.ok());

    return 0;
}
