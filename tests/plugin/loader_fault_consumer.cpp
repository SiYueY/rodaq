#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Expected 2 arguments\n";
        return 2;
    }
    rodaq::plugin::PluginManagerOptions options;
    options.search_paths = {argv[1]};
    options.max_instances = 1;
    options.max_loaded_plugins = 1;
    options.max_config_size = 1024;
    options.shutdown_timeout = 1s;
    auto created = rodaq::plugin::PluginManager::create(std::move(options));
    if (!created.ok()) {
        std::cerr << "PluginManager::create failed\n";
        return 3;
    }
    auto manager = std::move(created).value();
    const auto loaded = manager.load("com.example.counter", "1.0.0");
    if (loaded.ok()) {
        std::cerr << "Expected load failure\n";
        return 4;
    }
    const auto expected = static_cast<rodaq::StatusCode>(std::atoi(argv[2]));
    if (loaded.status().code() != expected) {
        std::cerr << "Unexpected status code: got " << static_cast<int>(loaded.status().code())
                  << ", expected " << static_cast<int>(expected) << "\n";
        return 5;
    }
    const auto* mode = std::getenv("RODAQ_LOADER_FAULT");
    const bool violation = mode != nullptr && std::string_view(mode).substr(0, 6) == "throw_";
    if (manager.restart_required() != violation) {
        std::cerr << "Unexpected restart_required state\n";
        return 6;
    }
    const auto stopped = manager.shutdown(rodaq::plugin::Deadline::after(2s).value());
    if (stopped.ok() == violation) {
        std::cerr << "Unexpected shutdown outcome\n";
        return 7;
    }
    return 0;
}
