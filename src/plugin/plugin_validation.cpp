#include "plugin/plugin_validation.hpp"

#include <rodaq/plugin/abi_support.hpp>
#include "plugin/plugin_error.hpp"

#include <array>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace rodaq::plugin {
namespace {

bool is_ascii_lower_or_digit(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

bool is_id_segment(std::string_view segment) noexcept {
    if (segment.empty()) {
        return false;
    }
    for (const char value : segment) {
        if (!is_ascii_lower_or_digit(value) && value != '_' && value != '-') {
            return false;
        }
    }
    return true;
}

bool is_identifier_char(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-';
}

bool is_ascii_digits(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

bool is_valid_core_identifier(std::string_view value) noexcept {
    return is_ascii_digits(value) && (value.size() == 1 || value[0] != '0');
}

bool is_valid_dot_identifier(std::string_view value, bool reject_numeric_leading_zero) noexcept {
    if (value.empty()) {
        return false;
    }

    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t dot = value.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? value.size() : dot;
        const std::string_view identifier = value.substr(start, end - start);
        if (identifier.empty()) {
            return false;
        }

        bool numeric = true;
        for (const char character : identifier) {
            if (!is_identifier_char(character)) {
                return false;
            }
            if (character < '0' || character > '9') {
                numeric = false;
            }
        }
        if (reject_numeric_leading_zero && numeric && identifier.size() > 1 &&
            identifier[0] == '0') {
            return false;
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return true;
}

Result<std::string> copy_text(
    rodaq_string_view value, std::string_view field_name, bool allow_empty) {
    if (value.size > 0 && value.data == nullptr) {
        return nonstd::make_unexpected(invalid_argument_error(
            "Plugin descriptor " + std::string(field_name) + " has null data"));
    }
    if (value.size == 0) {
        if (!allow_empty) {
            return nonstd::make_unexpected(invalid_argument_error(
                "Plugin descriptor " + std::string(field_name) + " is empty"));
        }
        return std::string();
    }

    const std::string_view text(value.data, static_cast<std::size_t>(value.size));
    if (!is_valid_utf8_without_nul(text)) {
        return nonstd::make_unexpected(invalid_argument_error(
            "Plugin descriptor " + std::string(field_name) + " is not valid UTF-8 without NUL"));
    }
    return std::string(text);
}

Result<void> validate_required_callbacks(const rodaq_acquisition_plugin& protocol) {
    if (protocol.descriptor == nullptr || protocol.create == nullptr ||
        protocol.destroy == nullptr || protocol.bind == nullptr || protocol.configure == nullptr ||
        protocol.start == nullptr || protocol.stop == nullptr ||
        protocol.query_interface == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Acquisition family protocol has a null required slot"));
    }
    return {};
}

Result<void> validate_required_callbacks(const rodaq_storage_plugin& protocol) {
    if (protocol.descriptor == nullptr || protocol.create == nullptr ||
        protocol.destroy == nullptr || protocol.bind == nullptr || protocol.configure == nullptr ||
        protocol.open == nullptr || protocol.submit == nullptr || protocol.flush == nullptr ||
        protocol.close == nullptr || protocol.query_interface == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Storage family protocol has a null required slot"));
    }
    return {};
}

}  // namespace

bool is_valid_plugin_id(std::string_view value) noexcept {
    if (value.empty() || value.front() == '.' || value.back() == '.') {
        return false;
    }

    std::size_t segment_count = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t dot = value.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? value.size() : dot;
        if (!is_id_segment(value.substr(start, end - start))) {
            return false;
        }
        ++segment_count;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return segment_count >= 2;
}

bool is_valid_semantic_version(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }

    const std::size_t build_separator = value.find('+');
    const std::string_view without_build = value.substr(0, build_separator);
    if (build_separator != std::string_view::npos) {
        const std::string_view build = value.substr(build_separator + 1);
        if (build.find('+') != std::string_view::npos || !is_valid_dot_identifier(build, false)) {
            return false;
        }
    }

    const std::size_t prerelease_separator = without_build.find('-');
    const std::string_view core = without_build.substr(0, prerelease_separator);
    if (prerelease_separator != std::string_view::npos) {
        const std::string_view prerelease = without_build.substr(prerelease_separator + 1);
        if (!is_valid_dot_identifier(prerelease, true)) {
            return false;
        }
    }

    const std::size_t first_dot = core.find('.');
    const std::size_t second_dot = first_dot == std::string_view::npos
                                       ? std::string_view::npos
                                       : core.find('.', first_dot + 1);
    if (first_dot == std::string_view::npos || second_dot == std::string_view::npos ||
        core.find('.', second_dot + 1) != std::string_view::npos) {
        return false;
    }

    return is_valid_core_identifier(core.substr(0, first_dot)) &&
           is_valid_core_identifier(core.substr(first_dot + 1, second_dot - first_dot - 1)) &&
           is_valid_core_identifier(core.substr(second_dot + 1));
}

bool is_valid_utf8_without_nul(std::string_view value) noexcept {
    return rodaq::is_valid_utf8_without_nul(value);
}

Result<PluginMetadata> validate_plugin_descriptor(const rodaq_plugin_descriptor& descriptor) {
    if (descriptor.struct_size < RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE) {
        return nonstd::make_unexpected(
            invalid_argument_error("Plugin descriptor struct_size is below the V1 minimum"));
    }
    if (descriptor.reserved != 0) {
        return nonstd::make_unexpected(
            invalid_argument_error("Plugin descriptor reserved field is non-zero"));
    }

    try {
        Result<std::string> id = copy_text(descriptor.id, "id", false);
        if (!id.has_value()) {
            return nonstd::make_unexpected(std::move(id.error()));
        }
        Result<std::string> name = copy_text(descriptor.name, "name", false);
        if (!name.has_value()) {
            return nonstd::make_unexpected(std::move(name.error()));
        }
        Result<std::string> version = copy_text(descriptor.version, "version", false);
        if (!version.has_value()) {
            return nonstd::make_unexpected(std::move(version.error()));
        }
        Result<std::string> description = copy_text(descriptor.description, "description", true);
        if (!description.has_value()) {
            return nonstd::make_unexpected(std::move(description.error()));
        }

        if (!is_valid_plugin_id(id.value())) {
            return nonstd::make_unexpected(
                invalid_argument_error("Plugin ID does not match the V1 identifier grammar"));
        }
        if (!is_valid_semantic_version(version.value())) {
            return nonstd::make_unexpected(
                invalid_argument_error("Plugin version is not valid SemVer 2.0"));
        }

        return PluginMetadata{
            std::move(id.value()),
            std::move(name.value()),
            std::move(version.value()),
            std::move(description.value()),
        };
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(Error(
            ErrorCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while validating Plugin descriptor"));
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(internal_error(
            std::string("Unexpected Plugin descriptor validation failure: ") + error.what()));
    }
}

Result<void> validate_acquisition_protocol(const rodaq_acquisition_plugin& protocol) {
    if (protocol.protocol_version != RODAQ_ACQUISITION_PROTOCOL_VERSION) {
        return nonstd::make_unexpected(
            invalid_argument_error("Unsupported Acquisition family protocol version"));
    }
    if (protocol.struct_size < RODAQ_ACQUISITION_PLUGIN_MIN_STRUCT_SIZE) {
        return nonstd::make_unexpected(
            invalid_argument_error("Acquisition family struct_size is below the V1 minimum"));
    }
    return validate_required_callbacks(protocol);
}

Result<void> validate_storage_protocol(const rodaq_storage_plugin& protocol) {
    if (protocol.protocol_version != RODAQ_STORAGE_PROTOCOL_VERSION) {
        return nonstd::make_unexpected(
            invalid_argument_error("Unsupported Storage family protocol version"));
    }
    if (protocol.struct_size < RODAQ_STORAGE_PLUGIN_MIN_STRUCT_SIZE) {
        return nonstd::make_unexpected(
            invalid_argument_error("Storage family struct_size is below the V1 minimum"));
    }
    return validate_required_callbacks(protocol);
}

Result<rodaq_interface> validate_acquisition_control_interface(rodaq_interface interface) {
    if (interface.context == nullptr || interface.vtable == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Acquisition control interface is incomplete"));
    }
    const auto* control = static_cast<const rodaq_acquisition_control_v1*>(interface.vtable);
    if (control->protocol_version != RODAQ_ACQUISITION_CONTROL_PROTOCOL_VERSION) {
        return nonstd::make_unexpected(invalid_argument_error(
            "Acquisition control interface protocol version is unsupported"));
    }
    if (control->struct_size < RODAQ_ACQUISITION_CONTROL_V1_MIN_STRUCT_SIZE) {
        return nonstd::make_unexpected(invalid_argument_error(
            "Acquisition control interface struct_size is below the V1 minimum"));
    }
    if (control->prepare_binding_switch == nullptr || control->commit_binding_switch == nullptr ||
        control->abort_binding_switch == nullptr || control->quiesce_stream == nullptr ||
        control->quiesce_plugin == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Acquisition control interface has a null required slot"));
    }
    return interface;
}

Result<rodaq_interface> validate_storage_control_interface(rodaq_interface interface) {
    if (interface.context == nullptr || interface.vtable == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Storage control interface is incomplete"));
    }
    const auto* control = static_cast<const rodaq_storage_control_v1*>(interface.vtable);
    if (control->protocol_version != RODAQ_STORAGE_CONTROL_PROTOCOL_VERSION) {
        return nonstd::make_unexpected(
            invalid_argument_error("Storage control interface protocol version is unsupported"));
    }
    if (control->struct_size < RODAQ_STORAGE_CONTROL_V1_MIN_STRUCT_SIZE) {
        return nonstd::make_unexpected(invalid_argument_error(
            "Storage control interface struct_size is below the V1 minimum"));
    }
    if (control->get_capabilities == nullptr || control->ensure_descriptor_ready == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Storage control interface has a null required slot"));
    }
    return interface;
}

}  // namespace rodaq::plugin
