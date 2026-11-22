#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>
#include <rodaq/storage/interface.hpp>
#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <atomic>

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
#include <optional>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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
    EXPECT_TRUE(manager.initialization_cleanup_settled());
}

class PluginRuntimeTest : public testing::Test {
protected:
    void SetUp() override {
        // Each CTest case owns a process; faulted Manager quarantine deliberately
        // survives that process. Fixture environment is fixed before any workers.
        unsetenv("RODAQ_FIXTURE_CREATE_MODE");
        std::array<char, 40> pattern{};
        const std::string text = "/tmp/rodaq-runtime-XXXXXX";
        std::copy(text.begin(), text.end(), pattern.begin());
        const auto* path = mkdtemp(pattern.data());
        ASSERT_NE(path, nullptr);
        root_ = path;
        fs::copy(RODAQ_COUNTER_PACKAGE, root_ / "counter", fs::copy_options::recursive);
        fs::copy(RODAQ_LIFECYCLE_PACKAGE, root_ / "lifecycle", fs::copy_options::recursive);
        fs::copy(RODAQ_STORAGE_PACKAGE, root_ / "storage", fs::copy_options::recursive);
        fs::copy(RODAQ_ACQUISITION_PACKAGE, root_ / "acquisition", fs::copy_options::recursive);
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
    EXPECT_EQ(future.result().status().code(), rodaq::StatusCode::InvalidState);
    auto copied = future;
    auto moved = std::move(copied);
    EXPECT_FALSE(
        static_cast<bool>(copied));  // NOLINT(bugprone-use-after-move): moved-from contract.
    EXPECT_TRUE(static_cast<bool>(moved));
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

namespace storage = rodaq::storage;

class StorageRuntimeTest : public PluginRuntimeTest {
protected:
    void SetUp() override {
        PluginRuntimeTest::SetUp();
        ASSERT_FALSE(HasFatalFailure());
        auto policy = options();
        policy.search_paths = {root_};
        auto created = plugin::PluginManager::create(policy);
        ASSERT_TRUE(created.ok());
        manager_.emplace(std::move(created).value());
        auto loaded = manager_->load("com.example.storage.probe");
        ASSERT_TRUE(loaded.ok()) << loaded.status().message();
        library_.emplace(std::move(loaded).value());
        ASSERT_TRUE(services_.freeze().ok());
    }

    void TearDown() override {
        if (manager_) {
            EXPECT_TRUE(manager_->shutdown(soon()).ok());
        }
        PluginRuntimeTest::TearDown();
    }

    rodaq::Result<plugin::PluginInstance> create(std::string mode) {
        return library_->create_instance(
            services_, nlohmann::json{{"mode", std::move(mode)}}.dump());
    }

    static storage::PrepareConfig envelope() {
        return {{sizeof(storage::PrepareConfig)}, 4, 8, 2, 2, 100, 1024, 4096, 2, 64, 1, 1, 1};
    }

    static std::array<storage::Record, 2> records() {
        std::array<storage::Record, 2> result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            auto& record = result[i];
            record.header.struct_size = sizeof(record);
            record.stream_id = 1;
            record.descriptor_id = 1;
            record.acceptance_sequence = i + 1;
            record.system_boot_id = 1;
            record.clock_epoch_id = 1;
            record.receive_monotonic = 1;
        }
        return result;
    }

    std::optional<plugin::PluginManager> manager_;
    std::optional<plugin::PluginLibrary> library_;
    plugin::ServiceRegistry services_;
};

TEST_F(StorageRuntimeTest, CapacityModelAndSinglePassOwnedSealResult) {
    auto initialized = create("normal");
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto queried = instance.query<storage::Interface>();
    ASSERT_TRUE(queried.ok());
    auto handle = std::move(queried).value();
    const auto prepared = handle.prepare(envelope(), soon());
    ASSERT_TRUE(prepared.ok());
    EXPECT_EQ(prepared.value().payload_expansion_denominator, 1);
    const storage::SegmentSpec segment{{sizeof(segment)}, 1, {"/tmp", 4}};
    EXPECT_TRUE(handle.open_segment(segment, soon()).ok());
    EXPECT_EQ(handle.segment_usage().value().file_count, 1);
    auto sealed = handle.seal_segment(soon());
    ASSERT_TRUE(sealed.ok()) << sealed.status().message();
    ASSERT_EQ(sealed.value().files.size(), 1);
    EXPECT_EQ(sealed.value().files[0].relative_path, "probe.bin");
    EXPECT_EQ(sealed.value().files[0].physical_size, 1);
    EXPECT_EQ(sealed.value().total_physical_bytes, 1);
    EXPECT_TRUE(handle.quiesce(soon()).ok());
    EXPECT_TRUE(instance.close(soon()).ok());
    EXPECT_EQ(sealed.value().files[0].relative_path, "probe.bin");
}

TEST_F(StorageRuntimeTest, NonOkPreservesBothConfirmedPrefixAndTailChannels) {
    for (const auto* mode : {"partial_unwritten", "partial_unknown", "full_failed", "bad_error"}) {
        SCOPED_TRACE(mode);
        auto initialized = create(mode);
        ASSERT_TRUE(initialized.ok());
        auto instance = std::move(initialized).value();
        auto queried = instance.query<storage::Interface>();
        ASSERT_TRUE(queried.ok());
        auto handle = std::move(queried).value();
        ASSERT_TRUE(handle.prepare(envelope(), soon()).ok());
        const auto batch = records();
        auto outcome = handle.write_batch({batch.data(), batch.size()}, soon());
        EXPECT_EQ(outcome.status.code(), rodaq::StatusCode::Unavailable);
        const bool full = std::string_view(mode) == "full_failed";
        EXPECT_EQ(outcome.batch_result.confirmed_prefix, full ? 2 : 1);
        const auto expected_tail = full ? RODAQ_STORAGE_TAIL_NONE
                                        : (std::string_view(mode) == "partial_unwritten"
                                               ? RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN
                                               : RODAQ_STORAGE_TAIL_UNKNOWN);
        EXPECT_EQ(outcome.batch_result.tail_state, expected_tail);
        EXPECT_TRUE(instance.close(soon()).ok());
    }
}

TEST_F(StorageRuntimeTest, MalformedResultsAndForeignExceptionsBecomeUnknown) {
    for (const auto* mode : {"bad_batch", "unknown_status", "throw_write"}) {
        SCOPED_TRACE(mode);
        auto initialized = create(mode);
        ASSERT_TRUE(initialized.ok());
        auto instance = std::move(initialized).value();
        auto queried = instance.query<storage::Interface>();
        ASSERT_TRUE(queried.ok());
        auto handle = std::move(queried).value();
        ASSERT_TRUE(handle.prepare(envelope(), soon()).ok());
        const auto batch = records();
        auto outcome = handle.write_batch({batch.data(), batch.size()}, soon());
        EXPECT_EQ(outcome.status.code(), rodaq::StatusCode::Internal);
        EXPECT_EQ(outcome.batch_result.confirmed_prefix, 0);
        EXPECT_EQ(outcome.batch_result.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);
        EXPECT_TRUE(instance.close(soon()).ok());
    }
}

TEST_F(StorageRuntimeTest, AllNestedIdentitiesIncludingUnusedSlotsPrecedeDereference) {
    for (const auto* mode :
         {"bad_files_pointer", "bad_used_pointer", "bad_unused_pointer", "bad_unused_capacity",
          "bad_unused_bytes", "bad_seal_total", "bad_seal_count"}) {
        SCOPED_TRACE(mode);
        auto initialized = create(mode);
        ASSERT_TRUE(initialized.ok());
        auto instance = std::move(initialized).value();
        auto queried = instance.query<storage::Interface>();
        ASSERT_TRUE(queried.ok());
        auto handle = std::move(queried).value();
        ASSERT_TRUE(handle.prepare(envelope(), soon()).ok());
        EXPECT_EQ(handle.seal_segment(soon()).status().code(), rodaq::StatusCode::Internal);
        EXPECT_TRUE(instance.close(soon()).ok());
    }
}

TEST_F(StorageRuntimeTest, ClosedAdmissionProvesUndispatchedBatchUnwritten) {
    auto initialized = create("normal");
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto queried = instance.query<storage::Interface>();
    ASSERT_TRUE(queried.ok());
    auto handle = std::move(queried).value();
    ASSERT_TRUE(handle.prepare(envelope(), soon()).ok());
    ASSERT_TRUE(instance.close(soon()).ok());
    const auto batch = records();
    auto outcome = handle.write_batch({batch.data(), batch.size()}, soon());
    EXPECT_EQ(outcome.status.code(), rodaq::StatusCode::InvalidState);
    EXPECT_EQ(outcome.batch_result.confirmed_prefix, 0);
    EXPECT_EQ(outcome.batch_result.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);
}

namespace acquisition = rodaq::acquisition;

struct AcquisitionHost {
    rodaq::Result<acquisition::SubmitResult> submit_record(
        const acquisition::RecordAttempt& record) {
        const auto slot = received.fetch_add(1);
        if (slot >= copies.size() || record.payload.logical_size != copies[slot].size()) {
            return rodaq::Status(rodaq::StatusCode::ResourceExhausted, "Probe Host capacity");
        }
        std::copy_n(record.payload.data, copies[slot].size(), copies[slot].data());
        threads[slot] = std::this_thread::get_id();
        tokens[slot] = record.descriptor_token;
        return acquisition::SubmitResult{
            {sizeof(acquisition::SubmitResult)}, RODAQ_SUBMIT_ACCEPTED, 0};
    }

    rodaq::Result<acquisition::DescriptorToken> propose_descriptor(
        const acquisition::DescriptorProposal& proposal, plugin::Deadline) {
        if (proposal.stream_id != 42 || proposal.data_profile != RODAQ_DATA_PROFILE_GENERIC) {
            return rodaq::Status(
                rodaq::StatusCode::InvalidArgument, "Unexpected descriptor identity");
        }
        ++descriptors;
        return acquisition::DescriptorToken{7, 9};
    }

    rodaq::Result<void> report_source_event(const acquisition::SourceEvent&) { return {}; }

    rodaq::Result<void> report_acquisition_event(const acquisition::AcquisitionEvent& event) {
        if (event.kind == RODAQ_ACQUISITION_EVENT_STREAM_READY && event.stream_id == 42) {
            ready.store(true);
        }
        return {};
    }

    std::atomic<std::uint32_t> received{0};
    std::atomic<std::uint32_t> descriptors{0};
    std::atomic<bool> ready{false};
    std::array<std::array<std::uint8_t, 4>, 4> copies{};
    std::array<std::thread::id, 4> threads{};
    std::array<acquisition::DescriptorToken, 4> tokens{};
};

TEST_F(PluginRuntimeTest, AcquisitionTypedOperationAndConcurrentManagedServiceCopyConverge) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.acquisition.probe");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    std::optional<plugin::PluginInstance> instance;
    std::weak_ptr<AcquisitionHost> weak;
    {
        auto host = std::make_shared<AcquisitionHost>();
        weak = host;
        plugin::ServiceRegistry services;
        ASSERT_TRUE(services.add_shared<acquisition::Service>(host).ok());
        ASSERT_TRUE(services.freeze().ok());
        auto initialized = library.create_instance(services, {});
        ASSERT_TRUE(initialized.ok());
        instance.emplace(std::move(initialized).value());
    }
    ASSERT_FALSE(weak.expired());
    auto queried = instance->query<acquisition::Interface>();
    ASSERT_TRUE(queried.ok());
    auto handle = std::move(queried).value();
    const acquisition::StreamBinding stream{{sizeof(stream)}, 42, {"sample", 6}, 4};
    const acquisition::PrepareConfig prepare{{sizeof(prepare)}, &stream, 1, 2};
    ASSERT_TRUE(handle.prepare(prepare, soon()).ok());
    ASSERT_TRUE(handle.start(soon()).ok());
    ASSERT_TRUE(handle.quiesce(soon()).ok());
    ASSERT_TRUE(handle.quiesce(soon()).ok());
    auto host = weak.lock();
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->received.load(), 2);
    EXPECT_EQ(host->descriptors.load(), 1);
    EXPECT_TRUE(host->ready.load());
    EXPECT_NE(host->threads[0], host->threads[1]);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(host->copies[i][0], 1);
        EXPECT_EQ(host->copies[i][1], 2);
        EXPECT_EQ(host->copies[i][2], 3);
        EXPECT_LT(host->copies[i][3], 2);
        EXPECT_EQ(host->tokens[i].value, 7);
        EXPECT_EQ(host->tokens[i].generation, 9);
    }
    host.reset();
    ASSERT_TRUE(instance->close(soon()).ok());
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(handle.start(soon()).status().code(), rodaq::StatusCode::InvalidState);
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, AcquisitionBindingMustExactlyMatchConfiguredStreamKey) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.acquisition.probe");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    auto host = std::make_shared<AcquisitionHost>();
    ASSERT_TRUE(services.add_shared<acquisition::Service>(host).ok());
    ASSERT_TRUE(services.freeze().ok());
    auto initialized = library.create_instance(services, {});
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto queried = instance.query<acquisition::Interface>();
    ASSERT_TRUE(queried.ok());
    auto handle = std::move(queried).value();
    const acquisition::StreamBinding stream{{sizeof(stream)}, 42, {"other", 5}, 4};
    const acquisition::PrepareConfig prepare{{sizeof(prepare)}, &stream, 1, 2};
    EXPECT_EQ(handle.prepare(prepare, soon()).status().code(), rodaq::StatusCode::InvalidArgument);
    EXPECT_EQ(host->descriptors.load(), 0);
    EXPECT_EQ(host->received.load(), 0);
    EXPECT_TRUE(instance.close(soon()).ok());
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

class CreateOutputFault : public PluginRuntimeTest,
                          public testing::WithParamInterface<const char*> {};

TEST_P(CreateOutputFault, KeepsUnresolvedOwnershipWithoutCallingPoisonedCleanup) {
    ASSERT_EQ(setenv("RODAQ_FIXTURE_CREATE_MODE", GetParam(), 1), 0);
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
    auto initialized = library.create_instance(services, config("normal"));
    ASSERT_FALSE(initialized.ok());
    EXPECT_EQ(initialized.status().code(), rodaq::StatusCode::Internal);
    EXPECT_EQ(manager.phase(), plugin::PluginManagerPhase::Degraded);
    EXPECT_TRUE(manager.restart_required());
    EXPECT_TRUE(events().empty());
    EXPECT_EQ(manager.shutdown(soon()).status().code(), rodaq::StatusCode::Internal);
    EXPECT_EQ(
        plugin::PluginManager::create(options()).status().code(), rodaq::StatusCode::Conflict);
}

INSTANTIATE_TEST_SUITE_P(
    AbiViolation, CreateOutputFault,
    testing::Values("unknown", "nondefault_failure", "malformed_success", "bad_error_ok"));

class CreateOrdinaryFailure : public PluginRuntimeTest,
                              public testing::WithParamInterface<const char*> {};

TEST_P(CreateOrdinaryFailure, PreservesPrimaryAndReleasesCapacityWithoutFault) {
    ASSERT_EQ(setenv("RODAQ_FIXTURE_CREATE_MODE", GetParam(), 1), 0);
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
    for (int attempt = 0; attempt < 2; ++attempt) {
        EXPECT_EQ(
            library.create_instance(services, {}).status().code(), rodaq::StatusCode::Unavailable);
    }
    EXPECT_FALSE(manager.restart_required());
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

INSTANTIATE_TEST_SUITE_P(
    AbiNormal, CreateOrdinaryFailure, testing::Values("normal_failure", "bad_error_failure"));

TEST_F(PluginRuntimeTest, UnknownConfigureFaultsLibraryAndNeverPublishesOpen) {
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
    EXPECT_EQ(
        library.create_instance(services, config("unknown_configure")).status().code(),
        rodaq::StatusCode::Internal);
    EXPECT_TRUE(manager.restart_required());
    EXPECT_FALSE(manager.initialization_cleanup_settled());
    EXPECT_EQ(manager.shutdown(soon()).status().code(), rodaq::StatusCode::Internal);
}

TEST_F(PluginRuntimeTest, UnknownQueryRejectsPartialOutputWithoutFaultingOtherCalls) {
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
    auto initialized = library.create_instance(services, config("unknown_query"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    EXPECT_EQ(instance.query<CounterInterface>().status().code(), rodaq::StatusCode::Internal);
    EXPECT_FALSE(manager.restart_required());
    EXPECT_TRUE(instance.close(soon()).ok());
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, UnknownShutdownCannotAuthorizeDestroy) {
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
    auto initialized = library.create_instance(services, config("unknown_shutdown"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto future = instance.async_close();
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    EXPECT_EQ(future.result().status().code(), rodaq::StatusCode::Internal);
    EXPECT_EQ(instance.phase(), plugin::PluginInstancePhase::Unresolved);
    EXPECT_TRUE(manager.restart_required());
    EXPECT_EQ(events(), "configure\nshutdown\n");
}

TEST_F(PluginRuntimeTest, QueryMetadataRemainsOwnedThroughHandleConstructionDuringClose) {
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
    auto initialized = library.create_instance(services, config("query_wait"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto pending =
        std::async(std::launch::async, [&] { return instance.query<CounterInterface>(); });
    EXPECT_TRUE(entered());
    auto future = instance.async_close();
    EXPECT_EQ(
        future.wait(plugin::Deadline::after(10ms).value()), plugin::CloseWaitStatus::TimedOut);
    EXPECT_EQ(events().find("destroy"), std::string::npos);
    EXPECT_TRUE(release());
    auto queried = pending.get();
    ASSERT_TRUE(queried.ok());
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    EXPECT_EQ(queried.value().current().status().code(), rodaq::StatusCode::InvalidState);
    EXPECT_TRUE(manager.shutdown(soon()).ok());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
}

TEST_F(PluginRuntimeTest, FailedConfigureKeepsCapacityUntilActualCleanupCompletes) {
    ASSERT_EQ(mkfifo((control_ / "gate").c_str(), 0600), 0);
    auto policy = options();
    policy.shutdown_timeout = 3s;
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    plugin::ServiceRegistry services;
    ASSERT_TRUE(services.freeze().ok());
    EXPECT_TRUE(manager.initialization_cleanup_settled());
    EXPECT_EQ(
        library.create_instance(services, config("configure_fail_wait")).status().code(),
        rodaq::StatusCode::Unavailable);
    EXPECT_TRUE(entered());
    EXPECT_FALSE(manager.initialization_cleanup_settled());
    EXPECT_EQ(
        library.create_instance(services, {}).status().code(),
        rodaq::StatusCode::ResourceExhausted);
    EXPECT_TRUE(release());
    std::optional<plugin::PluginInstance> replacement;
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < limit) {
        auto initialized = library.create_instance(services, {});
        if (initialized.ok()) {
            replacement.emplace(std::move(initialized).value());
            break;
        }
        EXPECT_EQ(initialized.status().code(), rodaq::StatusCode::ResourceExhausted);
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(replacement.has_value());
    EXPECT_TRUE(manager.initialization_cleanup_settled());
    EXPECT_TRUE(replacement->close(soon()).ok());
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

TEST(PluginManagerBasic, ConfigurationCapacityHasExactUint32BoundaryWithoutPreallocation) {
    auto policy = options();
    policy.max_config_size = static_cast<std::size_t>(UINT32_MAX) + 1;
    EXPECT_EQ(
        plugin::PluginManager::create(policy).status().code(), rodaq::StatusCode::InvalidArgument);
    policy.max_config_size = UINT32_MAX;
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    EXPECT_TRUE(created.value().shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, DestroyStageSupervisionKeepsOwnershipUntilActualReturn) {
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
    auto initialized = library.create_instance(services, config("destroy_wait"));
    ASSERT_TRUE(initialized.ok());
    auto instance = std::move(initialized).value();
    auto future = instance.async_close();
    EXPECT_TRUE(entered());
    const auto limit = std::chrono::steady_clock::now() + 35s;
    while (!manager.restart_required() && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(manager.restart_required());
    EXPECT_FALSE(future.ready());
    EXPECT_EQ(future.result().status().code(), rodaq::StatusCode::InvalidState);
    EXPECT_TRUE(release());
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    EXPECT_TRUE(future.result().ok());
    EXPECT_TRUE(manager.restart_required());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
}

TEST_F(PluginRuntimeTest, BackingReleaseSupervisionRunsWithoutStructuralLocks) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto loaded = manager.load("com.example.lifecycle");
    ASSERT_TRUE(loaded.ok());
    auto library = std::move(loaded).value();
    struct Gate {
        std::atomic<bool> armed{false};
        std::promise<void> entered;
        std::shared_future<void> resume;
    };
    auto gate = std::make_shared<Gate>();
    std::promise<void> resume;
    gate->resume = resume.get_future().share();
    auto entered_deleter = gate->entered.get_future();
    std::optional<plugin::PluginInstance> instance;
    {
        plugin::ServiceRegistry services;
        auto host = std::shared_ptr<HostProbe>(new HostProbe, [gate](HostProbe* provider) {
            if (gate->armed.load(std::memory_order_acquire)) {
                // The independent monitor and ready() still need both structural
                // locks while this deliberately slow custom deleter is blocked.
                gate->entered.set_value();
                gate->resume.wait();
            }
            delete provider;
        });
        ASSERT_TRUE(services.add_shared<LifecycleProbeService>(host).ok());
        ASSERT_TRUE(services.freeze().ok());
        auto initialized = library.create_instance(services, config("normal"));
        ASSERT_TRUE(initialized.ok());
        instance.emplace(std::move(initialized).value());
    }
    gate->armed.store(true, std::memory_order_release);
    auto future = instance->async_close();
    EXPECT_EQ(entered_deleter.wait_for(2s), std::future_status::ready);
    const auto limit = std::chrono::steady_clock::now() + 35s;
    while (!manager.restart_required() && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(manager.restart_required());
    EXPECT_FALSE(future.ready());
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
    resume.set_value();
    ASSERT_EQ(future.wait(soon()), plugin::CloseWaitStatus::Completed);
    EXPECT_TRUE(future.result().ok());
    EXPECT_TRUE(manager.restart_required());
}

TEST_F(PluginRuntimeTest, ConcurrentLoadSingleFlightSharesOnePublishedLibraryState) {
    auto policy = options();
    policy.search_paths = {root_};
    policy.max_loaded_plugins = 1;
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    std::vector<std::future<rodaq::Result<plugin::PluginLibrary>>> pending;
    for (int i = 0; i < 8; ++i) {
        pending.push_back(
            std::async(std::launch::async, [&] { return manager.load("com.example.counter"); }));
    }
    const plugin::PluginInfo* identity = nullptr;
    for (auto& item : pending) {
        auto loaded = item.get();
        ASSERT_TRUE(loaded.ok());
        if (identity == nullptr) {
            identity = &loaded.value().info();
        }
        EXPECT_EQ(&loaded.value().info(), identity);
    }
    EXPECT_EQ(
        manager.load("com.example.lifecycle").status().code(),
        rodaq::StatusCode::ResourceExhausted);
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, CleanBackingReleaseAllowsRecreationWhileMainDsoRemainsResident) {
    auto policy = options();
    policy.search_paths = {root_};
    {
        auto created = plugin::PluginManager::create(policy);
        ASSERT_TRUE(created.ok());
        auto manager = std::move(created).value();
        auto loaded = manager.load("com.example.counter");
        ASSERT_TRUE(loaded.ok());
        EXPECT_TRUE(manager.shutdown(soon()).ok());
    }
    const auto artifact = root_ / "counter/lib/libcounter_cpp.so";
    void* resident = dlopen(artifact.c_str(), RTLD_NOW | RTLD_NOLOAD);
    ASSERT_NE(resident, nullptr);
    EXPECT_NE(dlsym(resident, "rodaq_plugin_abi_version"), nullptr);
    EXPECT_EQ(dlclose(resident), 0);  // Only the test's extra reference is released.
    auto recreated = plugin::PluginManager::create(policy);
    ASSERT_TRUE(recreated.ok());
    EXPECT_TRUE(recreated.value().load("com.example.counter").ok());
    EXPECT_TRUE(recreated.value().shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, ReplacedMainArtifactIsRejectedBeforeNativeLoad) {
    auto policy = options();
    policy.search_paths = {root_};
    auto created = plugin::PluginManager::create(policy);
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    const auto artifact = root_ / "counter/lib/libcounter_cpp.so";
    fs::permissions(artifact, fs::perms::owner_all);
    {
        std::ofstream stream(artifact, std::ios::binary | std::ios::app);
        stream.put('x');
    }
    fs::permissions(artifact, fs::perms::owner_read | fs::perms::owner_exec);
    EXPECT_EQ(manager.load("com.example.counter").status().code(), rodaq::StatusCode::DataLoss);
    EXPECT_TRUE(manager.load("com.example.lifecycle").ok());
    EXPECT_FALSE(manager.restart_required());
    EXPECT_TRUE(manager.shutdown(soon()).ok());
}

TEST_F(PluginRuntimeTest, ResidentPathCannotBeHotReplacedAcrossCleanManagerRecreation) {
    auto policy = options();
    policy.search_paths = {root_};
    {
        auto created = plugin::PluginManager::create(policy);
        ASSERT_TRUE(created.ok());
        auto manager = std::move(created).value();
        auto loaded = manager.load("com.example.counter");
        ASSERT_TRUE(loaded.ok());
        EXPECT_TRUE(manager.shutdown(soon()).ok());
    }
    const auto artifact = root_ / "counter/lib/libcounter_cpp.so";
    const auto replacement = control_ / "replacement.so";
    fs::copy_file(artifact, replacement);
    fs::permissions(artifact.parent_path(), fs::perms::owner_all);
    fs::rename(replacement, artifact);
    fs::permissions(artifact.parent_path(), fs::perms::owner_read | fs::perms::owner_exec);
    auto recreated = plugin::PluginManager::create(policy);
    ASSERT_TRUE(recreated.ok());
    EXPECT_EQ(
        recreated.value().load("com.example.counter").status().code(), rodaq::StatusCode::Conflict);
    EXPECT_FALSE(recreated.value().restart_required());
    EXPECT_TRUE(recreated.value().shutdown(soon()).ok());
}

}  // namespace

namespace {

class InitializationSupervision : public PluginRuntimeTest,
                                  public testing::WithParamInterface<bool> {};

TEST_P(
    InitializationSupervision, HungNativeInitializationCannotBlockOtherInstanceCloseOrReopenLate) {
    ASSERT_EQ(mkfifo((control_ / "gate").c_str(), 0600), 0);
    const bool block_create = GetParam();
    if (block_create) {
        ASSERT_EQ(setenv("RODAQ_FIXTURE_CREATE_MODE", "wait", 1), 0);
        ASSERT_EQ(setenv("RODAQ_FIXTURE_CREATE_GATE", (control_ / "gate").c_str(), 1), 0);
        ASSERT_EQ(
            setenv("RODAQ_FIXTURE_CREATE_ENTERED", (control_ / "gate.entered").c_str(), 1), 0);
    }
    auto policy = options();
    policy.search_paths = {root_};
    policy.max_instances = 2;
    auto created = plugin::PluginManager::create(std::move(policy));
    ASSERT_TRUE(created.ok());
    auto manager = std::move(created).value();
    auto healthy_library = manager.load("com.example.counter", "1.0.0");
    ASSERT_TRUE(healthy_library.ok());
    plugin::ServiceRegistry services;
    ASSERT_TRUE(services.freeze().ok());
    auto healthy_result = healthy_library.value().create_instance(services, {});
    ASSERT_TRUE(healthy_result.ok());
    auto healthy = std::move(healthy_result).value();
    auto blocked_library = manager.load("com.example.lifecycle", "1.0.0");
    ASSERT_TRUE(blocked_library.ok());
    const auto configuration = config(block_create ? "normal" : "configure_wait");
    auto initializing = std::async(std::launch::async, [&] {
        return blocked_library.value().create_instance(services, configuration);
    });
    EXPECT_TRUE(entered());
    const auto limit = std::chrono::steady_clock::now() + 35s;
    while (!manager.restart_required() && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(manager.restart_required());
    EXPECT_EQ(manager.phase(), plugin::PluginManagerPhase::Degraded);
    // Lifecycle workers may be blocked, but independent close workers still run.
    EXPECT_TRUE(healthy.close(soon()).ok());
    ASSERT_EQ(initializing.wait_for(2s), std::future_status::ready);
    const auto result = initializing.get();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), rodaq::StatusCode::DeadlineExceeded);
    EXPECT_EQ(events(), block_create ? "" : "configure\n");
    // The observer already returned, but actual native work remains owned.
    EXPECT_TRUE(release());
    const auto cleaned = std::chrono::steady_clock::now() + 2s;
    while (events() != "configure\nshutdown\ndestroy\n" &&
           std::chrono::steady_clock::now() < cleaned) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(events(), "configure\nshutdown\ndestroy\n");
    EXPECT_TRUE(manager.restart_required());
    EXPECT_EQ(manager.shutdown(soon()).status().code(), rodaq::StatusCode::DeadlineExceeded);
}

INSTANTIATE_TEST_SUITE_P(NativeStages, InitializationSupervision, testing::Bool());

}  // namespace
