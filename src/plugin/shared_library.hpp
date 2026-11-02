#ifndef RODAQ_PLUGIN_SHARED_LIBRARY_HPP_
#define RODAQ_PLUGIN_SHARED_LIBRARY_HPP_

#include <rodaq/result.hpp>

#include <filesystem>
#include <string_view>

namespace rodaq::plugin {

class SharedLibrary final {
public:
    static Result<std::filesystem::path> canonicalize_path(const std::filesystem::path& path);

    static Result<SharedLibrary> open(const std::filesystem::path& path);

    SharedLibrary(SharedLibrary&& other) noexcept;
    SharedLibrary& operator=(SharedLibrary&& other) noexcept;

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    ~SharedLibrary() noexcept;

    Result<void*> find_symbol(std::string_view name) const;
    bool defines_symbol(const void* symbol) const noexcept;
    void retain_until_process_exit() noexcept;

    bool valid() const noexcept;
    const std::filesystem::path& canonical_path() const noexcept;

private:
    SharedLibrary(void* handle, void* load_base, std::filesystem::path canonical_path) noexcept;

    void close() noexcept;

    void* handle_;
    void* load_base_;
    std::filesystem::path canonical_path_;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_SHARED_LIBRARY_HPP_
