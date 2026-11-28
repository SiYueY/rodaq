#include "config.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <rmw/validate_full_topic_name.h>

#include "ros2/schema.hpp"

namespace rodaq::ros2_acquisition {
namespace {

using Json = nlohmann::json;

bool fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) {
        return false;
    }
    for (const auto& item : value.items()) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) {
            return false;
        }
    }
    return true;
}

Result<std::uint64_t> decimal_or_uint(const Json& value, std::uint64_t maximum) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= maximum) {
            return number;
        }
        return Status(StatusCode::InvalidArgument, {});
    }
    if (!value.is_string()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || (text.size() > 1 && text.front() == '0') || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || result > maximum) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return result;
}

Result<std::chrono::nanoseconds> duration(const Json& value, bool zero) {
    if (!fields(value, {"value", "unit"}) || !value.contains("value") || !value.contains("unit") ||
        !value["unit"].is_string()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto& unit = value["unit"].get_ref<const std::string&>();
    const auto multiplier = unit == "ns"   ? 1LL
                            : unit == "us" ? 1000LL
                            : unit == "ms" ? 1000000LL
                            : unit == "s"  ? 1000000000LL
                                           : 0LL;
    if (multiplier == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto parsed = decimal_or_uint(value["value"], 60000000000ULL / multiplier);
    if (!parsed.ok() || (!zero && parsed.value() == 0)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return std::chrono::nanoseconds(static_cast<std::int64_t>(parsed.value() * multiplier));
}

bool integer(const Json& value, unsigned minimum, unsigned maximum) {
    if (!value.is_number()) {
        return false;
    }
    const auto number = value.get<double>();
    return std::isfinite(number) && number >= minimum && number <= maximum &&
           std::trunc(number) == number;
}

Result<StreamConfig> stream_config(const Json& input) {
    if (!fields(input, {"stream_key", "topic", "type", "qos"}) || !input.contains("stream_key") ||
        !input.contains("topic") || !input.contains("qos") || !input["stream_key"].is_string() ||
        !input["topic"].is_string()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    StreamConfig config;
    config.key = input["stream_key"].get<std::string>();
    config.topic = input["topic"].get<std::string>();
    int valid = RMW_TOPIC_INVALID_IS_EMPTY_STRING;
    std::size_t invalid_at = 0;
    if (config.key.empty() || config.key.size() > 128 ||
        config.topic.find('\0') != std::string::npos ||
        !std::all_of(
            config.key.begin(), config.key.end(),
            [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }) ||
        rmw_validate_full_topic_name(config.topic.c_str(), &valid, &invalid_at) != RMW_RET_OK ||
        valid != RMW_TOPIC_VALID) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (input.contains("type")) {
        if (!input["type"].is_string() ||
            !ros2::valid_type_name(input["type"].get_ref<const std::string&>())) {
            return Status(StatusCode::InvalidArgument, {});
        }
        config.type = input["type"].get<std::string>();
    }
    const auto& qos = input["qos"];
    if (!qos.is_object() || !qos.contains("mode") || !qos["mode"].is_string()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (qos["mode"] == "adapt") {
        if (!fields(qos, {"mode", "depth"})) {
            return Status(StatusCode::InvalidArgument, {});
        }
    } else if (qos["mode"] == "explicit") {
        if (!fields(qos, {"mode", "depth", "reliability", "durability"}) ||
            !qos.contains("depth") || !qos.contains("reliability") || !qos.contains("durability") ||
            !qos["reliability"].is_string() || !qos["durability"].is_string()) {
            return Status(StatusCode::InvalidArgument, {});
        }
        config.adapt = false;
        if (qos["reliability"] == "reliable") {
            config.requested.reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
        } else if (qos["reliability"] != "best_effort") {
            return Status(StatusCode::InvalidArgument, {});
        }
        if (qos["durability"] == "transient_local") {
            config.requested.durability = RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL;
        } else if (qos["durability"] != "volatile") {
            return Status(StatusCode::InvalidArgument, {});
        }
    } else {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (qos.contains("depth")) {
        auto depth = decimal_or_uint(qos["depth"], 1000000);
        if (!depth.ok() || depth.value() == 0) {
            return Status(StatusCode::InvalidArgument, {});
        }
        config.requested.depth = depth.value();
    }
    return config;
}

}  // namespace

Result<Config> parse_config(std::string_view input) {
    if (input.empty() || input.size() > 512 * 1024) {
        return Status(StatusCode::InvalidArgument, "Invalid ROS 2 config size");
    }
    try {
        const auto value = Json::parse(input.begin(), input.end());
        if (!fields(
                value, {"format_version", "domain_id", "executor_threads", "graph_poll_interval",
                        "qos_change_debounce", "streams"}) ||
            !value.contains("format_version") || value["format_version"] != "1.0" ||
            !value.contains("streams") || !value["streams"].is_array() ||
            value["streams"].empty() || value["streams"].size() > 256) {
            return Status(StatusCode::InvalidArgument, {});
        }
        Config config;
        if (value.contains("domain_id")) {
            if (!integer(value["domain_id"], 0, 232)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            config.domain_id = static_cast<std::uint32_t>(value["domain_id"].get<double>());
        }
        if (value.contains("executor_threads")) {
            if (!integer(value["executor_threads"], 1, 8)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            config.executor_threads =
                static_cast<std::uint32_t>(value["executor_threads"].get<double>());
        }
        if (value.contains("graph_poll_interval")) {
            auto interval = duration(value["graph_poll_interval"], false);
            if (!interval.ok()) {
                return interval.status();
            }
            config.graph_poll_interval = interval.value();
        }
        if (value.contains("qos_change_debounce")) {
            auto interval = duration(value["qos_change_debounce"], true);
            if (!interval.ok()) {
                return interval.status();
            }
            config.qos_change_debounce = interval.value();
        }
        std::set<std::string> keys;
        for (const auto& entry : value["streams"]) {
            auto stream = stream_config(entry);
            if (!stream.ok()) {
                return stream.status();
            }
            if (!keys.insert(stream.value().key).second) {
                return Status(StatusCode::InvalidArgument, "Duplicate ROS stream key");
            }
            config.streams.push_back(std::move(stream.value()));
        }
        return config;
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, "Malformed ROS 2 config JSON");
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::ros2_acquisition
