#include <rodaq/plugin/manager.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::filesystem::path control = argv[2];
    rodaq::plugin::PluginManagerOptions options;
    options.search_paths = {argv[1]};
    options.max_instances = 1;
    options.max_loaded_plugins = 1;
    options.max_config_size = 1024;
    options.shutdown_timeout = 1s;
    auto created = rodaq::plugin::PluginManager::create(options);
    assert(!created.ok() && created.status().code() == rodaq::StatusCode::DeadlineExceeded);
    assert(std::filesystem::exists(control / "entered"));
    assert(!std::filesystem::exists(control / "returned"));
    const auto gate = open((control / "gate").c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    assert(gate >= 0);
    const char byte = 'x';
    assert(write(gate, &byte, 1) == 1);
    close(gate);
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!std::filesystem::exists(control / "returned") &&
           std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(1ms);
    }
    assert(std::filesystem::exists(control / "returned"));
    // Late Catalog work cannot produce a new live authority after degradation.
    auto retry = rodaq::plugin::PluginManager::create(options);
    assert(!retry.ok() && retry.status().code() == rodaq::StatusCode::Conflict);
}
