#pragma once

#include <memory>
#include <optional>

#include "catalog.hpp"

#include <rodaq/c/plugin.h>

namespace rodaq::plugin {

struct LibraryState {
    PluginInfo info;
    std::filesystem::path path;
    rodaq_plugin_entry_t entry{};
    void* native_handle{nullptr};
    // Protected by the current Manager's control mutex; never reset after fault.
    bool faulted{false};
};

/** Unpublished load owner. Destruction may dlclose, so always outside control locks. */
class PreparedLibrary {
public:
    PreparedLibrary(const PreparedLibrary&) = delete;
    PreparedLibrary& operator=(const PreparedLibrary&) = delete;
    PreparedLibrary(PreparedLibrary&& other) noexcept;
    PreparedLibrary& operator=(PreparedLibrary&&) = delete;
    ~PreparedLibrary();

    const std::shared_ptr<LibraryState>& library() const noexcept { return library_; }
    // Called only at Running Manager publication; bounded and invokes no DSO code.
    Result<void> commit();

private:
    friend Result<PreparedLibrary> prepare_library(const CatalogEntry& entry);

    PreparedLibrary(
        std::shared_ptr<LibraryState> library, std::optional<std::size_t> slot) noexcept;

    std::shared_ptr<LibraryState> library_;
    void* handle_{nullptr};
    std::optional<std::size_t> slot_;
};

Result<PreparedLibrary> prepare_library(const CatalogEntry& entry);

}  // namespace rodaq::plugin
