#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <string_view>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    assert(argc == 3);
    rodaq::plugin::PluginManagerOptions options;
    options.search_paths = {argv[1]};
    options.max_instances = 1;
    options.max_loaded_plugins = 1;
    options.max_config_size = 1024;
    options.shutdown_timeout = 1s;
    auto created = rodaq::plugin::PluginManager::create(std::move(options));
    assert(created.ok());
    auto manager = std::move(created).value();
    const auto loaded = manager.load("com.example.counter", "1.0.0");
    assert(!loaded.ok());
    const auto expected = static_cast<rodaq::StatusCode>(std::atoi(argv[2]));
    assert(loaded.status().code() == expected);
    const auto* mode = std::getenv("RODAQ_LOADER_FAULT");
    const bool violation = mode != nullptr && std::string_view(mode).substr(0, 6) == "throw_";
    assert(manager.restart_required() == violation);
    const auto stopped = manager.shutdown(rodaq::plugin::Deadline::after(2s).value());
    assert(stopped.ok() != violation);
}
