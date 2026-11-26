#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <rodaq/daq/runtime.hpp>
#include <rodaq/result.hpp>

namespace rodaq::server {

enum class ServerPhase { Recovering, Ready, Draining, Failed };

std::string_view to_wire_string(ServerPhase phase) noexcept;

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
    // Enforces Linux owner-only (no group/other rwx) mode check on credential_store.
    bool enforce_file_permissions{true};
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
    HttpConfig http;
    ExecutionConfig execution;
    SecurityConfig security;
    DownloadConfig download;
    TimeoutConfig timeouts;
    // Populated by installer/test override only; never supplied over HTTP.
    std::optional<std::filesystem::path> static_assets_root;
    // Canonicalized exact Host and Origin allowlists derived from listener + port.
    std::set<std::string> allowed_hosts;
    std::set<std::string> allowed_origins;
    // Derived canonical IPv6 flag from listen_address.
    bool ipv6{false};
};

struct WebManifest {
    std::string web_build_id;
    std::uint32_t api_major{0};
    std::uint32_t api_minor_min{0};
};

struct DeploymentConfig {
    ServerConfig server;
    DaqOpenOptions daq;
    std::chrono::nanoseconds observe_interval{std::chrono::milliseconds{500}};
};

Result<ServerConfig> parse_server_config_json(std::string_view json_text);

Result<void> validate_server_config(ServerConfig& config);

Result<DeploymentConfig> load_deployment_config(const std::filesystem::path& path);

Result<WebManifest> load_web_manifest(const std::filesystem::path& assets_root);

}  // namespace rodaq::server
