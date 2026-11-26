#pragma once

#include "config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::server {

struct StaticAssetResolution {
    bool found{false};
    std::filesystem::path file_path;
    std::string content_type;
    std::string cache_control;
    bool is_spa_fallback{false};
};

// Serves immutable built Vue static assets from a validated directory root.
// Verifies web_manifest.json API major compatibility on startup; never falls
// back /api/* routes to index.html and rejects any path traversal.
class StaticAssetService {
public:
    static StaticAssetService create(
        const std::optional<std::filesystem::path>& assets_root, std::uint32_t server_api_major);

    bool usable() const noexcept { return usable_; }

    const std::optional<std::string>& deployment_error() const noexcept {
        return deployment_error_;
    }

    const std::optional<WebManifest>& manifest() const noexcept { return manifest_; }

    StaticAssetResolution resolve(std::string_view request_path) const;

private:
    std::filesystem::path canonical_root_;
    std::optional<WebManifest> manifest_;
    std::optional<std::string> deployment_error_;
    bool usable_{false};
};

}  // namespace rodaq::server
