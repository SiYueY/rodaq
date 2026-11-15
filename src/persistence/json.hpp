#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <rodaq/result.hpp>

namespace rodaq::persistence {

// Private boundary only: domain Stores convert JSON immediately to strong types.
using Json = nlohmann::json;

struct JsonLimits {
    std::size_t max_bytes{1024 * 1024};
    std::size_t max_depth{32};
    std::size_t max_string_bytes{64 * 1024};
    std::size_t max_object_members{256};
    std::size_t max_array_elements{4096};
    std::size_t max_nodes{16384};
};

// JSON numbers use the JCS binary64 model. Domain schemas must require decimal
// strings for exact 64-bit quantities; this parser does not infer field meaning.
Result<Json> parse_json(std::string_view input, const JsonLimits& limits = {});
Result<std::string> canonical_json(const Json& value, const JsonLimits& limits = {});

// domain excludes its terminal NUL: this primitive appends exactly one NUL.
Result<std::string> semantic_digest(
    const Json& value, std::string_view domain, const JsonLimits& limits = {});

// The domain owner must validate its format/schema before integrity verification.
// Exactly the top-level content_digest is excluded; nested fields are preserved.
Result<void> verify_integrity(
    const Json& document, std::string_view domain, const JsonLimits& limits = {});
Result<std::string> encode_integrity(
    const Json& document, std::string_view domain, const JsonLimits& limits = {});

}  // namespace rodaq::persistence
