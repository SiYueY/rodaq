#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/result.hpp>

namespace rodaq::server {

struct HttpConfig {
    std::string listen_address{"127.0.0.1"};
    std::uint16_t port{8080};
    std::uint32_t worker_threads{2};
    std::uint64_t max_request_body_bytes{4194304};
    std::uint64_t max_header_bytes{32768};
    std::uint64_t max_query_bytes{8192};
    std::uint32_t max_json_depth{64};
    std::uint64_t max_json_string_bytes{1048576};
    std::uint32_t max_json_members{4096};
    std::uint64_t max_json_response_bytes{16777216};
    std::uint32_t max_page_size{200};
    std::chrono::nanoseconds recommended_polling_floor{std::chrono::milliseconds{500}};
};

struct ExecutionConfig {
    std::uint32_t control_workers{2};
    std::uint32_t control_reserve_workers{1};
    std::uint32_t query_workers{2};
    std::uint32_t query_reserve_workers{1};
    std::uint32_t maintenance_workers{1};
    std::uint32_t control_queue{64};
    std::uint32_t control_reserve_queue{8};
    std::uint32_t query_queue{128};
    std::uint32_t query_reserve_queue{16};
    std::uint32_t maintenance_queue{32};
};

struct SecurityConfig {
    std::filesystem::path credential_store{"/etc/rodaq/server_credentials.json"};
    std::uint32_t max_credentials{64};
    std::uint32_t max_sessions{32};
    std::chrono::nanoseconds session_idle_timeout{std::chrono::seconds{1800}};
    std::chrono::nanoseconds session_absolute_timeout{std::chrono::seconds{28800}};
    // In tests or non-packaged dev setups, caller may relax strict 0600 check only explicitly.
    bool enforce_credential_file_mode{true};
};

struct DownloadConfig {
    std::uint32_t workers{2};
    std::uint32_t max_concurrent{4};
    std::uint64_t chunk_bytes{1048576};
    std::uint64_t max_bytes_in_flight{8388608};
};

struct TimeoutConfig {
    std::chrono::nanoseconds health{std::chrono::seconds{1}};
    std::chrono::nanoseconds query{std::chrono::seconds{3}};
    std::chrono::nanoseconds mutation_acceptance{std::chrono::seconds{5}};
    std::chrono::nanoseconds shutdown{std::chrono::seconds{30}};
};

struct ServerConfig {
    std::string format_version{"1.0"};
    HttpConfig http;
    ExecutionConfig execution;
    SecurityConfig security;
    DownloadConfig download;
    TimeoutConfig timeouts;
    std::filesystem::path static_asset_root;
    std::chrono::nanoseconds idempotency_retention{std::chrono::hours{24}};
    // Derived canonical loopback Host / Origin allowlists.
    std::vector<std::string> allowed_hosts;
    std::vector<std::string> allowed_origins;
};

Result<ServerConfig> parse_server_config(std::string_view json_text);
Result<void> validate_server_config(ServerConfig& config);

}  // namespace rodaq::server
