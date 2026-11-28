#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <rodaq/daq/runtime.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/config.hpp>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

struct TestWorkspace {
    fs::path path;

    explicit TestWorkspace(const std::string& name) {
        path = fs::temp_directory_path() /
               ("rodaq-system-test-" + name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path / "packages");
        fs::create_directories(path / "state");
        fs::create_directories(path / "volume");
        fs::copy(
            fs::path(RODAQ_SYSTEM_STORAGE_PACKAGE) / "rodaq.fake.storage",
            path / "packages" / "rodaq.fake.storage", fs::copy_options::recursive);
        fs::copy(
            fs::path(RODAQ_SYSTEM_ACQUISITION_PACKAGE) / "rodaq.fake.acquisition",
            path / "packages" / "rodaq.fake.acquisition", fs::copy_options::recursive);
    }

    ~TestWorkspace() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

rodaq::DaqOpenOptions make_options(
    const fs::path& root, rodaq::DaqOpenMode mode, std::chrono::nanoseconds snapshot_max_age = 2s) {
    rodaq::DaqOpenOptions opts;
    opts.state_root = root / "state";
    opts.volume_root = root / "volume";
    opts.mode = mode;
    opts.plugin_search_paths = {root / "packages"};
    opts.qualified_runtime_profile_id = "test-profile-v1";
    opts.max_tasks = 2;
    opts.max_pipelines_per_task = 2;
    opts.system_snapshot_max_age = snapshot_max_age;
    opts.volume.name = "local";
    opts.volume.max_managed_bytes = 256ull * 1024 * 1024;
    opts.volume.max_commit_bytes = 32ull * 1024 * 1024;
    opts.volume.stop_free_bytes = 1024;
    opts.volume.low_free_bytes = 2048;
    opts.volume.recovery_free_bytes = 4096;
    opts.volume.max_segments = 8;
    opts.volume.max_commit_slots = 4;
    opts.volume.max_artifacts_per_volume = 64;

    rodaq::DaqResourceProfile profile;
    profile.name = "default";
    profile.max_records = 64;
    profile.max_logical_bytes = 4096;
    profile.payload_pool_bytes = 32768;
    profile.max_payload_bytes = 256;
    profile.payload_min_chunk_bytes = 256;
    profile.payload_max_chunks = 64;
    profile.queue_capacity = 32;
    profile.callback_concurrency = 2;
    profile.max_segment_physical_bytes = 1024 * 1024;
    profile.segment_reservation_bytes = 2 * 1024 * 1024;
    profile.max_segment_records = 1000;
    profile.max_segment_input_bytes = 256 * 1024;
    profile.max_batch_records = 16;
    profile.max_batch_input_bytes = 4096;
    profile.max_streams = 8;
    profile.max_descriptors = 16;
    profile.max_sealed_files = 4;
    profile.max_manifest_bytes = 65536;
    profile.max_prepared_descriptors = 16;
    opts.profiles.push_back(std::move(profile));

    opts.allowed_backends.push_back(
        {"rodaq.fake.acquisition", "1.0.0", rodaq::BackendRole::Acquisition, "",
         rodaq::SupportStatus::QualifiedCandidate, rodaq::QualificationStatus::Qualified});
    opts.allowed_backends.push_back(
        {"rodaq.fake.storage", "1.0.0", rodaq::BackendRole::Storage, "fake",
         rodaq::SupportStatus::Required, rodaq::QualificationStatus::Qualified});
    opts.allowed_backends.push_back(
        {"rodaq.not.installed", "1.0.0", rodaq::BackendRole::Acquisition, "",
         rodaq::SupportStatus::Deferred, rodaq::QualificationStatus::NotQualified});
    return opts;
}

rodaq::TaskConfig make_task_config(const std::string& name) {
    rodaq::TaskConfig config;
    config.name = name;
    config.segment_policy.max_duration = 60s;
    config.retention_policy.mode = rodaq::RetentionMode::KeepAll;

    rodaq::PipelineConfig pipeline;
    pipeline.required = true;
    pipeline.resource_profile = "default";
    pipeline.output_root = "local";
    pipeline.acquisition = {
        "rodaq.fake.acquisition", "1.0.0", R"({"streams":["s0"],"period_us":2000})"};
    pipeline.storage = {"rodaq.fake.storage", "1.0.0", "{}"};
    pipeline.streams.push_back({0, "s0", "s0", rodaq::ReadinessPolicy::EntryReady});
    config.pipelines.push_back(std::move(pipeline));
    return config;
}

TEST(SystemServiceTest, PublicDaqRuntimeSnapshotCapabilitiesAndFreshness) {
    TestWorkspace dir("snapshot");
    auto runtime =
        rodaq::DaqRuntime::open(make_options(dir.path, rodaq::DaqOpenMode::Initialize, 250ms));
    ASSERT_TRUE(runtime.ok()) << static_cast<int>(runtime.status().code());

    const auto caps = runtime.value()->system().capabilities();
    ASSERT_TRUE(caps.ok());
    EXPECT_FALSE(caps.value().application_version.empty());
    ASSERT_EQ(caps.value().output_roots.size(), 1u);
    EXPECT_EQ(caps.value().output_roots[0], "local");
    ASSERT_EQ(caps.value().resource_profiles.size(), 1u);
    EXPECT_EQ(caps.value().resource_profiles[0].name, "default");
    EXPECT_EQ(caps.value().resource_profiles[0].max_segment_physical_bytes, 1024u * 1024u);

    bool found_acq = false;
    bool found_storage = false;
    bool found_uninstalled = false;
    for (const auto& backend : caps.value().backends) {
        if (backend.plugin_id == "rodaq.fake.acquisition" && backend.plugin_version == "1.0.0") {
            found_acq = true;
            EXPECT_TRUE(backend.installed);
            EXPECT_TRUE(backend.allowed);
            ASSERT_TRUE(backend.role.has_value());
            EXPECT_EQ(*backend.role, rodaq::BackendRole::Acquisition);
            EXPECT_EQ(backend.support_status, rodaq::SupportStatus::QualifiedCandidate);
            EXPECT_EQ(backend.qualification_status, rodaq::QualificationStatus::Qualified);
        } else if (backend.plugin_id == "rodaq.fake.storage" && backend.plugin_version == "1.0.0") {
            found_storage = true;
            EXPECT_TRUE(backend.installed);
            EXPECT_TRUE(backend.allowed);
            ASSERT_TRUE(backend.role.has_value());
            EXPECT_EQ(*backend.role, rodaq::BackendRole::Storage);
            EXPECT_EQ(backend.support_status, rodaq::SupportStatus::Required);
            EXPECT_EQ(backend.qualification_status, rodaq::QualificationStatus::Qualified);
        } else if (
            backend.plugin_id == "rodaq.not.installed" && backend.plugin_version == "1.0.0") {
            found_uninstalled = true;
            EXPECT_FALSE(backend.installed);
            EXPECT_TRUE(backend.allowed);
            ASSERT_TRUE(backend.role.has_value());
            EXPECT_EQ(*backend.role, rodaq::BackendRole::Acquisition);
            EXPECT_EQ(backend.support_status, rodaq::SupportStatus::Deferred);
            EXPECT_EQ(backend.qualification_status, rodaq::QualificationStatus::NotQualified);
        }
    }
    EXPECT_TRUE(found_acq);
    EXPECT_TRUE(found_storage);
    EXPECT_TRUE(found_uninstalled);

    ASSERT_TRUE(runtime.value()->observe().ok());
    const auto snap1 = runtime.value()->system().snapshot();
    ASSERT_TRUE(snap1.ok());
    EXPECT_TRUE(snap1.value().core_ready);
    EXPECT_EQ(snap1.value().freshness, rodaq::SnapshotFreshness::Fresh);
    EXPECT_GT(snap1.value().snapshot_sequence, 0u);
    EXPECT_GT(snap1.value().resources.rss_bytes, 0u);
    EXPECT_GT(snap1.value().resources.threads, 0u);
    EXPECT_GT(snap1.value().resources.open_fds, 0u);
    ASSERT_EQ(snap1.value().volumes.size(), 1u);
    EXPECT_EQ(snap1.value().volumes[0].name, "local");
    EXPECT_GT(snap1.value().volumes[0].total_bytes, 0u);
    EXPECT_GT(snap1.value().volumes[0].free_bytes, 0u);
    EXPECT_EQ(snap1.value().volumes[0].pressure, rodaq::VolumePressure::Normal);

    std::this_thread::sleep_for(300ms);
    const auto stale = runtime.value()->system().snapshot();
    ASSERT_TRUE(stale.ok());
    EXPECT_EQ(stale.value().freshness, rodaq::SnapshotFreshness::Stale);
    EXPECT_EQ(stale.value().snapshot_sequence, snap1.value().snapshot_sequence);

    ASSERT_TRUE(runtime.value()->observe().ok());
    const auto snap2 = runtime.value()->system().snapshot();
    ASSERT_TRUE(snap2.ok());
    EXPECT_EQ(snap2.value().freshness, rodaq::SnapshotFreshness::Fresh);
    EXPECT_GT(snap2.value().snapshot_sequence, snap1.value().snapshot_sequence);

    ASSERT_TRUE(runtime.value()->shutdown(std::chrono::steady_clock::now() + 10s).ok());
    const auto after_shutdown = runtime.value()->system().snapshot();
    ASSERT_TRUE(after_shutdown.ok());
    EXPECT_FALSE(after_shutdown.value().core_ready);
}

TEST(SystemServiceTest, PublicRuntimeExecutesEndToEndTaskRecordingAndArtifactRead) {
    TestWorkspace dir("e2e");
    auto runtime = rodaq::DaqRuntime::open(make_options(dir.path, rodaq::DaqOpenMode::Initialize));
    ASSERT_TRUE(runtime.ok()) << static_cast<int>(runtime.status().code());
    struct Guard {
        rodaq::DaqRuntime* rt{nullptr};
        ~Guard() {
            if (rt) {
                (void)rt->shutdown(std::chrono::steady_clock::now() + 10s);
            }
        }
    } guard{runtime.value().get()};

    const auto created = runtime.value()->tasks().create_task(
        make_task_config("public-e2e"), rodaq::RequestIdentity{"create-e2e-1"});
    ASSERT_TRUE(created.ok()) << static_cast<int>(created.status().code());
    const auto task_id = created.value();
    const auto initial = runtime.value()->tasks().get_task(task_id);
    ASSERT_TRUE(initial.ok());

    const auto started = runtime.value()->tasks().start(
        task_id, rodaq::RequestIdentity{"start-e2e-1"},
        rodaq::ControlPrecondition{true, initial.value().control_generation});
    ASSERT_TRUE(started.ok()) << static_cast<int>(started.status().code());

    auto wait_terminal = [&](const rodaq::OperationId& op_id) -> rodaq::OperationState {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto op = runtime.value()->tasks().get_operation(task_id, op_id);
            if (op.ok() && (op.value().state == rodaq::OperationState::Succeeded ||
                            op.value().state == rodaq::OperationState::Failed)) {
                return op.value().state;
            }
            (void)runtime.value()->observe();
            std::this_thread::sleep_for(15ms);
        }
        return rodaq::OperationState::Accepted;
    };

    ASSERT_EQ(wait_terminal(started.value()), rodaq::OperationState::Succeeded);

    const auto record_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < record_deadline) {
        (void)runtime.value()->observe();
        const auto rt = runtime.value()->tasks().get_runtime_snapshot(task_id);
        if (rt.ok() && !rt.value().pipelines.empty() && rt.value().pipelines[0].facts &&
            rt.value().pipelines[0].facts->accepted_records > 0) {
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    (void)runtime.value()->observe();

    const auto detail = runtime.value()->tasks().get_task(task_id);
    ASSERT_TRUE(detail.ok());
    const auto stopped = runtime.value()->tasks().stop(
        task_id, rodaq::RequestIdentity{"stop-e2e-1"},
        rodaq::ControlPrecondition{true, detail.value().control_generation});
    ASSERT_TRUE(stopped.ok());
    ASSERT_EQ(wait_terminal(stopped.value()), rodaq::OperationState::Succeeded);

    rodaq::ArtifactQuery query;
    query.task_id = task_id;
    const auto artifacts = runtime.value()->artifacts().query_artifacts(query);
    ASSERT_TRUE(artifacts.ok());
    ASSERT_FALSE(artifacts.value().items.empty());
    EXPECT_GT(artifacts.value().items[0].physical_bytes, 0u);

    const auto info =
        runtime.value()->artifacts().get_artifact(artifacts.value().items[0].artifact_id);
    ASSERT_TRUE(info.ok());
    ASSERT_FALSE(info.value().files.empty());
    auto reader = runtime.value()->artifacts().acquire_read(
        info.value().summary.artifact_id, info.value().files[0].file_id);
    ASSERT_TRUE(reader.ok()) << static_cast<int>(reader.status().code());
    std::vector<std::byte> buf(4096);
    const auto read_res = reader.value().read_at(0, rodaq::MutableBytes{buf.data(), buf.size()});
    ASSERT_TRUE(read_res.ok());
    EXPECT_GT(read_res.value(), 0u);

    ASSERT_TRUE(runtime.value()->shutdown(std::chrono::steady_clock::now() + 10s).ok());
    guard.rt = nullptr;
}

TEST(SystemServiceTest, InstalledUnallowedPluginNeverGuessesRole) {
    TestWorkspace dir("unallowed");
    auto opts = make_options(dir.path, rodaq::DaqOpenMode::Initialize);
    opts.allowed_backends.erase(opts.allowed_backends.begin());
    auto runtime = rodaq::DaqRuntime::open(std::move(opts));
    ASSERT_TRUE(runtime.ok()) << static_cast<int>(runtime.status().code());

    const auto caps = runtime.value()->system().capabilities();
    ASSERT_TRUE(caps.ok());
    bool found_unallowed = false;
    for (const auto& backend : caps.value().backends) {
        if (backend.plugin_id == "rodaq.fake.acquisition" && backend.plugin_version == "1.0.0") {
            found_unallowed = true;
            EXPECT_TRUE(backend.installed);
            EXPECT_FALSE(backend.allowed);
            EXPECT_FALSE(backend.role.has_value());
            EXPECT_EQ(backend.support_status, rodaq::SupportStatus::Deferred);
            EXPECT_EQ(backend.qualification_status, rodaq::QualificationStatus::NotQualified);
        }
    }
    EXPECT_TRUE(found_unallowed);
    ASSERT_TRUE(runtime.value()->shutdown(std::chrono::steady_clock::now() + 10s).ok());
}

TEST(SystemServiceTest, InvalidOpenOptionsFailClosed) {
    TestWorkspace dir("invalid");
    auto opts = make_options(dir.path, rodaq::DaqOpenMode::Initialize);
    opts.profiles[0].max_records =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    const auto overflow = rodaq::DaqRuntime::open(opts);
    EXPECT_FALSE(overflow.ok());
    EXPECT_EQ(overflow.status().code(), rodaq::StatusCode::InvalidArgument);

    auto existing = make_options(dir.path, rodaq::DaqOpenMode::Existing);
    const auto missing = rodaq::DaqRuntime::open(existing);
    EXPECT_FALSE(missing.ok());
}

}  // namespace
