#include "pipeline/record_queue.hpp"
#include "pipeline/descriptor.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <rodaq/storage/validation.hpp>

namespace {

namespace pipeline = rodaq::pipeline;
using rodaq::StatusCode;

class RecordQueueTest : public testing::Test {
protected:
    void SetUp() override {
        auto budget = pipeline::RecordBudget::create(16, 64);
        auto pool = pipeline::PayloadPool::create({4096, 256, 16, 16});
        auto queue = pipeline::RecordQueue::create(8);
        ASSERT_TRUE(budget.ok());
        ASSERT_TRUE(pool.ok());
        ASSERT_TRUE(queue.ok());
        budget_.emplace(std::move(budget).value());
        pool_.emplace(std::move(pool).value());
        queue_.emplace(std::move(queue).value());

        rodaq::acquisition::Descriptor proposal{};
        proposal.header.struct_size = sizeof(proposal);
        proposal.stream_id = 1;
        proposal.data_profile = RODAQ_DATA_PROFILE_GENERIC;
        proposal.channel_name = {"/data", 5};
        proposal.message_encoding = {"json", 4};
        auto descriptor = pipeline::Descriptor::create(proposal, 100);
        ASSERT_TRUE(descriptor.ok());
        descriptor_ = std::move(descriptor).value();
        task_id_ = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    }

    rodaq::Result<pipeline::Record> make_record(
        std::uint64_t sequence, std::uint64_t received = 100) {
        auto charge = budget_->reserve(4);
        if (!charge.ok()) {
            return std::move(charge).status();
        }
        auto payload = pool_->reserve(4);
        if (!payload.ok()) {
            return std::move(payload).status();
        }
        const std::array<unsigned char, 4> bytes{1, 2, 3, 4};
        auto copied = payload.value().copy_from(bytes.data(), bytes.size());
        if (!copied.ok()) {
            return std::move(copied).status();
        }
        pipeline::Record value;
        value.charge = std::move(charge).value();
        value.payload = std::move(payload).value();
        value.descriptor = descriptor_;
        value.task_id = task_id_;
        value.pipeline_id = 1;
        value.instance_id.value[15] = 1;
        value.segment_id = 7;
        value.stream_id = 1;
        value.acceptance_sequence = sequence;
        value.received = {received, 1000, 1};
        value.recording_time_ns = 1000 + received;
        return value;
    }

    std::optional<pipeline::RecordBudget> budget_;
    std::optional<pipeline::PayloadPool> pool_;
    pipeline::DescriptorRef descriptor_;
    std::optional<pipeline::RecordQueue> queue_;
    rodaq::TaskId task_id_;
};

TEST(RecordBudget, ZeroRecordsRemainBoundedAndFullWidthBytesNeverOverflow) {
    auto created = pipeline::RecordBudget::create(2, std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(created.ok());
    auto& budget = created.value();
    auto all_bytes = budget.reserve(std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(all_bytes.ok());
    EXPECT_EQ(budget.reserve(1).status().code(), StatusCode::ResourceExhausted);
    auto empty = budget.reserve(0);
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(budget.reserve(0).status().code(), StatusCode::ResourceExhausted);
    all_bytes.value().reset();
    EXPECT_EQ(budget.snapshot().logical_bytes, 0ULL);
    EXPECT_EQ(budget.snapshot().records, 1U);
    auto replacement = budget.reserve(std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(replacement.ok());
    auto moved = std::move(replacement).value();
    moved = std::move(empty).value();
    EXPECT_EQ(budget.snapshot().records, 1U);
    EXPECT_EQ(budget.snapshot().logical_bytes, 0ULL);
    moved.reset();
    EXPECT_EQ(budget.snapshot().records, 0U);
    EXPECT_EQ(
        budget.snapshot().high_water_logical_bytes, std::numeric_limits<std::uint64_t>::max());
}

TEST_F(RecordQueueTest, ReservationOrderDoesNotDeterminePublishPositionAndRejectedCreditsRollback) {
    auto small = pipeline::RecordQueue::create(2);
    ASSERT_TRUE(small.ok());
    auto& queue = small.value();
    auto first_reserved = queue.reserve();
    auto second_reserved = queue.reserve();
    ASSERT_TRUE(first_reserved.ok());
    ASSERT_TRUE(second_reserved.ok());
    EXPECT_EQ(queue.reserve().status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(queue.snapshot().reserved_slots, 2U);
    auto first_commit = make_record(1, 100);
    auto second_commit = make_record(2, 50);
    ASSERT_TRUE(first_commit.ok());
    ASSERT_TRUE(second_commit.ok());
    queue.publish(std::move(second_reserved).value(), std::move(first_commit).value());
    queue.publish(std::move(first_reserved).value(), std::move(second_commit).value());
    const auto snapshot = queue.snapshot();
    EXPECT_EQ(snapshot.reserved_slots, 0U);
    EXPECT_EQ(snapshot.queued_records, 2U);
    EXPECT_EQ(snapshot.queued_logical_bytes, 8ULL);
    EXPECT_EQ(snapshot.oldest_receive_monotonic, 50ULL);
    auto first = queue.pop();
    auto second = queue.pop();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->acceptance_sequence, 1ULL);
    EXPECT_EQ(second->acceptance_sequence, 2ULL);
    EXPECT_FALSE(queue.pop());
    EXPECT_FALSE(queue.snapshot().oldest_receive_monotonic);
    EXPECT_EQ(budget_->snapshot().records, 2U);
    first.reset();
    second.reset();
    EXPECT_EQ(budget_->snapshot().records, 0U);
    EXPECT_EQ(pool_->snapshot().used_physical_bytes, 0ULL);
    {
        auto abandoned = queue.reserve();
        ASSERT_TRUE(abandoned.ok());
    }
    EXPECT_EQ(queue.snapshot().reserved_slots, 0U);
}

TEST_F(RecordQueueTest, StopSeparatesProducerReservationFromGuaranteedExistingPublicationAndDrain) {
    auto reserved = queue_->reserve();
    ASSERT_TRUE(reserved.ok());
    queue_->stop_reservations();
    EXPECT_EQ(queue_->reserve().status().code(), StatusCode::InvalidState);
    auto accepted = make_record(1);
    ASSERT_TRUE(accepted.ok());
    queue_->publish(std::move(reserved).value(), std::move(accepted).value());
    auto drain = queue_->pop();
    ASSERT_TRUE(drain);
    EXPECT_EQ(drain->segment_id, 7ULL);
    EXPECT_EQ(queue_->snapshot().queued_records, 0U);
    EXPECT_FALSE(queue_->snapshot().accepting);
    EXPECT_EQ(budget_->snapshot().records, 1U);
    EXPECT_EQ(pool_->snapshot().live_buffers, 1U);
    drain.reset();
    EXPECT_EQ(budget_->snapshot().records, 0U);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0U);
}

TEST_F(RecordQueueTest, StorageViewBorrowRemainsOwnedThroughActualCallAndPreservesPublishTime) {
    auto reserved = queue_->reserve();
    auto accepted = make_record(1);
    ASSERT_TRUE(reserved.ok());
    ASSERT_TRUE(accepted.ok());
    accepted.value().has_publish_time = 1;
    accepted.value().publish_time_ns = 987654321ULL;
    queue_->publish(std::move(reserved).value(), std::move(accepted).value());
    auto drain = queue_->pop();
    ASSERT_TRUE(drain);
    const auto view = drain->view();
    EXPECT_TRUE(rodaq::storage::abi::valid_record(view));
    EXPECT_EQ(view.recording_time, 1100ULL);
    EXPECT_EQ(view.has_publish_time, 1U);
    EXPECT_EQ(view.publish_time, 987654321ULL);
    std::promise<void> release;
    auto permission = release.get_future();
    auto actual_call = std::async(
        std::launch::async,
        [owned = std::move(*drain), permission = std::move(permission)]() mutable {
            const auto borrowed = owned.view();
            permission.wait();
            return borrowed.payload.size == 4 && borrowed.payload.data[0] == 1 &&
                   borrowed.payload.data[3] == 4;
        });
    drain.reset();
    EXPECT_EQ(queue_->snapshot().queued_records, 0U);
    EXPECT_EQ(budget_->snapshot().records, 1U);
    EXPECT_EQ(pool_->snapshot().live_buffers, 1U);
    release.set_value();
    EXPECT_TRUE(actual_call.get());
    EXPECT_EQ(budget_->snapshot().records, 0U);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0U);
}

TEST_F(RecordQueueTest, ConcurrentCapacityReservationAndStreamCommitPreserveSequenceWithoutGrowth) {
    std::mutex stream_commit;
    std::uint64_t next_sequence = 1;
    std::atomic<unsigned> finished{0};
    std::atomic<bool> valid{true};
    auto consumer = std::async(std::launch::async, [&] {
        std::uint64_t previous = 0;
        while (finished != 4 || queue_->snapshot().queued_records != 0) {
            auto item = queue_->pop();
            if (item) {
                if (item->acceptance_sequence != previous + 1) {
                    valid = false;
                }
                previous = item->acceptance_sequence;
            } else {
                std::this_thread::yield();
            }
        }
        return previous;
    });
    std::vector<std::future<void>> producers;
    for (unsigned producer = 0; producer < 4; ++producer) {
        producers.push_back(std::async(std::launch::async, [&] {
            for (unsigned attempt = 0; attempt < 100; ++attempt) {
                auto reserved = queue_->reserve();
                while (!reserved.ok() &&
                       reserved.status().code() == StatusCode::ResourceExhausted) {
                    std::this_thread::yield();
                    reserved = queue_->reserve();
                }
                if (!reserved.ok()) {
                    valid = false;
                    break;
                }
                auto pending = make_record(0);
                if (!pending.ok()) {
                    valid = false;
                    break;
                }
                std::lock_guard<std::mutex> lock(stream_commit);
                pending.value().acceptance_sequence = next_sequence++;
                queue_->publish(std::move(reserved).value(), std::move(pending).value());
            }
            ++finished;
        }));
    }
    for (auto& producer : producers) {
        producer.get();
    }
    EXPECT_EQ(consumer.get(), 400ULL);
    EXPECT_TRUE(valid.load());
    EXPECT_EQ(queue_->snapshot().capacity, 8U);
    EXPECT_EQ(queue_->snapshot().reserved_slots, 0U);
    EXPECT_EQ(budget_->snapshot().records, 0U);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0U);
    EXPECT_LE(queue_->snapshot().high_water_records, 8U);
}

TEST_F(RecordQueueTest, BatchPopNeedsPreparedBackingAndNeverCrossesSegmentOrByteBoundaries) {
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        auto pending = make_record(sequence);
        auto credit = queue_->reserve();
        ASSERT_TRUE(pending.ok());
        ASSERT_TRUE(credit.ok());
        if (sequence == 3) {
            pending.value().segment_id = 8;
        }
        queue_->publish(std::move(credit.value()), std::move(pending.value()));
    }
    std::vector<pipeline::Record> batch;
    EXPECT_EQ(queue_->pop_batch(7, 2, 8, batch).status().code(), StatusCode::InvalidArgument);
    EXPECT_EQ(queue_->snapshot().queued_records, 3U);
    batch.reserve(2);
    EXPECT_EQ(queue_->pop_batch(7, 2, 3, batch).status().code(), StatusCode::DataLoss);
    EXPECT_EQ(queue_->snapshot().queued_records, 3U);
    auto first = queue_->pop_batch(7, 2, 4, batch);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch[0].acceptance_sequence, 1ULL);
    EXPECT_EQ(first.value(), 7ULL);
    EXPECT_EQ(budget_->snapshot().records, 3U);
    batch.clear();
    auto second = queue_->pop_batch(7, 2, 8, batch);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch[0].acceptance_sequence, 2ULL);
    EXPECT_EQ(second.value(), 8ULL);
    EXPECT_EQ(queue_->snapshot().queued_records, 1U);
    batch.clear();
    auto third = queue_->pop_batch(8, 2, 8, batch);
    ASSERT_TRUE(third.ok());
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch[0].acceptance_sequence, 3ULL);
    EXPECT_FALSE(third.value());
    batch.clear();
    EXPECT_EQ(budget_->snapshot().records, 0U);
}

}  // namespace
