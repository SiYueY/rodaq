#include <counter.hpp>

#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace plugin = rodaq::plugin;
using namespace std::chrono_literals;

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

rodaq::Result<void> check_counter(plugin::PluginInstance& instance) {
    auto queried = instance.query<CounterInterface>();
    if (!queried.ok()) {
        return queried.status();
    }
    auto counter = std::move(queried).value();
    auto incremented = counter.increment(7);
    if (!incremented.ok()) {
        return incremented.status();
    }
    auto overflow = counter.increment(std::numeric_limits<std::int64_t>::max());
    if (overflow.ok() || overflow.status().code() != rodaq::StatusCode::InvalidArgument) {
        return rodaq::Status(rodaq::StatusCode::Internal, "Counter overflow contract failed");
    }
    auto current = counter.current();
    if (!current.ok()) {
        return current.status();
    }
    if (incremented.value() != 7 || current.value() != 7) {
        return rodaq::Status(rodaq::StatusCode::Internal, "Counter value contract failed");
    }
    return {};
}

rodaq::Result<void> exercise(plugin::PluginLibrary& library) {
    plugin::ServiceRegistry services;
    auto frozen = services.freeze();
    if (!frozen.ok()) {
        return frozen;
    }
    for (const bool asynchronous : {false, true}) {
        auto initialized = library.create_instance(services, {});
        if (!initialized.ok()) {
            return initialized.status();
        }
        auto instance = std::move(initialized).value();
        auto business = check_counter(instance);
        rodaq::Result<void> closed;
        if (asynchronous) {
            auto future = instance.async_close();
            closed =
                future.wait(soon()) == plugin::CloseWaitStatus::Completed
                    ? future.result()
                    : rodaq::Status(rodaq::StatusCode::DeadlineExceeded, "Close observer timeout");
        } else {
            closed = instance.close(soon());
        }
        if (!closed.ok()) {
            std::cerr << "Instance cleanup failed: " << closed.status().message() << '\n';
        }
        if (!business.ok()) {
            return business;
        }
        if (!closed.ok()) {
            return closed;
        }
    }
    return {};
}

rodaq::Result<void> run(std::string_view path, std::string_view id) {
    plugin::PluginManagerOptions options;
    options.search_paths = {path};
    options.max_instances = 1;
    options.max_loaded_plugins = 1;
    options.max_config_size = 1024;
    options.shutdown_timeout = 1s;
    auto created = plugin::PluginManager::create(std::move(options));
    if (!created.ok()) {
        return created.status();
    }
    auto manager = std::move(created).value();
    auto loaded = manager.load(id, "1.0.0");
    rodaq::Result<void> primary;
    if (!loaded.ok()) {
        primary = loaded.status();
    } else {
        auto repeated = manager.load(id, "1.0.0");
        if (!repeated.ok()) {
            primary = repeated.status();
        } else if (&repeated.value().info() != &loaded.value().info()) {
            primary =
                rodaq::Status(rodaq::StatusCode::Internal, "Repeated load duplicated Library");
        } else {
            primary = exercise(loaded.value());
        }
    }
    auto stopped = manager.shutdown(soon());
    if (!stopped.ok()) {
        std::cerr << "Manager cleanup failed: " << stopped.status().message() << '\n';
    }
    return primary.ok() ? stopped : primary;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    const auto result = run(argv[1], argv[2]);
    if (!result.ok()) {
        std::cerr << "Counter application failed: " << result.status().message() << '\n';
        return 1;
    }
    return 0;
}
