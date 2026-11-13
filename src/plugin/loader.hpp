#pragma once

#include "catalog.hpp"
#include "library_state.hpp"

#include <rodaq/result.hpp>

#include <memory>

namespace rodaq::plugin {

inline constexpr std::uint16_t kSupportedPluginAbiMajor = 1u;
inline constexpr std::uint16_t kSupportedPluginAbiMinor = 0u;

[[nodiscard]] Result<std::shared_ptr<PluginLibraryState>> load_plugin_library(
    const CatalogEntry& entry);

}  // namespace rodaq::plugin
