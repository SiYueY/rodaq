#include "manifest.hpp"

#include "artifact.hpp"
#include "json.hpp"
#include "semver.hpp"

#include <rodaq/status.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace rodaq::plugin {
namespace {

Result<const JsonValue*> required_member(const JsonValue& object, std::string_view name) {
    const auto* value = object.find(name);
    if (value == nullptr) {
        return Status{StatusCode::InvalidArgument, "manifest is missing a required field"};
    }
    return value;
}

void reject_unknown_members(
    const JsonValue& object, std::initializer_list<std::string_view> allowed) {
    for (const auto& member : object.as_object()) {
        bool found = false;
        for (const auto name : allowed) {
            if (member.first == name) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw Status{StatusCode::InvalidArgument, "manifest contains an unknown field"};
        }
    }
}

Result<std::uint64_t> unsigned_integer(const JsonValue& value, std::uint64_t maximum) {
    if (!value.is_number()) {
        return Status{StatusCode::InvalidArgument, "manifest integer field has invalid type"};
    }
    const double number = value.as_number();
    if (!std::isfinite(number) || number < 0.0 || number > static_cast<double>(maximum) ||
        std::floor(number) != number) {
        return Status{StatusCode::InvalidArgument, "manifest integer field is out of range"};
    }
    return static_cast<std::uint64_t>(number);
}

Result<std::string> string_member(const JsonValue& value) {
    if (!value.is_string()) {
        return Status{StatusCode::InvalidArgument, "manifest string field has invalid type"};
    }
    return value.as_string();
}

}  // namespace

bool valid_plugin_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 255u) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
              character == '.' || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_plugin_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 255u && value.find('\0') == std::string_view::npos &&
           valid_utf8(value);
}

Result<Manifest> parse_manifest(std::string_view text, ManifestLimits limits) {
    if (text.size() > limits.max_manifest_size) {
        return Status{StatusCode::ResourceExhausted, "manifest exceeds size limit"};
    }

    auto root_result = parse_strict_json(text, limits.max_depth);
    if (!root_result.ok()) {
        return root_result.status();
    }
    const auto& root = root_result.value();
    if (!root.is_object()) {
        return Status{StatusCode::InvalidArgument, "manifest root must be an object"};
    }

    try {
        reject_unknown_members(root, {"format_version", "plugin", "platform", "artifact"});

        auto format_result = required_member(root, "format_version");
        auto plugin_result = required_member(root, "plugin");
        auto platform_result = required_member(root, "platform");
        auto artifact_result = required_member(root, "artifact");
        if (!format_result.ok()) {
            return format_result.status();
        }
        if (!plugin_result.ok()) {
            return plugin_result.status();
        }
        if (!platform_result.ok()) {
            return platform_result.status();
        }
        if (!artifact_result.ok()) {
            return artifact_result.status();
        }

        auto format = unsigned_integer(*format_result.value(), 1u);
        if (!format.ok() || format.value() != 1u) {
            return Status{StatusCode::InvalidArgument, "unsupported manifest format version"};
        }

        const auto& plugin = *plugin_result.value();
        const auto& platform = *platform_result.value();
        const auto& artifact = *artifact_result.value();
        if (!plugin.is_object() || !platform.is_object() || !artifact.is_object()) {
            return Status{StatusCode::InvalidArgument, "manifest sections must be objects"};
        }
        reject_unknown_members(plugin, {"id", "name", "version", "abi"});
        reject_unknown_members(platform, {"os", "arch"});
        reject_unknown_members(artifact, {"path", "sha256"});

        auto id = required_member(plugin, "id");
        auto name = required_member(plugin, "name");
        auto version = required_member(plugin, "version");
        auto abi = required_member(plugin, "abi");
        auto os = required_member(platform, "os");
        auto arch = required_member(platform, "arch");
        auto path = required_member(artifact, "path");
        auto digest = required_member(artifact, "sha256");
        if (!id.ok() || !name.ok() || !version.ok() || !abi.ok() || !os.ok() || !arch.ok() ||
            !path.ok() || !digest.ok()) {
            return Status{StatusCode::InvalidArgument, "manifest is missing required fields"};
        }
        if (!abi.value()->is_object()) {
            return Status{StatusCode::InvalidArgument, "manifest ABI must be an object"};
        }
        reject_unknown_members(*abi.value(), {"major", "minor"});
        auto abi_major = required_member(*abi.value(), "major");
        auto abi_minor = required_member(*abi.value(), "minor");
        if (!abi_major.ok() || !abi_minor.ok()) {
            return Status{StatusCode::InvalidArgument, "manifest ABI is incomplete"};
        }

        auto id_text = string_member(*id.value());
        auto name_text = string_member(*name.value());
        auto version_text = string_member(*version.value());
        auto os_text = string_member(*os.value());
        auto arch_text = string_member(*arch.value());
        auto path_text = string_member(*path.value());
        auto digest_text = string_member(*digest.value());
        if (!id_text.ok() || !name_text.ok() || !version_text.ok() || !os_text.ok() ||
            !arch_text.ok() || !path_text.ok() || !digest_text.ok()) {
            return Status{StatusCode::InvalidArgument, "manifest string field has invalid type"};
        }

        auto major =
            unsigned_integer(*abi_major.value(), std::numeric_limits<std::uint16_t>::max());
        auto minor =
            unsigned_integer(*abi_minor.value(), std::numeric_limits<std::uint16_t>::max());
        if (!major.ok() || !minor.ok()) {
            return Status{StatusCode::InvalidArgument, "manifest ABI number is out of range"};
        }
        auto semver = parse_semver(version_text.value());
        if (!semver.ok()) {
            return semver.status();
        }
        if (!valid_plugin_id(id_text.value()) || !valid_plugin_name(name_text.value()) ||
            os_text.value() != "linux" ||
            (arch_text.value() != "amd64" && arch_text.value() != "arm64") ||
            !valid_sha256(digest_text.value())) {
            return Status{StatusCode::InvalidArgument, "manifest metadata is invalid"};
        }

        const auto package_path = std::filesystem::path{path_text.value()};
        if (package_path.empty() || package_path.is_absolute()) {
            return Status{StatusCode::InvalidArgument, "manifest artifact path is invalid"};
        }
        for (const auto& component : package_path) {
            if (component == "..") {
                return Status{StatusCode::InvalidArgument, "manifest artifact path escapes root"};
            }
        }

        return Manifest{
            1u,
            std::move(id_text).value(),
            std::move(name_text).value(),
            std::move(version_text).value(),
            static_cast<std::uint16_t>(major.value()),
            static_cast<std::uint16_t>(minor.value()),
            std::move(os_text).value(),
            std::move(arch_text).value(),
            package_path.generic_string(),
            std::move(digest_text).value()};
    } catch (const Status& status) {
        return status;
    }
}

Result<Manifest> load_manifest(const std::filesystem::path& path, ManifestLimits limits) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return Status{StatusCode::Unavailable, "manifest cannot be opened"};
    }
    std::string content;
    content.resize(static_cast<std::size_t>(limits.max_manifest_size) + 1u);
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (stream.bad()) {
        return Status{StatusCode::Unavailable, "manifest read failed"};
    }
    content.resize(static_cast<std::size_t>(stream.gcount()));
    return parse_manifest(content, limits);
}

}  // namespace rodaq::plugin
