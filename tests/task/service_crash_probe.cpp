#include "service_config.hpp"

#include <cstdlib>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 5) {
        return 2;
    }
    auto manager = rodaq::test::service_plugins(argv[2]);
    auto lane = rodaq::ExecutionLane::create(1, 8);
    if (!manager.ok() || !lane.ok()) {
        _exit(3);
    }
    auto options = rodaq::test::service_options(argv[1], manager.value(), lane.value());
    const auto stage = std::strtol(argv[3], nullptr, 10);
    const auto output = static_cast<int>(std::strtol(argv[4], nullptr, 10));
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
    auto service = rodaq::TaskService::open(std::move(options));
    auto config = rodaq::test::service_config();
    if (!service.ok() || !config.ok()) {
        _exit(5);
    }
    auto result = service.value()->create_task(config.value(), {"crash-create"});
    (void)result;
    _exit(6);  // Every configured crash boundary must have remained live.
}
