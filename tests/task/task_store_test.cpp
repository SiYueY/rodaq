#include "task/task_store.hpp"
#include "task/identity.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace task = rodaq::task;
namespace persistence = rodaq::persistence;
namespace fs = std::filesystem;
using rodaq::StatusCode;

class TaskConfigStoreTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-task-store-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        task_id_ = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
        std::ifstream input(RODAQ_TASK_CONFIG_FIXTURE);
        auto parsed = rodaq::parse_task_config(persistence::Json::parse(input).dump());
        ASSERT_TRUE(parsed.ok());
        config_ = std::move(parsed).value();
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    fs::path root_;
    rodaq::TaskId task_id_;
    rodaq::TaskConfig config_;
};

TEST_F(TaskConfigStoreTest, CreateUpdateLoadAndRemoveTasksWithAtomicTmpCleanup) {
    // Seed a stale *.tmp directory to prove load_all cleans it.
    fs::create_directories(root_ / "stale.tmp");
    std::ofstream(root_ / "stale.tmp/partial.json") << "{}";

    auto opened = task::TaskConfigStore::open(root_);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto store = std::move(opened.value());

    auto initial = store.load_all();
    ASSERT_TRUE(initial.ok());
    EXPECT_TRUE(initial.value().empty());
    EXPECT_FALSE(fs::exists(root_ / "stale.tmp"));

    // Create assigns IDs if omitted and publishes atomically.
    auto input = config_;
    input.pipelines[0].pipeline_id = 0;
    input.pipelines[0].streams[0].stream_id = 0;
    auto created = store.create_task(task_id_, input);
    ASSERT_TRUE(created.ok()) << created.status().message();
    EXPECT_EQ(created.value().active_config.pipelines[0].pipeline_id, 1ULL);
    EXPECT_EQ(created.value().active_config.pipelines[0].streams[0].stream_id, 1ULL);
    EXPECT_FALSE(created.value().metadata.active_config_version.empty());

    // Duplicate create for the same task_id fails-closed (RENAME_NOREPLACE / Conflict).
    EXPECT_EQ(store.create_task(task_id_, input).status().code(), StatusCode::Conflict);

    // Rebinding an existing stream_id to a different stream_key fails-closed.
    auto bad_rebind = created.value().active_config;
    bad_rebind.pipelines[0].streams[0].stream_key = "different-key";
    EXPECT_EQ(
        store.update_config(created.value().metadata, bad_rebind).status().code(),
        StatusCode::InvalidArgument);

    // Valid config update creates a new immutable config_version.
    auto updated_cfg = created.value().active_config;
    updated_cfg.name = "updated";
    auto updated = store.update_config(created.value().metadata, updated_cfg);
    ASSERT_TRUE(updated.ok()) << updated.status().message();
    EXPECT_NE(
        updated.value().metadata.active_config_version,
        created.value().metadata.active_config_version);
    EXPECT_EQ(updated.value().active_config.name, "updated");

    // Reload from disk recovers the latest active config.
    auto reloaded = store.load_all();
    ASSERT_TRUE(reloaded.ok());
    ASSERT_EQ(reloaded.value().size(), 1U);
    EXPECT_EQ(reloaded.value()[0].active_config.name, "updated");
    EXPECT_EQ(
        reloaded.value()[0].metadata.active_config_version,
        updated.value().metadata.active_config_version);

    // Remove task deletes directory cleanly.
    ASSERT_TRUE(store.remove_task(task_id_).ok());
    auto after_remove = store.load_all();
    ASSERT_TRUE(after_remove.ok());
    EXPECT_TRUE(after_remove.value().empty());
}

}  // namespace
