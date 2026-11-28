#pragma once

#include <algorithm>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <rodaq/result.hpp>

#include "../../internal/strict_json.hpp"
#include "digest.hpp"

namespace rodaq::persistence {

// Private boundary only: domain stores convert JSON immediately to strong types.
using Json = nlohmann::json;

struct JsonLimits {
    std::size_t max_bytes{1024 * 1024};
    std::size_t max_depth{32};
    std::size_t max_nodes{65536};
};

inline bool valid_digest(std::string_view value) noexcept {
    if (value.size() != 71 || value.substr(0, 7) != "sha256:") {
        return false;
    }
    return std::all_of(value.begin() + 7, value.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

inline Result<Json> parse_json(std::string_view input, const JsonLimits& limits = {}) {
    return internal::parse_strict_json(
        input, {limits.max_bytes, limits.max_depth, limits.max_nodes});
}

// Deterministic compact sorted-key UTF-8 JSON digest (nlohmann::json::dump()
// with lexicographically ordered object keys, no insignificant whitespace, no
// external JCS framework): SHA-256(domain || '\0' || serialized).
inline Result<std::string> semantic_digest(const Json& value, std::string_view domain) {
    try {
        const auto serialized = value.dump();
        std::string payload;
        payload.reserve(domain.size() + 1 + serialized.size());
        payload.append(domain);
        payload.push_back('\0');
        payload.append(serialized);
        return byte_digest(payload.data(), payload.size());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, {});
    }
}

inline Result<std::string> encode_integrity(const Json& document, std::string_view domain) {
    if (!document.is_object() || document.contains("content_digest")) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto digest = semantic_digest(document, domain);
    if (!digest.ok()) {
        return digest.status();
    }
    try {
        Json output = document;
        output["content_digest"] = std::move(digest.value());
        return output.dump();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

inline Result<void> verify_integrity(const Json& document, std::string_view domain) {
    if (!document.is_object() || !document.contains("content_digest") ||
        !document.at("content_digest").is_string()) {
        return Status(StatusCode::DataLoss, {});
    }
    const auto& declared = document.at("content_digest").get_ref<const std::string&>();
    if (!valid_digest(declared)) {
        return Status(StatusCode::DataLoss, {});
    }
    try {
        Json without = document;
        without.erase("content_digest");
        auto expected = semantic_digest(without, domain);
        if (!expected.ok()) {
            return expected.status();
        }
        return expected.value() == declared ? Result<void>{}
                                            : Result<void>(Status(StatusCode::DataLoss, {}));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::persistence
