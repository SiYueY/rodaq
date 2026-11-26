#include "static_assets.hpp"

#include <algorithm>
#include <sys/stat.h>

namespace rodaq::server {
namespace {

std::string mime_for_extension(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    if (ext == ".html") {
        return "text/html; charset=utf-8";
    }
    if (ext == ".js" || ext == ".mjs") {
        return "application/javascript; charset=utf-8";
    }
    if (ext == ".css") {
        return "text/css; charset=utf-8";
    }
    if (ext == ".json") {
        return "application/json; charset=utf-8";
    }
    if (ext == ".svg") {
        return "image/svg+xml";
    }
    if (ext == ".png") {
        return "image/png";
    }
    if (ext == ".ico") {
        return "image/x-icon";
    }
    if (ext == ".woff2") {
        return "font/woff2";
    }
    return "application/octet-stream";
}

bool is_under_root(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto r_it = root.begin();
    auto c_it = candidate.begin();
    for (; r_it != root.end(); ++r_it, ++c_it) {
        if (c_it == candidate.end() || *r_it != *c_it) {
            return false;
        }
    }
    return true;
}

bool is_regular_file_nofollow(const std::filesystem::path& p) noexcept {
    struct stat st {};
    return ::lstat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool has_path_traversal(std::string_view req_path) noexcept {
    if (req_path.find("..") != std::string_view::npos ||
        req_path.find('\\') != std::string_view::npos ||
        req_path.find('\0') != std::string_view::npos) {
        return true;
    }
    return false;
}

bool is_hashed_asset(std::string_view req_path) noexcept {
    return req_path.rfind("/assets/", 0) == 0;
}

}  // namespace

StaticAssetService StaticAssetService::create(
    const std::optional<std::filesystem::path>& assets_root, std::uint32_t server_api_major) {
    StaticAssetService svc;
    if (!assets_root) {
        return svc;
    }
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(*assets_root, ec);
    if (ec || !std::filesystem::is_directory(canonical, ec)) {
        svc.deployment_error_ = "static_assets_root_missing";
        return svc;
    }
    auto manifest = load_web_manifest(canonical);
    if (!manifest.ok()) {
        svc.deployment_error_ = "web_manifest_missing_or_invalid";
        return svc;
    }
    if (manifest.value().api_major != server_api_major) {
        svc.deployment_error_ = "web_bundle_api_major_mismatch";
        return svc;
    }
    if (!is_regular_file_nofollow(canonical / "index.html")) {
        svc.deployment_error_ = "web_index_html_missing";
        return svc;
    }
    svc.canonical_root_ = canonical;
    svc.manifest_ = std::move(manifest.value());
    svc.usable_ = true;
    return svc;
}

StaticAssetResolution StaticAssetService::resolve(std::string_view request_path) const {
    if (!usable_) {
        return {};
    }
    // Never SPA-fallback any /api or /api/* route.
    if (request_path == "/api" || request_path.rfind("/api/", 0) == 0) {
        return {};
    }
    if (request_path.empty() || request_path.front() != '/' || has_path_traversal(request_path)) {
        return {};
    }
    std::string rel = std::string(request_path.substr(1));
    if (rel.empty()) {
        rel = "index.html";
    }
    std::error_code ec;
    const auto candidate = std::filesystem::weakly_canonical(canonical_root_ / rel, ec);
    if (!ec && is_under_root(canonical_root_, candidate) && is_regular_file_nofollow(candidate)) {
        const bool is_index = (candidate == canonical_root_ / "index.html");
        const std::string cache = (!is_index && is_hashed_asset(request_path))
                                      ? "public, max-age=31536000, immutable"
                                      : "no-cache";
        return {true, candidate, mime_for_extension(candidate), cache, false};
    }
    // Do not SPA-fallback explicit asset file requests (e.g. /assets/missing.js).
    if (is_hashed_asset(request_path)) {
        return {};
    }
    const auto index_path = canonical_root_ / "index.html";
    if (!is_regular_file_nofollow(index_path)) {
        return {};
    }
    return {true, index_path, "text/html; charset=utf-8", "no-cache", true};
}

}  // namespace rodaq::server
