#include "catalog.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <new>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <nlohmann/json.hpp>

#include <rodaq/logging.hpp>
#include <rodaq/plugin/abi.hpp>
#include <rodaq/plugin/version.hpp>

#include "../../internal/strict_json.hpp"

namespace rodaq::plugin {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

bool digits(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char byte) {
        return byte >= '0' && byte <= '9';
    });
}

bool identifier_char(char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '-';
}

Result<void> valid_semver_identifiers(std::string_view text, bool prerelease) {
    while (!text.empty()) {
        const auto end = text.find('.');
        const auto part = text.substr(0, end);
        if (part.empty() || !std::all_of(part.begin(), part.end(), identifier_char) ||
            (prerelease && digits(part) && part.size() > 1 && part.front() == '0')) {
            return Status(StatusCode::InvalidArgument, "Invalid SemVer identifier");
        }
        if (end == std::string_view::npos) {
            return {};
        }
        text.remove_prefix(end + 1);
    }
    return Status(StatusCode::InvalidArgument, "Empty SemVer identifier");
}

class FileDescriptor {
public:
    explicit FileDescriptor(int value) noexcept : value_(value) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    ~FileDescriptor() {
        if (value_ >= 0) {
            close(value_);
        }
    }
    int get() const noexcept { return value_; }

private:
    int value_;
};

bool contained(const fs::path& root, const fs::path& child) {
    auto current = child.begin();
    for (const auto& component : root) {
        if (current == child.end() || *current != component) {
            return false;
        }
        ++current;
    }
    return true;
}

bool exact_keys(const Json& object, std::initializer_list<const char*> keys) {
    return object.is_object() && object.size() == keys.size() &&
           std::all_of(
               keys.begin(), keys.end(), [&](const char* key) { return object.contains(key); });
}

Result<CatalogEntry> read_manifest(const fs::path& path) {
    FileDescriptor file(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    struct stat info {};
    if (file.get() < 0 || fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0) {
        return Status(StatusCode::Unavailable, "Cannot open regular Plugin manifest");
    }
    if (static_cast<std::uint64_t>(info.st_size) > kMaxManifestSize) {
        return Status(StatusCode::InvalidArgument, "Plugin manifest size limit");
    }
    const auto expected_size = static_cast<std::size_t>(info.st_size);
    std::string content(expected_size, '\0');
    std::size_t used = 0;
    while (used < expected_size) {
        const auto count = read(file.get(), content.data() + used, expected_size - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return Status(StatusCode::Unavailable, "Plugin manifest read failed/truncated");
        }
        used += static_cast<std::size_t>(count);
    }

    auto parsed = internal::parse_strict_json(content, {kMaxManifestSize, 16, 4096});
    if (!parsed.ok()) {
        return parsed.status();
    }
    const auto& json = parsed.value();
    if (!exact_keys(json, {"format_version", "plugin", "platform", "library"}) ||
        !json["format_version"].is_number_unsigned() || json["format_version"] != 1 ||
        !exact_keys(json["plugin"], {"id", "name", "version", "abi"}) ||
        !exact_keys(json["plugin"]["abi"], {"major", "minor", "revision"}) ||
        !exact_keys(json["platform"], {"os", "arch"}) || !json["library"].is_string()) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin manifest fields");
    }
    const auto& plugin = json["plugin"];
    if (!plugin["id"].is_string() || !plugin["name"].is_string() ||
        !plugin["version"].is_string() || !json["platform"]["os"].is_string() ||
        !json["platform"]["arch"].is_string()) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin metadata types");
    }

    CatalogEntry entry;
    entry.info = {
        plugin["id"].get<std::string>(), plugin["name"].get<std::string>(),
        plugin["version"].get<std::string>()};
    if (!abi::valid_identifier(entry.info.plugin_id) || entry.info.plugin_name.empty() ||
        entry.info.plugin_name.size() > 255 ||
        entry.info.plugin_name.find('\0') != std::string::npos ||
        abi::utf8_prefix(entry.info.plugin_name, entry.info.plugin_name.size()) !=
            entry.info.plugin_name.size()) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin metadata");
    }
    auto semver = parse_semver(entry.info.plugin_version);
    if (!semver.ok()) {
        return semver.status();
    }
    const auto& requirement = plugin["abi"];
    if (!requirement["major"].is_number_unsigned() || !requirement["minor"].is_number_unsigned() ||
        !requirement["revision"].is_number_unsigned() ||
        requirement["major"].get<std::uint64_t>() > UINT16_MAX ||
        requirement["minor"].get<std::uint64_t>() > UINT16_MAX ||
        requirement["revision"].get<std::uint64_t>() > UINT32_MAX) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin ABI requirement");
    }
    entry.abi_major = requirement["major"].get<std::uint16_t>();
    entry.abi_minor = requirement["minor"].get<std::uint16_t>();
    entry.abi_revision = requirement["revision"].get<std::uint32_t>();
#if defined(__x86_64__)
    constexpr std::string_view kArchitecture = "amd64";
#elif defined(__aarch64__)
    constexpr std::string_view kArchitecture = "arm64";
#else
#error "Unsupported Plugin architecture"
#endif
    if (json["platform"]["os"] != "linux" || json["platform"]["arch"] != kArchitecture ||
        entry.abi_major != RODAQ_PLUGIN_ABI_MAJOR(RODAQ_PLUGIN_ABI_VERSION) ||
        entry.abi_minor > RODAQ_PLUGIN_ABI_MINOR(RODAQ_PLUGIN_ABI_VERSION) ||
        entry.abi_revision != RODAQ_PLUGIN_ABI_REVISION) {
        return Status(StatusCode::Unavailable, "Incompatible Plugin platform/ABI");
    }

    const auto relative = json["library"].get<std::string>();
    const fs::path library_rel(relative);
    if (relative.empty() || relative.size() > kMaxPathLength ||
        relative.find('\0') != std::string::npos || relative.find("://") != std::string::npos ||
        library_rel.is_absolute()) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin library path");
    }
    entry.root = fs::canonical(path.parent_path());
    entry.library_path = fs::canonical(entry.root / library_rel);
    if (!contained(entry.root, entry.library_path) ||
        entry.library_path.native().size() > kMaxPathLength ||
        !fs::is_regular_file(entry.library_path)) {
        return Status(StatusCode::PermissionDenied, "Plugin library escapes package or missing");
    }
    return entry;
}

}  // namespace

Result<void> parse_semver(std::string_view value) {
    if (value.empty() || value.size() > 128 || value.find('\0') != std::string_view::npos) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin version length");
    }
    const auto build = value.find('+');
    if (build != std::string_view::npos) {
        auto valid = valid_semver_identifiers(value.substr(build + 1), false);
        if (!valid.ok()) {
            return valid.status();
        }
        value = value.substr(0, build);
    }
    const auto prerelease = value.find('-');
    if (prerelease != std::string_view::npos) {
        auto valid = valid_semver_identifiers(value.substr(prerelease + 1), true);
        if (!valid.ok()) {
            return valid.status();
        }
        value = value.substr(0, prerelease);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const auto end = value.find('.');
        const auto part = value.substr(0, end);
        if (!digits(part) || (part.size() > 1 && part.front() == '0') ||
            (i < 2 && end == std::string_view::npos) || (i == 2 && end != std::string_view::npos)) {
            return Status(StatusCode::InvalidArgument, "Invalid SemVer core");
        }
        if (end != std::string_view::npos) {
            value.remove_prefix(end + 1);
        }
    }
    return {};
}

Result<void> validate_plugin_version(std::string_view version) {
    try {
        return parse_semver(version);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::vector<CatalogEntry>> discover_plugins(const std::vector<fs::path>& roots) {
    if (roots.size() > kMaxPluginRoots) {
        return Status(StatusCode::ResourceExhausted, "Plugin root limit");
    }
    std::vector<CatalogEntry> catalog;
    std::size_t scanned_files = 0;
    std::size_t packages = 0;
    std::size_t diagnostics = 0;
    try {
        for (const auto& requested : roots) {
            const auto root = fs::canonical(requested);
            if (!fs::is_directory(root) || root.native().size() > kMaxPathLength) {
                return Status(StatusCode::InvalidArgument, "Invalid Plugin search root");
            }
            for (fs::recursive_directory_iterator iterator(root);
                 iterator != fs::recursive_directory_iterator(); ++iterator) {
                const auto& file = *iterator;
                if (++scanned_files > kMaxScanFiles || iterator.depth() > 16 ||
                    file.path().native().size() > kMaxPathLength) {
                    return Status(StatusCode::ResourceExhausted, "Plugin scan file/path limit");
                }
                if (file.path().filename() != "manifest.json") {
                    continue;
                }
                if (++packages > kMaxPackageEntries) {
                    return Status(StatusCode::ResourceExhausted, "Plugin package count limit");
                }
                Result<CatalogEntry> entry(Status(StatusCode::InvalidArgument, "Invalid manifest"));
                try {
                    entry = read_manifest(file.path());
                } catch (const Json::exception&) {
                    entry = Status(StatusCode::InvalidArgument, "Malformed Plugin manifest JSON");
                } catch (const fs::filesystem_error&) {
                    entry = Status(StatusCode::InvalidArgument, "Invalid Plugin package path");
                }
                if (!entry.ok()) {
                    if (entry.status().code() == StatusCode::ResourceExhausted) {
                        return entry.status();
                    }
                    if (diagnostics++ < kMaxCatalogDiagnostics) {
                        const auto message = entry.status().message();
                        logging::write(
                            logging::Level::Warning, {},
                            message.substr(
                                0, abi::utf8_prefix(message, kMaxCatalogDiagnosticBytes)));
                    }
                    continue;
                }
                const auto existing = std::find_if(
                    catalog.begin(), catalog.end(),
                    [&](const auto& old) { return old.root == entry.value().root; });
                if (existing == catalog.end()) {
                    catalog.push_back(std::move(entry).value());
                }
            }
        }
        return catalog;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "Plugin catalog allocation failed");
    } catch (const fs::filesystem_error&) {
        return Status(StatusCode::Unavailable, "Plugin catalog scan failed");
    }
}

Result<std::size_t> select_plugin(
    const std::vector<CatalogEntry>& catalog, std::string_view id,
    std::optional<std::string_view> expected_version) {
    if (!abi::valid_identifier(id) || (expected_version && !parse_semver(*expected_version).ok())) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin identity");
    }
    std::optional<std::size_t> match;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        if (catalog[i].info.plugin_id != id) {
            continue;
        }
        if (match) {
            // Single-version Catalog: at most one installed package per plugin_id.
            return Status(StatusCode::Conflict, "Duplicate installed packages for plugin_id");
        }
        match = i;
    }
    if (!match) {
        return Status(StatusCode::NotFound, "Plugin not found");
    }
    if (expected_version && catalog[*match].info.plugin_version != *expected_version) {
        return Status(StatusCode::NotFound, "Installed Plugin version does not match expected");
    }
    return *match;
}

}  // namespace rodaq::plugin
