#include "plugin/shared_library.hpp"

#include "plugin/plugin_error.hpp"

#include <dlfcn.h>
#include <link.h>

#include <cerrno>
#include <exception>
#include <new>
#include <string>
#include <system_error>
#include <utility>

namespace rodaq::plugin {
namespace {

Error loader_error(std::string domain, std::string message) {
    return Error(ErrorCode::Unavailable, std::move(domain), 0, std::move(message));
}

std::string current_dl_error(const char* fallback) {
    const char* error = dlerror();
    if (error == nullptr) {
        return fallback;
    }
    return std::string(error);
}

}  // namespace

Result<std::filesystem::path> SharedLibrary::canonicalize_path(const std::filesystem::path& path) {
    if (path.empty()) {
        return nonstd::make_unexpected(invalid_argument_error("Plugin library path is empty"));
    }

    try {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            return nonstd::make_unexpected(loader_error("filesystem", error.message()));
        }
        if (!exists) {
            return nonstd::make_unexpected(Error(
                ErrorCode::NotFound, "filesystem", error.value(),
                "Plugin library path does not exist: " + path.string()));
        }

        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
        if (error) {
            return nonstd::make_unexpected(loader_error("filesystem", error.message()));
        }
        if (!std::filesystem::is_regular_file(canonical, error) || error) {
            return nonstd::make_unexpected(invalid_argument_error(
                "Plugin library path is not a regular file: " + canonical.string()));
        }
        return canonical;
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(
            internal_error(std::string("Failed to canonicalize Plugin path: ") + error.what()));
    }
}

Result<SharedLibrary> SharedLibrary::open(const std::filesystem::path& path) {
    Result<std::filesystem::path> canonical_result = canonicalize_path(path);
    if (!canonical_result.has_value()) {
        return nonstd::make_unexpected(std::move(canonical_result.error()));
    }
    const std::filesystem::path canonical = std::move(canonical_result.value());

    dlerror();
    void* handle = dlopen(canonical.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return nonstd::make_unexpected(loader_error("dlopen", current_dl_error("dlopen failed")));
    }

    link_map* map = nullptr;
    dlerror();
    if (dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 || map == nullptr) {
        const std::string message = current_dl_error("dlinfo failed");
        dlclose(handle);
        return nonstd::make_unexpected(loader_error("dlinfo", message));
    }

    return SharedLibrary(handle, reinterpret_cast<void*>(map->l_addr), canonical);
}

SharedLibrary::SharedLibrary(
    void* handle, void* load_base, std::filesystem::path canonical_path) noexcept
: handle_(handle), load_base_(load_base), canonical_path_(std::move(canonical_path)) {}

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
: handle_(std::exchange(other.handle_, nullptr)),
  load_base_(std::exchange(other.load_base_, nullptr)),
  canonical_path_(std::move(other.canonical_path_)) {}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
        load_base_ = std::exchange(other.load_base_, nullptr);
        canonical_path_ = std::move(other.canonical_path_);
    }
    return *this;
}

SharedLibrary::~SharedLibrary() noexcept { close(); }

Result<void*> SharedLibrary::find_symbol(std::string_view name) const {
    if (!valid()) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidState, "dlsym", 0,
            "Cannot resolve a symbol from an invalid SharedLibrary"));
    }

    try {
        const std::string owning_name(name);
        dlerror();
        void* symbol = dlsym(handle_, owning_name.c_str());
        const char* error = dlerror();
        if (error != nullptr || symbol == nullptr) {
            return nonstd::make_unexpected(Error(
                ErrorCode::NotFound, "dlsym", 0, "Plugin symbol was not found: " + owning_name));
        }
        return symbol;
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(Error(
            ErrorCode::ResourceExhausted, "dlsym", 0,
            "Memory exhausted while resolving Plugin symbol"));
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(internal_error(
            std::string("Unexpected Plugin symbol lookup failure: ") + error.what()));
    }
}

bool SharedLibrary::defines_symbol(const void* symbol) const noexcept {
    if (!valid() || symbol == nullptr) {
        return false;
    }

    Dl_info info{};
    if (dladdr(symbol, &info) == 0 || info.dli_fbase == nullptr) {
        return false;
    }
    return info.dli_fbase == load_base_;
}

void SharedLibrary::retain_until_process_exit() noexcept {
    handle_ = nullptr;
    load_base_ = nullptr;
}

bool SharedLibrary::valid() const noexcept { return handle_ != nullptr; }

const std::filesystem::path& SharedLibrary::canonical_path() const noexcept {
    return canonical_path_;
}

void SharedLibrary::close() noexcept {
    if (handle_ != nullptr) {
        dlclose(handle_);
        handle_ = nullptr;
    }
    load_base_ = nullptr;
}

}  // namespace rodaq::plugin
