#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>

#include "abi/counter.hpp"
#include "abi/probe.hpp"
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <functional>
#include <future>
#include <condition_variable>
#include <mutex>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace {

namespace plugin = rodaq::plugin;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

plugin::PluginManagerOptions options() {
    plugin::PluginManagerOptions result;
    result.max_loaded_plugins = 2;
    result.max_instances = 1;
    result.max_config_size = 1024;
    result.shutdown_timeout = 1s;
    return result;
}

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

TEST(PluginManagerBasic, InvalidOptionsAndExclusiveAuthority) {
    EXPECT_EQ(
        plugin::PluginManager::create({}).status().code(), rodaq::StatusCode::InvalidArgument);
    auto created = plugin::PluginManager::create(options());
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    EXPECT_EQ(
        plugin::PluginManager::create(options()).status().code(), rodaq::StatusCode::Conflict);
    ASSERT_TRUE(manager.shutdown(soon()).ok());
    EXPECT_EQ(manager.phase(), plugin::PluginManagerPhase::Stopped);
    EXPECT_FALSE(manager.restart_required());
}

class PluginRuntimeTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string text = "/tmp/rodaq-runtime-XXXXXX";
        std::copy(text.begin(), text.end(), pattern.begin());
        const auto* path = mkdtemp(pattern.data());
        ASSERT_NE(path, nullptr);
        root_ = path;
        fs::copy(RODAQ_COUNTER_PACKAGE, root_ / "counter", fs::copy_options::recursive);
        fs::copy(RODAQ_LIFECYCLE_PACKAGE, root_ / "lifecycle", fs::copy_options::recursive);
        control_ = root_.string() + "-control";
        fs::create_directory(control_);
        for (const auto& entry : fs::recursive_directory_iterator(root_)) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root_, fs::perms::owner_read | fs::perms::owner_exec);
    }

    void TearDown() override {
        std::error_code error;
        fs::permissions(root_, fs::perms::owner_all, error);
        for (fs::recursive_directory_iterator iterator(root_, error);
             iterator != fs::recursive_directory_iterator(); iterator.increment(error)) {
            fs::permissions(iterator->path(), fs::perms::owner_all, error);
        }
        fs::remove_all(root_, error);
        fs::remove_all(control_, error);
    }

    std::string config(std::string mode) const {
        return nlohmann::json{
            {"mode", std::move(mode)},
            {"gate", (control_ / "gate").string()},
            {"events", (control_ / "events").string()}}
            .dump();
    }

    bool entered() const {
        const auto limit = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < limit) {
            if (fs::exists(control_ / "gate.entered")) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    bool release() const {
        const auto limit = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < limit) {
            const auto fd = open((control_ / "gate").c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                const char byte = 'x';
                const auto count = write(fd, &byte, 1);
                close(fd);
                return count == 1;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    std::string events() const {
        std::ifstream stream(control_ / "events");
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    fs::path root_;
    fs::path control_;
};

TEST_F(PluginRuntimeTest, InstalledPluginFullLifecycleAndImmediateCapacityReuse) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(std::move(policy));
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.counter", "1.0.0");
    ASSERT_TRUE(loaded.ok()) << loaded.status().message();
    auto library = std::move(loaded).value();
    EXPECT_EQ(library.info().plugin_version, "1.0.0");
    EXPECT_EQ(library.binary().sha256.size(), 64);
    plugin::ServiceRegistry services;
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, {});
    ASSERT_TRUE(initialized.ok()) << initialized.status().message();
    auto instance = std::move(initialized).value();
    EXPECT_EQ(instance.phase(), plugin::PluginInstancePhase::Open);
    auto queried = instance.query<CounterInterface>();
    ASSERT_TRUE(queried.ok()) << queried.status().message();
    auto counter = std::move(queried).value();
    ASSERT_EQ(counter.increment(7).value(), 7);
    ASSERT_EQ(counter.current().value(), 7);
    EXPECT_EQ(
        library.create_instance(services, {}).status().code(),
        rodaq::StatusCode::ResourceExhausted);
    auto future = instance.async_close();
    auto repeated = instance.async_close();
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    ASSERT_TRUE(future.result().ok());
    EXPECT_TRUE(repeated.ready());
    EXPECT_EQ(instance.phase(), plugin::PluginInstancePhase::Closed);
    EXPECT_EQ(counter.current().status().code(), rodaq::StatusCode::InvalidState);
    auto replacement = library.create_instance(services, {});
    ASSERT_TRUE(replacement.ok()) << replacement.status().message();
    EXPECT_NE(replacement.value().id(), instance.id());
    ASSERT_TRUE(replacement.value().close(soon()).ok());
    ASSERT_TRUE(manager.shutdown(soon()).ok());
    EXPECT_EQ(
        library.create_instance(services, {}).status().code(), rodaq::StatusCode::InvalidState);
}

TEST_F(PluginRuntimeTest, ConfigureFailurePreservesPrimaryAndCleansExactlyOnce) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, config("configure_fail"));
    ASSERT_FALSE(initialized.ok());
    EXPECT_EQ(initialized.status().code(), rodaq::StatusCode::Unavailable);
    ASSERT_TRUE(manager.shutdown(soon()).ok());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
    EXPECT_EQ(manager.load("unknown").status().code(), rodaq::StatusCode::InvalidState);
}

TEST_F(PluginRuntimeTest, ObserverTimeoutDoesNotCancelNativeShutdown) {
    ASSERT_EQ(mkfifo((control_ / "gate").c_str(), 0600), 0);
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, config("shutdown_wait"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto future = instance.async_close();
    const bool reached = entered();
    EXPECT_TRUE(reached);
    EXPECT_EQ(future.wait(plugin::Deadline::after(0ns).value()), plugin::CloseWaitStatus::TimedOut);
    EXPECT_FALSE(future.ready());
    EXPECT_EQ(events(), "configure\nshutdown\n");
    EXPECT_TRUE(release());
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    EXPECT_TRUE(future.result().ok());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, NativeFailureRetainsUnresolvedOwnerAndAuthority) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, config("shutdown_fail"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto future = instance.async_close();
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    EXPECT_EQ(future.result().status().code(), rodaq::StatusCode::DeadlineExceeded);
    EXPECT_EQ(instance.phase(), plugin::PluginInstancePhase::Unresolved);
    EXPECT_TRUE(manager.restart_required());
    EXPECT_EQ(manager.phase(), plugin::PluginManagerPhase::Degraded);
    EXPECT_EQ(events(), "configure\nshutdown\n");
    EXPECT_EQ(
        plugin::PluginManager::create(options()).status().code(), rodaq::StatusCode::Conflict);
}

struct HostProbe {
    std::function<rodaq::Result<std::int64_t>(std::int64_t)> callback;
    rodaq::Result<std::int64_t> invoke(std::int64_t event) { return callback(event); }
};

TEST_F(PluginRuntimeTest, CallbackSelfWaitFailsFastAndCloseDrainsAfterReturn) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    auto probe = std::make_shared<HostProbe>();
    ASSERT_TRUE(services.add_shared<LifecycleProbeService>(probe).ok());
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, config("callback"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto queried = instance.query<CounterInterface>();
    ASSERT_TRUE(queried.ok());
    auto counter = std::move(queried).value();
    probe->callback = [&](std::int64_t value) -> rodaq::Result<std::int64_t> {
        EXPECT_EQ(instance.close(soon()).status().code(), rodaq::StatusCode::InvalidState);
        EXPECT_EQ(manager.shutdown(soon()).status().code(), rodaq::StatusCode::InvalidState);
        auto future = instance.async_close();
        EXPECT_EQ(future.wait(soon()), plugin::CloseWaitStatus::TimedOut);
        return value;
    };
    EXPECT_EQ(counter.increment(4).value(), 4);
    ASSERT_TRUE(instance.close(soon()).ok());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, CloseWaitsForAdmittedOperationBeforeDestroy) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    auto probe = std::make_shared<HostProbe>();
    std::promise<void> entered_call;
    std::promise<void> finish_call;
    auto finish = finish_call.get_future().share();
    probe->callback = [&](std::int64_t value) -> rodaq::Result<std::int64_t> {
        entered_call.set_value();
        finish.wait();
        return value;
    };
    ASSERT_TRUE(services.add_shared<LifecycleProbeService>(probe).ok());
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, config("callback"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto queried = instance.query<CounterInterface>();
    ASSERT_TRUE(queried.ok());
    auto counter = std::move(queried).value();
    auto operation = std::async(std::launch::async, [&] { return counter.increment(8); });
    const auto reached = entered_call.get_future().wait_for(2s);
    EXPECT_EQ(reached, std::future_status::ready);
    auto future = instance.async_close();
    EXPECT_EQ(
        future.wait(plugin::Deadline::after(10ms).value()), plugin::CloseWaitStatus::TimedOut);
    EXPECT_EQ(instance.query<CounterInterface>().status().code(), rodaq::StatusCode::InvalidState);
    EXPECT_EQ(counter.current().status().code(), rodaq::StatusCode::InvalidState);
    EXPECT_EQ(events().find("destroy"), std::string::npos);
    finish_call.set_value();
    EXPECT_EQ(operation.get().value(), 8);
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    EXPECT_TRUE(future.result().ok());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, LateSuccessfulShutdownCleansButKeepsRestartRequired) {
    ASSERT_EQ(mkfifo((control_ / "gate").c_str(), 0600), 0);
    auto policy = options();
    policy.shutdown_timeout = 30ms;
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, config("shutdown_wait"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto future = instance.async_close();
    EXPECT_TRUE(entered());
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!manager.restart_required() && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(manager.restart_required());
    EXPECT_FALSE(future.ready());
    EXPECT_TRUE(release());
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    EXPECT_TRUE(future.result().ok());
    EXPECT_EQ(instance.phase(), plugin::PluginInstancePhase::Closed);
    EXPECT_TRUE(manager.restart_required());
    EXPECT_EQ(manager.phase(), plugin::PluginManagerPhase::Degraded);
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
}

TEST_F(PluginRuntimeTest, LateConfigureCannotPublishOpenAfterManagerShutdown) {
    ASSERT_EQ(mkfifo((control_ / "gate").c_str(), 0600), 0);
    auto policy = options();
    policy.max_instances = 2;
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    ASSERT_TRUE(services.freeze().ok());
    auto healthy = library.create_instance(services, "{}");
    ASSERT_TRUE(healthy.ok());
    auto pending = std::async(std::launch::async, [&] {
        return library.create_instance(services, config("configure_wait"));
    });
    EXPECT_TRUE(entered());
    EXPECT_EQ(
        manager.shutdown(plugin::Deadline::after(10ms).value()).status().code(),
        rodaq::StatusCode::DeadlineExceeded);
    EXPECT_EQ(healthy.value().phase(), plugin::PluginInstancePhase::Closed);
    EXPECT_EQ(pending.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(pending.get().status().code(), rodaq::StatusCode::InvalidState);
    EXPECT_TRUE(release());
    EXPECT_TRUE(manager.shutdown(soon()).ok());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
}

TEST_F(PluginRuntimeTest, BackgroundServicePermitPreventsSelfWait) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    auto probe = std::make_shared<HostProbe>();
    std::promise<plugin::PluginInstance*> published;
    auto publication = published.get_future().share();
    std::promise<void> callback_done;
    probe->callback = [&](std::int64_t event) -> rodaq::Result<std::int64_t> {
        auto* instance = publication.get();
        if (instance != nullptr) {
            EXPECT_EQ(instance->close(soon()).status().code(), rodaq::StatusCode::InvalidState);
            EXPECT_EQ(manager.shutdown(soon()).status().code(), rodaq::StatusCode::InvalidState);
            auto future = instance->async_close();
            EXPECT_EQ(future.wait(soon()), plugin::CloseWaitStatus::TimedOut);
        }
        callback_done.set_value();
        return event;
    };
    ASSERT_TRUE(services.add_shared<LifecycleProbeService>(probe).ok());
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, config("background_callback"));
    if (!initialized.ok()) {
        published.set_value(nullptr);
        FAIL() << initialized.status().message();
    }
    auto instance = std::move(initialized).value();
    published.set_value(&instance);
    EXPECT_EQ(callback_done.get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(instance.close(soon()).ok());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

}  // namespace
