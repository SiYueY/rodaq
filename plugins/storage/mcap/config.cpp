#include "config.hpp"

#include <array>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <rodaq/plugin/abi.hpp>

namespace rodaq::mcap_storage {
namespace {

struct StrictAbort {
    const char* message;
};

nlohmann::json parse_strict_json(std::string_view input) {
    if (input.find('\0') != std::string_view::npos ||
        plugin::abi::utf8_prefix(input, input.size()) != input.size()) {
        throw StrictAbort{"MCAP config JSON contains invalid UTF-8 or NUL"};
    }
    std::vector<std::set<std::string>> key_stack;
    std::size_t nodes = 0;
    auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                        nlohmann::json& parsed) -> bool {
        if (depth > 8) {
            throw StrictAbort{"MCAP config JSON nesting depth exceeded"};
        }
        switch (event) {
            case nlohmann::json::parse_event_t::object_start:
                if (++nodes > 256) {
                    throw StrictAbort{"MCAP config JSON node limit exceeded"};
                }
                key_stack.emplace_back();
                break;
            case nlohmann::json::parse_event_t::object_end:
                if (!key_stack.empty()) {
                    key_stack.pop_back();
                }
                break;
            case nlohmann::json::parse_event_t::key: {
                const auto& key = parsed.get_ref<const std::string&>();
                if (key_stack.empty() || !key_stack.back().insert(key).second) {
                    throw StrictAbort{"Duplicate JSON object key in MCAP config"};
                }
                break;
            }
            case nlohmann::json::parse_event_t::array_start:
            case nlohmann::json::parse_event_t::value:
                if (++nodes > 256) {
                    throw StrictAbort{"MCAP config JSON node limit exceeded"};
                }
                break;
            case nlohmann::json::parse_event_t::array_end:
                break;
        }
        return true;
    };
    return nlohmann::json::parse(input.begin(), input.end(), callback);
}

}  // namespace

Result<void> validate_config(std::string_view input) {
    if (input.empty() || input.size() > 4096) {
        return Status(StatusCode::InvalidArgument, "Invalid MCAP config size");
    }
    nlohmann::json config;
    try {
        config = parse_strict_json(input);
    } catch (const StrictAbort& abort) {
        return Status(StatusCode::InvalidArgument, abort.message);
    } catch (const nlohmann::json::exception&) {
        return Status(StatusCode::InvalidArgument, "Malformed MCAP config JSON");
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "MCAP config allocation failed");
    }
    if (!config.is_object() || !config.contains("format_version")) {
        return Status(StatusCode::InvalidArgument, "Missing MCAP format_version");
    }
    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kStrings{{
        {"format_version", "1.0"},
        {"compression", "none"},
        {"chunk_mode", "none"},
        {"rosbag2_metadata", "legacy_ros_distro"},
    }};
    for (const auto& item : config.items()) {
        bool valid = false;
        for (const auto& field : kStrings) {
            valid = valid || (item.key() == field.first && item.value().is_string() &&
                              item.value().get_ref<const std::string&>() == field.second);
        }
        if (item.key() == "chunk_crc" || item.key() == "message_index" || item.key() == "summary") {
            valid = item.value().is_boolean() && !item.value().get<bool>();
        }
        if (!valid) {
            return Status(StatusCode::InvalidArgument, "Unsupported MCAP config field/value");
        }
    }
    return {};
}

}  // namespace rodaq::mcap_storage
