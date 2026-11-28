#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::persistence {

enum class FilePublishMode { Create, Replace };
enum class FileDisposition { NotPublished, Uncertain, Durable };
enum class FileStep {
    OpenDirectory,
    CreateTemporary,
    WriteTemporary,
    SyncFile,
    Publish,
    SyncDirectory,
    RemoveTemporary,
    Complete,
    RemoveAuthority
};

// Private test seam. Return an errno before the named syscall, or zero.
// No production callback is needed; calls execute on the owning persistence lane.
using FileFaultHook = std::function<int(FileStep)>;

struct FilePublication {
    Status status;
    FileDisposition disposition{FileDisposition::NotPublished};
    FileStep step{FileStep::OpenDirectory};
    int system_error{0};
    int cleanup_error{0};
    // Nonempty means the domain owner must reconcile/clean this temporary name.
    // Uncertain publication must be reconciled against durable domain authority.
    std::string retained_temporary;
};

FilePublication publish_file(
    const std::filesystem::path& directory, std::string_view name, std::string_view contents,
    FilePublishMode mode, std::size_t max_bytes, const FileFaultHook& faults = {},
    bool sync = true);

// Name syntax alone is not ownership proof; only an exclusive domain owner
// may clean these names in its own namespace.
bool is_temporary_file_name(std::string_view name) noexcept;

// Reconciliation must sync actual authority, not treat a successful read as
// durability proof. These reject symlinks/non-regular files like read_file.
Result<void> sync_file(
    const std::filesystem::path& directory, std::string_view name,
    const FileFaultHook& faults = {});
Result<void> sync_directory(
    const std::filesystem::path& directory, const FileFaultHook& faults = {});

Result<std::string> read_file(
    const std::filesystem::path& directory, std::string_view name, std::size_t max_bytes);

}  // namespace rodaq::persistence
