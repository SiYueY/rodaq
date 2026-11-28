#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>

#include <cassert>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;
namespace plugin = rodaq::plugin;

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::filesystem::path control = argv[2];
    plugin::PluginManagerOptions options;
    options.search_paths = {argv[1]};
    options.max_instances = 1;
    options.max_loaded_plugins = 2;
    options.max_config_size = 1024;
    options.shutdown_timeout = 1s;
    auto created = plugin::PluginManager::create(std::move(options));
    assert(created.ok());
    auto manager = std::move(created).value();
    auto healthy_library = manager.load("com.example.lifecycle", "1.0.0");
    assert(healthy_library.ok());
    auto initialized = healthy_library.value().create_instance({});
    assert(initialized.ok());
    auto healthy = std::move(initialized).value();
    auto loading = std::async(
        std::launch::async, [&] { return manager.load("com.example.counter", "1.0.0"); });
    const auto entered_limit = std::chrono::steady_clock::now() + 2s;
    while (!std::filesystem::exists(control / "entered") &&
           std::chrono::steady_clock::now() < entered_limit) {
        std::this_thread::sleep_for(1ms);
    }
    assert(std::filesystem::exists(control / "entered"));
    const auto limit = std::chrono::steady_clock::now() + 35s;
    while (!manager.restart_required() && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(10ms);
    }
    assert(manager.restart_required());
    assert(loading.wait_for(2s) == std::future_status::ready);
    const auto late = loading.get();
    assert(!late.ok() && late.status().code() == rodaq::StatusCode::DeadlineExceeded);
    assert(healthy.close(plugin::Deadline::after(2s).value()).ok());
    assert(!std::filesystem::exists(control / "unloaded"));
    const auto gate = open((control / "gate").c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    assert(gate >= 0);
    const char byte = 'x';
    assert(write(gate, &byte, 1) == 1);
    close(gate);
    const auto rollback_limit = std::chrono::steady_clock::now() + 2s;
    while (!std::filesystem::exists(control / "unloaded") &&
           std::chrono::steady_clock::now() < rollback_limit) {
        std::this_thread::sleep_for(1ms);
    }
    assert(std::filesystem::exists(control / "unloaded"));
    assert(manager.restart_required());
    assert(
        manager.shutdown(plugin::Deadline::after(2s).value()).status().code() ==
        rodaq::StatusCode::DeadlineExceeded);
}
