#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/result.hpp>

#include "ros2/qos.hpp"

namespace rodaq::ros2_acquisition {

struct StreamConfig {
    std::string key;
    std::string topic;
    std::optional<std::string> type;
    bool adapt{true};
    ros2::RequestedQos requested;
};

struct Config {
    std::uint32_t domain_id{0};
    std::uint32_t executor_threads{2};
    std::chrono::nanoseconds graph_poll_interval{std::chrono::milliseconds{500}};
    std::chrono::nanoseconds qos_change_debounce{std::chrono::milliseconds{500}};
    std::vector<StreamConfig> streams;
};

Result<Config> parse_config(std::string_view input);

}  // namespace rodaq::ros2_acquisition
