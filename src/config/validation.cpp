#include "config/validation.hpp"

#include "config/abi_bridge.hpp"

#include <filesystem>
#include <limits>
#include <string>
#include <type_traits>

namespace rodaq {
namespace {

Status validation_error(std::string message) {
    return Status(StatusCode::InvalidArgument, "config", 0, std::move(message));
}

Result<void> validate_resource_limits(const ResourceConfig& resources) {
    if (resources.max_sources == 0) {
        return validation_error("max_sources must be greater than zero");
    }
    if (resources.max_clocks == 0) {
        return validation_error("max_clocks must be greater than zero");
    }
    if (resources.max_schemas == 0) {
        return validation_error("max_schemas must be greater than zero");
    }
    if (resources.max_timestamp_key_sets == 0) {
        return validation_error("max_timestamp_key_sets must be greater than zero");
    }
    if (resources.max_streams == 0) {
        return validation_error("max_streams must be greater than zero");
    }
    if (resources.max_stream_descriptors == 0) {
        return validation_error("max_stream_descriptors must be greater than zero");
    }
    if (resources.max_bindings == 0) {
        return validation_error("max_bindings must be greater than zero");
    }
    if (resources.structural_arena_bytes == 0) {
        return validation_error("structural_arena_bytes must be greater than zero");
    }
    if (resources.max_single_payload_bytes == 0) {
        return validation_error("max_single_payload_bytes must be greater than zero");
    }
    if (resources.dispatch_capacity == 0) {
        return validation_error("dispatch_capacity must be greater than zero");
    }
    if (resources.operation_capacity == 0) {
        return validation_error("operation_capacity must be greater than zero");
    }
    if (resources.completion_capacity == 0) {
        return validation_error("completion_capacity must be greater than zero");
    }
    if (resources.payload_release_workers == 0) {
        return validation_error("payload_release_workers must be greater than zero");
    }
    if (resources.dispatch_capacity != resources.operation_capacity) {
        return validation_error("dispatch and operation capacity must be equal");
    }
    return {};
}

Result<void> validate_payload_configuration(const ResourceConfig& resources) {
    const PayloadPoolConfig& pool = resources.payload_pool;
    if (pool.classes.empty()) {
        return validation_error("payload_pool.classes must not be empty");
    }

    std::size_t previous_size = 0;
    for (const PayloadClassConfig& payload_class : pool.classes) {
        if (payload_class.max_payload_bytes == 0) {
            return validation_error("payload class max_payload_bytes must be greater than zero");
        }
        if (payload_class.block_count == 0) {
            return validation_error("payload class block_count must be greater than zero");
        }
        if (payload_class.max_payload_bytes <= previous_size) {
            return validation_error("payload classes must be ordered by increasing size");
        }
        if (payload_class.max_payload_bytes >
            std::numeric_limits<std::size_t>::max() / payload_class.block_count) {
            return validation_error("payload class physical byte capacity overflows");
        }
        previous_size = payload_class.max_payload_bytes;
    }
    if (pool.classes.back().max_payload_bytes < resources.max_single_payload_bytes) {
        return validation_error("payload pool capacity is below max_single_payload_bytes");
    }
    return {};
}

Result<void> validate_timeouts(const DAQConfig& config) {
    if (config.submit_wait_timeout.count() <= 0) {
        return validation_error("submit_wait_timeout must be greater than zero");
    }
    if (config.stop_progress_timeout.count() <= 0) {
        return validation_error("stop_progress_timeout must be greater than zero");
    }
    return {};
}

Result<void> validate_logging_config(const LoggingConfig& logging) {
    if (logging.name.empty()) {
        return validation_error("logging.name must not be empty");
    }
    if (logging.pattern.empty()) {
        return validation_error("logging.pattern must not be empty");
    }
    if (logging.sinks.empty()) {
        return validation_error("logging.sinks must not be empty");
    }
    if (logging.level < LogLevel::Trace || logging.level > LogLevel::Off ||
        logging.flush.level < LogLevel::Trace || logging.flush.level > LogLevel::Off) {
        return validation_error("logging levels must be valid");
    }
    if (logging.time_zone != TimeZone::Local && logging.time_zone != TimeZone::Utc) {
        return validation_error("logging time zone is invalid");
    }
    if (logging.async.overflow_policy != OverflowPolicy::Block &&
        logging.async.overflow_policy != OverflowPolicy::OverrunOldest &&
        logging.async.overflow_policy != OverflowPolicy::DiscardNew) {
        return validation_error("logging overflow policy is invalid");
    }
    if (logging.async.queue_records == 0 || logging.async.queue_records > 262144) {
        return validation_error("logging.async.queue_records must be in [1, 262144]");
    }
    if (logging.async.queue_bytes == 0) {
        return validation_error("logging.async.queue_bytes must be greater than zero");
    }
    if (logging.flush.interval.count() < 0) {
        return validation_error("logging.flush.interval must not be negative");
    }
    if (logging.backtrace_capacity > 65536) {
        return validation_error("logging.backtrace_capacity must be <= 65536");
    }
    if (logging.flush_timeout.count() <= 0) {
        return validation_error("logging.flush_timeout must be greater than zero");
    }
    if (logging.shutdown_timeout.count() <= 0) {
        return validation_error("logging.shutdown_timeout must be greater than zero");
    }
    for (const SinkConfig& sink : logging.sinks) {
        const auto valid = std::visit(
            [](const auto& value) -> const char* {
                using Sink = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Sink, ConsoleSinkConfig>) {
                    if (value.stream != ConsoleStream::Stdout &&
                        value.stream != ConsoleStream::Stderr) {
                        return "logging console stream is invalid";
                    }
                } else if constexpr (std::is_same_v<Sink, FileSinkConfig>) {
                    if (value.path.empty()) {
                        return "logging file path must not be empty";
                    }
                } else if constexpr (std::is_same_v<Sink, RotatingFileSinkConfig>) {
                    if (value.path.empty() || value.max_size == 0 || value.max_files == 0) {
                        return "rotating logging configuration is invalid";
                    }
                } else {
                    if (value.path.empty() || value.rotation_hour > 23 ||
                        value.rotation_minute > 59) {
                        return "daily logging configuration is invalid";
                    }
                }
                if (value.options.level < LogLevel::Trace || value.options.level > LogLevel::Off) {
                    return "logging sink level is invalid";
                }
                return nullptr;
            },
            sink);
        if (valid != nullptr) {
            return validation_error(valid);
        }
    }
    return {};
}

template <typename PluginConfig>
Result<void> validate_plugin_config(const PluginConfig& plugin, const char* plugin_role) {
    std::error_code error;
    if (plugin.path.empty() || !std::filesystem::is_regular_file(plugin.path, error)) {
        return Status(
            StatusCode::NotFound, "config", 0,
            std::string(plugin_role) + " plugin path is missing");
    }
    if (config_abi_view(plugin.config.root()).context == nullptr) {
        return validation_error(std::string(plugin_role) + " plugin config is empty");
    }
    return {};
}

}  // namespace

Result<void> validate_daq_config(const DAQConfig& config) {
    auto resources = validate_resource_limits(config.resources);
    if (!resources.ok()) {
        return resources;
    }
    auto payloads = validate_payload_configuration(config.resources);
    if (!payloads.ok()) {
        return payloads;
    }
    auto timeouts = validate_timeouts(config);
    if (!timeouts.ok()) {
        return timeouts;
    }
    auto logging = validate_logging_config(config.logging);
    if (!logging.ok()) {
        return logging;
    }
    if (config.acquisition_id.empty()) {
        return validation_error("acquisition_id must not be empty");
    }
    if (config.target_uri.empty()) {
        return validation_error("target_uri must not be empty");
    }
    if (config.acquisitions.empty()) {
        return validation_error("at least one Acquisition Plugin is required");
    }

    auto storage = validate_plugin_config(config.storage, "storage");
    if (!storage.ok()) {
        return storage;
    }
    for (const AcquisitionPluginConfig& acquisition : config.acquisitions) {
        auto validated = validate_plugin_config(acquisition, "acquisition");
        if (!validated.ok()) {
            return validated;
        }
    }
    return {};
}

}  // namespace rodaq
