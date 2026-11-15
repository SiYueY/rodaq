#include "pipeline/stream_table.hpp"

#include <rodaq/acquisition/validation.hpp>

#include <gtest/gtest.h>

#include <future>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace pipeline = rodaq::pipeline;
using rodaq::StatusCode;

std::vector<pipeline::StreamPreparation> prepared() {
    return {{2, "/right", 8, {100, 103}, {1, 3}}, {1, "/left", 4, {10, 13}, {1, 3}}};
}

TEST(StreamTable, FixedLookupAndPrepareViewsKeepExactTaskBindingAndIndependentStreamIdentities) {
    auto created = pipeline::StreamTable::create(prepared());
    ASSERT_TRUE(created.ok());
    auto& table = created.value();
    const auto& bindings = table.bindings();
    ASSERT_EQ(bindings.size(), 2);
    EXPECT_EQ(bindings[0].stream_id, 1);
    EXPECT_EQ(std::string_view(bindings[0].stream_key.data, bindings[0].stream_key.size), "/left");
    EXPECT_EQ(bindings[0].max_record_bytes, 4);
    EXPECT_EQ(bindings[1].stream_id, 2);
    EXPECT_EQ(bindings[1].max_record_bytes, 8);
    const rodaq::acquisition::PrepareConfig config{{sizeof(config)}, bindings.data(), 2, 1};
    EXPECT_TRUE(rodaq::acquisition::abi::valid_prepare(&config));
    EXPECT_EQ(table.find(1)->stream_id, 1);
    EXPECT_EQ(table.find(2)->max_record_bytes, 8);
    EXPECT_EQ(table.find(0), nullptr);
    EXPECT_EQ(table.find(3), nullptr);
    EXPECT_EQ(table.find(1)->descriptor_ids.allocate().value(), 1);
    EXPECT_EQ(table.find(2)->descriptor_ids.allocate().value(), 1);
    EXPECT_EQ(table.find(1)->sequences.allocate().value(), 10);
    EXPECT_EQ(table.find(2)->sequences.allocate().value(), 100);
}

TEST(StreamTable, MovingTheOwnerPreservesEntryAndShortAndLongKeyBacking) {
    auto input = prepared();
    input[0].stream_key = std::string(128, 'x');
    auto created = pipeline::StreamTable::create(std::move(input));
    ASSERT_TRUE(created.ok());
    auto* original = created.value().find(1);
    const auto* short_key = created.value().bindings()[0].stream_key.data;
    const auto* long_key = created.value().bindings()[1].stream_key.data;
    auto moved = std::move(created.value());
    auto target = pipeline::StreamTable::create({{9, "/unused", 8, {1, 2}, {1, 2}}});
    ASSERT_TRUE(target.ok());
    target.value() = std::move(moved);
    EXPECT_EQ(target.value().find(1), original);
    EXPECT_EQ(target.value().bindings()[0].stream_key.data, short_key);
    EXPECT_EQ(target.value().bindings()[1].stream_key.data, long_key);
    EXPECT_EQ(std::string_view(short_key, 5), "/left");
    EXPECT_EQ(std::string_view(long_key, 128), std::string(128, 'x'));
}

TEST(StreamTable, InvalidStructureAndRangesFailBeforePublishingAnyBinding) {
    EXPECT_FALSE(pipeline::StreamTable::create({}).ok());
    EXPECT_FALSE(pipeline::StreamTable::create(prepared(), 0).ok());
    EXPECT_FALSE(pipeline::StreamTable::create(prepared(), 1).ok());
    EXPECT_FALSE(pipeline::StreamTable::create(prepared(), 4097).ok());
    for (int invalid = 0; invalid != 10; ++invalid) {
        auto input = prepared();
        switch (invalid) {
            case 0:
                input[0].stream_id = 0;
                break;
            case 1:
                input[0].stream_id = input[1].stream_id;
                break;
            case 2:
                input[0].stream_key = input[1].stream_key;
                break;
            case 3:
                input[0].stream_key = "has space";
                break;
            case 4:
                input[0].stream_key = std::string(129, 'x');
                break;
            case 5:
                input[0].stream_key = std::string("key\0tail", 8);
                break;
            case 6:
                input[0].max_record_bytes = 0;
                break;
            case 7:
                input[0].max_record_bytes =
                    std::uint64_t{1} + std::numeric_limits<std::uint32_t>::max();
                break;
            case 8:
                input[0].sequences = {2, 2};
                break;
            case 9:
                input[0].descriptors = {0, 1};
                break;
        }
        EXPECT_EQ(
            pipeline::StreamTable::create(std::move(input)).status().code(),
            StatusCode::InvalidArgument);
    }
}

TEST(StreamTable, DescriptorConsumptionNeverConsumesTheAcceptanceRange) {
    auto created = pipeline::StreamTable::create(prepared());
    ASSERT_TRUE(created.ok());
    auto& stream = *created.value().find(1);
    std::lock_guard<std::mutex> lock(stream.mutex);
    EXPECT_EQ(stream.descriptor_ids.allocate().value(), 1);
    EXPECT_EQ(stream.descriptor_ids.allocate().value(), 2);
    EXPECT_EQ(stream.descriptor_ids.allocate().status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(stream.sequences.snapshot().active_remaining, 3);
    EXPECT_EQ(stream.sequences.allocate().value(), 10);
    EXPECT_EQ(stream.descriptor_ids.snapshot().active_remaining, 0);
}

TEST(StreamTable, ActualPrepareWorkRetainsAllBorrowedBindingBackingAfterFacadeRelease) {
    auto created = pipeline::StreamTable::create(prepared());
    ASSERT_TRUE(created.ok());
    auto owner = std::make_shared<pipeline::StreamTable>(std::move(created.value()));
    std::weak_ptr<pipeline::StreamTable> weak = owner;
    std::promise<void> entered;
    std::promise<void> release;
    auto permission = release.get_future();
    auto work = std::async(
        std::launch::async,
        [retained = owner, &entered, permission = std::move(permission)]() mutable {
            const auto& bindings = retained->bindings();
            const rodaq::acquisition::PrepareConfig input{{sizeof(input)}, bindings.data(), 2, 1};
            entered.set_value();
            permission.wait();
            return rodaq::acquisition::abi::valid_prepare(&input) &&
                   std::string_view(bindings[0].stream_key.data, bindings[0].stream_key.size) ==
                       "/left";
        });
    entered.get_future().wait();
    owner.reset();
    EXPECT_FALSE(weak.expired());
    release.set_value();
    EXPECT_TRUE(work.get());
}

}  // namespace
