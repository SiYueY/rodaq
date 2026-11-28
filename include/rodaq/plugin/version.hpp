#pragma once

#include <rodaq/export.hpp>

#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::plugin {

// Full exact SemVer identity, including prerelease/build. This uses the same
// validation as Manifest ingress and Loader selection; numeric parts need not fit uint64.
RODAQ_API Result<void> validate_plugin_version(std::string_view version);

}  // namespace rodaq::plugin
