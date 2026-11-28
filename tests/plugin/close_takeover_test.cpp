#include <rodaq/plugin/close_future.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <new>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace allocation_failure {
thread_local bool reject = false;
}

void* operator new(std::size_t size) {
    if (!allocation_failure::reject) {
        if (auto* pointer = std::malloc(size == 0 ? 1 : size)) {
            return pointer;
        }
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete[](void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    void* pointer = nullptr;
    if (!allocation_failure::reject &&
        posix_memalign(&pointer, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) == 0) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}

namespace {

namespace plugin = rodaq::plugin;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

class RejectAllocation {
public:
    RejectAllocation() noexcept { allocation_failure::reject = true; }

    RejectAllocation(const RejectAllocation&) = delete;
    RejectAllocation& operator=(const RejectAllocation&) = delete;

    ~RejectAllocation() noexcept { allocation_failure::reject = false; }
};

class CloseTakeover : public testing::Test {
protected:
    void SetUp() override {
        unsetenv("RODAQ_FIXTURE_CREATE_MODE");
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-takeover-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* directory = mkdtemp(pattern.data());
        ASSERT_NE(directory, nullptr);
        root_ = directory;
        fs::create_directory(root_ / "control");
        fs::copy(RODAQ_LIFECYCLE_PACKAGE, root_ / "package", fs::copy_options::recursive);
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    bool wait_marker(const fs::path& gate) {
        const auto limit = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < limit) {
            if (fs::exists(gate.string() + ".entered")) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    bool release(const fs::path& gate) {
        const auto limit = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < limit) {
            const auto file = open(gate.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (file >= 0) {
                const char byte = 'x';
                const auto count = write(file, &byte, 1);
                close(file);
                return count == 1;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    fs::path root_;
};

TEST_F(CloseTakeover, SaturatedWorkersAndFailedCallerAllocationCannotLoseCloseRequests) {
    bool injected = false;
    {
        RejectAllocation fail;
        try {
            auto* probe = new int(1);
            delete probe;
        } catch (const std::bad_alloc&) {
            injected = true;
        }
    }
    ASSERT_TRUE(injected);
    plugin::PluginManagerOptions policy;
    policy.search_paths = {root_ / "package"};
    policy.max_loaded_plugins = 1;
    policy.max_instances = 12;
    policy.max_config_size = 1024;
    policy.shutdown_timeout = 5s;
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    std::vector<plugin::PluginInstance> instances;
    std::vector<plugin::CloseFuture> futures;
    instances.reserve(12);
    futures.reserve(12);
    std::array<fs::path, 4> gates;
    for (std::size_t i = 0; i < 12; ++i) {
        std::string config;
        if (i < gates.size()) {
            gates[i] = root_ / "control" / std::to_string(i);
            ASSERT_EQ(mkfifo(gates[i].c_str(), 0600), 0);
            config = "{\"mode\":\"shutdown_wait\",\"gate\":\"" + gates[i].string() + "\"}";
        }
        auto initialized = library.create_instance(config);
        ASSERT_TRUE(initialized.ok());
        instances.emplace_back(std::move(initialized).value());
    }
    {
        RejectAllocation fail;
        for (std::size_t i = 0; i < gates.size(); ++i) {
            futures.push_back(instances[i].async_close());
        }
    }
    for (const auto& gate : gates) {
        EXPECT_TRUE(wait_marker(gate));
    }
    {
        RejectAllocation fail;
        for (std::size_t i = gates.size(); i < instances.size(); ++i) {
            futures.push_back(instances[i].async_close());
        }
        instances.clear();
    }
    for (const auto& future : futures) {
        EXPECT_FALSE(future.ready());
    }
    for (const auto& gate : gates) {
        EXPECT_TRUE(release(gate));
    }
    for (const auto& future : futures) {
        EXPECT_EQ(
            future.wait(plugin::Deadline::after(2s).value()), plugin::CloseWaitStatus::Completed);
        EXPECT_TRUE(future.result().ok());
    }
    EXPECT_FALSE(manager.restart_required());
    auto replacement = library.create_instance({});
    ASSERT_TRUE(replacement.ok());
    EXPECT_TRUE(replacement.value().close(plugin::Deadline::after(2s).value()).ok());
    EXPECT_TRUE(manager.shutdown(plugin::Deadline::after(2s).value()).ok());
}

}  // namespace
