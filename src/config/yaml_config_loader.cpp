#include <rodaq/config/yaml_config_loader.hpp>

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rodaq {
namespace {

Error config_error(ErrorCode code, std::string message) {
    return Error(code, "config", 0, std::move(message));
}

bool parse_bool(std::string_view text, bool* output) noexcept {
    std::string lowered(text);
    for (char& character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (lowered == "true" || lowered == "yes" || lowered == "on") {
        *output = true;
        return true;
    }
    if (lowered == "false" || lowered == "no" || lowered == "off") {
        *output = false;
        return true;
    }
    return false;
}

bool looks_like_integer(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t index = (text.front() == '+' || text.front() == '-') ? 1U : 0U;
    if (index == text.size()) {
        return false;
    }
    for (; index < text.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(text[index]))) {
            return false;
        }
    }
    return true;
}

bool looks_like_float(std::string_view text) noexcept {
    return text.find('.') != std::string_view::npos || text.find('e') != std::string_view::npos ||
           text.find('E') != std::string_view::npos;
}

Result<ConfigTree> convert_node(
    const YAML::Node& node, const YamlConfigLimits& limits, std::size_t depth,
    std::size_t& node_count) {
    if (depth > limits.max_depth) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::ResourceExhausted, "YAML config exceeds maximum depth"));
    }
    if (++node_count > limits.max_nodes) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::ResourceExhausted, "YAML config exceeds maximum node count"));
    }
    if (!node || node.IsNull()) {
        return ConfigTree::Null();
    }
    if (node.IsSequence()) {
        std::vector<ConfigTree> items;
        items.reserve(node.size());
        for (const YAML::Node& item : node) {
            auto converted = convert_node(item, limits, depth + 1U, node_count);
            if (!converted.has_value()) {
                return nonstd::make_unexpected(std::move(converted.error()));
            }
            items.push_back(std::move(converted.value()));
        }
        return ConfigTree::List(std::move(items));
    }
    if (node.IsMap()) {
        std::vector<ConfigTree::MapEntry> entries;
        entries.reserve(node.size());
        for (const auto& item : node) {
            if (!item.first.IsScalar()) {
                return nonstd::make_unexpected(
                    config_error(ErrorCode::InvalidArgument, "YAML map key must be a scalar"));
            }
            auto converted = convert_node(item.second, limits, depth + 1U, node_count);
            if (!converted.has_value()) {
                return nonstd::make_unexpected(std::move(converted.error()));
            }
            entries.emplace_back(item.first.Scalar(), std::move(converted.value()));
        }
        return ConfigTree::Map(std::move(entries));
    }
    if (!node.IsScalar()) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidArgument, "YAML node kind is unsupported"));
    }

    const std::string scalar = node.Scalar();
    const std::string tag = node.Tag();
    bool bool_value = false;
    if (tag == "tag:yaml.org,2002:bool" || (tag == "?" && parse_bool(scalar, &bool_value))) {
        if (!parse_bool(scalar, &bool_value)) {
            return nonstd::make_unexpected(
                config_error(ErrorCode::InvalidArgument, "YAML boolean scalar is invalid"));
        }
        return ConfigTree::Boolean(bool_value);
    }
    if (tag == "tag:yaml.org,2002:int" || (tag == "?" && looks_like_integer(scalar))) {
        try {
            std::size_t consumed = 0;
            const long long value = std::stoll(scalar, &consumed, 0);
            if (consumed != scalar.size() || value < std::numeric_limits<std::int64_t>::min() ||
                value > std::numeric_limits<std::int64_t>::max()) {
                throw std::out_of_range("integer");
            }
            return ConfigTree::Int64(static_cast<std::int64_t>(value));
        } catch (const std::exception&) {
            return nonstd::make_unexpected(
                config_error(ErrorCode::InvalidArgument, "YAML integer scalar is invalid"));
        }
    }
    if (tag == "tag:yaml.org,2002:float" || (tag == "?" && looks_like_float(scalar))) {
        try {
            std::size_t consumed = 0;
            const double value = std::stod(scalar, &consumed);
            if (consumed != scalar.size()) {
                throw std::invalid_argument("float");
            }
            return ConfigTree::Double(value);
        } catch (const std::exception&) {
            if (tag != "?") {
                return nonstd::make_unexpected(
                    config_error(ErrorCode::InvalidArgument, "YAML float scalar is invalid"));
            }
        }
    }
    return ConfigTree::String(scalar);
}

}  // namespace

Result<ConfigTree> LoadYamlConfig(std::string_view input, const YamlConfigLimits& limits) {
    if (limits.max_file_bytes == 0 || limits.max_nodes == 0 || limits.max_depth == 0 ||
        input.size() > limits.max_file_bytes) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "config", 0, "YAML file size limit is exceeded"));
    }
    try {
        const YAML::Node root = YAML::Load(std::string(input));
        std::size_t node_count = 0;
        return convert_node(root, limits, 0, node_count);
    } catch (const YAML::Exception& error) {
        return nonstd::make_unexpected(config_error(
            ErrorCode::InvalidArgument, std::string("YAML parse failed: ") + error.what()));
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::ResourceExhausted, "YAML config exhausted memory"));
    }
}

Result<ConfigTree> LoadYamlConfigFile(
    const std::filesystem::path& path, const YamlConfigLimits& limits) {
    if (path.empty()) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::InvalidArgument, "YAML config path is empty"));
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::NotFound, "YAML config file is unavailable"));
    }
    if (size > limits.max_file_bytes) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::ResourceExhausted, "YAML config file exceeds size limit"));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::Unavailable, "failed to open YAML config file"));
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return nonstd::make_unexpected(
            config_error(ErrorCode::Unavailable, "failed to read YAML config file"));
    }
    return LoadYamlConfig(contents.str(), limits);
}

}  // namespace rodaq
