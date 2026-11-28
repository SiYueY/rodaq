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

constexpr std::uint32_t kApiMajor = 1;
constexpr std::uint32_t kApiMinor = 0;

struct ApiVersion {
    std::uint32_t major{kApiMajor};
    std::uint32_t minor{kApiMinor};
};

enum class ServerPhase { Starting, Ready, Draining, Failed };

std::string_view to_wire_string(ServerPhase phase) noexcept;

struct HttpConfig {
    std::string listen_address{"127.0.0.1"};
    std::uint16_t port{8080};
    std::uint32_t worker_threads{2};
    std::size_t max_request_body_bytes{4194304};
    std::size_t max_header_bytes{16384};
    std::size_t max_query_bytes{4096};
    std::size_t max_json_depth{32};
    std::size_t max_json_response_bytes{8388608};
    std::uint32_t max_page_size{200};
    std::chrono::milliseconds recommended_polling_floor{500};
};

struct ExecutionConfig {
    std::uint32_t control_workers{2};
    std::uint32_t control_queue{64};
};

struct SecurityConfig {
    std::filesystem::path bearer_token_file{"/etc/rodaq/server.token"};
    bool enforce_file_permissions{true};
};

struct DownloadConfig {
    std::uint32_t workers{2};
    std::uint32_t max_concurrent{4};
    std::uint64_t chunk_bytes{1048576};
    std::uint64_t max_bytes_in_flight{8388608};
};

struct TimeoutConfig {
    std::chrono::milliseconds query{3000};
    std::chrono::milliseconds mutation_acceptance{5000};
    std::chrono::milliseconds shutdown{30000};
};

struct ServerConfig {
    HttpConfig http;
    ExecutionConfig execution;
    SecurityConfig security;
    DownloadConfig download;
    TimeoutConfig timeouts;
    std::optional<std::filesystem::path> static_assets_root;

    // Derived during validation from canonical loopback address + port.
    bool ipv6{false};
    std::set<std::string> allowed_hosts;
    std::set<std::string> allowed_origins;
};

struct WebManifest {
    std::string web_build_id;
    std::uint32_t api_major{0};
    std::uint32_t api_minor_min{0};
};

struct DeploymentConfig {
    ServerConfig server;
    DaqOpenOptions daq;
};

// Validates ServerConfig bounds, canonicalizes loopback address, and populates
// exact allowed Host/Origin sets.
Result<void> validate_server_config(ServerConfig& config);

Result<ServerConfig> parse_server_config_json(std::string_view json_text);

Result<DeploymentConfig> load_deployment_config(const std::filesystem::path& path);

Result<WebManifest> load_web_manifest(const std::filesystem::path& assets_root);

}  // namespace rodaq::server
