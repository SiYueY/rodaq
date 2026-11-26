#include <rodaq/system/system_service.hpp>
#include "system_service.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

#include "../persistence/file_descriptor.hpp"

namespace rodaq {
namespace {

struct CpuSample {
    std::uint64_t process_ticks{0};
    std::chrono::steady_clock::time_point sampled_at;
    bool valid{false};
};

std::optional<std::uint64_t> read_process_cpu_ticks() {
    std::ifstream file("/proc/self/stat");
    if (!file) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(file, line)) {
        return std::nullopt;
    }
    // Field 2 (comm) may contain spaces/parentheses; anchor on the last ')'.
    const auto close_paren = line.rfind(')');
    if (close_paren == std::string::npos || close_paren + 2 >= line.size()) {
        return std::nullopt;
    }
    std::string_view rest(line.data() + close_paren + 2, line.size() - (close_paren + 2));
    // After ') ', fields start at field 3 (state). utime is field 14 (index 11),
    // stime is field 15 (index 12).
    std::uint64_t utime = 0;
    std::uint64_t stime = 0;
    bool got_u = false;
    bool got_s = false;
    for (std::size_t index = 0; index <= 12; ++index) {
        while (!rest.empty() && rest.front() == ' ') {
            rest.remove_prefix(1);
        }
        const auto space = rest.find(' ');
        const auto token = rest.substr(0, space);
        if (token.empty()) {
            return std::nullopt;
        }
        if (index == 11) {
            const auto res = std::from_chars(token.data(), token.data() + token.size(), utime);
            if (res.ec != std::errc{} || res.ptr != token.data() + token.size()) {
                return std::nullopt;
            }
            got_u = true;
        } else if (index == 12) {
            const auto res = std::from_chars(token.data(), token.data() + token.size(), stime);
            if (res.ec != std::errc{} || res.ptr != token.data() + token.size()) {
                return std::nullopt;
            }
            got_s = true;
        }
        if (space == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(space + 1);
    }
    if (!got_u || !got_s) {
        return std::nullopt;
    }
    if (stime > std::numeric_limits<std::uint64_t>::max() - utime) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return utime + stime;
}

std::uint64_t read_rss_bytes() {
    std::ifstream file("/proc/self/statm");
    if (!file) {
        return 0;
    }
    std::uint64_t total_pages = 0;
    std::uint64_t resident_pages = 0;
    if (!(file >> total_pages >> resident_pages)) {
        return 0;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }
    const auto ps = static_cast<std::uint64_t>(page_size);
    if (resident_pages > std::numeric_limits<std::uint64_t>::max() / ps) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return resident_pages * ps;
}

std::uint64_t read_thread_count() {
    std::ifstream file("/proc/self/status");
    if (!file) {
        return 0;
    }
    std::string line;
    constexpr std::string_view kPrefix = "Threads:";
    while (std::getline(file, line)) {
        if (line.size() <= kPrefix.size() ||
            std::string_view(line.data(), kPrefix.size()) != kPrefix) {
            continue;
        }
        std::string_view rest(line.data() + kPrefix.size(), line.size() - kPrefix.size());
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
            rest.remove_prefix(1);
        }
        std::uint64_t count = 0;
        const auto res = std::from_chars(rest.data(), rest.data() + rest.size(), count);
        if (res.ec == std::errc{}) {
            return count;
        }
        return 0;
    }
    return 0;
}

std::uint64_t read_open_fd_count() {
    persistence::FileDescriptor dir(::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0) {
        return 0;
    }
    const auto close_dir = [](DIR* d) noexcept { (void)closedir(d); };
    std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(dir.get()), close_dir);
    if (!entries) {
        return 0;
    }
    (void)dir.release();
    std::uint64_t count = 0;
    while (const auto* entry = readdir(entries.get())) {
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        ++count;
    }
    // Exclude the directory FD opened by this measurement itself.
    return count > 0 ? count - 1 : 0;
}

ProcessResourceSnapshot sample_process_resources(CpuSample& previous) {
    ProcessResourceSnapshot result;
    const auto now = std::chrono::steady_clock::now();
    const auto ticks = read_process_cpu_ticks();
    const long clk_tck = sysconf(_SC_CLK_TCK);
    if (ticks && clk_tck > 0) {
        if (previous.valid && *ticks >= previous.process_ticks && now > previous.sampled_at) {
            const auto elapsed_s = std::chrono::duration<double>(now - previous.sampled_at).count();
            if (elapsed_s > 0.0) {
                const double cpu_seconds = static_cast<double>(*ticks - previous.process_ticks) /
                                           static_cast<double>(clk_tck);
                result.cpu_cores = cpu_seconds / elapsed_s;
            }
        }
        previous = {*ticks, now, true};
    }
    result.rss_bytes = read_rss_bytes();
    result.threads = read_thread_count();
    result.open_fds = read_open_fd_count();
    return result;
}

}  // namespace

struct SystemService::State {
    struct Published {
        SystemSnapshot snapshot;
        std::chrono::steady_clock::time_point observed_at;
    };

    SystemCapabilities capabilities;
    std::chrono::nanoseconds snapshot_max_age{std::chrono::seconds{2}};
    // Serializes maintenance refresh() only; queries never take this lock.
    std::mutex refresh_mutex;
    CpuSample last_cpu;
    std::uint64_t next_sequence{1};
    std::shared_ptr<const Published> published;
};

SystemService::SystemService(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

SystemService::~SystemService() noexcept = default;

Result<std::unique_ptr<SystemService>> SystemService::create(system::SystemServiceOptions options) {
    if (options.capabilities.application_version.empty() ||
        options.snapshot_max_age <= std::chrono::nanoseconds::zero() ||
        options.snapshot_max_age > std::chrono::hours{24}) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_shared<State>();
        state->capabilities = std::move(options.capabilities);
        state->snapshot_max_age = options.snapshot_max_age;
        auto initial = std::make_shared<State::Published>();
        initial->snapshot.freshness = SnapshotFreshness::NotObserved;
        initial->observed_at = std::chrono::steady_clock::now();
        std::atomic_store(
            &state->published, std::shared_ptr<const State::Published>(std::move(initial)));
        return std::unique_ptr<SystemService>(new SystemService(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> SystemService::refresh(const system::SystemObservation& observation) {
    try {
        std::lock_guard<std::mutex> lock(state_->refresh_mutex);
        if (state_->next_sequence == std::numeric_limits<std::uint64_t>::max()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        auto next = std::make_shared<State::Published>();
        next->snapshot.core_ready = observation.core_ready;
        next->snapshot.recovery = observation.recovery;
        next->snapshot.resources = sample_process_resources(state_->last_cpu);
        next->snapshot.volumes = observation.volumes;
        next->snapshot.backlog = observation.backlog;
        next->snapshot.captured_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        next->snapshot.snapshot_sequence = state_->next_sequence++;
        next->snapshot.freshness = SnapshotFreshness::Fresh;
        next->snapshot.age = std::chrono::nanoseconds::zero();
        next->observed_at = std::chrono::steady_clock::now();
        std::atomic_store(
            &state_->published, std::shared_ptr<const State::Published>(std::move(next)));
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<SystemSnapshot> SystemService::snapshot() const {
    try {
        const auto current = std::atomic_load(&state_->published);
        auto result = current->snapshot;
        if (result.freshness != SnapshotFreshness::NotObserved) {
            result.age = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - current->observed_at);
            if (result.age < std::chrono::nanoseconds::zero()) {
                result.age = std::chrono::nanoseconds::zero();
            }
            if (result.freshness == SnapshotFreshness::Fresh &&
                result.age > state_->snapshot_max_age) {
                result.freshness = SnapshotFreshness::Stale;
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<SystemCapabilities> SystemService::capabilities() const {
    try {
        return state_->capabilities;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
