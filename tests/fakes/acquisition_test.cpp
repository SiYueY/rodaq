#include <rodaq/acquisition/interface.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace plugin = rodaq::plugin;
namespace acquisition = rodaq::acquisition;
using rodaq::StatusCode;
using namespace std::chrono_literals;

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

struct CapturedRecord {
    std::uint64_t stream;
    rodaq_descriptor_id_t descriptor_id;
    std::vector<std::uint8_t> payload;
};

struct ServiceFacts {
    std::vector<CapturedRecord> records;
    std::vector<std::string> schemas;
    std::array<std::uint64_t, 11> acquisition_events{};
    StatusCode terminal_status{StatusCode::Ok};
    std::uint32_t inflight{0};
    std::uint32_t high_water_inflight{0};
    std::uint32_t stale{0};
    std::uint32_t oversized{0};
};

class Receiver {
public:
    static rodaq_status_code_t c_prepare_descriptor(
        void* ctx, rodaq_stream_id_t stream_id, const rodaq_acquisition_descriptor_t* descriptor,
        rodaq_descriptor_id_t* descriptor_id, rodaq_error_t*) noexcept {
        return reinterpret_cast<Receiver*>(ctx)->prepare_descriptor(
            stream_id, *descriptor, descriptor_id);
    }

    static rodaq_record_disposition_t c_submit_record(
        void* ctx, const rodaq_acquisition_record_attempt_t* record) noexcept {
        return reinterpret_cast<Receiver*>(ctx)->submit_record(*record);
    }

    static void c_report_event(void* ctx, const rodaq_acquisition_event_t* event) noexcept {
        reinterpret_cast<Receiver*>(ctx)->report_event(*event);
    }

    acquisition::CallbacksTable table() noexcept {
        return {
            {sizeof(acquisition::CallbacksTable)},
            this,
            c_prepare_descriptor,
            c_submit_record,
            c_report_event};
    }

    rodaq_status_code_t prepare_descriptor(
        rodaq_stream_id_t stream_id, const acquisition::Descriptor& descriptor,
        rodaq_descriptor_id_t* out_id) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (reject_descriptor || facts_.schemas.size() == 32) {
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;
        }
        auto slot = std::find_if(descriptors_.begin(), descriptors_.end(), [&](const auto& entry) {
            return entry.stream == stream_id || entry.stream == 0;
        });
        if (slot == descriptors_.end()) {
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;
        }
        const rodaq_descriptor_id_t id = facts_.schemas.size() + 1;
        slot->stream = stream_id;
        slot->descriptor_id = id;
        facts_.schemas.emplace_back(
            reinterpret_cast<const char*>(descriptor.schema.data), descriptor.schema.size);
        *out_id = id;
        return RODAQ_STATUS_OK;
    }

    rodaq_record_disposition_t submit_record(const acquisition::RecordAttempt& record) {
        std::unique_lock<std::mutex> lock(mutex_);
        ++facts_.inflight;
        facts_.high_water_inflight = std::max(facts_.high_water_inflight, facts_.inflight);
        changed_.notify_all();
        changed_.wait(lock, [&] { return !block_; });
        auto disposition = RODAQ_SUBMIT_ACCEPTED;
        const auto slot = std::find_if(
            descriptors_.begin(), descriptors_.end(),
            [&](const auto& entry) { return entry.stream == record.stream_id; });
        if (slot == descriptors_.end() || slot->descriptor_id != record.descriptor_id) {
            ++facts_.stale;
            disposition = RODAQ_SUBMIT_STALE_DESCRIPTOR;
        } else if (record.payload.size > max_bytes) {
            ++facts_.oversized;
            disposition = RODAQ_SUBMIT_RECORD_TOO_LARGE;
        } else if (facts_.records.size() == 256) {
            disposition = RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
        } else {
            CapturedRecord copy{record.stream_id, record.descriptor_id, {}};
            if (record.payload.size != 0) {
                copy.payload.assign(record.payload.data, record.payload.data + record.payload.size);
            }
            facts_.records.push_back(std::move(copy));
        }
        --facts_.inflight;
        changed_.notify_all();
        return static_cast<rodaq_record_disposition_t>(disposition);
    }

    void report_event(const acquisition::AcquisitionEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event.kind < facts_.acquisition_events.size()) {
            ++facts_.acquisition_events[event.kind];
        }
        if (event.kind == RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE) {
            facts_.terminal_status = static_cast<StatusCode>(event.status);
        }
        changed_.notify_all();
    }

    void block() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_ = true;
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_ = false;
        changed_.notify_all();
    }

    bool wait_records(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] { return facts_.records.size() >= count; });
    }

    bool wait_terminal_or_records(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] {
            return facts_.records.size() >= count || facts_.terminal_status != StatusCode::Ok ||
                   facts_.stale >= count || facts_.oversized >= count;
        });
    }

    bool wait_inflight() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] { return facts_.inflight != 0; });
    }

    ServiceFacts snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return facts_;
    }

    bool reject_descriptor{false};
    std::uint64_t max_bytes{8};

private:
    struct Slot {
        std::uint64_t stream{0};
        rodaq_descriptor_id_t descriptor_id{0};
    };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Slot, 16> descriptors_{};
    ServiceFacts facts_;
    bool block_{false};
};

class FakeAcquisitionTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-fake-acq-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        fs::copy(RODAQ_FAKE_ACQUISITION_PACKAGE, root_ / "package", fs::copy_options::recursive);
        plugin::PluginManagerOptions options;
        options.search_paths = {root_ / "package"};
        options.max_loaded_plugins = 1;
        options.max_instances = 1;
        options.max_config_size = 4096;
        options.shutdown_timeout = 2s;
        auto manager = plugin::PluginManager::create(std::move(options));
        ASSERT_TRUE(manager.ok());
        manager_.emplace(std::move(manager.value()));
        auto library = manager_->load("rodaq.fake.acquisition", "1.0.0");
        ASSERT_TRUE(library.ok());
        library_.emplace(std::move(library.value()));
        receiver_ = std::make_shared<Receiver>();
    }

    void TearDown() override {
        receiver_->release();
        if (handle_) {
            (void)handle_->quiesce(soon());
        }
        handle_.reset();
        if (instance_) {
            (void)instance_->close(soon());
        }
        instance_.reset();
        library_.reset();
        if (manager_) {
            (void)manager_->shutdown(soon());
        }
        manager_.reset();
        std::error_code error;
        fs::remove_all(root_, error);
    }

    static std::string config(nlohmann::json values = nlohmann::json::object()) {
        values["streams"] = {"/first", "/second"};
        return values.dump();
    }

    rodaq::Result<plugin::PluginInstance> create_after_cleanup(const std::string& json) {
        const auto limit = std::chrono::steady_clock::now() + 2s;
        for (;;) {
            auto created = library_->create_instance(json);
            if (created.ok() || created.status().code() != StatusCode::ResourceExhausted ||
                std::chrono::steady_clock::now() >= limit) {
                return created;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    void create(nlohmann::json values = nlohmann::json::object()) {
        auto instance = create_after_cleanup(config(std::move(values)));
        ASSERT_TRUE(instance.ok()) << instance.status().message();
        instance_.emplace(std::move(instance.value()));
        auto handle = instance_->query<acquisition::Interface>();
        ASSERT_TRUE(handle.ok());
        handle_.emplace(std::move(handle.value()));
    }

    rodaq::Result<void> prepare(std::uint64_t max_bytes = 8) {
        const std::array<rodaq_acquisition_stream_binding_t, 2> bindings{
            {{{sizeof(rodaq_acquisition_stream_binding_t)}, 12, {"/second", 7}, max_bytes},
             {{sizeof(rodaq_acquisition_stream_binding_t)}, 11, {"/first", 6}, max_bytes}}};
        const acquisition::PrepareConfig envelope{{sizeof(envelope)}, bindings.data(), 2, 1};
        return handle_->prepare(envelope, receiver_->table(), soon());
    }

    fs::path root_;
    std::shared_ptr<Receiver> receiver_;
    std::optional<plugin::PluginManager> manager_;
    std::optional<plugin::PluginLibrary> library_;
    std::optional<plugin::PluginInstance> instance_;
    std::optional<acquisition::Handle> handle_;
};

TEST_F(FakeAcquisitionTest, RealSdkMultiStreamWorkerUsesReturnedDescriptorIdsAndReusesPayload) {
    create({{"record_limit", 4}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    auto prepared = receiver_->snapshot();
    EXPECT_EQ(prepared.schemas.size(), 2U);
    EXPECT_EQ(prepared.acquisition_events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 2ULL);
    EXPECT_EQ(prepared.acquisition_events[RODAQ_ACQUISITION_EVENT_TRANSPORT_READY], 1ULL);
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_records(8));
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    ASSERT_EQ(facts.records.size(), 8U);
    for (std::size_t i = 0; i < facts.records.size(); ++i) {
        const auto& record = facts.records[i];
        const auto sequence = i / 2 + 1;
        EXPECT_EQ(record.stream, i % 2 == 0 ? 11ULL : 12ULL);
        EXPECT_EQ(record.descriptor_id, i % 2 + 1);
        ASSERT_EQ(record.payload.size(), 8U);
        for (std::size_t byte = 0; byte < record.payload.size(); ++byte) {
            EXPECT_EQ(record.payload[byte], sequence + byte);
        }
    }
    EXPECT_EQ(facts.high_water_inflight, 1U);
    EXPECT_EQ(facts.terminal_status, StatusCode::Ok);
}

TEST_F(FakeAcquisitionTest, QuiesceDeadlineRetainsBlockedCallbackInputUntilItsActualReturn) {
    receiver_->block();
    create({{"record_limit", 0}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_inflight());
    EXPECT_EQ(
        handle_->quiesce(plugin::Deadline::after(5ms).value()).status().code(),
        StatusCode::DeadlineExceeded);
    EXPECT_EQ(receiver_->snapshot().inflight, 1U);
    receiver_->release();
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    ASSERT_EQ(facts.records.size(), 1U);
    EXPECT_EQ(facts.records[0].payload, (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
    EXPECT_EQ(facts.inflight, 0U);
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(receiver_->snapshot().records.size(), facts.records.size());
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
}

TEST_F(FakeAcquisitionTest, ActiveStopWakesProtocolWaitAndPreventsEveryLaterCallback) {
    create({{"record_limit", 0}, {"period_us", 1000000}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_records(2));
    const auto before = std::chrono::steady_clock::now();
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_LT(std::chrono::steady_clock::now() - before, 500ms);
    const auto facts = receiver_->snapshot();
    EXPECT_EQ(facts.inflight, 0U);
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(receiver_->snapshot().records.size(), facts.records.size());
}

TEST_F(FakeAcquisitionTest, RejectedDescriptorNeverReportsReadyAndCreatedQuiesceIsIdempotent) {
    receiver_->reject_descriptor = true;
    create();
    ASSERT_TRUE(handle_);
    EXPECT_EQ(prepare().status().code(), StatusCode::ResourceExhausted);
    EXPECT_TRUE(receiver_->snapshot().schemas.empty());
    EXPECT_EQ(receiver_->snapshot().acquisition_events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 0ULL);
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_FALSE(handle_->start(soon()).ok());
}

TEST_F(FakeAcquisitionTest, ExactBindingBijectionAndEmptyPayloadsWorkCleanly) {
    create();
    ASSERT_TRUE(handle_);
    EXPECT_FALSE(prepare(4).ok());
    const std::array<rodaq_acquisition_stream_binding_t, 2> bad_bindings{
        {{{sizeof(rodaq_acquisition_stream_binding_t)}, 11, {"/first", 6}, 8},
         {{sizeof(rodaq_acquisition_stream_binding_t)}, 12, {"/missing", 8}, 8}}};
    const acquisition::PrepareConfig bad_env{{sizeof(bad_env)}, bad_bindings.data(), 2, 1};
    EXPECT_EQ(
        handle_->prepare(bad_env, receiver_->table(), soon()).status().code(),
        StatusCode::InvalidArgument);
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    handle_.reset();
    ASSERT_TRUE(instance_->close(soon()).ok());
    instance_.reset();

    create({{"record_limit", 1}, {"payload_bytes", 0}});
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_records(2));
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    ASSERT_EQ(facts.records.size(), 2U);
    EXPECT_TRUE(facts.records[0].payload.empty());
}

TEST_F(FakeAcquisitionTest, StaleTokenAndOversizeRejectionsDoNotCrashOrRetry) {
    create({{"mode", "stale_token"}, {"record_limit", 2}});
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_terminal_or_records(4));
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_EQ(receiver_->snapshot().stale, 4U);
    handle_.reset();
    ASSERT_TRUE(instance_->close(soon()).ok());
    instance_.reset();

    receiver_ = std::make_unique<Receiver>();
    receiver_->max_bytes = 4;
    create({{"mode", "oversize_attempt"}, {"record_limit", 1}});
    ASSERT_TRUE(prepare(4).ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_terminal_or_records(2));
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_EQ(receiver_->snapshot().oversized, 2U);
}

class FakeAcquisitionFailureTest : public FakeAcquisitionTest,
                                   public testing::WithParamInterface<const char*> {};

TEST_P(FakeAcquisitionFailureTest, FaultIsReportedAtItsActualTypedBoundary) {
    const std::string mode = GetParam();
    create({{"mode", mode}, {"record_limit", 1}});
    ASSERT_TRUE(handle_);
    const auto prepared = prepare();
    if (mode == "fail_prepare") {
        EXPECT_FALSE(prepared.ok());
        EXPECT_TRUE(receiver_->snapshot().schemas.empty());
        EXPECT_TRUE(handle_->quiesce(soon()).ok());
        return;
    }
    ASSERT_TRUE(prepared.ok());
    const auto started = handle_->start(soon());
    if (mode == "fail_start") {
        EXPECT_FALSE(started.ok());
        EXPECT_TRUE(receiver_->snapshot().records.empty());
        return;
    }
    ASSERT_TRUE(started.ok());
    if (mode == "terminal_event") {
        ASSERT_TRUE(receiver_->wait_terminal_or_records(1));
        EXPECT_TRUE(handle_->quiesce(soon()).ok());
        EXPECT_EQ(receiver_->snapshot().terminal_status, StatusCode::Unavailable);
        return;
    }
    ASSERT_TRUE(receiver_->wait_records(2));
    if (mode == "fail_quiesce") {
        EXPECT_FALSE(handle_->quiesce(soon()).ok());
    } else if (mode == "fail_shutdown") {
        EXPECT_TRUE(handle_->quiesce(soon()).ok());
        EXPECT_FALSE(instance_->close(soon()).ok());
    }
}

INSTANTIATE_TEST_SUITE_P(
    Faults, FakeAcquisitionFailureTest,
    testing::Values(
        "fail_prepare", "fail_start", "fail_quiesce", "fail_shutdown", "terminal_event"));

}  // namespace
