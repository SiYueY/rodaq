#include <rodaq/daq/runtime.hpp>
#include <rodaq/task/config.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rodaq::ControlPrecondition;
using rodaq::DesiredState;
using rodaq::OperationId;
using rodaq::OperationState;
using rodaq::RequestIdentity;
using rodaq::StatusCode;
using rodaq::TaskConfig;
using rodaq::TaskId;

class ResetRetireTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("rodaq-reset-retire-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_ / "packages");
        fs::create_directories(root_ / "state");
        fs::create_directories(root_ / "volume");
        fs::copy(
            fs::path(RODAQ_RESET_STORAGE_PACKAGE) / "rodaq.fake.storage",
            root_ / "packages/rodaq.fake.storage", fs::copy_options::recursive);
        fs::copy(
            fs::path(RODAQ_RESET_ACQUISITION_PACKAGE) / "rodaq.fake.acquisition",
            root_ / "packages/rodaq.fake.acquisition", fs::copy_options::recursive);
    }

    void TearDown() override {
        if (runtime_) {
            (void)runtime_->shutdown(std::chrono::steady_clock::now() + 5s);
            runtime_.reset();
        }
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    rodaq::DaqOpenOptions make_options(rodaq::DaqOpenMode mode) {
        rodaq::DaqOpenOptions opts;
        opts.state_root = root_ / "state";
        opts.volume_root = root_ / "volume";
        opts.mode = mode;
        opts.plugin_search_paths = {root_ / "packages"};
        opts.qualified_runtime_profile_id = "test-reset-retire";
        opts.max_tasks = 1;
        opts.max_pipelines_per_task = 2;
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
        profile.name = "basic";
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

        opts.allowed_backends = {
            {"rodaq.fake.acquisition", "1.0.0", rodaq::BackendRole::Acquisition, "",
             rodaq::SupportStatus::QualifiedCandidate, rodaq::QualificationStatus::Qualified},
            {"rodaq.fake.storage", "1.0.0", rodaq::BackendRole::Storage, "fake",
             rodaq::SupportStatus::Required, rodaq::QualificationStatus::Qualified}};
        return opts;
    }

    TaskConfig make_task_config(std::string name = "reset-retire-task") {
        TaskConfig config;
        config.name = std::move(name);
        config.segment_policy.max_duration = 60s;
        config.retention_policy.mode = rodaq::RetentionMode::KeepAll;
        rodaq::PipelineConfig pipe;
        pipe.required = true;
        pipe.resource_profile = "basic";
        pipe.output_root = "local";
        pipe.acquisition = {
            "rodaq.fake.acquisition", "1.0.0", R"({"streams":["sample"],"period_us":1000})"};
        pipe.storage = {"rodaq.fake.storage", "1.0.0", "{}"};
        pipe.streams.push_back({0, "sample", "sample", rodaq::ReadinessPolicy::EntryReady});
        config.pipelines.push_back(std::move(pipe));
        return config;
    }

    OperationState wait_operation(const TaskId& task_id, const OperationId& op_id) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto op = runtime_->tasks().get_operation(task_id, op_id);
            if (op.ok() && (op.value().state == OperationState::Succeeded ||
                            op.value().state == OperationState::PartiallySucceeded ||
                            op.value().state == OperationState::Failed ||
                            op.value().state == OperationState::Superseded)) {
                return op.value().state;
            }
            if (!op.ok() && op.status().code() == StatusCode::NotFound &&
                runtime_->tasks().get_task(task_id).status().code() == StatusCode::NotFound) {
                return OperationState::Succeeded;
            }
            (void)runtime_->observe();
            std::this_thread::sleep_for(15ms);
        }
        return OperationState::Accepted;
    }

    fs::path root_;
    std::unique_ptr<rodaq::DaqRuntime> runtime_;
};

TEST_F(ResetRetireTest, ResetDeletesArtifactsRetireFreesTaskSlotAndRestartRebuildsCatalog) {
    auto opened = rodaq::DaqRuntime::open(make_options(rodaq::DaqOpenMode::Initialize));
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    runtime_ = std::move(opened.value());

    auto created = runtime_->tasks().create_task(make_task_config(), RequestIdentity{"create-1"});
    ASSERT_TRUE(created.ok()) << created.status().message();
    const auto task_id = created.value();

    // Start, record, stop -> produces at least 1 committed artifact.
    auto gen = runtime_->tasks().get_task(task_id).value().control_generation;
    auto start_op = runtime_->tasks().start(
        task_id, RequestIdentity{"start-1"}, ControlPrecondition{true, gen});
    ASSERT_TRUE(start_op.ok());
    ASSERT_EQ(wait_operation(task_id, start_op.value()), OperationState::Succeeded);
    std::this_thread::sleep_for(100ms);
    (void)runtime_->observe();

    gen = runtime_->tasks().get_task(task_id).value().control_generation;
    auto stop_op =
        runtime_->tasks().stop(task_id, RequestIdentity{"stop-1"}, ControlPrecondition{true, gen});
    ASSERT_TRUE(stop_op.ok());
    ASSERT_EQ(wait_operation(task_id, stop_op.value()), OperationState::Succeeded);

    rodaq::ArtifactQuery query;
    query.task_id = task_id;
    auto artifacts = runtime_->artifacts().query_artifacts(query);
    ASSERT_TRUE(artifacts.ok());
    ASSERT_FALSE(artifacts.value().items.empty());

    // Retire must fail while committed artifacts still exist.
    gen = runtime_->tasks().get_task(task_id).value().control_generation;
    auto retire_blocked = runtime_->tasks().retire(
        task_id, RequestIdentity{"retire-blocked"}, ControlPrecondition{true, gen});
    if (retire_blocked.ok()) {
        EXPECT_EQ(wait_operation(task_id, retire_blocked.value()), OperationState::Failed);
    } else {
        EXPECT_NE(retire_blocked.status().code(), StatusCode::Ok);
    }

    // Restart: Task returns as Stopped, Artifacts are synchronously rebuilt and visible.
    ASSERT_TRUE(runtime_->shutdown(std::chrono::steady_clock::now() + 5s).ok());
    runtime_.reset();
    auto reopened = rodaq::DaqRuntime::open(make_options(rodaq::DaqOpenMode::Existing));
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    runtime_ = std::move(reopened.value());
    EXPECT_EQ(runtime_->tasks().get_task(task_id).value().desired_state, DesiredState::Stopped);
    EXPECT_FALSE(runtime_->artifacts().query_artifacts(query).value().items.empty());

    // Reset removes all artifacts belonging to the Task.
    gen = runtime_->tasks().get_task(task_id).value().control_generation;
    auto reset_op = runtime_->tasks().reset(
        task_id, RequestIdentity{"reset-1"}, ControlPrecondition{true, gen});
    ASSERT_TRUE(reset_op.ok()) << reset_op.status().message();
    ASSERT_EQ(wait_operation(task_id, reset_op.value()), OperationState::Succeeded);
    EXPECT_TRUE(runtime_->artifacts().query_artifacts(query).value().items.empty());

    // Retire succeeds after Reset and frees the max_tasks=1 slot.
    gen = runtime_->tasks().get_task(task_id).value().control_generation;
    auto retire_op = runtime_->tasks().retire(
        task_id, RequestIdentity{"retire-1"}, ControlPrecondition{true, gen});
    ASSERT_TRUE(retire_op.ok()) << retire_op.status().message();
    ASSERT_EQ(wait_operation(task_id, retire_op.value()), OperationState::Succeeded);
    EXPECT_EQ(runtime_->tasks().get_task(task_id).status().code(), StatusCode::NotFound);

    // New Task can now be created in the freed slot.
    auto replacement =
        runtime_->tasks().create_task(make_task_config("second"), RequestIdentity{"create-2"});
    ASSERT_TRUE(replacement.ok()) << replacement.status().message();
}

TEST_F(ResetRetireTest, FixedRequiredOptionalPrepareBarrierAllowsOptionalFailure) {
    auto opened = rodaq::DaqRuntime::open(make_options(rodaq::DaqOpenMode::Initialize));
    ASSERT_TRUE(opened.ok());
    runtime_ = std::move(opened.value());

    auto cfg = make_task_config("required-optional");
    rodaq::PipelineConfig optional_pipe = cfg.pipelines[0];
    optional_pipe.required = false;
    optional_pipe.acquisition.json = R"({"streams":["opt"],"mode":"fail_prepare"})";
    optional_pipe.streams[0].stream_key = "opt";
    cfg.pipelines.push_back(std::move(optional_pipe));

    auto created = runtime_->tasks().create_task(cfg, RequestIdentity{"create-opt"});
    ASSERT_TRUE(created.ok()) << created.status().message();
    const auto task_id = created.value();
    const auto gen = runtime_->tasks().get_task(task_id).value().control_generation;
    auto start_op = runtime_->tasks().start(
        task_id, RequestIdentity{"start-opt"}, ControlPrecondition{true, gen});
    ASSERT_TRUE(start_op.ok());
    ASSERT_EQ(wait_operation(task_id, start_op.value()), OperationState::PartiallySucceeded);

    const auto detail = runtime_->tasks().get_task(task_id);
    ASSERT_TRUE(detail.ok());
    EXPECT_EQ(detail.value().desired_state, DesiredState::Running);

    auto stop_op = runtime_->tasks().stop(
        task_id, RequestIdentity{"stop-opt"},
        ControlPrecondition{true, detail.value().control_generation});
    ASSERT_TRUE(stop_op.ok());
    ASSERT_EQ(wait_operation(task_id, stop_op.value()), OperationState::Succeeded);
}

}  // namespace
