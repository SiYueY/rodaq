#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>

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
    acquisition::DescriptorToken token;
    std::uint64_t source_sequence;
    rodaq_source_id_t source;
    std::uint64_t session;
    std::vector<std::uint8_t> payload;
};

struct ServiceFacts {
    std::vector<CapturedRecord> records;
    std::vector<std::string> schemas;
    std::array<std::uint64_t, 11> acquisition_events{};
    std::array<std::uint64_t, 7> source_events{};
    StatusCode terminal_status{StatusCode::Ok};
    std::uint32_t descriptor_inflight{0};
    std::uint32_t inflight{0};
    std::uint32_t high_water_inflight{0};
    std::uint32_t stale{0};
    std::uint32_t oversized{0};
};

// SDK protocol test receiver only. No Task/Pipeline/Admission policy is replaced
// by this receiver; formal Core integration uses the real instance-owned host.
class Receiver {
public:
    rodaq::Result<acquisition::DescriptorToken> propose_descriptor(
        const acquisition::DescriptorProposal& proposal, plugin::Deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (reject_descriptor || facts_.schemas.size() == 32) {
            return rodaq::Status(StatusCode::ResourceExhausted, {});
        }
        const std::string_view schema(
            reinterpret_cast<const char*>(proposal.schema.data), proposal.schema.size);
        if (schema == R"({"version":2})") {
            ++facts_.descriptor_inflight;
            changed_.notify_all();
            changed_.wait(lock, [&] { return !block_changed_descriptor_; });
            --facts_.descriptor_inflight;
        }
        auto slot = std::find_if(tokens_.begin(), tokens_.end(), [&](const auto& entry) {
            return entry.stream == proposal.stream_id || entry.stream == 0;
        });
        if (slot == tokens_.end()) {
            return rodaq::Status(StatusCode::ResourceExhausted, {});
        }
        const acquisition::DescriptorToken token{facts_.schemas.size() + 1, 1};
        slot->stream = proposal.stream_id;
        slot->token = token;
        facts_.schemas.emplace_back(
            reinterpret_cast<const char*>(proposal.schema.data), proposal.schema.size);
        return token;
    }

    rodaq::Result<acquisition::SubmitResult> submit_record(
        const acquisition::RecordAttempt& record) {
        std::unique_lock<std::mutex> lock(mutex_);
        ++facts_.inflight;
        facts_.high_water_inflight = std::max(facts_.high_water_inflight, facts_.inflight);
        changed_.notify_all();
        changed_.wait(lock, [&] { return !block_; });
        auto disposition = RODAQ_SUBMIT_ACCEPTED;
        const auto token = std::find_if(tokens_.begin(), tokens_.end(), [&](const auto& entry) {
            return entry.stream == record.stream_id;
        });
        if (token == tokens_.end() || token->token.value != record.descriptor_token.value ||
            token->token.generation != record.descriptor_token.generation) {
            ++facts_.stale;
            disposition = RODAQ_SUBMIT_STALE_DESCRIPTOR;
        } else if (record.payload.logical_size > max_bytes) {
            ++facts_.oversized;
            disposition = RODAQ_SUBMIT_RECORD_TOO_LARGE;
        } else if (facts_.records.size() == 256) {
            disposition = RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
        } else {
            CapturedRecord copy{record.stream_id, record.descriptor_token,  record.source_sequence,
                                record.source_id, record.source_session_id, {}};
            if (record.payload.logical_size != 0) {
                copy.payload.assign(
                    record.payload.data, record.payload.data + record.payload.logical_size);
            }
            EXPECT_EQ(record.sequence_scope, RODAQ_SEQUENCE_SCOPE_SOURCE_SESSION);
            EXPECT_EQ(record.has_source_id, 1);
            EXPECT_EQ(record.source_id.kind, RODAQ_SOURCE_ID_RTPS_GUID);
            EXPECT_EQ(record.source_id.size, 16);
            EXPECT_EQ(record.has_source_session_id, 1);
            EXPECT_EQ(record.source_session_id, 1);
            facts_.records.push_back(std::move(copy));
        }
        --facts_.inflight;
        changed_.notify_all();
        return acquisition::SubmitResult{
            {sizeof(acquisition::SubmitResult)},
            static_cast<rodaq_submit_disposition_t>(disposition),
            0};
    }

    rodaq::Result<void> report_source_event(const acquisition::SourceEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++facts_.source_events[event.kind];
        changed_.notify_all();
        return {};
    }

    rodaq::Result<void> report_acquisition_event(const acquisition::AcquisitionEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++facts_.acquisition_events[event.kind];
        if (event.kind == RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE) {
            facts_.terminal_status = static_cast<StatusCode>(event.status);
        }
        changed_.notify_all();
        return {};
    }

    void block() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_ = true;
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_ = false;
        block_changed_descriptor_ = false;
        changed_.notify_all();
    }

    void block_changed_descriptor() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_changed_descriptor_ = true;
    }

    bool wait_descriptor_inflight() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] { return facts_.descriptor_inflight != 0; });
    }

    bool wait_records(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] { return facts_.records.size() >= count; });
    }

    bool wait_end(std::size_t streams = 2) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] {
            return facts_.source_events[RODAQ_SOURCE_EVENT_SESSION_ENDED] == streams ||
                   facts_.terminal_status != StatusCode::Ok;
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

    bool reject_descriptor{false};  // Set only before starting any Native work.
    std::uint64_t max_bytes{8};

private:
    struct Token {
        std::uint64_t stream{0};
        acquisition::DescriptorToken token{};
    };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<Token, 16> tokens_{};
    ServiceFacts facts_;
    bool block_{false};
    bool block_changed_descriptor_{false};
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
        for (const auto& item : fs::recursive_directory_iterator(root_ / "package")) {
            fs::permissions(item.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root_ / "package", fs::perms::owner_read | fs::perms::owner_exec);
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
        ASSERT_TRUE(services_.add_shared<acquisition::Service>(receiver_).ok());
        ASSERT_TRUE(services_.freeze().ok());
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
        fs::permissions(root_ / "package", fs::perms::owner_all, error);
        for (const auto& item : fs::recursive_directory_iterator(root_ / "package", error)) {
            fs::permissions(item.path(), fs::perms::owner_all, error);
        }
        fs::remove_all(root_, error);
    }

    static std::string config(nlohmann::json values = nlohmann::json::object()) {
        values["streams"] = {"/first", "/second"};
        return values.dump();
    }

    rodaq::Result<plugin::PluginInstance> create_after_cleanup(
        const plugin::ServiceRegistry& services, const std::string& json) {
        const auto limit = std::chrono::steady_clock::now() + 2s;
        for (;;) {
            auto created = library_->create_instance(services, json);
            if (created.ok() || created.status().code() != StatusCode::ResourceExhausted ||
                std::chrono::steady_clock::now() >= limit) {
                return created;
            }
            // Failed configure returns before its asynchronous actual cleanup.
            // Capacity rejection has no new Native side effects; do not invent
            // a released slot or bypass the Framework's retained owner.
            std::this_thread::sleep_for(1ms);
        }
    }

    void create(nlohmann::json values = nlohmann::json::object()) {
        auto instance = create_after_cleanup(services_, config(std::move(values)));
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
        return handle_->prepare(envelope, soon());
    }

    fs::path root_;
    std::shared_ptr<Receiver> receiver_;
    plugin::ServiceRegistry services_;
    std::optional<plugin::PluginManager> manager_;
    std::optional<plugin::PluginLibrary> library_;
    std::optional<plugin::PluginInstance> instance_;
    std::optional<acquisition::Handle> handle_;
};

TEST_F(
    FakeAcquisitionTest,
    RealSdkMultiStreamWorkerUsesOnlyReturnedTokensAndImmediatelyReusablePayload) {
    create({{"record_limit", 4}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    auto prepared = receiver_->snapshot();
    EXPECT_EQ(prepared.schemas.size(), 2);
    EXPECT_EQ(prepared.acquisition_events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 2);
    EXPECT_EQ(prepared.acquisition_events[RODAQ_ACQUISITION_EVENT_TRANSPORT_READY], 1);
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_end());
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    ASSERT_EQ(facts.records.size(), 8);
    for (std::size_t i = 0; i < facts.records.size(); ++i) {
        const auto& record = facts.records[i];
        const auto sequence = i / 2 + 1;
        EXPECT_EQ(record.stream, i % 2 == 0 ? 11 : 12);
        EXPECT_EQ(record.source_sequence, sequence);
        EXPECT_EQ(record.token.value, i % 2 + 1);
        ASSERT_EQ(record.payload.size(), 8);
        for (std::size_t byte = 0; byte < record.payload.size(); ++byte) {
            EXPECT_EQ(record.payload[byte], sequence + byte);
        }
    }
    EXPECT_EQ(facts.high_water_inflight, 1);
    EXPECT_EQ(facts.terminal_status, StatusCode::Ok);
    EXPECT_EQ(facts.source_events[RODAQ_SOURCE_EVENT_SESSION_STARTED], 2);
    EXPECT_EQ(facts.source_events[RODAQ_SOURCE_EVENT_SESSION_ENDED], 2);
}

TEST_F(FakeAcquisitionTest, SchemaChangeObtainsNewCoreTokensBeforeAnyChangedRecord) {
    create({{"record_limit", 4}, {"schema_change_after", 2}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_end());
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    ASSERT_EQ(facts.schemas.size(), 4);
    EXPECT_EQ(facts.schemas[0], R"({"version":1})");
    EXPECT_EQ(facts.schemas[2], R"({"version":2})");
    ASSERT_EQ(facts.records.size(), 8);
    EXPECT_EQ(facts.records[3].token.value, 2);
    EXPECT_EQ(facts.records[4].token.value, 3);
    EXPECT_EQ(facts.records[5].token.value, 4);
    EXPECT_EQ(facts.stale, 0);
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
    EXPECT_EQ(receiver_->snapshot().inflight, 1);
    receiver_->release();
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    ASSERT_EQ(facts.records.size(), 1);
    EXPECT_EQ(facts.records[0].payload, (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
    EXPECT_EQ(facts.inflight, 0);
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
    EXPECT_EQ(facts.inflight, 0);
    EXPECT_EQ(facts.source_events[RODAQ_SOURCE_EVENT_SESSION_ENDED], 2);
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(receiver_->snapshot().records.size(), facts.records.size());
}

TEST_F(FakeAcquisitionTest, LateStartDoesNotBecomeCompletionWhenItsObserverWaitExpires) {
    create({{"mode", "late_start"}, {"delay_ms", 100}, {"record_limit", 1}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    auto work = std::async(
        std::launch::async, [&] { return handle_->start(plugin::Deadline::after(1ms).value()); });
    EXPECT_EQ(work.wait_for(5ms), std::future_status::timeout);
    EXPECT_TRUE(receiver_->snapshot().records.empty());
    ASSERT_TRUE(work.get().ok());
    ASSERT_TRUE(receiver_->wait_end());
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
}

TEST_F(FakeAcquisitionTest, RejectedDescriptorNeverReportsReadyAndCreatedQuiesceIsIdempotent) {
    receiver_->reject_descriptor = true;
    create();
    ASSERT_TRUE(handle_);
    EXPECT_EQ(prepare().status().code(), StatusCode::ResourceExhausted);
    EXPECT_TRUE(receiver_->snapshot().schemas.empty());
    EXPECT_EQ(receiver_->snapshot().acquisition_events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 0);
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_FALSE(handle_->start(soon()).ok());
}

TEST_F(FakeAcquisitionTest, ExactBindingBijectionAndPayloadEnvelopeFailBeforeAnyCallback) {
    create();
    ASSERT_TRUE(handle_);
    EXPECT_FALSE(prepare(4).ok());
    EXPECT_TRUE(receiver_->snapshot().schemas.empty());
    EXPECT_TRUE(receiver_->snapshot().records.empty());
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
}

TEST_F(FakeAcquisitionTest, EmptyPayloadsRemainValidCallbackAttempts) {
    create({{"record_limit", 1}, {"payload_bytes", 0}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_end());
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    ASSERT_EQ(facts.records.size(), 2);
    EXPECT_TRUE(facts.records[0].payload.empty());
}

TEST_F(FakeAcquisitionTest, UnknownConfiguredStreamBindingFailsBeforeEveryServiceCallback) {
    create();
    ASSERT_TRUE(handle_);
    const std::array<rodaq_acquisition_stream_binding_t, 2> bindings{
        {{{sizeof(rodaq_acquisition_stream_binding_t)}, 11, {"/first", 6}, 8},
         {{sizeof(rodaq_acquisition_stream_binding_t)}, 12, {"/missing", 8}, 8}}};
    const acquisition::PrepareConfig envelope{{sizeof(envelope)}, bindings.data(), 2, 1};
    EXPECT_EQ(handle_->prepare(envelope, soon()).status().code(), StatusCode::InvalidArgument);
    const auto facts = receiver_->snapshot();
    EXPECT_TRUE(facts.schemas.empty());
    EXPECT_EQ(facts.acquisition_events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 0);
    EXPECT_TRUE(facts.records.empty());
}

TEST_F(FakeAcquisitionTest, FailedCreateSlotBecomesReusableOnlyAfterActualAsynchronousCleanup) {
    plugin::ServiceRegistry empty;
    ASSERT_TRUE(empty.freeze().ok());
    auto missing = create_after_cleanup(empty, config());
    EXPECT_FALSE(missing.ok());
    auto duplicate = create_after_cleanup(services_, R"({"streams":["/first","/first"]})");
    EXPECT_EQ(duplicate.status().code(), StatusCode::InvalidArgument);
    auto negative = create_after_cleanup(services_, R"({"streams":["/first"],"period_us":-1})");
    EXPECT_EQ(negative.status().code(), StatusCode::InvalidArgument);
    create({{"record_limit", 1}});
    ASSERT_TRUE(handle_);  // The single actual Instance slot is reusable.
    EXPECT_TRUE(prepare().ok());
}

TEST_F(FakeAcquisitionTest, IllegalAttemptIsRejectedBySdkBeforeTheHostAndReportsTerminalFailure) {
    create({{"mode", "invalid_attempt"}, {"record_limit", 1}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_end());
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    EXPECT_EQ(facts.terminal_status, StatusCode::InvalidArgument);
    EXPECT_TRUE(facts.records.empty());
    EXPECT_EQ(facts.high_water_inflight, 0);
}

TEST_F(FakeAcquisitionTest, StaleOpaqueTokenBusinessRejectDoesNotCauseRetryOrPrivateBacklog) {
    create({{"mode", "stale_token"}, {"record_limit", 2}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_end());
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    EXPECT_EQ(facts.stale, 4);
    EXPECT_EQ(facts.terminal_status, StatusCode::Ok);
    EXPECT_TRUE(facts.records.empty());
    EXPECT_EQ(facts.source_events[RODAQ_SOURCE_EVENT_SESSION_ENDED], 2);
}

TEST_F(FakeAcquisitionTest, OversizeFaultRetainsValidBackingAndAcceptsCoreBusinessRejection) {
    receiver_->max_bytes = 4;
    create({{"mode", "oversize_attempt"}, {"record_limit", 1}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare(4).ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_end());
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    EXPECT_EQ(facts.oversized, 2);
    EXPECT_EQ(facts.terminal_status, StatusCode::Ok);
    EXPECT_TRUE(facts.records.empty());
}

TEST_F(
    FakeAcquisitionTest,
    NativeProducerRestartUsesNewSourceIdentityInsteadOfFalseSequenceContinuity) {
    create({{"record_limit", 1}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_records(2));
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    handle_.reset();
    ASSERT_TRUE(instance_->close(soon()).ok());
    instance_.reset();
    create({{"record_limit", 1}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_records(4));
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    ASSERT_EQ(facts.records.size(), 4);
    EXPECT_EQ(facts.records[0].stream, facts.records[2].stream);
    EXPECT_EQ(facts.records[0].source_sequence, 1);
    EXPECT_EQ(facts.records[2].source_sequence, 1);
    EXPECT_FALSE(std::equal(
        std::begin(facts.records[0].source.data), std::end(facts.records[0].source.data),
        std::begin(facts.records[2].source.data)));
}

TEST_F(
    FakeAcquisitionTest,
    StopDuringDescriptorProposalRetainsCallAndPreventsFollowingRecordEmission) {
    receiver_->block_changed_descriptor();
    create({{"record_limit", 4}, {"schema_change_after", 1}});
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle_->start(soon()).ok());
    ASSERT_TRUE(receiver_->wait_descriptor_inflight());
    EXPECT_EQ(receiver_->snapshot().records.size(), 2);
    EXPECT_EQ(
        handle_->quiesce(plugin::Deadline::after(5ms).value()).status().code(),
        StatusCode::DeadlineExceeded);
    EXPECT_EQ(receiver_->snapshot().descriptor_inflight, 1);
    receiver_->release();
    ASSERT_TRUE(handle_->quiesce(soon()).ok());
    const auto facts = receiver_->snapshot();
    EXPECT_EQ(facts.records.size(), 2);
    EXPECT_EQ(facts.schemas.size(), 3);
    EXPECT_EQ(facts.descriptor_inflight, 0);
    EXPECT_EQ(facts.source_events[RODAQ_SOURCE_EVENT_SESSION_ENDED], 2);
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
    ASSERT_TRUE(receiver_->wait_end());
    if (mode == "fail_quiesce") {
        EXPECT_FALSE(handle_->quiesce(soon()).ok());
    } else if (mode == "fail_shutdown") {
        EXPECT_TRUE(handle_->quiesce(soon()).ok());
        EXPECT_FALSE(instance_->close(soon()).ok());
    } else {
        EXPECT_TRUE(handle_->quiesce(soon()).ok());
        EXPECT_EQ(receiver_->snapshot().terminal_status, StatusCode::Unavailable);
        EXPECT_TRUE(receiver_->snapshot().records.empty());
    }
}

INSTANTIATE_TEST_SUITE_P(
    Faults, FakeAcquisitionFailureTest,
    testing::Values(
        "fail_prepare", "fail_start", "fail_quiesce", "fail_shutdown", "terminal_event"));

}  // namespace
