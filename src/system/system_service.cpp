#include <rodaq/system/system_service.hpp>

#include <dirent.h>
#include <fstream>
#include <new>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace rodaq {

struct SystemService::State {
    SnapshotProvider snapshot;
    CapabilitiesProvider capabilities;
};

SystemService::SystemService(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

SystemService::~SystemService() noexcept = default;

Result<std::unique_ptr<SystemService>> SystemService::create(
    SnapshotProvider snapshot_provider, CapabilitiesProvider capabilities_provider) {
    if (!snapshot_provider || !capabilities_provider) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_unique<State>();
        state->snapshot = std::move(snapshot_provider);
        state->capabilities = std::move(capabilities_provider);
        return std::unique_ptr<SystemService>(new SystemService(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<SystemSnapshot> SystemService::snapshot() const {
    if (!state_ || !state_->snapshot) {
        return Status(StatusCode::InvalidState, {});
    }
    try {
        return state_->snapshot();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

Result<SystemCapabilities> SystemService::capabilities() const {
    if (!state_ || !state_->capabilities) {
        return Status(StatusCode::InvalidState, {});
    }
    try {
        return state_->capabilities();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

namespace system {

ProcessResources sample_process_resources() noexcept {
    ProcessResources res;
    res.cpu_cores = std::thread::hardware_concurrency();

    std::ifstream statm("/proc/self/statm");
    std::uint64_t size_pages = 0;
    std::uint64_t rss_pages = 0;
    if (statm >> size_pages >> rss_pages) {
        const long page_size = ::sysconf(_SC_PAGESIZE);
        if (page_size > 0) {
            res.rss_bytes = rss_pages * static_cast<std::uint64_t>(page_size);
        }
    }

    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 8, "Threads:") == 0) {
            try {
                res.threads = std::stoull(line.substr(8));
            } catch (...) {
                res.threads = 0;
            }
            break;
        }
    }

    if (DIR* dir = ::opendir("/proc/self/fd")) {
        std::uint64_t count = 0;
        while (const auto* entry = ::readdir(dir)) {
            if (entry->d_name[0] != '.') {
                ++count;
            }
        }
        (void)::closedir(dir);
        // Subtract the descriptor opened by opendir itself.
        res.open_fds = count > 0 ? count - 1 : 0;
    }

    return res;
}

}  // namespace system
}  // namespace rodaq
