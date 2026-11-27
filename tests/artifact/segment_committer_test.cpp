#include "artifact/artifact_catalog.hpp"
#include "artifact/segment_committer.hpp"
#include "manifest_fixture.hpp"
#include "persistence/atomic_file.hpp"
#include "artifact/recovery_scan.hpp"
#include "task/control_record.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <tuple>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace artifact = rodaq::artifact;
namespace persistence = rodaq::persistence;
namespace fs = std::filesystem;
using rodaq::StatusCode;

class SegmentCommitterTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-commit-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        auto accounting = artifact::VolumeCapacity::create({1000000, 1000000, 0, 100, 16, 16, 32});
        ASSERT_TRUE(accounting.ok());
        volume_capacity_ =
            std::make_unique<artifact::VolumeCapacity>(std::move(accounting.value()));
        auto probe = volume_capacity_->begin_space_sample(100);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(volume_capacity_->finish_space_sample(probe.value(), 1000000, 100).ok());
        fs::create_directory(root_ / "volume");
        auto volume =
            artifact::ManagedVolume::open(root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(volume.ok());
        volume_ = std::move(volume.value());
        fs::create_directory(root_ / "ledger");
        auto opened = artifact::LedgerStore::open(
            root_ / "ledger", artifact::LedgerOpenMode::Initialize, 8,
            [this](auto step) { return ledger_fault_ ? ledger_fault_(step) : 0; });
        ASSERT_TRUE(opened.ok());
        ledger_ = std::move(opened.value());
        auto staging = volume_->create_staging(7);
        ASSERT_TRUE(staging.ok());
        staging_ = staging.value();
        std::ofstream(staging_ / "data.bin", std::ios::binary) << "abc";
        seal_.files = {{"data.bin", 3}};
        seal_.total_physical_bytes = 3;
        envelope_.max_segment_file_count = 1;
        envelope_.max_relative_path_bytes = 32;
        envelope_.max_segment_physical_bytes = 1024;
        manifest_.identity = rodaq::test::manifest_identity();
        manifest_.identity.volume_id = volume_->identity();
        auto descriptor = rodaq::test::manifest_descriptor();
        descriptor.accepted_records = 2;
        manifest_.descriptors = {descriptor};
        manifest_.coverage = {{1, 2, 1, 1, 0, 10, 11}};
        manifest_.epochs = {{1, rodaq::time::ClockEpochOrigin::Boot, {}, 2, 100, 200, 1}};
        manifest_.first_receive_monotonic = 100;
        manifest_.last_receive_monotonic = 200;
        manifest_.min_receive_realtime = 1100;
        manifest_.max_receive_realtime = 1200;
        manifest_.recording_end_ns = 100;
    }

    void TearDown() override {
        ledger_.reset();
        volume_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    rodaq::Result<std::unique_ptr<artifact::SegmentCommitter>> prepare(
        persistence::FileFaultHook faults = {}) {
        auto files =
            artifact::SealedFiles::open(volume_, 7, seal_, envelope_, 8192, std::move(faults));
        if (!files.ok()) {
            return files.status();
        }
        manifest_.files = files.value()->evidence();
        auto registration = ledger_->reserve_registration(7);
        if (!registration.ok()) {
            return registration.status();
        }
        auto capacity = volume_capacity_->reserve(8195, 100);
        if (!capacity.ok()) {
            return capacity.status();
        }
        capacity.value().mark_effects_possible();
        auto usage = capacity.value().record_usage(3, 8192);
        if (!usage.ok()) {
            return usage.status();
        }
        artifact::CommitReservation reservation{
            std::move(capacity.value()), std::move(registration.value()), 3, 8192};
        return artifact::SegmentCommitter::create(files.value(), manifest_, {}, reservation);
    }

    void crash_at(int boundary);

    void interrupt_publication(bool final_phase) {
        auto prepared = prepare([final_phase, publishes = 0, directories = 0](auto step) mutable {
            if ((!final_phase && step == persistence::FileStep::Publish && ++publishes == 2) ||
                (final_phase && step == persistence::FileStep::SyncDirectory &&
                 ++directories == 3)) {
                return EIO;
            }
            return 0;
        });
        ASSERT_TRUE(prepared.ok());
        ASSERT_FALSE(prepared.value()->commit(*ledger_).ok());
        ASSERT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
        prepared.value().reset();
        ledger_.reset();
        auto reopened = artifact::LedgerStore::open(
            root_ / "ledger", artifact::LedgerOpenMode::Existing, 8,
            [this](auto step) { return ledger_fault_ ? ledger_fault_(step) : 0; });
        ASSERT_TRUE(reopened.ok());
        ledger_ = std::move(reopened.value());
    }

    fs::path final() const { return volume_->artifacts_directory() / "7"; }

    fs::path root_;
    fs::path staging_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::unique_ptr<artifact::LedgerStore> ledger_;
    std::unique_ptr<artifact::VolumeCapacity> volume_capacity_;
    persistence::FileFaultHook ledger_fault_;
    rodaq::storage::SegmentSealResult seal_;
    rodaq::storage::PrepareConfig envelope_{};
    artifact::SegmentManifest manifest_;
};

class CommitAbortQuarantineTest : public SegmentCommitterTest,
                                  public testing::WithParamInterface<bool> {
protected:
    rodaq::task::TaskControlRecord reset_fence() {
        const auto record = ledger_->get(7).value();
        rodaq::task::TaskControlRecord control;
        control.task_id = record.task_id;
        control.control_generation = record.control_generation + 1;
        control.reset_generation = record.reset_generation_at_commit + 1;
        return control;
    }

    fs::path quarantined(const rodaq::task::TaskControlRecord& control) const {
        return volume_->root() / ".rodaq/quarantine" /
               (rodaq::to_string(control.task_id) + ".g" +
                std::to_string(control.control_generation - 1) + ".r" +
                std::to_string(control.reset_generation - 1) + ".s7" +
                (GetParam() ? ".final" : ".tmp"));
    }
};

TEST_P(
    CommitAbortQuarantineTest,
    ResetInvalidationPreservesOpaqueTreeAndRemovesOnlyPrecommitAuthority) {
    interrupt_publication(GetParam());
    const auto control = reset_fence();
    ASSERT_TRUE(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control).ok());
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::NotFound);
    EXPECT_FALSE(fs::exists(staging_));
    EXPECT_FALSE(fs::exists(final()));
    EXPECT_EQ(persistence::read_file(quarantined(control), "data.bin", 16).value(), "abc");
    EXPECT_TRUE(fs::is_regular_file(quarantined(control) / "manifest.json"));
    // Quarantine is retained recovery evidence, never normal Artifact adoption
    // or proof of zero managed usage for the next capacity projection.
    EXPECT_EQ(
        artifact::inspect_committed_volume(volume_, *ledger_, {}).status().code(),
        StatusCode::Unavailable);
}

TEST_P(CommitAbortQuarantineTest, NoResetFenceLeavesBothFilesAndNormalRecoveryPermissionUnchanged) {
    interrupt_publication(GetParam());
    auto control = reset_fence();
    --control.reset_generation;
    EXPECT_EQ(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control)
            .status()
            .code(),
        StatusCode::Conflict);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    EXPECT_EQ(fs::exists(final()), GetParam());
    EXPECT_TRUE(fs::is_empty(volume_->root() / ".rodaq/quarantine"));
    EXPECT_TRUE(artifact::SegmentCommitter::recover(
                    volume_, *ledger_, 7, {},
                    [](const auto&, const auto&) -> rodaq::Result<void> { return {}; })
                    .ok());
}

TEST_P(CommitAbortQuarantineTest, ResetFenceCannotCrossTaskOrFixedControlScope) {
    interrupt_publication(GetParam());
    const auto expected = reset_fence();
    auto wrong_task = expected;
    wrong_task.task_id.value[0] ^= 0x80;
    EXPECT_EQ(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, wrong_task)
            .status()
            .code(),
        StatusCode::Conflict);
    auto older_control = expected;
    older_control.control_generation -= 2;
    EXPECT_EQ(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, older_control)
            .status()
            .code(),
        StatusCode::Conflict);
    auto out_of_scope = expected;
    rodaq::OperationId operation;
    operation.value[0] = 1;
    out_of_scope.reset_intent = rodaq::task::ResetIntent{
        operation, expected.reset_generation, expected.control_generation - 2};
    EXPECT_EQ(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, out_of_scope)
            .status()
            .code(),
        StatusCode::Conflict);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    EXPECT_TRUE(fs::exists((GetParam() ? final() : staging_) / "data.bin"));
    EXPECT_TRUE(fs::is_empty(volume_->root() / ".rodaq/quarantine"));
}

TEST_P(
    CommitAbortQuarantineTest, ExistingQuarantineTargetCannotBeOverwrittenOrCountAsAMovedSource) {
    interrupt_publication(GetParam());
    const auto control = reset_fence();
    ASSERT_TRUE(fs::create_directory(quarantined(control)));
    std::ofstream(quarantined(control) / "foreign") << "retain";
    EXPECT_EQ(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control)
            .status()
            .code(),
        StatusCode::Conflict);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    EXPECT_EQ(persistence::read_file(quarantined(control), "foreign", 16).value(), "retain");
    EXPECT_TRUE(fs::exists((GetParam() ? final() : staging_) / "data.bin"));
}

TEST_P(
    CommitAbortQuarantineTest, FailedLedgerUnlinkOrSyncRetainsAbortOwnershipUntilProofIsRenewed) {
    interrupt_publication(GetParam());
    const auto control = reset_fence();
    ledger_fault_ = [](auto step) {
        return step == persistence::FileStep::RemoveAuthority ? EIO : 0;
    };
    EXPECT_FALSE(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control).ok());
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(ledger_->reconcile(7).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(ledger_->collect_deleted(7).status().code(), StatusCode::Conflict);
    ledger_fault_ = [](auto step) {
        return step == persistence::FileStep::SyncDirectory ? EIO : 0;
    };
    EXPECT_FALSE(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control).ok());
    EXPECT_EQ(ledger_->recovery_snapshot().status().code(), StatusCode::Unavailable);
    ledger_fault_ = {};
    ASSERT_TRUE(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control).ok());
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::NotFound);
    EXPECT_EQ(persistence::read_file(quarantined(control), "data.bin", 16).value(), "abc");
}

TEST_P(CommitAbortQuarantineTest, ASourceReappearingDuringShardSyncCannotDischargeTheAbortOwner) {
    interrupt_publication(GetParam());
    const auto control = reset_fence();
    const auto name = GetParam() ? final() : staging_;
    ledger_fault_ = [&](auto step) {
        if (step == persistence::FileStep::SyncDirectory) {
            EXPECT_TRUE(fs::create_directory(name));
            std::ofstream(name / "foreign") << "retain";
        }
        return 0;
    };
    EXPECT_EQ(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control)
            .status()
            .code(),
        StatusCode::Conflict);
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(persistence::read_file(name, "foreign", 16).value(), "retain");
    EXPECT_EQ(persistence::read_file(quarantined(control), "data.bin", 16).value(), "abc");
    ledger_fault_ = {};
    fs::remove_all(name);  // Only the foreign test fixture created above.
    EXPECT_TRUE(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control).ok());
}

TEST_P(CommitAbortQuarantineTest, TargetReplacementDuringLedgerSyncDoesNotBecomeANewBaseline) {
    interrupt_publication(GetParam());
    const auto control = reset_fence();
    const auto target = quarantined(control);
    const auto held = volume_->root() / ".rodaq/quarantine/held-original";
    ledger_fault_ = [&](auto step) {
        if (step == persistence::FileStep::SyncDirectory) {
            fs::rename(target, held);
            EXPECT_TRUE(fs::create_directory(target));
            std::ofstream(target / "foreign") << "retain";
        }
        return 0;
    };
    EXPECT_EQ(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control)
            .status()
            .code(),
        StatusCode::DataLoss);
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(persistence::read_file(held, "data.bin", 16).value(), "abc");
    EXPECT_EQ(persistence::read_file(target, "foreign", 16).value(), "retain");
    ledger_fault_ = {};
    fs::remove_all(target);  // Only the replacement created by this test.
    fs::rename(held, target);
    EXPECT_TRUE(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control).ok());
}

INSTANTIATE_TEST_SUITE_P(StagingOrFinal, CommitAbortQuarantineTest, testing::Bool());

class QuarantineIoFailureTest : public SegmentCommitterTest,
                                public testing::WithParamInterface<std::tuple<bool, unsigned>> {};

TEST_P(QuarantineIoFailureTest, ANewAttemptReprovesBothParentsWithoutOverwritingRetainedData) {
    const auto final_phase = std::get<0>(GetParam());
    const auto selected = std::get<1>(GetParam());
    interrupt_publication(final_phase);
    const auto record = ledger_->get(7).value();
    rodaq::task::TaskControlRecord control;
    control.task_id = record.task_id;
    control.control_generation = record.control_generation + 1;
    control.reset_generation = record.reset_generation_at_commit + 1;
    unsigned directories = 0;
    const auto failed = artifact::quarantine_reset_invalidated_artifact(
        volume_, *ledger_, 7, control, [&](auto step) {
            if ((selected == 0 && step == persistence::FileStep::Publish) ||
                (selected == 3 && step == persistence::FileStep::Complete)) {
                return EIO;
            }
            return step == persistence::FileStep::SyncDirectory && ++directories == selected ? EIO
                                                                                             : 0;
        });
    EXPECT_FALSE(failed.ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    ASSERT_TRUE(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control).ok());
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::NotFound);
    EXPECT_EQ(
        std::distance(
            fs::directory_iterator(volume_->root() / ".rodaq/quarantine"),
            fs::directory_iterator()),
        1);
}

INSTANTIATE_TEST_SUITE_P(
    PhaseAndIoBoundary, QuarantineIoFailureTest,
    testing::Combine(testing::Bool(), testing::Values(0u, 1u, 2u, 3u)));

class QuarantineCrashTest : public SegmentCommitterTest,
                            public testing::WithParamInterface<std::tuple<bool, unsigned>> {};

TEST_P(QuarantineCrashTest, ProcessExitAtEachDurabilityBoundaryKeepsDataOutsideNormalPublication) {
    interrupt_publication(std::get<0>(GetParam()));
    const auto boundary = std::get<1>(GetParam());
    const auto record = ledger_->get(7).value();
    rodaq::task::TaskControlRecord control;
    control.task_id = record.task_id;
    control.control_generation = record.control_generation + 1;
    control.record_revision = control.control_generation;
    control.reset_generation = record.reset_generation_at_commit + 1;
    control.active_config_version = manifest_.identity.config_version;
    const auto encoded = rodaq::task::encode_control_record(control, {});
    ASSERT_TRUE(encoded.ok());
    ASSERT_TRUE(persistence::publish_file(
                    root_, "reset-control.json", encoded.value(),
                    persistence::FilePublishMode::Create, 65536)
                    .status.ok());
    ledger_fault_ = [boundary](auto step) {
        if ((boundary == 2 && step == persistence::FileStep::RemoveAuthority) ||
            (boundary == 3 && step == persistence::FileStep::SyncDirectory)) {
            _exit(81 + static_cast<int>(boundary));
        }
        return 0;
    };
    // No Plugin/worker threads in this fixture. This is process-crash evidence,
    // not filesystem power-loss qualification.
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        (void)artifact::quarantine_reset_invalidated_artifact(
            volume_, *ledger_, 7, control,
            [boundary, directories = 0u, completions = 0u](auto step) mutable {
                if ((boundary < 2 && step == persistence::FileStep::SyncDirectory &&
                     ++directories == boundary + 1) ||
                    (boundary == 4 && step == persistence::FileStep::Complete &&
                     ++completions == 2)) {
                    _exit(81 + static_cast<int>(boundary));
                }
                return 0;
            });
        _exit(99);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 81 + static_cast<int>(boundary));
    ledger_.reset();
    ledger_fault_ = {};
    auto reopened =
        artifact::LedgerStore::open(root_ / "ledger", artifact::LedgerOpenMode::Existing, 8);
    ASSERT_TRUE(reopened.ok());
    ledger_ = std::move(reopened.value());
    const auto bytes = persistence::read_file(root_, "reset-control.json", 65536);
    ASSERT_TRUE(bytes.ok());
    const auto durable_control = rodaq::task::decode_control_record(bytes.value(), {});
    ASSERT_TRUE(durable_control.ok());
    const auto actual = ledger_->get(7);
    if (actual.ok()) {
        ASSERT_EQ(actual.value().state, artifact::LedgerState::Committing);
        ASSERT_TRUE(artifact::quarantine_reset_invalidated_artifact(
                        volume_, *ledger_, 7, durable_control.value())
                        .ok());
    } else {
        EXPECT_EQ(actual.status().code(), StatusCode::NotFound);
    }
    EXPECT_FALSE(fs::exists(staging_));
    EXPECT_FALSE(fs::exists(final()));
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::NotFound);
    const auto directory = volume_->root() / ".rodaq/quarantine";
    ASSERT_EQ(std::distance(fs::directory_iterator(directory), fs::directory_iterator()), 1);
    const auto retained = fs::directory_iterator(directory)->path();
    EXPECT_EQ(persistence::read_file(retained, "data.bin", 16).value(), "abc");
    EXPECT_EQ(
        artifact::inspect_committed_volume(volume_, *ledger_, {}).status().code(),
        StatusCode::Unavailable);
}

INSTANTIATE_TEST_SUITE_P(
    PhaseAndCrashBoundary, QuarantineCrashTest,
    testing::Combine(testing::Bool(), testing::Values(0u, 1u, 2u, 3u, 4u)));

class CommitRecoveryTest : public SegmentCommitterTest, public testing::WithParamInterface<bool> {};

TEST_P(CommitRecoveryTest, ReopenedLedgerAndExactTreeRecoverBothPublicationPhases) {
    interrupt_publication(GetParam());
    unsigned checks = 0;
    ASSERT_TRUE(
        artifact::SegmentCommitter::recover(
            volume_, *ledger_, 7, {},
            [&](const auto& record, const auto& manifest) -> rodaq::Result<void> {
                ++checks;
                EXPECT_EQ(record.state, artifact::LedgerState::Committing);
                EXPECT_EQ(record.task_id, manifest.identity.task_id);
                EXPECT_EQ(record.manifest_digest, artifact::manifest_digest(manifest, {}).value());
                return {};
            })
            .ok());
    EXPECT_EQ(checks, 2);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
    EXPECT_FALSE(fs::exists(staging_));
    EXPECT_TRUE(fs::is_directory(final()));
    EXPECT_EQ(persistence::read_file(final(), "data.bin", 16).value(), "abc");
    const auto usage = artifact::inspect_committed_volume(volume_, *ledger_, {});
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage.value().artifact_slots, 1);
    EXPECT_GT(usage.value().physical_bytes, 3);
}

TEST_P(CommitRecoveryTest, ResetFenceIsCheckedBeforeEffectsAndAgainAfterPublication) {
    interrupt_publication(GetParam());
    const auto denied = artifact::SegmentCommitter::recover(
        volume_, *ledger_, 7, {}, [](const auto&, const auto&) -> rodaq::Result<void> {
            return rodaq::Status(StatusCode::Conflict, {});
        });
    EXPECT_EQ(denied.status().code(), StatusCode::Conflict);
    EXPECT_EQ(fs::exists(final()), GetParam());
    unsigned checks = 0;
    const auto late = artifact::SegmentCommitter::recover(
        volume_, *ledger_, 7, {}, [&](const auto&, const auto&) -> rodaq::Result<void> {
            return ++checks == 1 ? rodaq::Result<void>{}
                                 : rodaq::Result<void>(rodaq::Status(StatusCode::Conflict, {}));
        });
    EXPECT_EQ(late.status().code(), StatusCode::Conflict);
    EXPECT_EQ(checks, 2);
    EXPECT_TRUE(fs::exists(final()));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
}

TEST_P(CommitRecoveryTest, CorruptBytesNeverReachAuthorizationOrLedgerPromotion) {
    interrupt_publication(GetParam());
    std::ofstream((GetParam() ? final() : staging_) / "data.bin", std::ios::binary) << "bad";
    unsigned checks = 0;
    const auto result = artifact::SegmentCommitter::recover(
        volume_, *ledger_, 7, {}, [&](const auto&, const auto&) -> rodaq::Result<void> {
            ++checks;
            return {};
        });
    EXPECT_EQ(result.status().code(), StatusCode::DataLoss);
    EXPECT_EQ(checks, 0);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
}

TEST_P(CommitRecoveryTest, BothNamesAreAConflictAndNeverAuthorizeOverwrite) {
    interrupt_publication(GetParam());
    ASSERT_TRUE(fs::create_directory(GetParam() ? staging_ : final()));
    const auto result = artifact::SegmentCommitter::recover(
        volume_, *ledger_, 7, {}, [](const auto&, const auto&) -> rodaq::Result<void> {
            ADD_FAILURE() << "Ambiguous filesystem phase cannot authorize publication";
            return {};
        });
    EXPECT_EQ(result.status().code(), StatusCode::Conflict);
    EXPECT_TRUE(fs::is_directory(staging_));
    EXPECT_TRUE(fs::is_directory(final()));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
}

TEST_P(CommitRecoveryTest, UncertainLedgerCompletionRequiresActualDurableReconciliation) {
    interrupt_publication(GetParam());
    ledger_fault_ = [](auto step) {
        return step == persistence::FileStep::SyncDirectory ? EIO : 0;
    };
    const auto result = artifact::SegmentCommitter::recover(
        volume_, *ledger_, 7, {},
        [](const auto&, const auto&) -> rodaq::Result<void> { return {}; });
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::Unavailable);
    EXPECT_TRUE(fs::is_directory(final()));
    ledger_fault_ = {};
    ASSERT_TRUE(ledger_->reconcile(7).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
    EXPECT_TRUE(artifact::inspect_committed_volume(volume_, *ledger_, {}).ok());
}

INSTANTIATE_TEST_SUITE_P(StagingOrFinal, CommitRecoveryTest, testing::Bool());

TEST_F(SegmentCommitterTest, RecoveryUsesAtomicNoReplaceWhenAFinalNameAppearsDuringPublication) {
    interrupt_publication(false);
    auto result = artifact::SegmentCommitter::recover(
        volume_, *ledger_, 7, {},
        [](const auto&, const auto&) -> rodaq::Result<void> { return {}; },
        [&](auto step) {
            if (step == persistence::FileStep::Publish) {
                EXPECT_TRUE(fs::create_directory(final()));
                std::ofstream(final() / "foreign") << "retain";
            }
            return 0;
        });
    EXPECT_EQ(result.status().code(), StatusCode::Conflict);
    EXPECT_TRUE(fs::exists(staging_ / "data.bin"));
    EXPECT_EQ(persistence::read_file(final(), "foreign", 16).value(), "retain");
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
}

TEST_F(SegmentCommitterTest, RecoveryRechecksStagingAbsenceAfterTheActualSecondFenceCheck) {
    interrupt_publication(true);
    unsigned checks = 0;
    const auto result = artifact::SegmentCommitter::recover(
        volume_, *ledger_, 7, {}, [&](const auto&, const auto&) -> rodaq::Result<void> {
            if (++checks == 2) {
                EXPECT_TRUE(fs::create_directory(staging_));
            }
            return {};
        });
    EXPECT_EQ(result.status().code(), StatusCode::Conflict);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    EXPECT_TRUE(fs::is_directory(staging_));
}

TEST_F(SegmentCommitterTest, RecoveryRetainsVolumeAndPinnedFilesUntilTheActualParentSyncReturns) {
    interrupt_publication(true);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    auto owner = volume_;
    std::weak_ptr<artifact::ManagedVolume> retained = owner;
    auto work = std::async(std::launch::async, [&, owner = std::move(owner)] {
        return artifact::SegmentCommitter::recover(
            owner, *ledger_, 7, {},
            [](const auto&, const auto&) -> rodaq::Result<void> { return {}; },
            [&, directories = 0](auto step) mutable {
                if (step == persistence::FileStep::SyncDirectory && ++directories == 2) {
                    entered.set_value();
                    released.wait();
                }
                return 0;
            });
    });
    const bool blocked =
        entered.get_future().wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    if (blocked) {
        volume_.reset();
        EXPECT_FALSE(retained.expired());
        EXPECT_EQ(work.wait_for(std::chrono::milliseconds{1}), std::future_status::timeout);
        EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    }
    release.set_value();
    const auto completed = work.get();
    EXPECT_TRUE(blocked);
    EXPECT_TRUE(completed.ok());
    EXPECT_TRUE(retained.expired());
}

class CommitRecoverySyncTest : public SegmentCommitterTest,
                               public testing::WithParamInterface<std::tuple<bool, unsigned>> {};

TEST_P(CommitRecoverySyncTest, SyncFailureRetainsAuthorityAndAFreshRecoveryCanFinish) {
    interrupt_publication(std::get<0>(GetParam()));
    const auto selected = std::get<1>(GetParam());
    unsigned directories = 0;
    auto authorize = [](const auto&, const auto&) -> rodaq::Result<void> { return {}; };
    auto failed =
        artifact::SegmentCommitter::recover(volume_, *ledger_, 7, {}, authorize, [&](auto step) {
            if (selected == 0 && step == persistence::FileStep::SyncFile) {
                return EIO;
            }
            return step == persistence::FileStep::SyncDirectory && ++directories == selected ? EIO
                                                                                             : 0;
        });
    EXPECT_FALSE(failed.ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    EXPECT_TRUE(artifact::SegmentCommitter::recover(volume_, *ledger_, 7, {}, authorize).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
}

INSTANTIATE_TEST_SUITE_P(
    PhaseAndDurabilityBoundary, CommitRecoverySyncTest,
    testing::Combine(testing::Bool(), testing::Values(0u, 1u, 2u)));

TEST_F(SegmentCommitterTest, ActualManifestTreeAndLedgerPublishInOrderAndSurviveReopen) {
    auto prepared = prepare([this, publishes = 0](auto step) mutable {
        if (step == persistence::FileStep::Publish && ++publishes == 2) {
            auto record = ledger_->get(7);
            EXPECT_TRUE(record.ok());
            if (record.ok()) {
                EXPECT_EQ(record.value().state, artifact::LedgerState::Committing);
            }
            EXPECT_TRUE(fs::exists(staging_ / "manifest.json"));
            EXPECT_FALSE(fs::exists(final()));
        }
        return 0;
    });
    ASSERT_TRUE(prepared.ok());
    auto& owner = prepared.value();
    EXPECT_FALSE(owner->committed());
    EXPECT_EQ(owner->take_committed_charge().status().code(), StatusCode::InvalidState);
    EXPECT_EQ(volume_capacity_->snapshot().commit_slots, 1);
    EXPECT_FALSE(ledger_->get(7).ok());
    ASSERT_TRUE(owner->commit(*ledger_).ok());
    EXPECT_TRUE(owner->committed());
    EXPECT_EQ(owner->storage_bytes(), 3);
    EXPECT_FALSE(fs::exists(staging_));
    auto bytes = persistence::read_file(final(), "manifest.json", 8192);
    ASSERT_TRUE(bytes.ok());
    EXPECT_EQ(bytes.value().size(), owner->manifest_bytes());
    auto decoded = artifact::decode_manifest(bytes.value(), {});
    ASSERT_TRUE(decoded.ok());
    auto record = ledger_->get(7);
    ASSERT_TRUE(record.ok());
    EXPECT_EQ(record.value().state, artifact::LedgerState::Committed);
    EXPECT_EQ(
        record.value().manifest_digest, artifact::manifest_digest(decoded.value(), {}).value());
    EXPECT_TRUE(record.value().committed_at.has_value());
    EXPECT_EQ(owner->commit(*ledger_).status().code(), StatusCode::InvalidState);
    const auto retained_bytes = owner->storage_bytes() + owner->manifest_bytes();
    EXPECT_EQ(volume_capacity_->snapshot().actual_bytes, retained_bytes);
    // Component settlement only; Catalog integration has its own later gate.
    auto retained = owner->take_committed_charge();
    ASSERT_TRUE(retained.ok());
    EXPECT_EQ(volume_capacity_->snapshot().commit_slots, 0);
    EXPECT_EQ(volume_capacity_->snapshot().actual_bytes, retained_bytes);
    EXPECT_EQ(volume_capacity_->snapshot().artifact_slots, 1);
    EXPECT_EQ(owner->take_committed_charge().status().code(), StatusCode::InvalidState);
    ledger_.reset();
    auto reopened =
        artifact::LedgerStore::open(root_ / "ledger", artifact::LedgerOpenMode::Existing);
    ASSERT_TRUE(reopened.ok());
    EXPECT_EQ(reopened.value()->get(7).value().state, artifact::LedgerState::Committed);
}

TEST_F(SegmentCommitterTest, TreeSyncFailureCannotCreateCommittingAuthorityOrFinalName) {
    auto prepared =
        prepare([](auto step) { return step == persistence::FileStep::SyncFile ? EIO : 0; });
    ASSERT_TRUE(prepared.ok());
    EXPECT_FALSE(prepared.value()->commit(*ledger_).ok());
    EXPECT_FALSE(prepared.value()->committed());
    EXPECT_FALSE(ledger_->get(7).ok());
    EXPECT_FALSE(fs::exists(final()));
    EXPECT_TRUE(fs::exists(staging_));
}

TEST_F(SegmentCommitterTest, CommittingPublicationFailureNeverRenamesAndRequiresReconciliation) {
    ledger_fault_ = [](auto step) {
        return step == persistence::FileStep::SyncDirectory ? EIO : 0;
    };
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    EXPECT_FALSE(prepared.value()->commit(*ledger_).ok());
    EXPECT_FALSE(prepared.value()->committed());
    EXPECT_FALSE(fs::exists(final()));
    EXPECT_TRUE(fs::exists(staging_));
    EXPECT_FALSE(ledger_->get(7).ok());
    ledger_fault_ = {};
    ASSERT_TRUE(ledger_->reconcile(7).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
}

TEST_F(SegmentCommitterTest, ExistingFinalDirectoryIsNeverOverwrittenOrAdopted) {
    fs::create_directory(final());
    std::ofstream(final() / "sentinel") << "keep";
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    EXPECT_EQ(prepared.value()->commit(*ledger_).status().code(), StatusCode::Conflict);
    EXPECT_FALSE(prepared.value()->committed());
    EXPECT_TRUE(fs::exists(final() / "sentinel"));
    EXPECT_FALSE(fs::exists(final() / "manifest.json"));
    EXPECT_TRUE(fs::exists(staging_ / "data.bin"));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
}

TEST_F(SegmentCommitterTest, ParentSyncFailureRetainsCommittingAndNeverReportsCommitted) {
    auto prepared = prepare([directories = 0](auto step) mutable {
        return step == persistence::FileStep::SyncDirectory && ++directories == 3 ? EIO : 0;
    });
    ASSERT_TRUE(prepared.ok());
    EXPECT_FALSE(prepared.value()->commit(*ledger_).ok());
    EXPECT_FALSE(prepared.value()->committed());
    EXPECT_TRUE(fs::exists(final() / "data.bin"));
    EXPECT_FALSE(fs::exists(staging_));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    EXPECT_EQ(prepared.value()->commit(*ledger_).status().code(), StatusCode::InvalidState);
}

TEST_F(SegmentCommitterTest, FinalLedgerUncertaintyDoesNotBecomeSuccessBeforeActualReconciliation) {
    ledger_fault_ = [directories = 0](auto step) mutable {
        return step == persistence::FileStep::SyncDirectory && ++directories == 2 ? EIO : 0;
    };
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    EXPECT_FALSE(prepared.value()->commit(*ledger_).ok());
    EXPECT_FALSE(prepared.value()->committed());
    EXPECT_TRUE(fs::exists(final() / "manifest.json"));
    EXPECT_FALSE(ledger_->get(7).ok());
    ledger_fault_ = {};
    ASSERT_TRUE(ledger_->reconcile(7).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
    // Reconciliation is authority for the recovery owner; it does not turn an
    // old failed Commit frame into a second successful execution.
    EXPECT_FALSE(prepared.value()->committed());
}

TEST_F(SegmentCommitterTest, FileMutationAfterCommittingIsDetectedBeforeRename) {
    auto prepared = prepare([this, publishes = 0](auto step) mutable {
        if (step == persistence::FileStep::Publish && ++publishes == 2) {
            std::ofstream(staging_ / "data.bin", std::ios::trunc) << "bad";
        }
        return 0;
    });
    ASSERT_TRUE(prepared.ok());
    EXPECT_EQ(prepared.value()->commit(*ledger_).status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(final()));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
}

TEST_F(SegmentCommitterTest, UnknownOrEntirelyUnwrittenCoverageCannotEnterNormalCommit) {
    manifest_.coverage[0] = {1, 2, 1, 0, 1, 10, 11};
    EXPECT_EQ(prepare().status().code(), StatusCode::DataLoss);
    manifest_.coverage[0] = {1, 2, 0, 2, 0, 10, 11};
    EXPECT_EQ(prepare().status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(staging_ / "manifest.json"));
    EXPECT_FALSE(ledger_->get(7).ok());
    EXPECT_TRUE(ledger_->reserve_registration(7).ok());
}

TEST_F(SegmentCommitterTest, ObserverDeadlineDoesNotCompleteTheActualBlockedParentSync) {
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    auto prepared = prepare([&, directories = 0](auto step) mutable {
        if (step == persistence::FileStep::SyncDirectory && ++directories == 3) {
            entered.set_value();
            released.wait();
        }
        return 0;
    });
    ASSERT_TRUE(prepared.ok());
    auto work = std::async(std::launch::async, [&] { return prepared.value()->commit(*ledger_); });
    entered.get_future().wait();
    EXPECT_EQ(work.wait_for(std::chrono::milliseconds(5)), std::future_status::timeout);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committing);
    EXPECT_TRUE(fs::exists(final()));
    EXPECT_EQ(volume_capacity_->snapshot().commit_slots, 1);
    EXPECT_GT(volume_capacity_->snapshot().commit_bytes, 3);
    release.set_value();
    EXPECT_TRUE(work.get().ok());
    EXPECT_TRUE(prepared.value()->committed());
}

void SegmentCommitterTest::crash_at(int boundary) {
    // Process crash only: this does not qualify power-loss or a filesystem.
    auto prepared = prepare([boundary, directories = 0](auto step) mutable {
        if (step == persistence::FileStep::SyncDirectory && ++directories == 3 && boundary == 0) {
            _exit(71);
        }
        return 0;
    });
    ASSERT_TRUE(prepared.ok());
    ledger_fault_ = [boundary, publishes = 0](auto step) mutable {
        if (step == persistence::FileStep::Publish && ++publishes == 2 && boundary == 1) {
            _exit(72);
        }
        return 0;
    };
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        (void)prepared.value()->commit(*ledger_);
        _exit(99);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), boundary == 0 ? 71 : 72);
    prepared.value().reset();
    // The still-running parent's dropped effectful work retains its charges
    // conservatively; only a verified recovery projection may restore admission.
    EXPECT_EQ(volume_capacity_->snapshot().orphaned_reservations, 1);
    ledger_.reset();
    auto reopened =
        artifact::LedgerStore::open(root_ / "ledger", artifact::LedgerOpenMode::Existing, 8);
    ASSERT_TRUE(reopened.ok());
    EXPECT_EQ(reopened.value()->get(7).value().state, artifact::LedgerState::Committing);
    EXPECT_TRUE(fs::exists(final() / "manifest.json"));
}

TEST_F(SegmentCommitterTest, ProcessExitAfterRenameNeverCreatesCommittedAuthority) { crash_at(0); }

TEST_F(SegmentCommitterTest, ProcessExitAfterParentSyncNeverCreatesCommittedAuthority) {
    crash_at(1);
}

TEST_F(SegmentCommitterTest, PrecommitFailureQuarantineMovesCorruptStagingAndAbortsLedgerRow) {
    interrupt_publication(false);
    std::ofstream(staging_ / "data.bin", std::ios::binary) << "corrupt!";
    const auto record = ledger_->get(7).value();
    ASSERT_TRUE(artifact::quarantine_failed_precommit_artifact(volume_, *ledger_, 7).ok());
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::NotFound);
    EXPECT_FALSE(fs::exists(staging_));
    const auto expected_name =
        rodaq::to_string(record.task_id) + ".g" + std::to_string(record.control_generation) + ".r" +
        std::to_string(record.reset_generation_at_commit) + ".s7.precommit.tmp";
    const auto retained = volume_->root() / ".rodaq/quarantine" / expected_name;
    EXPECT_TRUE(fs::is_directory(retained));
    EXPECT_EQ(persistence::read_file(retained, "data.bin", 32).value(), "corrupt!");

    const auto boot = rodaq::time::parse_boot_identity("11111111-2222-3333-4444-555555555555");
    ASSERT_TRUE(boot.ok());
    const auto report = artifact::inspect_volume_recovery(volume_, *ledger_, {}, boot.value());
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().committed_count, 0u);
    EXPECT_EQ(report.value().artifact_slots, 1u);
    ASSERT_EQ(report.value().quarantined.size(), 1u);
    EXPECT_EQ(report.value().quarantined[0].reason, artifact::QuarantineReason::PrecommitFailure);
    EXPECT_EQ(report.value().quarantined[0].task_id, record.task_id);
    EXPECT_EQ(report.value().quarantined[0].control_generation, record.control_generation);
    EXPECT_EQ(report.value().quarantined[0].segment_id, 7u);
    EXPECT_GT(report.value().quarantined_bytes, 8u);
    EXPECT_EQ(report.value().boot_evidence.state, rodaq::time::BootArtifactState::Unknown);
}

TEST_F(SegmentCommitterTest, VolumeRecoveryScanClassifiesLedgerAbsentAndQuarantineWithoutAdoption) {
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(prepared.value()->commit(*ledger_).ok());
    ASSERT_TRUE(prepared.value()->take_committed_charge().ok());

    const auto orphan_final = volume_->artifacts_directory() / "999";
    const auto orphan_tmp = volume_->artifacts_directory() / "998.tmp";
    const auto unknown_q = volume_->root() / ".rodaq/quarantine/foreign-entry";
    ASSERT_TRUE(fs::create_directory(orphan_final));
    ASSERT_TRUE(fs::create_directory(orphan_tmp));
    ASSERT_TRUE(fs::create_directory(unknown_q));
    std::ofstream(orphan_final / "payload.bin", std::ios::binary) << "12345";
    std::ofstream(orphan_tmp / "partial.bin", std::ios::binary) << "6789";
    std::ofstream(unknown_q / "opaque.bin", std::ios::binary) << "012";

    const auto boot = rodaq::time::parse_boot_identity("11111111-2222-3333-4444-555555555555");
    ASSERT_TRUE(boot.ok());
    const auto report = artifact::inspect_volume_recovery(volume_, *ledger_, {}, boot.value());
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().committed_count, 1u);
    EXPECT_EQ(report.value().verification_required_count, 2u);
    EXPECT_EQ(report.value().quarantined.size(), 3u);
    EXPECT_EQ(report.value().quarantined_bytes, 12u);
    EXPECT_EQ(report.value().artifact_slots, 4u);

    auto cap = artifact::VolumeCapacity::create(
        {1000000, 1000000, 0, 100, 16, 16, 32}, report.value().total_physical_bytes(),
        report.value().artifact_slots);
    ASSERT_TRUE(cap.ok());
    auto capacity = std::make_shared<artifact::VolumeCapacity>(std::move(cap.value()));
    artifact::CatalogOptions options;
    options.volume = volume_;
    options.ledger = std::move(ledger_);
    options.capacity = capacity;
    options.max_artifacts = 16;
    auto catalog = artifact::ArtifactCatalog::create(options);
    ASSERT_TRUE(catalog.ok());
    ASSERT_TRUE(catalog.value()->rebuild(report.value()).ok());

    const auto page = catalog.value()->query({});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().items.size(), 1u);
    EXPECT_EQ(page.value().items[0].segment_id, 7u);
    EXPECT_EQ(catalog.value()->get({999}).status().code(), StatusCode::NotFound);

    const auto snap = catalog.value()->snapshot();
    EXPECT_EQ(snap.visible, 1u);
    EXPECT_EQ(snap.quarantined, 3u);
    EXPECT_EQ(snap.verification_required, 2u);
    EXPECT_EQ(snap.quarantined_bytes, 12u);

    // Unattributable quarantine entry conservatively blocks Retire for any Task.
    rodaq::TaskId other_task{};
    other_task.value[0] = 0x55;
    EXPECT_FALSE(catalog.value()->retire_eligible(other_task).value());
    EXPECT_TRUE(catalog.value()->has_task_references(other_task).value());
}

TEST_F(SegmentCommitterTest, VolumeRecoveryScanDetectsCorruptedCommittedAndReappearedDeleted) {
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(prepared.value()->commit(*ledger_).ok());
    ASSERT_TRUE(prepared.value()->take_committed_charge().ok());

    std::ofstream(final() / "data.bin", std::ios::binary) << "tampered";
    const auto boot = rodaq::time::parse_boot_identity("11111111-2222-3333-4444-555555555555");
    ASSERT_TRUE(boot.ok());
    const auto report = artifact::inspect_volume_recovery(volume_, *ledger_, {}, boot.value());
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.value().corrupted_committed_count, 1u);
    EXPECT_EQ(report.value().health, StatusCode::DataLoss);
    EXPECT_FALSE(report.value().usage_complete);

    auto cap = artifact::VolumeCapacity::create(
        {1000000, 1000000, 0, 100, 16, 16, 32}, report.value().total_physical_bytes(),
        report.value().artifact_slots);
    ASSERT_TRUE(cap.ok());
    auto capacity = std::make_shared<artifact::VolumeCapacity>(std::move(cap.value()));
    artifact::CatalogOptions options;
    options.volume = volume_;
    options.ledger = std::move(ledger_);
    options.capacity = capacity;
    options.max_artifacts = 16;
    auto catalog = artifact::ArtifactCatalog::create(options);
    ASSERT_TRUE(catalog.ok());
    ASSERT_TRUE(catalog.value()->rebuild(report.value()).ok());
    EXPECT_EQ(capacity->snapshot().protection, StatusCode::DataLoss);
    EXPECT_EQ(catalog.value()->snapshot().corrupted_committed, 1u);
    EXPECT_FALSE(catalog.value()->retire_eligible(manifest_.identity.task_id).value());
}

TEST_F(SegmentCommitterTest, VolumeRecoveryScanComputesSameBootPresentAndNewBootAbsentEvidence) {
    const auto same_boot = rodaq::time::parse_boot_identity("11111111-2222-3333-4444-555555555555");
    const auto next_boot = rodaq::time::parse_boot_identity("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    ASSERT_TRUE(same_boot.ok());
    ASSERT_TRUE(next_boot.ok());
    manifest_.identity.playback_anchor.boot_id = same_boot.value().id;
    const auto expected_anchor = manifest_.identity.playback_anchor;

    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(prepared.value()->commit(*ledger_).ok());
    ASSERT_TRUE(prepared.value()->take_committed_charge().ok());

    const auto same_report =
        artifact::inspect_volume_recovery(volume_, *ledger_, {}, same_boot.value());
    ASSERT_TRUE(same_report.ok());
    EXPECT_EQ(same_report.value().boot_evidence.state, rodaq::time::BootArtifactState::Present);
    EXPECT_EQ(same_report.value().boot_evidence.anchor.boot_id, expected_anchor.boot_id);
    EXPECT_EQ(same_report.value().boot_evidence.anchor.monotonic_ns, expected_anchor.monotonic_ns);
    EXPECT_EQ(same_report.value().boot_evidence.anchor.realtime_ns, expected_anchor.realtime_ns);

    const auto next_report =
        artifact::inspect_volume_recovery(volume_, *ledger_, {}, next_boot.value());
    ASSERT_TRUE(next_report.ok());
    EXPECT_EQ(next_report.value().boot_evidence.state, rodaq::time::BootArtifactState::Absent);
}

TEST_F(SegmentCommitterTest, CatalogPurgeQuarantinedForResetReclaimsBytesAndSlotsWithinFixedScope) {
    interrupt_publication(false);
    const auto record = ledger_->get(7).value();
    rodaq::task::TaskControlRecord control;
    control.task_id = record.task_id;
    control.control_generation = record.control_generation + 1;
    control.reset_generation = record.reset_generation_at_commit + 1;
    ASSERT_TRUE(
        artifact::quarantine_reset_invalidated_artifact(volume_, *ledger_, 7, control).ok());

    const auto boot = rodaq::time::parse_boot_identity("11111111-2222-3333-4444-555555555555");
    ASSERT_TRUE(boot.ok());
    const auto report = artifact::inspect_volume_recovery(volume_, *ledger_, {}, boot.value());
    ASSERT_TRUE(report.ok());
    ASSERT_EQ(report.value().quarantined.size(), 1u);
    ASSERT_GT(report.value().quarantined_bytes, 0u);

    auto cap = artifact::VolumeCapacity::create(
        {1000000, 1000000, 0, 100, 16, 16, 32}, report.value().total_physical_bytes(),
        report.value().artifact_slots);
    ASSERT_TRUE(cap.ok());
    auto capacity = std::make_shared<artifact::VolumeCapacity>(std::move(cap.value()));
    artifact::CatalogOptions options;
    options.volume = volume_;
    options.ledger = std::move(ledger_);
    options.capacity = capacity;
    options.max_artifacts = 16;
    auto catalog = artifact::ArtifactCatalog::create(options);
    ASSERT_TRUE(catalog.ok());
    ASSERT_TRUE(catalog.value()->rebuild(report.value()).ok());
    EXPECT_FALSE(catalog.value()->retire_eligible(record.task_id).value());

    // Out-of-scope generation does not purge the quarantined entry.
    ASSERT_TRUE(catalog.value()
                    ->purge_quarantined_for_reset(record.task_id, record.control_generation - 1)
                    .ok());
    EXPECT_EQ(catalog.value()->snapshot().quarantined, 1u);

    // In-scope Reset purges the entry and reclaims both bytes and slot.
    ASSERT_TRUE(catalog.value()
                    ->purge_quarantined_for_reset(record.task_id, record.control_generation)
                    .ok());
    EXPECT_EQ(catalog.value()->snapshot().quarantined, 0u);
    EXPECT_EQ(capacity->snapshot().actual_bytes, 0u);
    EXPECT_EQ(capacity->snapshot().artifact_slots, 0u);
    EXPECT_TRUE(catalog.value()->retire_eligible(record.task_id).value());
    EXPECT_FALSE(catalog.value()->has_task_references(record.task_id).value());
}

TEST_F(SegmentCommitterTest, CancelReadersIsNoexceptFencesNewAcquiresAndRetainsInflightBacking) {
    // IC-R01: ArtifactCatalog::cancel_readers() must be noexcept, close
    // accepting_reads without allocation, cancel active readers, and retain
    // ReadBacking while an in-flight read_at() is still active.
    static_assert(noexcept(std::declval<artifact::ArtifactCatalog&>().cancel_readers()));
    static_assert(
        noexcept(std::declval<artifact::ArtifactCatalog&>().cancel_readers(rodaq::ArtifactId{1})));

    auto committer = prepare();
    ASSERT_TRUE(committer.ok());
    ASSERT_TRUE(committer.value()->commit(*ledger_).ok());

    const auto boot = rodaq::time::parse_boot_identity("11111111-2222-3333-4444-555555555555");
    ASSERT_TRUE(boot.ok());
    const auto report = artifact::inspect_volume_recovery(volume_, *ledger_, {}, boot.value());
    ASSERT_TRUE(report.ok());
    auto cap_res = artifact::VolumeCapacity::create(
        {1000000, 1000000, 0, 100, 16, 16, 32}, report.value().total_physical_bytes(),
        report.value().artifact_slots);
    ASSERT_TRUE(cap_res.ok());
    auto capacity = std::make_shared<artifact::VolumeCapacity>(std::move(cap_res.value()));
    artifact::CatalogOptions options;
    options.volume = volume_;
    options.ledger = std::move(ledger_);
    options.capacity = capacity;
    options.max_artifacts = 16;
    options.max_read_leases = 4;
    auto catalog = artifact::ArtifactCatalog::create(options);
    ASSERT_TRUE(catalog.ok());
    ASSERT_TRUE(catalog.value()->rebuild(report.value()).ok());

    {
        auto lease = catalog.value()->acquire(rodaq::ArtifactId{7}, rodaq::ArtifactFileId{1});
        ASSERT_TRUE(lease.ok());
        EXPECT_EQ(catalog.value()->snapshot().read_leases, 1u);

        catalog.value()->cancel_readers();

        // New acquire immediately rejected after global cancel fence.
        auto blocked = catalog.value()->acquire(rodaq::ArtifactId{7}, rodaq::ArtifactFileId{1});
        EXPECT_EQ(blocked.status().code(), StatusCode::InvalidState);

        // Existing lease is cancelled on read_at() and still owns its slot until destroyed.
        std::array<std::byte, 4> buf{};
        auto read_res = lease.value().read_at(0, {buf.data(), buf.size()});
        EXPECT_EQ(read_res.status().code(), StatusCode::InvalidState);
        EXPECT_EQ(catalog.value()->snapshot().read_leases, 1u);
    }
    EXPECT_EQ(catalog.value()->snapshot().read_leases, 0u);
}

}  // namespace
