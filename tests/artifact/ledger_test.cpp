#include "artifact/ledger_store.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <limits>
#include <optional>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace artifact = rodaq::artifact;
namespace persistence = rodaq::persistence;
namespace fs = std::filesystem;
using artifact::LedgerOpenMode;
using artifact::LedgerRecord;
using artifact::LedgerState;
using artifact::LedgerStore;
using rodaq::StatusCode;

LedgerRecord record(std::uint64_t id = 1) {
    LedgerRecord result;
    result.segment_id = id;
    result.task_id = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    result.pipeline_id = 3;
    result.volume_id = artifact::parse_volume_id("11234567-89ab-4cde-8fab-0123456789ab").value();
    result.managed_relative_path = ".rodaq/artifacts/" + std::to_string(id);
    result.manifest_digest = "sha256:" + std::string(64, 'a');
    result.control_generation = 7;
    result.reset_generation_at_commit = 2;
    return result;
}

LedgerRecord committed(LedgerRecord value) {
    value.state = LedgerState::Committed;
    value.committed_at = rodaq::time::WallTime(std::chrono::nanoseconds(-1));
    return value;
}

TEST(LedgerRecord, IndependentGoldenAndManifestDigestParticipationAreStable) {
    auto encoded = artifact::encode_ledger_record(record());
    ASSERT_TRUE(encoded.ok());
    auto document = persistence::parse_json(encoded.value());
    ASSERT_TRUE(document.ok());
    // Independently computed with Python hashlib and sorted compact JSON; all
    // keys/values here are ASCII strings or null, so that encoding equals JCS.
    EXPECT_EQ(
        document.value().at("content_digest"),
        "sha256:5dba8ae41192c0b233827e44cc3acae02ab684d17af89acfb18490d48e4b94a3");
    EXPECT_TRUE(artifact::decode_ledger_record(encoded.value()).ok());
    document.value()["manifest_digest"] = "sha256:" + std::string(64, 'b');
    EXPECT_EQ(
        artifact::decode_ledger_record(document.value().dump()).status().code(),
        StatusCode::DataLoss);
}

TEST(LedgerRecord, LifecycleIsClosedMonotonicAndPreservesImmutableIdentityDespiteBackwardWallTime) {
    const auto initial = record();
    auto next = committed(initial);
    EXPECT_TRUE(artifact::validate_ledger_transition(initial, next).ok());
    EXPECT_EQ(
        artifact::validate_ledger_transition(next, initial).status().code(), StatusCode::Conflict);
    auto deleting = next;
    deleting.state = LedgerState::Deleting;
    deleting.delete_generation = 1;
    deleting.delete_reason = "reset";
    EXPECT_TRUE(artifact::validate_ledger_transition(next, deleting).ok());
    EXPECT_EQ(
        artifact::validate_ledger_transition(initial, deleting).status().code(),
        StatusCode::Conflict);
    auto deleted = deleting;
    deleted.state = LedgerState::Deleted;
    deleted.deleted_at = rodaq::time::WallTime(std::chrono::nanoseconds(-2));
    EXPECT_TRUE(artifact::validate_ledger_transition(deleting, deleted).ok());
    EXPECT_TRUE(
        artifact::decode_ledger_record(artifact::encode_ledger_record(deleted).value()).ok());
    ++deleted.control_generation;
    EXPECT_EQ(
        artifact::validate_ledger_transition(deleting, deleted).status().code(),
        StatusCode::Conflict);
    next.committed_at.reset();
    EXPECT_EQ(artifact::validate_ledger_record(next).status().code(), StatusCode::InvalidArgument);
}

TEST(LedgerRecord, ExactU64AndClosedSchemaRejectMalformedOrUnsupportedFacts) {
    auto value = record(std::numeric_limits<std::uint64_t>::max());
    value.control_generation = std::numeric_limits<std::uint64_t>::max();
    auto encoded = artifact::encode_ledger_record(value);
    ASSERT_TRUE(encoded.ok());
    auto decoded = artifact::decode_ledger_record(encoded.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().segment_id, value.segment_id);
    auto json = persistence::parse_json(encoded.value()).value();
    json["state"] = "unknown";
    auto altered = persistence::encode_integrity(json, "rodaq.artifact-ledger.v1");
    ASSERT_TRUE(altered.ok());
    EXPECT_EQ(
        artifact::decode_ledger_record(altered.value()).status().code(), StatusCode::DataLoss);
    json["state"] = "committing";
    json["pipeline_id"] = 3;
    altered = persistence::encode_integrity(json, "rodaq.artifact-ledger.v1");
    ASSERT_TRUE(altered.ok());
    EXPECT_EQ(
        artifact::decode_ledger_record(altered.value()).status().code(), StatusCode::DataLoss);
}

class LedgerStoreTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-ledger-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    rodaq::Result<std::unique_ptr<LedgerStore>> open(
        std::uint32_t limit = 4, persistence::FileFaultHook faults = {},
        LedgerOpenMode mode = LedgerOpenMode::Initialize) {
        return LedgerStore::open(root_, mode, limit, std::move(faults));
    }

    void create(LedgerStore& owner, std::uint64_t id = 1) {
        auto registration = owner.reserve_registration(id);
        ASSERT_TRUE(registration.ok());
        ASSERT_TRUE(owner.create_committing(registration.value(), record(id)).ok());
    }

    void crash_after_rename(bool advancing) {
        auto initialized = open();
        ASSERT_TRUE(initialized.ok());
        if (advancing) {
            create(*initialized.value());
        }
        initialized.value().reset();
        const auto child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            bool armed = false;
            auto owner = open(
                4,
                [&](persistence::FileStep step) {
                    if (armed && step == persistence::FileStep::SyncDirectory) {
                        _exit(73);
                    }
                    return 0;
                },
                LedgerOpenMode::Existing);
            if (!owner.ok()) {
                _exit(74);
            }
            armed = true;
            if (advancing) {
                (void)owner.value()->advance(committed(record()), LedgerState::Committing);
            } else {
                auto reserved = owner.value()->reserve_registration(1);
                if (!reserved.ok()) {
                    _exit(75);
                }
                (void)owner.value()->create_committing(reserved.value(), record());
            }
            _exit(76);
        }
        int status = 0;
        ASSERT_EQ(waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 73);
        auto recovered = open(4, {}, LedgerOpenMode::Existing);
        ASSERT_TRUE(recovered.ok());
        auto persisted = recovered.value()->get(1);
        ASSERT_TRUE(persisted.ok());
        EXPECT_EQ(
            persisted.value().state, advancing ? LedgerState::Committed : LedgerState::Committing);
    }

    fs::path root_;
};

TEST_F(
    LedgerStoreTest,
    FixedShardsAndRegistrationCapacityExistBeforePublicationAndRollbackIsMemoryOnly) {
    auto opened = open(1);
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(std::distance(fs::directory_iterator(root_), fs::directory_iterator()), 257);
    EXPECT_EQ(open(1).status().code(), StatusCode::Conflict);
    {
        auto slot = opened.value()->reserve_registration(1);
        ASSERT_TRUE(slot.ok());
        EXPECT_EQ(opened.value()->reserve_registration(1).status().code(), StatusCode::Conflict);
        EXPECT_EQ(
            opened.value()->reserve_registration(2).status().code(), StatusCode::ResourceExhausted);
        EXPECT_EQ(opened.value()->get(1).status().code(), StatusCode::NotFound);
        EXPECT_FALSE(fs::exists(root_ / "01/1.json"));
        auto moved = std::move(slot.value());
    }
    EXPECT_TRUE(opened.value()->reserve_registration(2).ok());
}

TEST_F(
    LedgerStoreTest, DurableLifecyclePersistsInFixedShardAndTombstoneRetainsItsRegistrationSlot) {
    auto opened = open(1);
    ASSERT_TRUE(opened.ok());
    auto& owner = *opened.value();
    create(owner);
    auto active = committed(record());
    ASSERT_TRUE(owner.advance(active, LedgerState::Committing).ok());
    EXPECT_EQ(owner.advance(active, LedgerState::Committing).status().code(), StatusCode::Conflict);
    ASSERT_TRUE(owner.advance(active, LedgerState::Committed).ok());
    active.state = LedgerState::Deleting;
    active.delete_generation = 1;
    active.delete_reason = "retention";
    ASSERT_TRUE(owner.advance(active, LedgerState::Committed).ok());
    active.state = LedgerState::Deleted;
    active.deleted_at = rodaq::time::WallTime(std::chrono::nanoseconds(-2));
    ASSERT_TRUE(owner.advance(active, LedgerState::Deleting).ok());
    EXPECT_EQ(owner.reserve_registration(2).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(owner.get(1).value().state, LedgerState::Deleted);
    opened.value().reset();
    auto recovered = open(1, {}, LedgerOpenMode::Existing);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value()->recovery_snapshot().value().size(), 1);
    EXPECT_EQ(recovered.value()->get(1).value().state, LedgerState::Deleted);
}

TEST_F(LedgerStoreTest, CreateUncertaintyCannotReturnAuthorityOrReleaseCapacityWhenTokenIsDropped) {
    bool armed = false;
    auto opened = open(1, [&](persistence::FileStep step) {
        return armed && step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(opened.ok());
    {
        auto registration = opened.value()->reserve_registration(1);
        ASSERT_TRUE(registration.ok());
        armed = true;
        EXPECT_FALSE(opened.value()->create_committing(registration.value(), record()).ok());
        EXPECT_EQ(opened.value()->get(1).status().code(), StatusCode::Unavailable);
        EXPECT_FALSE(opened.value()->reconcile(1).ok());
    }
    EXPECT_EQ(
        opened.value()->reserve_registration(2).status().code(), StatusCode::ResourceExhausted);
    armed = false;
    ASSERT_TRUE(opened.value()->reconcile(1).ok());
    EXPECT_EQ(opened.value()->get(1).value().state, LedgerState::Committing);
    EXPECT_EQ(opened.value()->reserve_registration(1).status().code(), StatusCode::Conflict);
}

TEST_F(LedgerStoreTest, FailedInitialPublishReconcilesAbsenceAndRetryUsesTheSameReservedIdentity) {
    bool armed = false;
    auto opened = open(1, [&](persistence::FileStep step) {
        return armed && step == persistence::FileStep::WriteTemporary ? ENOSPC : 0;
    });
    ASSERT_TRUE(opened.ok());
    auto reserved = opened.value()->reserve_registration(1);
    ASSERT_TRUE(reserved.ok());
    armed = true;
    EXPECT_FALSE(opened.value()->create_committing(reserved.value(), record()).ok());
    armed = false;
    ASSERT_TRUE(opened.value()->reconcile(1).ok());
    EXPECT_EQ(opened.value()->get(1).status().code(), StatusCode::NotFound);
    ASSERT_TRUE(opened.value()->create_committing(reserved.value(), record()).ok());
    EXPECT_EQ(opened.value()->get(1).value().state, LedgerState::Committing);
}

TEST_F(LedgerStoreTest, AdvanceUncertaintyReconcilesActualKnownCandidateAndNeverRollsBack) {
    bool armed = false;
    auto opened = open(1, [&](persistence::FileStep step) {
        return armed && step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(opened.ok());
    create(*opened.value());
    armed = true;
    EXPECT_FALSE(opened.value()->advance(committed(record()), LedgerState::Committing).ok());
    EXPECT_EQ(opened.value()->get(1).status().code(), StatusCode::Unavailable);
    armed = false;
    ASSERT_TRUE(opened.value()->reconcile(1).ok());
    EXPECT_EQ(opened.value()->get(1).value().state, LedgerState::Committed);
    EXPECT_EQ(
        opened.value()->advance(record(), LedgerState::Committed).status().code(),
        StatusCode::Conflict);
}

TEST_F(LedgerStoreTest, BlockedPersistenceDoesNotBlockOtherProjectionOrCapacityOwners) {
    bool armed = false;
    std::promise<void> entered;
    std::promise<void> release;
    auto allowed = release.get_future().share();
    auto opened = open(4, [&](persistence::FileStep step) {
        if (armed && step == persistence::FileStep::SyncFile) {
            entered.set_value();
            allowed.wait();
        }
        return 0;
    });
    ASSERT_TRUE(opened.ok());
    create(*opened.value(), 1);
    create(*opened.value(), 2);
    armed = true;
    auto flight = std::async(std::launch::async, [&] {
        return opened.value()->advance(committed(record()), LedgerState::Committing);
    });
    const bool inside =
        entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(inside);
    auto projection = std::async(std::launch::async, [&] {
        return opened.value()->get(1).status().code() == StatusCode::Unavailable &&
               opened.value()->get(2).ok() && opened.value()->reserve_registration(3).ok() &&
               opened.value()
                       ->advance(committed(record(2)), LedgerState::Committing)
                       .status()
                       .code() == StatusCode::Conflict;
    });
    const bool responsive =
        projection.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(responsive);
    release.set_value();
    EXPECT_TRUE(projection.get());
    EXPECT_TRUE(flight.get().ok());
}

TEST_F(
    LedgerStoreTest, RecoveryOverConfiguredCapacityRetainsExistingFactsAndRejectsNewRegistration) {
    auto opened = open(2);
    ASSERT_TRUE(opened.ok());
    create(*opened.value(), 1);
    create(*opened.value(), 2);
    opened.value().reset();
    auto recovered = open(1, {}, LedgerOpenMode::Existing);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value()->recovery_snapshot().value().size(), 2);
    EXPECT_EQ(
        recovered.value()->reserve_registration(3).status().code(), StatusCode::ResourceExhausted);
    EXPECT_TRUE(recovered.value()->advance(committed(record(2)), LedgerState::Committing).ok());
}

TEST_F(LedgerStoreTest, MissingFixedShardAndWrongShardRecordFailClosed) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    create(*opened.value());
    opened.value().reset();
    fs::rename(root_ / "01/1.json", root_ / "02/1.json");
    EXPECT_EQ(open(4, {}, LedgerOpenMode::Existing).status().code(), StatusCode::DataLoss);
    fs::rename(root_ / "02/1.json", root_ / "01/1.json");
    fs::remove_all(root_ / "ff");
    EXPECT_EQ(open(4, {}, LedgerOpenMode::Existing).status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(root_ / "ff"));
}

TEST_F(LedgerStoreTest, ProcessExitAfterCommittingRenameRecoversOnlyCommittingAuthority) {
    crash_after_rename(false);
}

TEST_F(LedgerStoreTest, ProcessExitAfterCommittedRenameRecoversActualDurableSelectedAuthority) {
    crash_after_rename(true);
}

TEST_F(
    LedgerStoreTest,
    ExternalDivergenceWithholdsCachedAuthorityUntilTheKnownRecordIsRestoredDurably) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    create(*opened.value());
    auto altered = record();
    altered.manifest_digest = "sha256:" + std::string(64, 'b');
    const auto bytes = artifact::encode_ledger_record(altered);
    ASSERT_TRUE(bytes.ok());
    auto published = persistence::publish_file(
        root_ / "01", "1.json", bytes.value(), persistence::FilePublishMode::Replace, 4096);
    ASSERT_TRUE(published.status.ok());
    EXPECT_EQ(
        opened.value()->advance(committed(record()), LedgerState::Committing).status().code(),
        StatusCode::DataLoss);
    EXPECT_EQ(opened.value()->get(1).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(opened.value()->reconcile(1).status().code(), StatusCode::DataLoss);
    const auto original = artifact::encode_ledger_record(record());
    ASSERT_TRUE(original.ok());
    published = persistence::publish_file(
        root_ / "01", "1.json", original.value(), persistence::FilePublishMode::Replace, 4096);
    ASSERT_TRUE(published.status.ok());
    ASSERT_TRUE(opened.value()->reconcile(1).ok());
    EXPECT_EQ(opened.value()->get(1).value().state, LedgerState::Committing);
}

}  // namespace

namespace {

void make_deleted(LedgerStore& owner, std::uint64_t id = 1) {
    auto value = committed(record(id));
    ASSERT_TRUE(owner.advance(value, LedgerState::Committing).ok());
    value.state = LedgerState::Deleting;
    value.delete_generation = 7;
    value.delete_reason = "retention";
    ASSERT_TRUE(owner.advance(value, LedgerState::Committed).ok());
    value.state = LedgerState::Deleted;
    value.deleted_at = rodaq::time::WallTime(std::chrono::seconds{10});
    ASSERT_TRUE(owner.advance(value, LedgerState::Deleting).ok());
}

TEST_F(LedgerStoreTest, TombstoneCollectionReleasesOnlyDurablyDeletedRegistrationCapacity) {
    auto owner = open(1);
    ASSERT_TRUE(owner.ok());
    create(*owner.value());
    EXPECT_EQ(owner.value()->collect_deleted(1).status().code(), StatusCode::Conflict);
    make_deleted(*owner.value());
    ASSERT_TRUE(owner.value()->collect_deleted(1).ok());
    EXPECT_EQ(owner.value()->get(1).status().code(), StatusCode::NotFound);
    EXPECT_TRUE(owner.value()->recovery_snapshot().value().empty());
    EXPECT_FALSE(fs::exists(root_ / "01/1.json"));
    create(*owner.value(), 2);
    EXPECT_EQ(owner.value()->get(2).value().state, LedgerState::Committing);
    owner.value().reset();
    owner = open(1, {}, LedgerOpenMode::Existing);
    ASSERT_TRUE(owner.ok());
    EXPECT_EQ(owner.value()->recovery_snapshot().value().size(), 1);
    EXPECT_EQ(owner.value()->get(2).value().segment_id, 2);
}

TEST_F(LedgerStoreTest, ControlledAbortCannotRemoveCommittedAuthorityOrBypassCleanupFailure) {
    auto owner = open(1);
    ASSERT_TRUE(owner.ok());
    create(*owner.value());
    EXPECT_EQ(owner.value()->abort_committing(1, {}).status().code(), StatusCode::InvalidArgument);
    const auto denied = owner.value()->abort_committing(1, [](const auto&) -> rodaq::Result<void> {
        return rodaq::Status(StatusCode::Conflict, {});
    });
    EXPECT_EQ(denied.status().code(), StatusCode::Conflict);
    EXPECT_EQ(owner.value()->get(1).value().state, LedgerState::Committing);
    auto committed = owner.value()->get(1).value();
    committed.state = LedgerState::Committed;
    committed.committed_at = rodaq::WallTime{std::chrono::seconds{2}};
    ASSERT_TRUE(owner.value()->advance(committed, LedgerState::Committing).ok());
    EXPECT_EQ(
        owner.value()
            ->abort_committing(
                1,
                [](const auto&) -> rodaq::Result<void> {
                    ADD_FAILURE() << "Committed authority cannot enter pre-commit abort";
                    return {};
                })
            .status()
            .code(),
        StatusCode::Conflict);
    EXPECT_EQ(owner.value()->get(1).value().state, LedgerState::Committed);
}

TEST_F(LedgerStoreTest, TombstoneCollectionCannotInvalidateALiveRegistrationToken) {
    bool armed = false;
    auto owner = open(1, [&](auto step) {
        return armed && step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(owner.ok());
    {
        auto token = owner.value()->reserve_registration(1);
        ASSERT_TRUE(token.ok());
        // A failed publication retains the original registration wrapper even
        // after reconciliation discovers its durable committing record.
        armed = true;
        ASSERT_FALSE(owner.value()->create_committing(token.value(), record()).ok());
        armed = false;
        ASSERT_TRUE(owner.value()->reconcile(1).ok());
        make_deleted(*owner.value());
        EXPECT_EQ(owner.value()->collect_deleted(1).status().code(), StatusCode::Conflict);
        EXPECT_TRUE(fs::exists(root_ / "01/1.json"));
    }
    ASSERT_TRUE(owner.value()->collect_deleted(1).ok());
    create(*owner.value(), 2);
    EXPECT_EQ(owner.value()->get(2).value().segment_id, 2);
}

TEST_F(LedgerStoreTest, AbortUncertaintyRetainsCapacityAndCannotInvalidateALiveRegistrationToken) {
    bool publication_fault = false;
    bool removal_fault = false;
    auto owner = open(1, [&](auto step) {
        return (publication_fault && step == persistence::FileStep::SyncDirectory) ||
                       (removal_fault && step == persistence::FileStep::RemoveAuthority)
                   ? EIO
                   : 0;
    });
    ASSERT_TRUE(owner.ok());
    const auto proof = [](const auto&) -> rodaq::Result<void> { return {}; };
    {
        auto token = owner.value()->reserve_registration(1);
        ASSERT_TRUE(token.ok());
        publication_fault = true;
        ASSERT_FALSE(owner.value()->create_committing(token.value(), record()).ok());
        publication_fault = false;
        ASSERT_TRUE(owner.value()->reconcile(1).ok());
        EXPECT_EQ(owner.value()->abort_committing(1, proof).status().code(), StatusCode::Conflict);
    }
    removal_fault = true;
    EXPECT_EQ(owner.value()->abort_committing(1, proof).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(
        owner.value()->reserve_registration(2).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(owner.value()->reconcile(1).status().code(), StatusCode::Unavailable);
    removal_fault = false;
    ASSERT_TRUE(owner.value()->abort_committing(1, proof).ok());
    EXPECT_TRUE(owner.value()->reserve_registration(2).ok());
}

class LedgerCollectionFaultTest : public LedgerStoreTest,
                                  public testing::WithParamInterface<persistence::FileStep> {};

TEST_P(LedgerCollectionFaultTest, UncertainRemovalRetainsCapacityUntilExplicitMaintenanceRetry) {
    bool armed = false;
    auto owner = open(1, [&](auto step) { return armed && step == GetParam() ? EIO : 0; });
    ASSERT_TRUE(owner.ok());
    create(*owner.value());
    make_deleted(*owner.value());
    armed = true;
    EXPECT_EQ(owner.value()->collect_deleted(1).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(
        fs::exists(root_ / "01/1.json"), GetParam() == persistence::FileStep::RemoveAuthority);
    EXPECT_EQ(owner.value()->get(1).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(owner.value()->recovery_snapshot().status().code(), StatusCode::Unavailable);
    EXPECT_EQ(owner.value()->reconcile(1).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(
        owner.value()->reserve_registration(2).status().code(), StatusCode::ResourceExhausted);
    armed = false;
    ASSERT_TRUE(owner.value()->collect_deleted(1).ok());
    create(*owner.value(), 2);
    EXPECT_EQ(owner.value()->get(2).value().segment_id, 2);
}

INSTANTIATE_TEST_SUITE_P(
    RemovalBoundaries, LedgerCollectionFaultTest,
    testing::Values(persistence::FileStep::RemoveAuthority, persistence::FileStep::SyncDirectory));

TEST_F(LedgerStoreTest, FirstCollectionCannotReinterpretAnUnexpectedMissingTombstoneAsLegalGC) {
    auto owner = open(1);
    ASSERT_TRUE(owner.ok());
    create(*owner.value());
    make_deleted(*owner.value());
    ASSERT_TRUE(fs::remove(root_ / "01/1.json"));
    EXPECT_EQ(owner.value()->collect_deleted(1).status().code(), StatusCode::DataLoss);
    EXPECT_EQ(owner.value()->collect_deleted(1).status().code(), StatusCode::DataLoss);
    EXPECT_EQ(
        owner.value()->reserve_registration(2).status().code(), StatusCode::ResourceExhausted);
}

TEST_F(LedgerStoreTest, BlockedCollectionNeverHoldsTheProjectionLockOrCreditsCapacityEarly) {
    bool armed = false;
    std::promise<void> entered;
    std::promise<void> release;
    auto allowed = release.get_future().share();
    auto owner = open(1, [&](auto step) {
        if (armed && step == persistence::FileStep::SyncDirectory) {
            entered.set_value();
            allowed.wait();
        }
        return 0;
    });
    ASSERT_TRUE(owner.ok());
    create(*owner.value());
    make_deleted(*owner.value());
    armed = true;
    auto collecting =
        std::async(std::launch::async, [&] { return owner.value()->collect_deleted(1); });
    const auto observed = entered.get_future().wait_for(std::chrono::seconds{2});
    if (observed == std::future_status::ready) {
        EXPECT_EQ(owner.value()->get(1).status().code(), StatusCode::Unavailable);
        EXPECT_EQ(
            owner.value()->reserve_registration(2).status().code(), StatusCode::ResourceExhausted);
        EXPECT_EQ(owner.value()->collect_deleted(1).status().code(), StatusCode::Conflict);
    }
    release.set_value();
    EXPECT_EQ(observed, std::future_status::ready);
    EXPECT_TRUE(collecting.get().ok());
    armed = false;
    create(*owner.value(), 2);
}

}  // namespace
