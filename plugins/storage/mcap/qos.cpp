#include "qos.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <new>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <rodaq/plugin/abi.hpp>

namespace rodaq::mcap_storage {
namespace {

using Json = nlohmann::json;

struct QosProfile {
    std::uint32_t history{0};
    std::uint32_t reliability{0};
    std::uint32_t durability{0};
    std::uint32_t liveliness{0};
    std::uint64_t depth{0};
    std::uint64_t deadline_sec{0};
    std::uint64_t deadline_nsec{0};
    std::uint64_t lifespan_sec{0};
    std::uint64_t lifespan_nsec{0};
    std::uint64_t lease_sec{0};
    std::uint64_t lease_nsec{0};
    bool avoid_ros_namespace_conventions{false};
};

struct StrictAbort {
    const char* message;
};

Json parse_strict_json(std::string_view input) {
    if (input.find('\0') != std::string_view::npos ||
        plugin::abi::utf8_prefix(input, input.size()) != input.size()) {
        throw StrictAbort{"Invalid UTF-8 or NUL in ros2/1.0 metadata"};
    }
    std::vector<std::set<std::string>> key_stack;
    std::size_t nodes = 0;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) -> bool {
        if (depth > 16) {
            throw StrictAbort{"ros2/1.0 metadata nesting depth exceeded"};
        }
        switch (event) {
            case Json::parse_event_t::object_start:
                if (++nodes > 16384) {
                    throw StrictAbort{"ros2/1.0 metadata node count exceeded"};
                }
                key_stack.emplace_back();
                break;
            case Json::parse_event_t::object_end:
                if (!key_stack.empty()) {
                    key_stack.pop_back();
                }
                break;
            case Json::parse_event_t::key: {
                const auto& key = parsed.get_ref<const std::string&>();
                if (key_stack.empty() || !key_stack.back().insert(key).second) {
                    throw StrictAbort{"Duplicate key in ros2/1.0 metadata"};
                }
                break;
            }
            case Json::parse_event_t::array_start:
            case Json::parse_event_t::value:
                if (++nodes > 16384) {
                    throw StrictAbort{"ros2/1.0 metadata node count exceeded"};
                }
                break;
            case Json::parse_event_t::array_end:
                break;
        }
        return true;
    };
    return Json::parse(input.begin(), input.end(), callback);
}

bool read_u64(const Json& obj, const char* key, std::uint64_t& out) {
    if (!obj.contains(key)) {
        return false;
    }
    const auto& value = obj.at(key);
    if (value.is_number_unsigned()) {
        out = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const auto signed_val = value.get<std::int64_t>();
        if (signed_val < 0) {
            return false;
        }
        out = static_cast<std::uint64_t>(signed_val);
        return true;
    }
    return false;
}

bool read_u32_bounded(const Json& obj, const char* key, std::uint32_t max_val, std::uint32_t& out) {
    std::uint64_t raw = 0;
    if (!read_u64(obj, key, raw) || raw > max_val) {
        return false;
    }
    out = static_cast<std::uint32_t>(raw);
    return true;
}

bool parse_qos(const Json& item, QosProfile& out) {
    if (!item.is_object() || item.size() != 12) {
        return false;
    }
    if (!read_u32_bounded(item, "history", 3, out.history) ||
        !read_u32_bounded(item, "reliability", 3, out.reliability) ||
        !read_u32_bounded(item, "durability", 3, out.durability) ||
        !read_u32_bounded(item, "liveliness", 4, out.liveliness) ||
        !read_u64(item, "depth", out.depth) || !read_u64(item, "deadline_sec", out.deadline_sec) ||
        !read_u64(item, "deadline_nsec", out.deadline_nsec) ||
        !read_u64(item, "lifespan_sec", out.lifespan_sec) ||
        !read_u64(item, "lifespan_nsec", out.lifespan_nsec) ||
        !read_u64(item, "lease_sec", out.lease_sec) ||
        !read_u64(item, "lease_nsec", out.lease_nsec) ||
        !item.contains("avoid_ros_namespace_conventions") ||
        !item.at("avoid_ros_namespace_conventions").is_boolean()) {
        return false;
    }
    if (out.deadline_nsec >= 1000000000ULL || out.lifespan_nsec >= 1000000000ULL ||
        out.lease_nsec >= 1000000000ULL) {
        return false;
    }
    out.avoid_ros_namespace_conventions = item.at("avoid_ros_namespace_conventions").get<bool>();
    return true;
}

bool valid_type_hash(std::string_view hash) noexcept {
    return hash.size() == 71 && hash.substr(0, 7) == "RIHS01_" &&
           std::all_of(hash.begin() + 7, hash.end(), [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

}  // namespace

Result<Ros2ChannelMetadata> parse_ros2_metadata(const rodaq_descriptor_metadata_t& metadata) {
    if (metadata.format.size == 0 && metadata.json.size == 0) {
        return Ros2ChannelMetadata{};
    }
    if (metadata.format.size == 0 || metadata.json.size == 0 || metadata.format.data == nullptr ||
        metadata.json.data == nullptr) {
        return Status(StatusCode::InvalidArgument, "Half-present descriptor metadata");
    }
    const std::string_view format(metadata.format.data, metadata.format.size);
    if (format != kRos2MetadataFormat) {
        return Status(StatusCode::InvalidArgument, "Unsupported MCAP ros2 descriptor metadata");
    }
    if (metadata.json.size > kMaxRos2MetadataBytes) {
        return Status(StatusCode::ResourceExhausted, "ros2/1.0 metadata exceeds byte limit");
    }
    try {
        const auto doc = parse_strict_json({metadata.json.data, metadata.json.size});
        if (!doc.is_object()) {
            return Status(StatusCode::InvalidArgument, "ros2/1.0 metadata must be a JSON object");
        }
        for (const auto& entry : doc.items()) {
            if (entry.key() != "offered_qos_profiles" && entry.key() != "type_description_hash" &&
                entry.key() != "subscription") {
                return Status(StatusCode::InvalidArgument, "Unknown ros2/1.0 metadata field");
            }
        }
        Ros2ChannelMetadata result;
        if (doc.contains("type_description_hash")) {
            if (!doc.at("type_description_hash").is_string()) {
                return Status(StatusCode::InvalidArgument, "Invalid type_description_hash type");
            }
            const auto& hash = doc.at("type_description_hash").get_ref<const std::string&>();
            if (!hash.empty()) {
                if (!valid_type_hash(hash)) {
                    return Status(StatusCode::InvalidArgument, "Invalid RIHS01 type hash");
                }
                result.type_description_hash = hash;
            }
        }
        if (doc.contains("offered_qos_profiles")) {
            const auto& profiles = doc.at("offered_qos_profiles");
            if (!profiles.is_array()) {
                return Status(StatusCode::InvalidArgument, "offered_qos_profiles must be array");
            }
            if (profiles.size() > kMaxQosProfiles) {
                return Status(StatusCode::ResourceExhausted, "offered_qos_profiles limit exceeded");
            }
            if (!profiles.empty()) {
                std::string output;
                output.reserve(profiles.size() * kMaxEncodedQosBytes);
                for (const auto& item : profiles) {
                    QosProfile qos;
                    if (!parse_qos(item, qos)) {
                        return Status(StatusCode::InvalidArgument, "Invalid ROS 2 QoS profile");
                    }
                    fmt::format_to(
                        std::back_inserter(output), kQosFormat, qos.history, qos.depth,
                        qos.reliability, qos.durability, qos.deadline_sec, qos.deadline_nsec,
                        qos.lifespan_sec, qos.lifespan_nsec, qos.liveliness, qos.lease_sec,
                        qos.lease_nsec, qos.avoid_ros_namespace_conventions);
                }
                result.offered_qos_yaml = std::move(output);
            }
        }
        return result;
    } catch (const StrictAbort& abort) {
        return Status(StatusCode::InvalidArgument, abort.message);
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, "Malformed ros2/1.0 metadata JSON");
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::mcap_storage
