#include "config.hpp"

#include "api/json_codec.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <sys/stat.h>

namespace rodaq::server {
namespace {

constexpr std::size_t kMaxJsonResponseHardCap = 64ull * 1024 * 1024;
constexpr std::uint64_t kMaxDownloadInFlightHardCap = 64ull * 1024 * 1024;

bool read_bounded_file(const std::filesystem::path& path, std::size_t max_bytes, std::string& out) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > max_bytes) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return out.size() <= max_bytes;
}

bool check_allowed_keys(
    const nlohmann::json& obj, std::initializer_list<std::string_view> allowed) {
    if (!obj.is_object()) {
        return false;
    }
    for (const auto& [k, _] : obj.items()) {
        bool ok = false;
        for (const auto a : allowed) {
            if (k == a) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

template <typename Int>
bool read_opt_int(const nlohmann::json& obj, const char* key, Int& out) {
    if (!obj.contains(key)) {
        return true;
    }
    const auto& v = obj[key];
    if (!v.is_number_integer() && !v.is_number_unsigned()) {
        return false;
    }
    if (v.is_number_integer() && v.get<std::int64_t>() < 0) {
        return false;
    }
    const auto u = v.get<std::uint64_t>();
    if (u > static_cast<std::uint64_t>(std::numeric_limits<Int>::max())) {
        return false;
    }
    out = static_cast<Int>(u);
    return true;
}

bool read_opt_ms(const nlohmann::json& obj, const char* key, std::chrono::milliseconds& out) {
    std::uint64_t ms = 0;
    if (!obj.contains(key)) {
        return true;
    }
    if (!read_opt_int(obj, key, ms) || ms == 0 ||
        ms > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000000LL)) {
        return false;
    }
    out = std::chrono::milliseconds{static_cast<std::int64_t>(ms)};
    return true;
}

bool read_opt_ns_from_ms(
    const nlohmann::json& obj, const char* key, std::chrono::nanoseconds& out) {
    std::chrono::milliseconds ms{0};
    if (!obj.contains(key)) {
        return true;
    }
    if (!read_opt_ms(obj, key, ms)) {
        return false;
    }
    out = std::chrono::duration_cast<std::chrono::nanoseconds>(ms);
    return true;
}

bool canonicalize_loopback(std::string_view input, std::string& out_addr, bool& is_ipv6) {
    std::string raw(input);
    if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']') {
        raw = raw.substr(1, raw.size() - 2);
    }
    in_addr v4{};
    if (inet_pton(AF_INET, raw.c_str(), &v4) == 1) {
        const std::uint32_t host = ntohl(v4.s_addr);
        if ((host & 0xFF000000u) != 0x7F000000u) {
            return false;
        }
        char buf[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &v4, buf, sizeof(buf))) {
            return false;
        }
        out_addr = buf;
        is_ipv6 = false;
        return true;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, raw.c_str(), &v6) == 1) {
        if (!IN6_IS_ADDR_LOOPBACK(&v6)) {
            return false;
        }
        char buf[INET6_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET6, &v6, buf, sizeof(buf))) {
            return false;
        }
        out_addr = buf;
        is_ipv6 = true;
        return true;
    }
    return false;
}

Result<ServerConfig> parse_server_object(const nlohmann::json& root) {
    if (!check_allowed_keys(
            root, {"format_version", "http", "execution", "security", "download", "timeouts",
                   "static_assets_root"})) {
        return Status(StatusCode::InvalidArgument, "unexpected key in server config");
    }
    if (!root.contains("format_version") || !root["format_version"].is_string() ||
        root["format_version"].get_ref<const std::string&>() != "1.0") {
        return Status(StatusCode::InvalidArgument, "server config requires format_version 1.0");
    }

    ServerConfig cfg;
    if (root.contains("http")) {
        const auto& h = root["http"];
        if (!check_allowed_keys(
                h, {"listen_address", "port", "worker_threads", "max_request_body_bytes",
                    "max_header_bytes", "max_query_bytes", "max_json_depth",
                    "max_json_response_bytes", "max_page_size", "recommended_polling_floor_ms"})) {
            return Status(StatusCode::InvalidArgument, "unexpected key in server.http");
        }
        if (h.contains("listen_address")) {
            if (!h["listen_address"].is_string()) {
                return Status(StatusCode::InvalidArgument, "http.listen_address must be string");
            }
            cfg.http.listen_address = h["listen_address"].get<std::string>();
        }
        if (!read_opt_int(h, "port", cfg.http.port) ||
            !read_opt_int(h, "worker_threads", cfg.http.worker_threads) ||
            !read_opt_int(h, "max_request_body_bytes", cfg.http.max_request_body_bytes) ||
            !read_opt_int(h, "max_header_bytes", cfg.http.max_header_bytes) ||
            !read_opt_int(h, "max_query_bytes", cfg.http.max_query_bytes) ||
            !read_opt_int(h, "max_json_depth", cfg.http.max_json_depth) ||
            !read_opt_int(h, "max_json_response_bytes", cfg.http.max_json_response_bytes) ||
            !read_opt_int(h, "max_page_size", cfg.http.max_page_size) ||
            !read_opt_ms(h, "recommended_polling_floor_ms", cfg.http.recommended_polling_floor)) {
            return Status(StatusCode::InvalidArgument, "invalid server.http numeric setting");
        }
    }

    if (root.contains("execution")) {
        const auto& e = root["execution"];
        if (!check_allowed_keys(e, {"control_workers", "control_queue"})) {
            return Status(StatusCode::InvalidArgument, "unexpected key in server.execution");
        }
        if (!read_opt_int(e, "control_workers", cfg.execution.control_workers) ||
            !read_opt_int(e, "control_queue", cfg.execution.control_queue)) {
            return Status(StatusCode::InvalidArgument, "invalid server.execution setting");
        }
    }

    if (root.contains("security")) {
        const auto& s = root["security"];
        if (!check_allowed_keys(s, {"bearer_token_file", "enforce_file_permissions"})) {
            return Status(StatusCode::InvalidArgument, "unexpected key in server.security");
        }
        if (s.contains("bearer_token_file")) {
            if (!s["bearer_token_file"].is_string()) {
                return Status(
                    StatusCode::InvalidArgument, "security.bearer_token_file must be string");
            }
            cfg.security.bearer_token_file = s["bearer_token_file"].get<std::string>();
        }
        if (s.contains("enforce_file_permissions")) {
            if (!s["enforce_file_permissions"].is_boolean()) {
                return Status(
                    StatusCode::InvalidArgument,
                    "security.enforce_file_permissions must be boolean");
            }
            cfg.security.enforce_file_permissions = s["enforce_file_permissions"].get<bool>();
        }
    }

    if (root.contains("download")) {
        const auto& d = root["download"];
        if (!check_allowed_keys(
                d, {"workers", "max_concurrent", "chunk_bytes", "max_bytes_in_flight"})) {
            return Status(StatusCode::InvalidArgument, "unexpected key in server.download");
        }
        if (!read_opt_int(d, "workers", cfg.download.workers) ||
            !read_opt_int(d, "max_concurrent", cfg.download.max_concurrent) ||
            !read_opt_int(d, "chunk_bytes", cfg.download.chunk_bytes) ||
            !read_opt_int(d, "max_bytes_in_flight", cfg.download.max_bytes_in_flight)) {
            return Status(StatusCode::InvalidArgument, "invalid server.download setting");
        }
    }

    if (root.contains("timeouts")) {
        const auto& t = root["timeouts"];
        if (!check_allowed_keys(t, {"query_ms", "mutation_acceptance_ms", "shutdown_ms"})) {
            return Status(StatusCode::InvalidArgument, "unexpected key in server.timeouts");
        }
        if (!read_opt_ms(t, "query_ms", cfg.timeouts.query) ||
            !read_opt_ms(t, "mutation_acceptance_ms", cfg.timeouts.mutation_acceptance) ||
            !read_opt_ms(t, "shutdown_ms", cfg.timeouts.shutdown)) {
            return Status(StatusCode::InvalidArgument, "invalid server.timeouts setting");
        }
    }

    if (root.contains("static_assets_root")) {
        if (!root["static_assets_root"].is_string()) {
            return Status(StatusCode::InvalidArgument, "static_assets_root must be string");
        }
        std::filesystem::path p(root["static_assets_root"].get<std::string>());
        if (!p.is_absolute()) {
            return Status(StatusCode::InvalidArgument, "static_assets_root must be absolute");
        }
        cfg.static_assets_root = std::move(p);
    }

    auto valid = validate_server_config(cfg);
    if (!valid.ok()) {
        return valid.status();
    }
    return cfg;
}

Result<DaqOpenOptions> parse_daq_object(const nlohmann::json& d) {
    if (!check_allowed_keys(
            d, {"state_root", "volume_root", "mode", "plugin_search_paths", "max_loaded_plugins",
                "max_plugin_instances", "max_plugin_config_bytes", "plugin_shutdown_timeout_ms",
                "deployment_generation", "qualified_runtime_profile_id", "max_tasks",
                "max_pipelines_per_task", "native_timeout_ms", "system_snapshot_max_age_ms",
                "volume", "profiles", "allowed_backends"})) {
        return Status(StatusCode::InvalidArgument, "unexpected key in daq config");
    }
    if (!d.contains("state_root") || !d["state_root"].is_string() || !d.contains("volume_root") ||
        !d["volume_root"].is_string() || !d.contains("allowed_backends") ||
        !d["allowed_backends"].is_array()) {
        return Status(
            StatusCode::InvalidArgument,
            "daq requires state_root, volume_root, and allowed_backends");
    }

    DaqOpenOptions opts;
    opts.state_root = d["state_root"].get<std::string>();
    opts.volume_root = d["volume_root"].get<std::string>();
    if (!opts.state_root.is_absolute() || !opts.volume_root.is_absolute()) {
        return Status(StatusCode::InvalidArgument, "daq roots must be absolute paths");
    }
    if (d.contains("mode")) {
        if (!d["mode"].is_string()) {
            return Status(StatusCode::InvalidArgument, "daq.mode must be string");
        }
        const auto& mode = d["mode"].get_ref<const std::string&>();
        if (mode == "initialize") {
            opts.mode = DaqOpenMode::Initialize;
        } else if (mode == "existing") {
            opts.mode = DaqOpenMode::Existing;
        } else {
            return Status(StatusCode::InvalidArgument, "daq.mode must be initialize or existing");
        }
    }
    if (d.contains("plugin_search_paths")) {
        if (!d["plugin_search_paths"].is_array()) {
            return Status(StatusCode::InvalidArgument, "daq.plugin_search_paths must be array");
        }
        for (const auto& p : d["plugin_search_paths"]) {
            if (!p.is_string()) {
                return Status(
                    StatusCode::InvalidArgument, "daq.plugin_search_paths entry must be string");
            }
            std::filesystem::path dir(p.get<std::string>());
            if (!dir.is_absolute()) {
                return Status(StatusCode::InvalidArgument, "plugin_search_paths must be absolute");
            }
            opts.plugin_search_paths.push_back(std::move(dir));
        }
    }
    if (!read_opt_int(d, "max_loaded_plugins", opts.max_loaded_plugins) ||
        !read_opt_int(d, "max_plugin_instances", opts.max_plugin_instances) ||
        !read_opt_int(d, "max_plugin_config_bytes", opts.max_plugin_config_bytes) ||
        !read_opt_ns_from_ms(d, "plugin_shutdown_timeout_ms", opts.plugin_shutdown_timeout) ||
        !read_opt_int(d, "deployment_generation", opts.deployment_generation) ||
        !read_opt_int(d, "max_tasks", opts.max_tasks) ||
        !read_opt_int(d, "max_pipelines_per_task", opts.max_pipelines_per_task) ||
        !read_opt_ns_from_ms(d, "native_timeout_ms", opts.native_timeout) ||
        !read_opt_ns_from_ms(d, "system_snapshot_max_age_ms", opts.system_snapshot_max_age)) {
        return Status(StatusCode::InvalidArgument, "invalid daq numeric setting");
    }
    if (d.contains("qualified_runtime_profile_id")) {
        if (!d["qualified_runtime_profile_id"].is_string()) {
            return Status(
                StatusCode::InvalidArgument, "daq.qualified_runtime_profile_id must be string");
        }
        opts.qualified_runtime_profile_id = d["qualified_runtime_profile_id"].get<std::string>();
    }

    if (d.contains("volume")) {
        const auto& v = d["volume"];
        if (!check_allowed_keys(
                v, {"name", "max_managed_bytes", "max_commit_bytes", "stop_free_bytes",
                    "low_free_bytes", "recovery_free_bytes", "space_sample_max_age_ms",
                    "recovery_stable_window_ms", "max_segments", "max_commit_slots",
                    "max_artifacts_per_volume", "durability_profile"})) {
            return Status(StatusCode::InvalidArgument, "unexpected key in daq.volume");
        }
        if (v.contains("name")) {
            if (!v["name"].is_string()) {
                return Status(StatusCode::InvalidArgument, "daq.volume.name must be string");
            }
            opts.volume.name = v["name"].get<std::string>();
        }
        if (v.contains("durability_profile")) {
            if (!v["durability_profile"].is_string()) {
                return Status(
                    StatusCode::InvalidArgument, "daq.volume.durability_profile must be string");
            }
            const auto& profile = v["durability_profile"].get_ref<const std::string&>();
            if (profile == "normal") {
                opts.volume.durability = DurabilityProfile::Normal;
            } else if (profile == "power_loss_hardened") {
                opts.volume.durability = DurabilityProfile::PowerLossHardened;
            } else {
                return Status(
                    StatusCode::InvalidArgument,
                    "daq.volume.durability_profile must be normal or power_loss_hardened");
            }
        }
        if (!read_opt_int(v, "max_managed_bytes", opts.volume.max_managed_bytes) ||
            !read_opt_int(v, "max_commit_bytes", opts.volume.max_commit_bytes) ||
            !read_opt_int(v, "stop_free_bytes", opts.volume.stop_free_bytes) ||
            !read_opt_int(v, "low_free_bytes", opts.volume.low_free_bytes) ||
            !read_opt_int(v, "recovery_free_bytes", opts.volume.recovery_free_bytes) ||
            !read_opt_ns_from_ms(v, "space_sample_max_age_ms", opts.volume.space_sample_max_age) ||
            !read_opt_ns_from_ms(
                v, "recovery_stable_window_ms", opts.volume.recovery_stable_window) ||
            !read_opt_int(v, "max_segments", opts.volume.max_segments) ||
            !read_opt_int(v, "max_commit_slots", opts.volume.max_commit_slots) ||
            !read_opt_int(v, "max_artifacts_per_volume", opts.volume.max_artifacts_per_volume)) {
            return Status(StatusCode::InvalidArgument, "invalid daq.volume setting");
        }
    }

    if (d.contains("profiles")) {
        if (!d["profiles"].is_array() || d["profiles"].empty()) {
            return Status(StatusCode::InvalidArgument, "daq.profiles must be non-empty array");
        }
        for (const auto& p : d["profiles"]) {
            if (!check_allowed_keys(
                    p, {"name",
                        "max_records",
                        "max_logical_bytes",
                        "payload_pool_bytes",
                        "max_payload_bytes",
                        "payload_min_chunk_bytes",
                        "payload_max_chunks",
                        "queue_capacity",
                        "callback_concurrency",
                        "max_segment_physical_bytes",
                        "segment_reservation_bytes",
                        "max_segment_records",
                        "max_segment_input_bytes",
                        "max_batch_records",
                        "max_batch_input_bytes",
                        "max_streams",
                        "max_descriptors",
                        "max_sealed_files",
                        "max_manifest_bytes",
                        "max_prepared_descriptors"})) {
                return Status(StatusCode::InvalidArgument, "unexpected key in daq profile");
            }
            if (!p.contains("name") || !p["name"].is_string()) {
                return Status(StatusCode::InvalidArgument, "profile.name is required");
            }
            DaqResourceProfile profile;
            profile.name = p["name"].get<std::string>();
            const bool has_explicit_res = p.contains("segment_reservation_bytes");
            if (!read_opt_int(p, "max_records", profile.max_records) ||
                !read_opt_int(p, "max_logical_bytes", profile.max_logical_bytes) ||
                !read_opt_int(p, "payload_pool_bytes", profile.payload_pool_bytes) ||
                !read_opt_int(p, "max_payload_bytes", profile.max_payload_bytes) ||
                !read_opt_int(p, "payload_min_chunk_bytes", profile.payload_min_chunk_bytes) ||
                !read_opt_int(p, "payload_max_chunks", profile.payload_max_chunks) ||
                !read_opt_int(p, "queue_capacity", profile.queue_capacity) ||
                !read_opt_int(p, "callback_concurrency", profile.callback_concurrency) ||
                !read_opt_int(
                    p, "max_segment_physical_bytes", profile.max_segment_physical_bytes) ||
                !read_opt_int(p, "segment_reservation_bytes", profile.segment_reservation_bytes) ||
                !read_opt_int(p, "max_segment_records", profile.max_segment_records) ||
                !read_opt_int(p, "max_segment_input_bytes", profile.max_segment_input_bytes) ||
                !read_opt_int(p, "max_batch_records", profile.max_batch_records) ||
                !read_opt_int(p, "max_batch_input_bytes", profile.max_batch_input_bytes) ||
                !read_opt_int(p, "max_streams", profile.max_streams) ||
                !read_opt_int(p, "max_descriptors", profile.max_descriptors) ||
                !read_opt_int(p, "max_sealed_files", profile.max_sealed_files) ||
                !read_opt_int(p, "max_manifest_bytes", profile.max_manifest_bytes) ||
                !read_opt_int(p, "max_prepared_descriptors", profile.max_prepared_descriptors)) {
                return Status(StatusCode::InvalidArgument, "invalid daq profile setting");
            }
            if (!has_explicit_res) {
                profile.segment_reservation_bytes =
                    profile.max_segment_physical_bytes + (profile.max_segment_physical_bytes / 4);
            }
            if (profile.max_segment_input_bytes > profile.max_segment_physical_bytes) {
                profile.max_segment_input_bytes = profile.max_segment_physical_bytes;
            }
            if (profile.max_batch_input_bytes > profile.max_segment_input_bytes) {
                profile.max_batch_input_bytes = profile.max_segment_input_bytes;
            }
            if (profile.max_payload_bytes > profile.max_batch_input_bytes) {
                profile.max_payload_bytes = profile.max_batch_input_bytes;
            }
            if (profile.max_logical_bytes > profile.payload_pool_bytes) {
                profile.max_logical_bytes = profile.payload_pool_bytes;
            }
            if (profile.max_manifest_bytes > profile.max_segment_physical_bytes / 4) {
                profile.max_manifest_bytes =
                    std::max<std::uint64_t>(4096, profile.max_segment_physical_bytes / 16);
            }
            opts.profiles.push_back(std::move(profile));
        }
    } else {
        DaqResourceProfile default_profile;
        default_profile.name = "default";
        opts.profiles.push_back(std::move(default_profile));
    }

    for (const auto& b : d["allowed_backends"]) {
        if (!check_allowed_keys(
                b, {"plugin_id", "plugin_version", "role", "storage_format", "support_status",
                    "qualification_status"})) {
            return Status(StatusCode::InvalidArgument, "unexpected key in allowed_backends");
        }
        if (!b.contains("plugin_id") || !b["plugin_id"].is_string() ||
            !b.contains("plugin_version") || !b["plugin_version"].is_string() ||
            !b.contains("role") || !b["role"].is_string()) {
            return Status(StatusCode::InvalidArgument, "missing required backend fields");
        }
        AllowedBackend backend;
        backend.plugin_id = b["plugin_id"].get<std::string>();
        backend.plugin_version = b["plugin_version"].get<std::string>();
        const auto& role = b["role"].get_ref<const std::string&>();
        if (role == "acquisition") {
            backend.role = BackendRole::Acquisition;
        } else if (role == "storage") {
            backend.role = BackendRole::Storage;
        } else {
            return Status(StatusCode::InvalidArgument, "invalid backend role");
        }
        if (b.contains("storage_format")) {
            if (!b["storage_format"].is_string()) {
                return Status(StatusCode::InvalidArgument, "storage_format must be string");
            }
            backend.storage_format = b["storage_format"].get<std::string>();
        }
        if (b.contains("support_status")) {
            if (!b["support_status"].is_string()) {
                return Status(StatusCode::InvalidArgument, "support_status must be string");
            }
            const auto& ss = b["support_status"].get_ref<const std::string&>();
            if (ss == "required") {
                backend.support_status = SupportStatus::Required;
            } else if (ss == "qualified_candidate") {
                backend.support_status = SupportStatus::QualifiedCandidate;
            } else if (ss == "deferred") {
                backend.support_status = SupportStatus::Deferred;
            } else {
                return Status(StatusCode::InvalidArgument, "invalid support_status");
            }
        }
        if (b.contains("qualification_status")) {
            if (!b["qualification_status"].is_string()) {
                return Status(StatusCode::InvalidArgument, "qualification_status must be string");
            }
            const auto& qs = b["qualification_status"].get_ref<const std::string&>();
            if (qs == "qualified") {
                backend.qualification_status = QualificationStatus::Qualified;
            } else if (qs == "not_qualified") {
                backend.qualification_status = QualificationStatus::NotQualified;
            } else {
                return Status(StatusCode::InvalidArgument, "invalid qualification_status");
            }
        }
        opts.allowed_backends.push_back(std::move(backend));
    }
    return opts;
}

}  // namespace

std::string_view to_wire_string(ServerPhase phase) noexcept {
    switch (phase) {
        case ServerPhase::Starting:
            return "starting";
        case ServerPhase::Ready:
            return "ready";
        case ServerPhase::Draining:
            return "draining";
        case ServerPhase::Failed:
            return "failed";
    }
    return "failed";
}

Result<void> validate_server_config(ServerConfig& config) {
    using namespace std::chrono_literals;
    std::string canonical_addr;
    bool ipv6 = false;
    if (!canonicalize_loopback(config.http.listen_address, canonical_addr, ipv6)) {
        return Status(StatusCode::InvalidArgument, "listen_address must be a loopback literal");
    }
    config.http.listen_address = canonical_addr;
    config.ipv6 = ipv6;

    const auto& h = config.http;
    if (h.port == 0 || h.worker_threads == 0 || h.worker_threads > 32 ||
        h.max_request_body_bytes < 65536 || h.max_request_body_bytes > 33554432 ||
        h.max_header_bytes < 4096 || h.max_header_bytes > 131072 || h.max_query_bytes < 1024 ||
        h.max_query_bytes > 65536 || h.max_json_depth < 8 || h.max_json_depth > 128 ||
        h.max_json_response_bytes < h.max_request_body_bytes ||
        h.max_json_response_bytes > kMaxJsonResponseHardCap || h.max_page_size == 0 ||
        h.max_page_size > 1000 || h.recommended_polling_floor <= 0ms) {
        return Status(StatusCode::InvalidArgument, "server.http value out of range");
    }

    const auto& e = config.execution;
    if (e.control_workers == 0 || e.control_workers > 32 || e.control_queue < 1 ||
        e.control_queue > 4096) {
        return Status(StatusCode::InvalidArgument, "server.execution value out of range");
    }

    const auto& s = config.security;
    if (s.bearer_token_file.empty() || !s.bearer_token_file.is_absolute()) {
        return Status(
            StatusCode::InvalidArgument, "server.security.bearer_token_file must be absolute");
    }

    const auto& d = config.download;
    if (d.workers == 0 || d.workers > 16 || d.max_concurrent == 0 || d.max_concurrent > 64 ||
        d.chunk_bytes < 65536 || d.chunk_bytes > 4194304 || d.max_bytes_in_flight < d.chunk_bytes ||
        d.max_bytes_in_flight > kMaxDownloadInFlightHardCap) {
        return Status(StatusCode::InvalidArgument, "server.download value out of range");
    }

    const auto& t = config.timeouts;
    const auto max_ordinary = std::max(t.query, t.mutation_acceptance);
    if (t.query < 100ms || t.query > 8000ms || t.mutation_acceptance < 100ms ||
        t.mutation_acceptance > 12000ms || t.shutdown < 5000ms || t.shutdown > 300000ms ||
        t.shutdown <= max_ordinary) {
        return Status(StatusCode::InvalidArgument, "server.timeouts value out of range");
    }

    const std::string port_str = std::to_string(h.port);
    config.allowed_hosts.clear();
    config.allowed_origins.clear();
    config.allowed_hosts.insert("localhost:" + port_str);
    config.allowed_origins.insert("http://localhost:" + port_str);
    if (ipv6) {
        const std::string bracketed = "[" + canonical_addr + "]:" + port_str;
        config.allowed_hosts.insert(bracketed);
        config.allowed_origins.insert("http://" + bracketed);
    } else {
        const std::string lit = canonical_addr + ":" + port_str;
        config.allowed_hosts.insert(lit);
        config.allowed_origins.insert("http://" + lit);
    }
    if (h.port == 80) {
        config.allowed_hosts.insert("localhost");
        config.allowed_origins.insert("http://localhost");
        if (ipv6) {
            config.allowed_hosts.insert("[" + canonical_addr + "]");
            config.allowed_origins.insert("http://[" + canonical_addr + "]");
        } else {
            config.allowed_hosts.insert(canonical_addr);
            config.allowed_origins.insert("http://" + canonical_addr);
        }
    }
    return {};
}

Result<ServerConfig> parse_server_config_json(std::string_view json_text) {
    const auto parsed = parse_bounded_json(json_text, {1048576, 32});
    if (!parsed.ok) {
        return Status(StatusCode::InvalidArgument, parsed.error.message);
    }
    return parse_server_object(parsed.value);
}

Result<DeploymentConfig> load_deployment_config(const std::filesystem::path& path) {
    if (!path.is_absolute()) {
        return Status(StatusCode::InvalidArgument, "deployment path must be absolute");
    }
    std::string raw;
    if (!read_bounded_file(path, 1048576, raw)) {
        return Status(StatusCode::NotFound, "deployment config file unreadable");
    }
    const auto parsed = parse_bounded_json(raw, {1048576, 32});
    if (!parsed.ok) {
        return Status(StatusCode::InvalidArgument, parsed.error.message);
    }
    const auto& root = parsed.value;
    if (!check_allowed_keys(root, {"format_version", "server", "daq", "static_assets_root"})) {
        return Status(StatusCode::InvalidArgument, "unexpected top-level key in deployment.json");
    }
    if (!root.contains("format_version") || !root["format_version"].is_string() ||
        root["format_version"].get_ref<const std::string&>() != "1.0" || !root.contains("server") ||
        !root.contains("daq")) {
        return Status(
            StatusCode::InvalidArgument,
            "deployment.json requires format_version 1.0, server, daq");
    }
    auto server = parse_server_object(root["server"]);
    if (!server.ok()) {
        return server.status();
    }
    auto daq = parse_daq_object(root["daq"]);
    if (!daq.ok()) {
        return daq.status();
    }
    if (root.contains("static_assets_root")) {
        if (!root["static_assets_root"].is_string()) {
            return Status(StatusCode::InvalidArgument, "static_assets_root must be string");
        }
        std::filesystem::path assets(root["static_assets_root"].get<std::string>());
        if (!assets.is_absolute()) {
            return Status(StatusCode::InvalidArgument, "static_assets_root must be absolute");
        }
        server.value().static_assets_root = std::move(assets);
    }
    DeploymentConfig result;
    result.server = std::move(server.value());
    result.daq = std::move(daq.value());
    return result;
}

Result<WebManifest> load_web_manifest(const std::filesystem::path& assets_root) {
    const auto manifest_path = assets_root / "web_manifest.json";
    std::string raw;
    if (!read_bounded_file(manifest_path, 65536, raw)) {
        return Status(StatusCode::NotFound, "web_manifest.json not found");
    }
    const auto parsed = parse_bounded_json(raw, {65536, 8});
    if (!parsed.ok) {
        return Status(StatusCode::InvalidArgument, parsed.error.message);
    }
    const auto& obj = parsed.value;
    if (!check_allowed_keys(obj, {"web_build_id", "api_major", "api_minor_min"}) ||
        !obj.contains("web_build_id") || !obj["web_build_id"].is_string() ||
        !obj.contains("api_major") || !obj.contains("api_minor_min")) {
        return Status(StatusCode::InvalidArgument, "invalid web_manifest.json schema");
    }
    WebManifest manifest;
    manifest.web_build_id = obj["web_build_id"].get<std::string>();
    if (!is_visible_ascii(manifest.web_build_id, 1, 128) ||
        !read_opt_int(obj, "api_major", manifest.api_major) ||
        !read_opt_int(obj, "api_minor_min", manifest.api_minor_min)) {
        return Status(StatusCode::InvalidArgument, "invalid web_manifest.json fields");
    }
    return manifest;
}

}  // namespace rodaq::server
