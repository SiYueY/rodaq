#ifndef RODAQ_CONFIG_YAML_CONFIG_LOADER_HPP_
#define RODAQ_CONFIG_YAML_CONFIG_LOADER_HPP_

#include <rodaq/config/config.hpp>

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace rodaq {

struct YamlConfigLimits {
    std::size_t max_file_bytes = 1024U * 1024U;
    std::size_t max_nodes = 16384;
    std::size_t max_depth = 64;
};

Result<ConfigTree> LoadYamlConfig(std::string_view input, const YamlConfigLimits& limits = {});

Result<ConfigTree> LoadYamlConfigFile(
    const std::filesystem::path& path, const YamlConfigLimits& limits = {});

}  // namespace rodaq

#endif  // RODAQ_CONFIG_YAML_CONFIG_LOADER_HPP_
