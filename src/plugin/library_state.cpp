#include "library_state.hpp"

namespace rodaq::plugin {

PluginLibraryState::PluginLibraryState(
    void* native_handle, Manifest manifest, ArtifactDigest artifact,
    const rodaq_plugin_entry_t* entry)
: native_handle_(native_handle),
  manifest_(std::move(manifest)),
  artifact_(std::move(artifact)),
  entry_(entry),
  info_{manifest_.plugin_id, manifest_.plugin_name, manifest_.plugin_version},
  artifact_info_{manifest_.plugin_id, manifest_.plugin_version, artifact_.sha256} {}

PluginLibraryState::~PluginLibraryState() = default;

}  // namespace rodaq::plugin
