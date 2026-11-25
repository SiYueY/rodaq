#include "server_config.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace rodaq::server {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kMaxResponseBytesHard = 64ull * 1024 * 1024;
constexpr std::uint64_t kMaxInFlightBytesHard = 128ull * 1024 * 1024;

// Strict JSON parser that rejects duplicate keys, trailing garbage, and non-finite numbers.
Result<Json> parse_strict_json(std::string_view text) {
    struct DuplicateTracker {
        std::vector<std::set<std::string>> stack;
        bool duplicate{false};
    } tracker;

    auto callback = [&tracker](int /*depth*/, Json::parse_event_t event, Json& parsed) -> bool {
        if (event == Json::parse_event_t::object_start) {
            tracker.stack.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            if (!tracker.stack.empty()) {
                tracker.stack.pop_back();
            }
        } else if (event == Json::parse_event_t::key) {
            if (!tracker.stack.empty()) {
                const auto& key = parsed.get_ref<const std::string&>();
                if (!tracker.stack.back().insert(key).second) {
                    tracker.duplicate = true;
                    return false;
                }
            }
        }
        return true;
    };

    try {
        auto parsed = Json::parse(text.begin(), text.end(), callback, true, false);
        if (tracker.duplicate || parsed.is_discarded()) {
            return Status(StatusCode::InvalidArgument, "Invalid or duplicate-key JSON");
        }
        return parsed;
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, "Malformed JSON");
    }
}

Result<std::uint64_t> parse_decimal_u64(const Json& value) {
    if (!value.is_string()) {
        return Status(StatusCode::InvalidArgument, "Expected decimal string");
    }
    const auto& s = value.get_ref<const std::string&>();
    if (s.empty() || s.size() > 20 || (s.size() > 1 && s[0] == '0')) {
        return Status(StatusCode::InvalidArgument, "Invalid decimal string");
    }
    std::uint64_t out = 0;
    const auto* first = s.data();
    const auto* last = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec != std::errc{} || ptr != last) {
        return Status(StatusCode::InvalidArgument, "Decimal string out of range");
    }
    return out;
}

Result<std::uint32_t> parse_u32_int(const Json& value) {
    if (!value.is_number_integer()) {
        return Status(StatusCode::InvalidArgument, "Expected integer");
    }
    const auto signed_val = value.get<std::int64_t>();
    if (signed_val < 0 || signed_val > std::numeric_limits<std::uint32_t>::max()) {
        return Status(StatusCode::InvalidArgument, "Integer out of range");
    }
    return static_cast<std::uint32_t>(signed_val);
}

Result<std::chrono::nanoseconds> parse_duration(const Json& value) {
    if (!value.is_object()) {
        return Status(StatusCode::InvalidArgument, "Duration must be an object");
    }
    for (const auto& [k, _] : value.items()) {
        if (k != "value" && k != "unit") {
            return Status(StatusCode::InvalidArgument, "Unknown duration field");
        }
    }
    if (!value.contains("value") || !value.contains("unit") || !value["unit"].is_string()) {
        return Status(StatusCode::InvalidArgument, "Duration requires value and unit");
    }
    auto amount = parse_decimal_u64(value["value"]);
    if (!amount.ok()) {
        return amount.status();
    }
    const auto& unit = value["unit"].get_ref<const std::string&>();
    std::uint64_t mult = 0;
    if (unit == "ns") {
        mult = 1;
    } else if (unit == "us") {
        mult = 1'000;
    } else if (unit == "ms") {
        mult = 1'000'000;
    } else if (unit == "s") {
        mult = 1'000'000'000;
    } else {
        return Status(StatusCode::InvalidArgument, "Unsupported duration unit");
    }
    if (amount.value() >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / mult) {
        return Status(StatusCode::InvalidArgument, "Duration overflow");
    }
    return std::chrono::nanoseconds{static_cast<std::int64_t>(amount.value() * mult)};
}

Result<void> check_allowed_keys(const Json& obj, const std::set<std::string_view>& allowed) {
    if (!obj.is_object()) {
        return Status(StatusCode::InvalidArgument, "Expected JSON object");
    }
    for (const auto& [k, _] : obj.items()) {
        if (allowed.find(k) == allowed.end()) {
            return Status(StatusCode::InvalidArgument, "Unknown configuration field: " + k);
        }
    }
    return {};
}

Result<std::string> canonicalize_loopback(const std::string& raw) {
    struct in_addr v4 {};
    if (::inet_pton(AF_INET, raw.c_str(), &v4) == 1) {
        const auto host_order = ntohl(v4.s_addr);
        if ((host_order & 0xFF000000u) != 0x7F000000u) {
            return Status(StatusCode::InvalidArgument, "listen_address must be loopback");
        }
        char buf[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &v4, buf, sizeof(buf)) == nullptr) {
            return Status(StatusCode::InvalidArgument, "Failed to canonicalize IPv4 loopback");
        }
        return std::string(buf);
    }

    struct in6_addr v6 {};
    if (::inet_pton(AF_INET6, raw.c_str(), &v6) == 1) {
        if (!IN6_IS_ADDR_LOOPBACK(&v6)) {
            return Status(StatusCode::InvalidArgument, "listen_address must be loopback");
        }
        char buf[INET6_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET6, &v6, buf, sizeof(buf)) == nullptr) {
            return Status(StatusCode::InvalidArgument, "Failed to canonicalize IPv6 loopback");
        }
        return std::string(buf);
    }

    return Status(
        StatusCode::InvalidArgument, "listen_address must be an IPv4/IPv6 loopback literal");
}

}  // namespace

Result<ServerConfig> parse_server_config(std::string_view json_text) {
    auto parsed = parse_strict_json(json_text);
    if (!parsed.ok()) {
        return parsed.status();
    }
    const auto& root = parsed.value();
    auto root_check = check_allowed_keys(
        root, {"format_version", "http", "execution", "security", "download", "timeouts",
               "static_asset_root"});
    if (!root_check.ok()) {
        return root_check.status();
    }

    ServerConfig cfg;
    if (root.contains("format_version")) {
        if (!root["format_version"].is_string()) {
            return Status(StatusCode::InvalidArgument, "format_version must be string");
        }
        cfg.format_version = root["format_version"].get<std::string>();
    }
    if (root.contains("static_asset_root")) {
        if (!root["static_asset_root"].is_string()) {
            return Status(StatusCode::InvalidArgument, "static_asset_root must be string");
        }
        cfg.static_asset_root = root["static_asset_root"].get<std::string>();
    }

    if (root.contains("http")) {
        const auto& h = root["http"];
        auto chk = check_allowed_keys(
            h, {"listen_address", "port", "worker_threads", "max_request_body_bytes",
                "max_header_bytes", "max_query_bytes", "max_json_depth", "max_json_string_bytes",
                "max_json_members", "max_json_response_bytes", "max_page_size",
                "recommended_polling_floor"});
        if (!chk.ok()) {
            return chk.status();
        }
        if (h.contains("listen_address")) {
            if (!h["listen_address"].is_string()) {
                return Status(StatusCode::InvalidArgument, "listen_address must be string");
            }
            cfg.http.listen_address = h["listen_address"].get<std::string>();
        }
        if (h.contains("port")) {
            auto v = parse_u32_int(h["port"]);
            if (!v.ok() || v.value() == 0 || v.value() > 65535) {
                return Status(StatusCode::InvalidArgument, "Invalid port");
            }
            cfg.http.port = static_cast<std::uint16_t>(v.value());
        }
        if (h.contains("worker_threads")) {
            auto v = parse_u32_int(h["worker_threads"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.worker_threads = v.value();
        }
        if (h.contains("max_request_body_bytes")) {
            auto v = parse_decimal_u64(h["max_request_body_bytes"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.max_request_body_bytes = v.value();
        }
        if (h.contains("max_header_bytes")) {
            auto v = parse_decimal_u64(h["max_header_bytes"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.max_header_bytes = v.value();
        }
        if (h.contains("max_query_bytes")) {
            auto v = parse_decimal_u64(h["max_query_bytes"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.max_query_bytes = v.value();
        }
        if (h.contains("max_json_depth")) {
            auto v = parse_u32_int(h["max_json_depth"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.max_json_depth = v.value();
        }
        if (h.contains("max_json_string_bytes")) {
            auto v = parse_decimal_u64(h["max_json_string_bytes"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.max_json_string_bytes = v.value();
        }
        if (h.contains("max_json_members")) {
            auto v = parse_u32_int(h["max_json_members"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.max_json_members = v.value();
        }
        if (h.contains("max_json_response_bytes")) {
            auto v = parse_decimal_u64(h["max_json_response_bytes"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.max_json_response_bytes = v.value();
        }
        if (h.contains("max_page_size")) {
            auto v = parse_u32_int(h["max_page_size"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.max_page_size = v.value();
        }
        if (h.contains("recommended_polling_floor")) {
            auto v = parse_duration(h["recommended_polling_floor"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.http.recommended_polling_floor = v.value();
        }
    }

    if (root.contains("execution")) {
        const auto& e = root["execution"];
        auto chk = check_allowed_keys(
            e,
            {"control_workers", "control_reserve_workers", "query_workers", "query_reserve_workers",
             "maintenance_workers", "control_queue", "control_reserve_queue", "query_queue",
             "query_reserve_queue", "maintenance_queue"});
        if (!chk.ok()) {
            return chk.status();
        }
        auto load_u32 = [&](const char* key, std::uint32_t& dst) -> Result<void> {
            if (!e.contains(key)) {
                return {};
            }
            auto v = parse_u32_int(e[key]);
            if (!v.ok()) {
                return v.status();
            }
            dst = v.value();
            return {};
        };
        for (const auto& [k, ptr] : std::vector<std::pair<const char*, std::uint32_t*>>{
                 {"control_workers", &cfg.execution.control_workers},
                 {"control_reserve_workers", &cfg.execution.control_reserve_workers},
                 {"query_workers", &cfg.execution.query_workers},
                 {"query_reserve_workers", &cfg.execution.query_reserve_workers},
                 {"maintenance_workers", &cfg.execution.maintenance_workers},
                 {"control_queue", &cfg.execution.control_queue},
                 {"control_reserve_queue", &cfg.execution.control_reserve_queue},
                 {"query_queue", &cfg.execution.query_queue},
                 {"query_reserve_queue", &cfg.execution.query_reserve_queue},
                 {"maintenance_queue", &cfg.execution.maintenance_queue}}) {
            auto r = load_u32(k, *ptr);
            if (!r.ok()) {
                return r.status();
            }
        }
    }

    if (root.contains("security")) {
        const auto& s = root["security"];
        auto chk = check_allowed_keys(
            s, {"credential_store", "max_credentials", "max_sessions", "session_idle_timeout",
                "session_absolute_timeout"});
        if (!chk.ok()) {
            return chk.status();
        }
        if (s.contains("credential_store")) {
            if (!s["credential_store"].is_string()) {
                return Status(StatusCode::InvalidArgument, "credential_store must be string");
            }
            cfg.security.credential_store = s["credential_store"].get<std::string>();
        }
        if (s.contains("max_credentials")) {
            auto v = parse_u32_int(s["max_credentials"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.security.max_credentials = v.value();
        }
        if (s.contains("max_sessions")) {
            auto v = parse_u32_int(s["max_sessions"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.security.max_sessions = v.value();
        }
        if (s.contains("session_idle_timeout")) {
            auto v = parse_duration(s["session_idle_timeout"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.security.session_idle_timeout = v.value();
        }
        if (s.contains("session_absolute_timeout")) {
            auto v = parse_duration(s["session_absolute_timeout"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.security.session_absolute_timeout = v.value();
        }
    }

    if (root.contains("download")) {
        const auto& d = root["download"];
        auto chk = check_allowed_keys(
            d, {"workers", "max_concurrent", "chunk_bytes", "max_bytes_in_flight"});
        if (!chk.ok()) {
            return chk.status();
        }
        if (d.contains("workers")) {
            auto v = parse_u32_int(d["workers"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.download.workers = v.value();
        }
        if (d.contains("max_concurrent")) {
            auto v = parse_u32_int(d["max_concurrent"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.download.max_concurrent = v.value();
        }
        if (d.contains("chunk_bytes")) {
            auto v = parse_decimal_u64(d["chunk_bytes"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.download.chunk_bytes = v.value();
        }
        if (d.contains("max_bytes_in_flight")) {
            auto v = parse_decimal_u64(d["max_bytes_in_flight"]);
            if (!v.ok()) {
                return v.status();
            }
            cfg.download.max_bytes_in_flight = v.value();
        }
    }

    if (root.contains("timeouts")) {
        const auto& t = root["timeouts"];
        auto chk = check_allowed_keys(t, {"health", "query", "mutation_acceptance", "shutdown"});
        if (!chk.ok()) {
            return chk.status();
        }
        auto load_dur = [&](const char* key, std::chrono::nanoseconds& dst) -> Result<void> {
            if (!t.contains(key)) {
                return {};
            }
            auto v = parse_duration(t[key]);
            if (!v.ok()) {
                return v.status();
            }
            dst = v.value();
            return {};
        };
        for (const auto& [k, ptr] : std::vector<std::pair<const char*, std::chrono::nanoseconds*>>{
                 {"health", &cfg.timeouts.health},
                 {"query", &cfg.timeouts.query},
                 {"mutation_acceptance", &cfg.timeouts.mutation_acceptance},
                 {"shutdown", &cfg.timeouts.shutdown}}) {
            auto r = load_dur(k, *ptr);
            if (!r.ok()) {
                return r.status();
            }
        }
    }

    auto validated = validate_server_config(cfg);
    if (!validated.ok()) {
        return validated.status();
    }
    return cfg;
}

Result<void> validate_server_config(ServerConfig& cfg) {
    if (cfg.format_version != "1.0") {
        return Status(StatusCode::InvalidArgument, "format_version must be 1.0");
    }

    auto canon = canonicalize_loopback(cfg.http.listen_address);
    if (!canon.ok()) {
        return canon.status();
    }
    cfg.http.listen_address = std::move(canon.value());

    if (cfg.http.port == 0 || cfg.http.worker_threads < 1 || cfg.http.worker_threads > 32 ||
        cfg.http.max_request_body_bytes < 65536 || cfg.http.max_request_body_bytes > 33554432 ||
        cfg.http.max_header_bytes < 4096 || cfg.http.max_header_bytes > 131072 ||
        cfg.http.max_query_bytes < 1024 || cfg.http.max_query_bytes > 65536 ||
        cfg.http.max_json_depth < 8 || cfg.http.max_json_depth > 128 ||
        cfg.http.max_json_string_bytes < 1024 ||
        cfg.http.max_json_string_bytes > cfg.http.max_request_body_bytes ||
        cfg.http.max_json_members < 16 || cfg.http.max_json_members > 65536 ||
        cfg.http.max_json_response_bytes < cfg.http.max_request_body_bytes ||
        cfg.http.max_json_response_bytes > kMaxResponseBytesHard || cfg.http.max_page_size < 1 ||
        cfg.http.max_page_size > 1000 ||
        cfg.http.recommended_polling_floor <= std::chrono::nanoseconds::zero()) {
        return Status(StatusCode::InvalidArgument, "Invalid HTTP server configuration bounds");
    }

    if (cfg.execution.control_workers < 1 || cfg.execution.control_workers > 32 ||
        cfg.execution.control_reserve_workers < 1 || cfg.execution.control_reserve_workers > 4 ||
        cfg.execution.query_workers < 1 || cfg.execution.query_workers > 32 ||
        cfg.execution.query_reserve_workers < 1 || cfg.execution.query_reserve_workers > 4 ||
        cfg.execution.maintenance_workers < 1 || cfg.execution.maintenance_workers > 8 ||
        cfg.execution.control_queue < 4 || cfg.execution.control_queue > 4096 ||
        cfg.execution.control_reserve_queue < 1 || cfg.execution.control_reserve_queue > 128 ||
        cfg.execution.query_queue < 4 || cfg.execution.query_queue > 8192 ||
        cfg.execution.query_reserve_queue < 1 || cfg.execution.query_reserve_queue > 256 ||
        cfg.execution.maintenance_queue < 1 || cfg.execution.maintenance_queue > 1024) {
        return Status(StatusCode::InvalidArgument, "Invalid execution domain bounds");
    }

    if (!cfg.security.credential_store.is_absolute() || cfg.security.max_credentials < 1 ||
        cfg.security.max_credentials > 1024 || cfg.security.max_sessions < 1 ||
        cfg.security.max_sessions > 4096 ||
        cfg.security.session_idle_timeout <= std::chrono::nanoseconds::zero() ||
        cfg.security.session_absolute_timeout <= std::chrono::nanoseconds::zero() ||
        cfg.security.session_idle_timeout > cfg.security.session_absolute_timeout) {
        return Status(StatusCode::InvalidArgument, "Invalid security configuration");
    }

    if (cfg.download.workers < 1 || cfg.download.workers > 16 || cfg.download.max_concurrent < 1 ||
        cfg.download.max_concurrent > 64 || cfg.download.chunk_bytes < 65536 ||
        cfg.download.chunk_bytes > 4194304 ||
        cfg.download.max_bytes_in_flight < cfg.download.chunk_bytes ||
        cfg.download.max_bytes_in_flight > kMaxInFlightBytesHard) {
        return Status(StatusCode::InvalidArgument, "Invalid download configuration");
    }

    using namespace std::chrono_literals;
    if (cfg.timeouts.health < 100ms || cfg.timeouts.health > 4s || cfg.timeouts.query < 500ms ||
        cfg.timeouts.query > 8s || cfg.timeouts.mutation_acceptance < 1s ||
        cfg.timeouts.mutation_acceptance > 12s || cfg.timeouts.shutdown < 5s ||
        cfg.timeouts.shutdown > 300s) {
        return Status(StatusCode::InvalidArgument, "Invalid timeout configuration");
    }
    const auto max_ordinary =
        std::max({cfg.timeouts.health, cfg.timeouts.query, cfg.timeouts.mutation_acceptance});
    if (cfg.timeouts.shutdown <= max_ordinary) {
        return Status(
            StatusCode::InvalidArgument, "shutdown timeout must exceed observer timeouts");
    }

    // Derive exact loopback Host and Origin allowlists for the configured listener + port.
    cfg.allowed_hosts.clear();
    cfg.allowed_origins.clear();
    const auto port_str = std::to_string(cfg.http.port);
    const bool is_v6 = cfg.http.listen_address.find(':') != std::string::npos;
    const std::string literal_host =
        is_v6 ? ("[" + cfg.http.listen_address + "]") : cfg.http.listen_address;
    cfg.allowed_hosts.push_back(literal_host + ":" + port_str);
    cfg.allowed_hosts.push_back("localhost:" + port_str);
    if (cfg.http.port == 80) {
        cfg.allowed_hosts.push_back(literal_host);
        cfg.allowed_hosts.push_back("localhost");
    }
    for (const auto& host : cfg.allowed_hosts) {
        cfg.allowed_origins.push_back("http://" + host);
    }
    return {};
}

}  // namespace rodaq::server
