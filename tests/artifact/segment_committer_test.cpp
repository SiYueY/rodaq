#include "artifact/segment_committer.hpp"
#include "manifest_fixture.hpp"
#include "persistence/atomic_file.hpp"
#include "artifact/recovery_scan.hpp"

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

}  // namespace
