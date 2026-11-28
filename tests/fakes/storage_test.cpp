#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/storage/interface.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;
namespace plugin = rodaq::plugin;
namespace storage = rodaq::storage;
using rodaq::StatusCode;
using namespace std::chrono_literals;

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

// Independent reader: no encoder, native struct layout or backend-private code.
class Reader {
public:
    explicit Reader(const fs::path& file) {
        std::ifstream input(file, std::ios::binary);
        bytes_ = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    std::uint64_t number(unsigned width) {
        if (width > 8 || offset_ > bytes_.size() || width > bytes_.size() - offset_) {
            throw std::runtime_error("truncated Fake file");
        }
        std::uint64_t result = 0;
        for (unsigned i = 0; i < width; ++i) {
            result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[offset_++]))
                      << (8 * i);
        }
        return result;
    }

    std::string bytes(std::size_t size) {
        if (offset_ > bytes_.size() || size > bytes_.size() - offset_) {
            throw std::runtime_error("truncated Fake bytes");
        }
        auto result = bytes_.substr(offset_, size);
        offset_ += size;
        return result;
    }

    std::string blob() { return bytes(static_cast<std::size_t>(number(4))); }
    std::size_t position() const noexcept { return offset_; }
    std::size_t size() const noexcept { return bytes_.size(); }

private:
    std::string bytes_;
    std::size_t offset_{0};
};

class FakeStorageTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-fake-store-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        package_ = root_ / "package";
        fs::copy(RODAQ_FAKE_STORAGE_PACKAGE, package_, fs::copy_options::recursive);
        plugin::PluginManagerOptions policy;
        policy.search_paths = {package_};
        policy.max_loaded_plugins = 1;
        policy.max_instances = 1;
        policy.max_config_size = 1024;
        policy.shutdown_timeout = 2s;
        auto manager = plugin::PluginManager::create(std::move(policy));
        ASSERT_TRUE(manager.ok());
        manager_.emplace(std::move(manager.value()));
        auto library = manager_->load("rodaq.fake.storage");
        ASSERT_TRUE(library.ok()) << library.status().message();
        library_.emplace(std::move(library.value()));
    }

    void TearDown() override {
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

    void create_unprepared(std::string_view config) {
        auto instance = library_->create_instance(config);
        ASSERT_TRUE(instance.ok()) << instance.status().message();
        instance_.emplace(std::move(instance.value()));
        auto handle = instance_->query<storage::Interface>();
        ASSERT_TRUE(handle.ok());
        handle_.emplace(std::move(handle.value()));
    }

    void create(std::string_view config = "{}") {
        create_unprepared(config);
        ASSERT_TRUE(handle_);
        ASSERT_TRUE(handle_->prepare(limits(), soon()).ok());
    }

    static storage::PrepareConfig limits() {
        storage::PrepareConfig cfg{};
        cfg.header.struct_size = sizeof(cfg);
        cfg.max_record_bytes = 64;
        cfg.max_batch_bytes = 256;
        cfg.max_batch_records = 4;
        cfg.max_descriptors_per_segment = 4;
        cfg.max_segment_records = 100;
        cfg.max_segment_input_bytes = 4096;
        cfg.max_segment_physical_bytes = 65536;
        cfg.max_segment_file_count = 1;
        cfg.max_relative_path_bytes = 32;
        return cfg;
    }

    storage::StreamDescriptor descriptor(std::uint64_t stream = 1, std::uint64_t id = 1) const {
        storage::StreamDescriptor result{};
        result.header.struct_size = sizeof(result);
        result.stream_id = stream;
        result.descriptor_id = id;
        result.channel_name = {"/fake", 5};
        result.message_encoding = {"json", 4};
        result.schema_name = {"schema", 6};
        result.schema_encoding = {"jsonschema", 10};
        result.schema = {reinterpret_cast<const std::uint8_t*>("{}"), 2};
        return result;
    }

    void open(std::uint64_t id = 1) {
        staging_ = root_ / (std::to_string(id) + ".tmp");
        fs::create_directory(staging_);
        const auto path = staging_.string();
        const storage::SegmentSpec spec{
            {sizeof(storage::SegmentSpec)},
            id,
            {path.data(), static_cast<std::uint32_t>(path.size())}};
        ASSERT_TRUE(handle_->open_segment(spec, soon()).ok());
    }

    storage::Record record(
        std::uint64_t stream = 1, std::uint64_t descriptor_id = 1,
        std::uint64_t recording_time = 100) const {
        storage::Record result{};
        result.header.struct_size = sizeof(result);
        result.stream_id = stream;
        result.descriptor_id = descriptor_id;
        result.recording_time = recording_time;
        result.publish_time = 0;
        result.has_publish_time = 0;
        result.payload = {payload_.data(), static_cast<std::uint32_t>(payload_.size())};
        return result;
    }

    std::vector<std::uint64_t> read(std::uint64_t expected_records) {
        Reader reader(staging_ / "records.fake");
        EXPECT_EQ(reader.bytes(8), "RDAQFK2\n");
        (void)reader.number(8);  // segment_id
        std::vector<std::uint64_t> times;
        while (reader.position() < reader.size()) {
            const auto kind = reader.number(1);
            if (kind == 3) {
                EXPECT_EQ(reader.number(8), expected_records);
                EXPECT_EQ(reader.position(), reader.size());
                return times;
            }
            const auto size = reader.number(4);
            const auto end = reader.position() + size;
            if (kind == 1) {
                EXPECT_NE(reader.number(8), 0U);
                EXPECT_NE(reader.number(8), 0U);
                EXPECT_EQ(reader.blob(), "/fake");
                EXPECT_EQ(reader.blob(), "json");
                EXPECT_EQ(reader.blob(), "schema");
                EXPECT_EQ(reader.blob(), "jsonschema");
                EXPECT_EQ(reader.blob(), "{}");
                EXPECT_EQ(reader.blob(), "");
                EXPECT_EQ(reader.blob(), "");
            } else if (kind == 2) {
                EXPECT_NE(reader.number(8), 0U);
                EXPECT_NE(reader.number(8), 0U);
                times.push_back(reader.number(8));
                EXPECT_EQ(reader.number(8), 0U);
                EXPECT_EQ(reader.number(1), 0U);
                EXPECT_EQ(reader.blob(), std::string("\x01\x02\x03\x04", 4));
            } else {
                ADD_FAILURE() << "Unknown frame kind";
                return {};
            }
            EXPECT_EQ(reader.position(), end);
        }
        ADD_FAILURE() << "Missing seal footer";
        return {};
    }

    fs::path root_;
    fs::path package_;
    fs::path staging_;
    std::optional<plugin::PluginManager> manager_;
    std::optional<plugin::PluginLibrary> library_;
    std::optional<plugin::PluginInstance> instance_;
    std::optional<storage::Handle> handle_;
    std::array<std::uint8_t, 4> payload_{1, 2, 3, 4};
};

TEST_F(FakeStorageTest, MultiStreamFilesAreSelfDecodingAndPreparedDescriptorsSurviveRotation) {
    create();
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(handle_->prepare_descriptor(descriptor(), soon()).ok());
    ASSERT_TRUE(handle_->prepare_descriptor(descriptor(2, 1), soon()).ok());
    open();
    std::array<storage::Record, 3> records{record(1, 1, 10), record(2, 1, 20), record(1, 1, 30)};
    auto written = handle_->write_batch({records.data(), records.size()}, soon());
    ASSERT_TRUE(written.status.ok());
    ASSERT_EQ(written.batch_result.confirmed_prefix, 3U);
    const auto usage = handle_->segment_usage();
    ASSERT_TRUE(usage.ok());
    auto sealed = handle_->seal_segment(soon());
    ASSERT_TRUE(sealed.ok());
    EXPECT_EQ(sealed.value().files.size(), 1U);
    EXPECT_EQ(sealed.value().total_physical_bytes, fs::file_size(staging_ / "records.fake"));
    EXPECT_EQ(read(3), (std::vector<std::uint64_t>{10, 20, 30}));
    open(2);
    auto next = record(1, 1, 40);
    EXPECT_TRUE(handle_->write_batch({&next, 1}, soon()).status.ok());
    ASSERT_TRUE(handle_->seal_segment(soon()).ok());
    EXPECT_EQ(read(1), (std::vector<std::uint64_t>{40}));
}

TEST_F(FakeStorageTest, PartialUnwrittenKeepsPrefixAndOnlyConfirmedRecordsAppearInSealedFile) {
    create(R"({"mode":"prefix_unwritten","prefix":1})");
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(handle_->prepare_descriptor(descriptor(), soon()).ok());
    open();
    std::array<storage::Record, 3> records{record(1, 1, 10), record(1, 1, 20), record(1, 1, 30)};
    auto result = handle_->write_batch({records.data(), records.size()}, soon());
    EXPECT_EQ(result.status.code(), StatusCode::Unavailable);
    EXPECT_EQ(result.batch_result.confirmed_prefix, 1U);
    EXPECT_EQ(result.batch_result.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);
    ASSERT_TRUE(handle_->seal_segment(soon()).ok());
    EXPECT_EQ(read(1), (std::vector<std::uint64_t>{10}));
}

TEST_F(FakeStorageTest, UnknownSuffixMayHavePhysicalEffectsAndCannotSealOrContinueNormalWrites) {
    create(R"({"mode":"prefix_unknown","prefix":1})");
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(handle_->prepare_descriptor(descriptor(), soon()).ok());
    open();
    std::array<storage::Record, 3> records{record(1, 1, 10), record(1, 1, 20), record(1, 1, 30)};
    auto result = handle_->write_batch({records.data(), records.size()}, soon());
    EXPECT_EQ(result.status.code(), StatusCode::Unavailable);
    EXPECT_EQ(result.batch_result.confirmed_prefix, 1U);
    EXPECT_EQ(result.batch_result.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);
    EXPECT_GT(handle_->segment_usage().value().physical_bytes, 16ULL);
    EXPECT_FALSE(handle_->seal_segment(soon()).ok());
    EXPECT_FALSE(handle_->write_batch({records.data(), 1}, soon()).status.ok());
}

TEST_F(FakeStorageTest, NonOkAfterFullConfirmedPrefixDoesNotEraseWrittenFacts) {
    create(R"({"mode":"full_failed"})");
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(handle_->prepare_descriptor(descriptor(), soon()).ok());
    open();
    auto value = record(1, 1, 10);
    auto result = handle_->write_batch({&value, 1}, soon());
    EXPECT_EQ(result.status.code(), StatusCode::Unavailable);
    EXPECT_EQ(result.batch_result.confirmed_prefix, 1U);
    EXPECT_EQ(result.batch_result.tail_state, RODAQ_STORAGE_TAIL_NONE);
    ASSERT_TRUE(handle_->seal_segment(soon()).ok());
    EXPECT_EQ(read(1), (std::vector<std::uint64_t>{10}));
}

TEST_F(FakeStorageTest, ActualInvalidAbiOutputBecomesEntireDispatchedBatchUnknown) {
    create(R"({"mode":"invalid_output"})");
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(handle_->prepare_descriptor(descriptor(), soon()).ok());
    open();
    auto value = record();
    auto result = handle_->write_batch({&value, 1}, soon());
    EXPECT_EQ(result.status.code(), StatusCode::Internal);
    EXPECT_EQ(result.batch_result.confirmed_prefix, 0U);
    EXPECT_EQ(result.batch_result.tail_state, RODAQ_STORAGE_TAIL_UNKNOWN);
}

TEST_F(FakeStorageTest, ActualLateReturnRetainsNativeInputsAndCanFinishAfterObserverDeadline) {
    create(R"({"mode":"late_write","delay_ms":100})");
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(handle_->prepare_descriptor(descriptor(), soon()).ok());
    open();
    auto value = record(1, 1, 10);
    auto work = std::async(std::launch::async, [&] {
        return handle_->write_batch({&value, 1}, plugin::Deadline::after(1ms).value());
    });
    EXPECT_EQ(work.wait_for(5ms), std::future_status::timeout);
    auto result = work.get();
    EXPECT_TRUE(result.status.ok());
    EXPECT_EQ(result.batch_result.confirmed_prefix, 1U);
    ASSERT_TRUE(handle_->seal_segment(soon()).ok());
    EXPECT_EQ(read(1), (std::vector<std::uint64_t>{10}));
}

TEST_F(FakeStorageTest, DescriptorByteBudgetRejectsBeforeRetainingFacts) {
    create(R"({"catalog_slots":4,"descriptor_bytes":128})");
    ASSERT_TRUE(handle_);
    EXPECT_EQ(
        handle_->prepare_descriptor(descriptor(), soon()).status().code(),
        StatusCode::ResourceExhausted);
}

TEST_F(FakeStorageTest, FixedCatalogExhaustionAndIdentityConflictNeverReplacePreparedFacts) {
    create(R"({"catalog_slots":4})");
    ASSERT_TRUE(handle_);
    for (std::uint64_t id = 1; id <= 4; ++id) {
        ASSERT_TRUE(handle_->prepare_descriptor(descriptor(id, id), soon()).ok());
    }
    EXPECT_EQ(
        handle_->prepare_descriptor(descriptor(5, 5), soon()).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_TRUE(handle_->prepare_descriptor(descriptor(), soon()).ok());
    auto conflict = descriptor();
    conflict.channel_name = {"/new", 4};
    EXPECT_EQ(handle_->prepare_descriptor(conflict, soon()).status().code(), StatusCode::Conflict);
    open();
    auto value = record(1, 1, 10);
    ASSERT_TRUE(handle_->write_batch({&value, 1}, soon()).status.ok());
    ASSERT_TRUE(handle_->seal_segment(soon()).ok());
    EXPECT_EQ(read(1), (std::vector<std::uint64_t>{10}));
}

TEST_F(FakeStorageTest, PreparedEnvelopeRejectsOversizedRecordBeforeEffects) {
    create();
    ASSERT_TRUE(handle_);
    ASSERT_TRUE(handle_->prepare_descriptor(descriptor(), soon()).ok());
    open();
    auto value = record();
    std::array<std::uint8_t, 65> oversized{};
    value.payload = {oversized.data(), 65};
    auto rejected = handle_->write_batch({&value, 1}, soon());
    EXPECT_EQ(rejected.status.code(), StatusCode::InvalidArgument);
    EXPECT_EQ(rejected.batch_result.confirmed_prefix, 0U);
    EXPECT_EQ(rejected.batch_result.tail_state, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN);
    EXPECT_EQ(handle_->segment_usage().value().physical_bytes, 16ULL);
}

TEST_F(FakeStorageTest, PermanentNativeHangRemainsLiveUntilItsDisposableProcessIsSupervised) {
    const auto staging = root_ / "hung.tmp";
    fs::create_directory(staging);
    const auto package = package_.string();
    const auto path = staging.string();
    char* arguments[]{
        const_cast<char*>(RODAQ_FAKE_STORAGE_HANG_PROBE), const_cast<char*>(package.c_str()),
        const_cast<char*>(path.c_str()), nullptr};
    pid_t child = -1;
    ASSERT_EQ(
        posix_spawn(&child, RODAQ_FAKE_STORAGE_HANG_PROBE, nullptr, nullptr, arguments, environ),
        0);
    const auto limit = std::chrono::steady_clock::now() + 2s;
    bool entered = false;
    bool exited = false;
    int status = 0;
    while (std::chrono::steady_clock::now() < limit) {
        if (waitpid(child, &status, WNOHANG) == child) {
            exited = true;
            break;
        }
        std::error_code error;
        if (fs::file_size(staging / "records.fake", error) == 17 && !error) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(entered);
    EXPECT_FALSE(exited);
    if (!exited) {
        std::this_thread::sleep_for(5ms);
        const auto waited = waitpid(child, &status, WNOHANG);
        EXPECT_EQ(waited, 0);
        if (waited == 0) {
            EXPECT_EQ(kill(child, SIGKILL), 0);
            EXPECT_EQ(waitpid(child, &status, 0), child);
            EXPECT_TRUE(WIFSIGNALED(status));
            EXPECT_EQ(WTERMSIG(status), SIGKILL);
        }
    }
    EXPECT_FALSE(fs::exists(staging / "manifest.json"));
}

TEST_F(FakeStorageTest, CreatedInstanceQuiescesIdempotentlyAndCannotPrepareAfterward) {
    create_unprepared("{}");
    ASSERT_TRUE(handle_);
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_TRUE(handle_->quiesce(soon()).ok());
    EXPECT_EQ(handle_->prepare(limits(), soon()).status().code(), StatusCode::InvalidState);
    EXPECT_EQ(
        handle_->prepare_descriptor(descriptor(), soon()).status().code(),
        StatusCode::InvalidState);
}

class FakeStorageFailureTest : public FakeStorageTest,
                               public testing::WithParamInterface<const char*> {};

TEST_P(FakeStorageFailureTest, ActualTypedOperationReportsInjectedFailureWithoutInventingSeal) {
    const std::string mode = GetParam();
    create_unprepared("{\"mode\":\"" + mode + "\"}");
    ASSERT_TRUE(handle_);
    auto prepared = handle_->prepare(limits(), soon());
    if (mode == "fail_prepare") {
        EXPECT_FALSE(prepared.ok());
        EXPECT_TRUE(handle_->quiesce(soon()).ok());
        EXPECT_TRUE(handle_->quiesce(soon()).ok());
        return;
    }
    ASSERT_TRUE(prepared.ok());
    auto descriptor_prepared = handle_->prepare_descriptor(descriptor(), soon());
    if (mode == "fail_descriptor") {
        EXPECT_FALSE(descriptor_prepared.ok());
        return;
    }
    ASSERT_TRUE(descriptor_prepared.ok());
    if (mode == "fail_open") {
        fs::create_directory(root_ / "failed.tmp");
        const auto path = (root_ / "failed.tmp").string();
        const storage::SegmentSpec spec{
            {sizeof(storage::SegmentSpec)},
            1,
            {path.data(), static_cast<std::uint32_t>(path.size())}};
        EXPECT_FALSE(handle_->open_segment(spec, soon()).ok());
        EXPECT_TRUE(fs::is_empty(root_ / "failed.tmp"));
        return;
    }
    open();
    auto value = record();
    ASSERT_TRUE(handle_->write_batch({&value, 1}, soon()).status.ok());
    if (mode == "fail_seal") {
        EXPECT_FALSE(handle_->seal_segment(soon()).ok());
        EXPECT_TRUE(fs::exists(staging_ / "records.fake"));
    } else if (mode == "fail_quiesce") {
        EXPECT_FALSE(handle_->quiesce(soon()).ok());
    } else {
        ASSERT_EQ(mode, "fail_shutdown");
        ASSERT_TRUE(handle_->quiesce(soon()).ok());
        EXPECT_FALSE(instance_->close(soon()).ok());
    }
}

INSTANTIATE_TEST_SUITE_P(
    Faults, FakeStorageFailureTest,
    testing::Values(
        "fail_prepare", "fail_descriptor", "fail_open", "fail_seal", "fail_quiesce",
        "fail_shutdown"));

}  // namespace
