#include "service_config.hpp"

#include <cstdlib>
#include <atomic>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        return 2;
    }
    auto manager = rodaq::test::service_plugins(argv[2]);
    auto lane = rodaq::ExecutionLane::create(1, 8);
    auto control_lane = rodaq::ExecutionLane::create(1, 8);
    if (!manager.ok() || !lane.ok() || !control_lane.ok()) {
        _exit(3);
    }
    auto options =
        rodaq::test::service_options(argv[1], manager.value(), lane.value(), control_lane.value());
    const auto stage = std::strtol(argv[3], nullptr, 10);
    const auto output = static_cast<int>(std::strtol(argv[4], nullptr, 10));
    std::atomic<bool> configuring{false};
    std::atomic<bool> identity_ready{false};
    std::atomic<int> control_publications{0};
    const auto stop = [output] {
        if (write(output, "x", 1) != 1) {
            _exit(4);
        }
        for (;;) {
            pause();
        }
    };
    if (argc == 5) {
        options.materialization_faults = [stage, output](auto step) {
            if (static_cast<long>(step) == stage) {
                if (write(output, "x", 1) != 1) {
                    _exit(4);
                }
                for (;;) {
                    pause();
                }
            }
            return 0;
        };
    } else {
        options.file_faults = [&](auto domain, auto step) {
            if (!configuring) {
                return 0;
            }
            if (domain == rodaq::task::ServiceFileDomain::StreamIdentity &&
                step == rodaq::persistence::FileStep::SyncDirectory) {
                identity_ready = true;
            }
            if (identity_ready && domain == rodaq::task::ServiceFileDomain::Task &&
                step == rodaq::persistence::FileStep::Publish &&
                ++control_publications == stage + 1) {
                stop();
            }
            return 0;
        };
    }
    auto service = rodaq::TaskService::open(std::move(options));
    auto config = rodaq::test::service_config();
    if (!service.ok() || !config.ok()) {
        _exit(5);
    }
    auto result = service.value()->create_task(
        config.value(), {argc == 5 ? "crash-create" : "configure-seed"});
    if (!result.ok()) {
        _exit(7);
    }
    if (argc == 6) {
        auto changed = service.value()->get_active_config(result.value());
        if (!changed.ok()) {
            _exit(8);
        }
        changed.value().name = "crash-configured";
        changed.value().pipelines[0].streams.push_back(
            {0, "second", {}, rodaq::ReadinessPolicy::EntryReady});
        changed.value().pipelines[0].acquisition.json = R"({"streams":["sample","second"]})";
        configuring = true;
        auto accepted = service.value()->configure(
            result.value(), changed.value(), {"crash-configure"}, {true, 1});
        (void)accepted;
        // The Accepted response may precede the terminal-status worker frame.
        for (;;) {
            pause();
        }
    }
    _exit(6);  // Every configured crash boundary must have remained live.
}
