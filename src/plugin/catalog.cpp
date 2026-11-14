#include "catalog.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <rodaq/logging.hpp>
#include <rodaq/plugin/abi.hpp>

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

Result<std::vector<std::string>> version_identifiers(std::string_view text, bool prerelease) {
    std::vector<std::string> parts;
    while (!text.empty()) {
        const auto end = text.find('.');
        const auto part = text.substr(0, end);
        if (part.empty() || !std::all_of(part.begin(), part.end(), identifier_char) ||
            (prerelease && digits(part) && part.size() > 1 && part.front() == '0')) {
            return Status(StatusCode::InvalidArgument, "Invalid SemVer identifier");
        }
        parts.emplace_back(part);
        if (end == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(end + 1);
    }
    return Status(StatusCode::InvalidArgument, "Empty SemVer identifier");
}

int compare_numeric(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return left.size() < right.size() ? -1 : 1;
    }
    return left == right ? 0 : (left < right ? -1 : 1);
}

class FileDescriptor {
public:
    explicit FileDescriptor(int value) noexcept : value_(value) {}

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&&) = delete;
    FileDescriptor& operator=(FileDescriptor&&) = delete;

    ~FileDescriptor() {
        if (value_ >= 0) {
            close(value_);
        }
    }

    int get() const noexcept { return value_; }

private:
    int value_;
};

FileIdentity identity_from_stat(const struct stat& value) noexcept {
    return {
        static_cast<std::uint64_t>(value.st_dev),
        static_cast<std::uint64_t>(value.st_ino),
        static_cast<std::uint64_t>(value.st_size),
        value.st_mtim.tv_sec,
        value.st_mtim.tv_nsec,
        value.st_ctim.tv_sec,
        value.st_ctim.tv_nsec};
}

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

Result<void> not_writable(const fs::path& path) {
    if (faccessat(AT_FDCWD, path.c_str(), W_OK, AT_EACCESS) == 0) {
        return Status(StatusCode::PermissionDenied, "Writable Plugin release content");
    }
    if (errno != EACCES && errno != EROFS) {
        return Status(StatusCode::Unavailable, "Cannot verify Plugin permissions");
    }
    return {};
}

Result<void> immutable_package(const fs::path& root, const fs::path& search_root) {
    std::error_code error;
    std::size_t count = 0;
    if (!contained(search_root, root)) {
        return Status(StatusCode::PermissionDenied, "Plugin package escapes search root");
    }
    for (auto parent = root;; parent = parent.parent_path()) {
        auto immutable = not_writable(parent);
        if (!immutable.ok()) {
            return immutable.status();
        }
        if (parent == search_root) {
            break;
        }
    }
    fs::recursive_directory_iterator iterator(root, fs::directory_options::none, error);
    if (error) {
        return Status(StatusCode::PermissionDenied, "Cannot inspect Plugin release directory");
    }
    for (; iterator != fs::recursive_directory_iterator(); iterator.increment(error)) {
        if (error) {
            return Status(StatusCode::Unavailable, "Cannot scan Plugin release directory");
        }
        if (++count > kMaxPackageFiles || iterator.depth() > 16 ||
            iterator->path().native().size() > kMaxPathLength) {
            return Status(StatusCode::InvalidArgument, "Plugin package file/path limit");
        }
        const auto canonical = fs::canonical(iterator->path(), error);
        if (error || !contained(root, canonical)) {
            return Status(StatusCode::PermissionDenied, "Plugin package link escapes root");
        }
        const auto immutable = not_writable(canonical);
        if (!immutable.ok()) {
            return immutable.status();
        }
    }
    if (error) {
        return Status(StatusCode::Unavailable, "Cannot scan Plugin release directory");
    }
    return {};
}

Result<std::string> hash_artifact(
    const fs::path& path, std::uint64_t& scan_bytes, FileIdentity& identity) {
    FileDescriptor file(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    struct stat before {};
    if (file.get() < 0 || fstat(file.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0) {
        return Status(StatusCode::Unavailable, "Cannot read Plugin artifact");
    }
    if (static_cast<std::uint64_t>(before.st_size) > kMaxArtifactSize) {
        return Status(StatusCode::InvalidArgument, "Plugin artifact size limit");
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        return Status(StatusCode::ResourceExhausted, "Cannot initialize SHA-256");
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    std::uint64_t file_bytes = 0;
    const auto expected_size = static_cast<std::uint64_t>(before.st_size);
    while (file_bytes < expected_size) {
        if (scan_bytes >= kMaxScanBytes) {
            return Status(StatusCode::ResourceExhausted, "Plugin scan byte limit");
        }
        const auto capacity = std::min(
            {std::uint64_t(buffer.size()), kMaxScanBytes - scan_bytes, expected_size - file_bytes});
        const auto count = read(file.get(), buffer.data(), static_cast<std::size_t>(capacity));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return Status(StatusCode::Unavailable, "Plugin artifact read failed");
        }
        if (count == 0) {
            return Status(StatusCode::DataLoss, "Plugin artifact truncated while hashing");
        }
        scan_bytes += static_cast<std::uint64_t>(count);
        file_bytes += static_cast<std::uint64_t>(count);
        if (EVP_DigestUpdate(digest.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) {
            return Status(StatusCode::Internal, "SHA-256 update failed");
        }
    }
    struct stat after {};
    if (fstat(file.get(), &after) != 0 ||
        !same_file(identity_from_stat(before), identity_from_stat(after)) ||
        file_bytes != static_cast<std::uint64_t>(after.st_size)) {
        return Status(StatusCode::DataLoss, "Plugin artifact changed while hashing");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) != 1 || size != 32) {
        return Status(StatusCode::Internal, "SHA-256 finalization failed");
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string hex(64, '0');
    for (std::size_t i = 0; i < 32; ++i) {
        hex[i * 2] = kHex[bytes[i] >> 4];
        hex[i * 2 + 1] = kHex[bytes[i] & 15];
    }
    identity = identity_from_stat(after);
    return hex;
}

bool exact_keys(const Json& object, std::initializer_list<const char*> keys) {
    return object.is_object() && object.size() == keys.size() &&
           std::all_of(
               keys.begin(), keys.end(), [&](const char* key) { return object.contains(key); });
}

Result<CatalogEntry> read_manifest(
    const fs::path& path, const fs::path& search_root, std::uint64_t& scan_bytes) {
    FileDescriptor file(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    struct stat before {};
    if (file.get() < 0 || fstat(file.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0) {
        return Status(StatusCode::Unavailable, "Cannot open regular Plugin manifest");
    }
    if (static_cast<std::uint64_t>(before.st_size) > kMaxManifestSize) {
        return Status(StatusCode::InvalidArgument, "Plugin manifest size limit");
    }
    const auto expected_size = static_cast<std::size_t>(before.st_size);
    if (expected_size > kMaxScanBytes - scan_bytes) {
        return Status(StatusCode::ResourceExhausted, "Plugin scan byte limit");
    }
    std::array<char, kMaxManifestSize> buffer{};
    std::size_t used = 0;
    while (used < expected_size) {
        const auto count = read(file.get(), buffer.data() + used, expected_size - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return Status(StatusCode::Unavailable, "Plugin manifest read failed/truncated");
        }
        used += static_cast<std::size_t>(count);
        scan_bytes += static_cast<std::size_t>(count);
    }
    struct stat after {};
    if (fstat(file.get(), &after) != 0 ||
        !same_file(identity_from_stat(before), identity_from_stat(after))) {
        return Status(StatusCode::DataLoss, "Plugin manifest changed while reading");
    }
    std::vector<std::set<std::string>> objects;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 16) {
            throw std::invalid_argument("Manifest nesting limit");
        }
        if (event == Json::parse_event_t::object_start) {
            objects.emplace_back();
        } else if (event == Json::parse_event_t::key) {
            if (objects.empty() || !objects.back().insert(parsed.get<std::string>()).second) {
                throw std::invalid_argument("Duplicate manifest key");
            }
        } else if (event == Json::parse_event_t::object_end) {
            objects.pop_back();
        }
        return true;
    };
    const auto json = Json::parse(buffer.begin(), buffer.begin() + used, callback);
    if (!exact_keys(json, {"format_version", "plugin", "platform", "artifact"}) ||
        !json["format_version"].is_number_unsigned() || json["format_version"] != 1 ||
        !exact_keys(json["plugin"], {"id", "name", "version", "abi"}) ||
        !exact_keys(json["plugin"]["abi"], {"major", "minor"}) ||
        !exact_keys(json["platform"], {"os", "arch"}) ||
        !exact_keys(json["artifact"], {"path", "sha256"})) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin manifest fields");
    }
    const auto& plugin = json["plugin"];
    CatalogEntry entry;
    entry.info = {
        plugin.at("id").get<std::string>(), plugin.at("name").get<std::string>(),
        plugin.at("version").get<std::string>()};
    if (!abi::valid_identifier(entry.info.plugin_id) || entry.info.plugin_name.empty() ||
        entry.info.plugin_name.size() > 255 ||
        entry.info.plugin_name.find('\0') != std::string::npos ||
        abi::utf8_prefix(entry.info.plugin_name, entry.info.plugin_name.size()) !=
            entry.info.plugin_name.size()) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin metadata");
    }
    auto version = parse_version(entry.info.plugin_version);
    if (!version.ok()) {
        return version.status();
    }
    entry.version = std::move(version).value();
    const auto& requirement = plugin["abi"];
    if (!requirement["major"].is_number_unsigned() || !requirement["minor"].is_number_unsigned() ||
        requirement["major"].get<std::uint64_t>() > UINT16_MAX ||
        requirement["minor"].get<std::uint64_t>() > UINT16_MAX) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin ABI requirement");
    }
    entry.abi_major = requirement["major"].get<std::uint16_t>();
    entry.abi_minor = requirement["minor"].get<std::uint16_t>();
#if defined(__x86_64__)
    constexpr std::string_view kArchitecture = "amd64";
#elif defined(__aarch64__)
    constexpr std::string_view kArchitecture = "arm64";
#else
#error "Unsupported Plugin architecture"
#endif
    if (json["platform"]["os"] != "linux" || json["platform"]["arch"] != kArchitecture ||
        entry.abi_major != RODAQ_PLUGIN_ABI_MAJOR(RODAQ_PLUGIN_ABI_VERSION) ||
        entry.abi_minor > RODAQ_PLUGIN_ABI_MINOR(RODAQ_PLUGIN_ABI_VERSION)) {
        return Status(StatusCode::Unavailable, "Incompatible Plugin platform/ABI");
    }
    const auto relative = json["artifact"]["path"].get<std::string>();
    const fs::path artifact(relative);
    if (relative.empty() || relative.size() > kMaxPathLength ||
        relative.find('\0') != std::string::npos || relative.find("://") != std::string::npos ||
        artifact.is_absolute()) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin artifact path");
    }
    entry.root = fs::canonical(path.parent_path());
    entry.search_root = search_root;
    entry.artifact = fs::canonical(entry.root / artifact);
    if (!contained(entry.root, entry.artifact) || entry.artifact.native().size() > kMaxPathLength) {
        return Status(StatusCode::PermissionDenied, "Plugin artifact escapes package");
    }
    const auto immutable = immutable_package(entry.root, entry.search_root);
    if (!immutable.ok()) {
        return immutable.status();
    }
    entry.binary = {
        entry.info.plugin_id, entry.info.plugin_version,
        json["artifact"]["sha256"].get<std::string>()};
    if (entry.binary.sha256.size() != 64 ||
        !std::all_of(entry.binary.sha256.begin(), entry.binary.sha256.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
        })) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin artifact digest");
    }
    const auto digest = hash_artifact(entry.artifact, scan_bytes, entry.identity);
    if (!digest.ok()) {
        return digest.status();
    }
    if (digest.value() != entry.binary.sha256) {
        return Status(StatusCode::DataLoss, "Plugin artifact digest mismatch");
    }
    return entry;
}

}  // namespace

Result<PluginVersion> parse_version(std::string_view value) {
    if (value.empty() || value.size() > 128 || value.find('\0') != std::string_view::npos) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin version length");
    }
    const auto build = value.find('+');
    if (build != std::string_view::npos) {
        auto identifiers = version_identifiers(value.substr(build + 1), false);
        if (!identifiers.ok()) {
            return identifiers.status();
        }
        value = value.substr(0, build);
    }
    PluginVersion version;
    const auto prerelease = value.find('-');
    if (prerelease != std::string_view::npos) {
        auto identifiers = version_identifiers(value.substr(prerelease + 1), true);
        if (!identifiers.ok()) {
            return identifiers.status();
        }
        version.prerelease = std::move(identifiers).value();
        value = value.substr(0, prerelease);
    }
    std::array<std::string*, 3> fields{&version.major, &version.minor, &version.patch};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto end = value.find('.');
        const auto part = value.substr(0, end);
        if (!digits(part) || (part.size() > 1 && part.front() == '0') ||
            (i < 2 && end == std::string_view::npos) || (i == 2 && end != std::string_view::npos)) {
            return Status(StatusCode::InvalidArgument, "Invalid SemVer core");
        }
        fields[i]->assign(part);
        if (end != std::string_view::npos) {
            value.remove_prefix(end + 1);
        }
    }
    return version;
}

int compare_version(const PluginVersion& left, const PluginVersion& right) noexcept {
    for (const auto& pair :
         {std::pair<std::string_view, std::string_view>{left.major, right.major},
          {left.minor, right.minor},
          {left.patch, right.patch}}) {
        const auto comparison = compare_numeric(pair.first, pair.second);
        if (comparison != 0) {
            return comparison;
        }
    }
    if (left.prerelease.empty() != right.prerelease.empty()) {
        return left.prerelease.empty() ? 1 : -1;
    }
    for (std::size_t i = 0; i < std::min(left.prerelease.size(), right.prerelease.size()); ++i) {
        const auto& a = left.prerelease[i];
        const auto& b = right.prerelease[i];
        if (a == b) {
            continue;
        }
        const bool numeric_a = digits(a);
        const bool numeric_b = digits(b);
        if (numeric_a != numeric_b) {
            return numeric_a ? -1 : 1;
        }
        return numeric_a ? compare_numeric(a, b) : (a < b ? -1 : 1);
    }
    return left.prerelease.size() == right.prerelease.size()
               ? 0
               : (left.prerelease.size() < right.prerelease.size() ? -1 : 1);
}

bool same_file(const FileIdentity& left, const FileIdentity& right) noexcept {
    return left.device == right.device && left.inode == right.inode && left.size == right.size &&
           left.modified_seconds == right.modified_seconds &&
           left.modified_nanoseconds == right.modified_nanoseconds &&
           left.changed_seconds == right.changed_seconds &&
           left.changed_nanoseconds == right.changed_nanoseconds;
}

Result<FileIdentity> file_identity(const fs::path& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        return Status(StatusCode::Unavailable, "Cannot identify Plugin artifact");
    }
    return identity_from_stat(status);
}

Result<std::vector<CatalogEntry>> discover_plugins(const std::vector<fs::path>& roots) {
    if (roots.size() > kMaxPluginRoots) {
        return Status(StatusCode::ResourceExhausted, "Plugin root limit");
    }
    std::vector<CatalogEntry> catalog;
    std::uint64_t scan_bytes = 0;
    std::size_t scanned_files = 0;
    std::size_t packages = 0;
    std::size_t diagnostics = 0;
    try {
        for (const auto& requested : roots) {
            const auto root = fs::canonical(requested);
            if (!fs::is_directory(root) || root.native().size() > kMaxPathLength) {
                return Status(StatusCode::InvalidArgument, "Invalid Plugin search root");
            }
            const auto immutable = not_writable(root);
            if (!immutable.ok()) {
                return immutable.status();
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
                    entry = read_manifest(file.path(), root, scan_bytes);
                } catch (const Json::exception&) {
                    entry = Status(StatusCode::InvalidArgument, "Malformed Plugin manifest JSON");
                } catch (const std::invalid_argument&) {
                    entry = Status(StatusCode::InvalidArgument, "Duplicate/deep manifest JSON");
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
    std::optional<std::string_view> version) {
    if (!abi::valid_identifier(id) || (version && !parse_version(*version).ok())) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin identity");
    }
    std::optional<std::size_t> selected;
    bool found_id = false;
    bool conflict = false;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& entry = catalog[i];
        if (entry.info.plugin_id != id) {
            continue;
        }
        found_id = true;
        if ((version && entry.info.plugin_version != *version) ||
            (!version && !entry.version.prerelease.empty())) {
            continue;
        }
        const auto comparison =
            selected ? compare_version(entry.version, catalog[*selected].version) : 1;
        if (comparison > 0) {
            selected = i;
            conflict = false;
        } else if (comparison == 0) {
            const auto& previous = catalog[*selected];
            conflict = conflict || entry.info.plugin_version != previous.info.plugin_version ||
                       entry.binary.sha256 != previous.binary.sha256 ||
                       entry.info.plugin_name != previous.info.plugin_name;
        }
    }
    if (conflict) {
        return Status(StatusCode::Conflict, "Ambiguous Plugin version/artifact");
    }
    if (!selected) {
        return Status(
            found_id ? StatusCode::Unavailable : StatusCode::NotFound, "Plugin unavailable");
    }
    return *selected;
}

Result<FileIdentity> verify_artifact(const CatalogEntry& entry) {
    const auto immutable = immutable_package(entry.root, entry.search_root);
    if (!immutable.ok()) {
        return immutable.status();
    }
    std::uint64_t bytes = 0;
    FileIdentity identity;
    const auto digest = hash_artifact(entry.artifact, bytes, identity);
    if (!digest.ok()) {
        return digest.status();
    }
    if (digest.value() != entry.binary.sha256 || !same_file(identity, entry.identity)) {
        return Status(StatusCode::DataLoss, "Plugin artifact changed after discovery");
    }
    return identity;
}

}  // namespace rodaq::plugin
