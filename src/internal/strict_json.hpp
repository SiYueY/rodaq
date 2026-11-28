#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <rodaq/plugin/abi.hpp>
#include <rodaq/result.hpp>

namespace rodaq::internal {

struct StrictJsonLimits {
    std::size_t max_bytes{1024 * 1024};
    std::size_t max_depth{32};
    std::size_t max_nodes{65536};
};

// Private strict JSON parser built on nlohmann::json parser_callback_t.
// Rejects out-of-bounds input, raw NUL bytes, invalid UTF-8, nesting depth overflow,
// node-count amplification bombs, and duplicate object keys at every nesting level.
inline Result<nlohmann::json> parse_strict_json(
    std::string_view input, const StrictJsonLimits& limits = {}) {
    if (input.empty() || input.size() > limits.max_bytes) {
        return Status(StatusCode::InvalidArgument, "JSON size out of bounds");
    }
    if (input.find('\0') != std::string_view::npos ||
        plugin::abi::utf8_prefix(input, input.size()) != input.size()) {
        return Status(StatusCode::InvalidArgument, "JSON contains invalid UTF-8 or NUL");
    }

    struct Abort {
        StatusCode code;
        const char* message;
    };

    std::vector<std::set<std::string>> key_stack;
    std::size_t nodes = 0;

    try {
        auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                            nlohmann::json& parsed) -> bool {
            if (depth < 0 || static_cast<std::size_t>(depth) > limits.max_depth) {
                throw Abort{StatusCode::InvalidArgument, "JSON nesting depth exceeded"};
            }
            switch (event) {
                case nlohmann::json::parse_event_t::object_start:
                    if (++nodes > limits.max_nodes) {
                        throw Abort{StatusCode::InvalidArgument, "JSON node count exceeded"};
                    }
                    key_stack.emplace_back();
                    break;
                case nlohmann::json::parse_event_t::object_end:
                    if (!key_stack.empty()) {
                        key_stack.pop_back();
                    }
                    break;
                case nlohmann::json::parse_event_t::key: {
                    if (!parsed.is_string()) {
                        throw Abort{StatusCode::InvalidArgument, "Invalid JSON key"};
                    }
                    const auto& key = parsed.get_ref<const std::string&>();
                    if (key_stack.empty() || !key_stack.back().insert(key).second) {
                        throw Abort{StatusCode::InvalidArgument, "Duplicate JSON object key"};
                    }
                    break;
                }
                case nlohmann::json::parse_event_t::array_start:
                case nlohmann::json::parse_event_t::value:
                    if (++nodes > limits.max_nodes) {
                        throw Abort{StatusCode::InvalidArgument, "JSON node count exceeded"};
                    }
                    break;
                case nlohmann::json::parse_event_t::array_end:
                    break;
            }
            return true;
        };
        return nlohmann::json::parse(input.begin(), input.end(), callback);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const Abort& abort) {
        return Status(abort.code, abort.message);
    } catch (const nlohmann::json::exception&) {
        return Status(StatusCode::InvalidArgument, "Malformed JSON");
    }
}

}  // namespace rodaq::internal
