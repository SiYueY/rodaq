#include "pipeline/descriptor_registry.hpp"
#include "persistence/digest.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include <rodaq/storage/validation.hpp>
#include <rodaq/acquisition/validation.hpp>
#include <sys/mman.h>
#include <unistd.h>

namespace {

namespace pipeline = rodaq::pipeline;
using rodaq::StatusCode;

rodaq_string_t text(std::string_view value) {
    return {value.empty() ? nullptr : value.data(), static_cast<std::uint32_t>(value.size())};
}

struct Proposal {
    rodaq_data_profile_t profile{RODAQ_DATA_PROFILE_GENERIC};
    std::string channel{"/data"};
    std::string encoding{"json"};
    std::string schema_name{"Sample"};
    std::string schema_encoding{"jsonschema"};
    std::vector<std::uint8_t> schema{'a', 'b', 'c'};
    std::string type_hash;
    std::vector<rodaq_ros2_qos_profile_t> qos;

    rodaq::acquisition::DescriptorProposal view(rodaq_stream_id_t stream = 1) const {
        rodaq::acquisition::DescriptorProposal result{};
        result.header.struct_size = sizeof(result);
        result.stream_id = stream;
        result.data_profile = profile;
        result.channel_name = text(channel);
        result.message_encoding = text(encoding);
        result.schema_name = text(schema_name);
        result.schema_encoding = text(schema_encoding);
        result.schema = {
            schema.empty() ? nullptr : schema.data(), static_cast<std::uint32_t>(schema.size())};
        result.ros2_type_description_hash = text(type_hash);
        result.ros2_offered_qos_profiles = qos.empty() ? nullptr : qos.data();
        result.ros2_offered_qos_profile_count = static_cast<std::uint32_t>(qos.size());
        return result;
    }
};

TEST(DescriptorRegistry, OldDescriptorPrefixAtGuardPageNeverReadsTheNewSubscriptionTail) {
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto* memory = static_cast<unsigned char*>(
        mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(memory, MAP_FAILED);
    struct Mapping {
        void* memory;
        std::size_t bytes;
        ~Mapping() { munmap(memory, bytes); }
    } mapping{memory, 2 * page};
    ASSERT_EQ(mprotect(memory + page, page, PROT_NONE), 0);
    Proposal source;
    auto proposal = source.view();
    constexpr auto prefix =
        offsetof(rodaq_descriptor_proposal_t, reserved1) + sizeof(proposal.reserved1);
    constexpr auto storage = offsetof(rodaq_descriptor_proposal_t, ros2_subscription);
    proposal.header.struct_size = prefix;
    std::memcpy(memory + page - storage, &proposal, prefix);
    const auto* old = reinterpret_cast<const rodaq_descriptor_proposal_t*>(memory + page - storage);
    ASSERT_TRUE(rodaq::acquisition::abi::valid_descriptor(old));
    auto registry = pipeline::DescriptorRegistry::create({1}).value();
    const auto captured = registry.capture(*old, 1);
    ASSERT_TRUE(captured.ok());
    EXPECT_FALSE(captured.value()->facts.ros2_subscription);
}

TEST(DescriptorRegistry, SubscriptionSnapshotIsOwnedVersionedAndChargedToExistingDescriptorQuota) {
    Proposal source;
    source.profile = RODAQ_DATA_PROFILE_ROS2;
    source.encoding = "cdr";
    source.schema_name = "std_msgs/msg/String";
    source.schema_encoding = "ros2msg";
    std::string distribution = "humble";
    std::string rmw = "rmw_fastrtps_cpp";
    rodaq_ros2_subscription_facts_t subscription{
        {sizeof(subscription)}, text(distribution), text(rmw), {}, 1, 1, 2};
    subscription.installed_requested_qos.history = RODAQ_ROS2_QOS_HISTORY_KEEP_LAST;
    subscription.installed_requested_qos.depth = 16;
    auto proposal = source.view();
    proposal.ros2_subscription = &subscription;
    auto registry = pipeline::DescriptorRegistry::create({1}).value();
    auto first = registry.capture(proposal, 1);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value()->facts.ros2_subscription);
    const auto base = source.view();
    auto plain_registry = pipeline::DescriptorRegistry::create({1}).value();
    const auto plain = plain_registry.capture(base, 1);
    ASSERT_TRUE(plain.ok());
    EXPECT_EQ(
        registry.snapshot().owned_data_bytes - plain_registry.snapshot().owned_data_bytes,
        sizeof(subscription) + distribution.size() + rmw.size());
    subscription.subscription_generation = 2;
    auto changed = registry.capture(proposal, 2);
    ASSERT_TRUE(changed.ok());
    EXPECT_NE(first.value()->facts.fingerprint, changed.value()->facts.fingerprint);
    distribution.assign(64, 'x');
    rmw.assign(128, 'x');
    EXPECT_EQ(first.value()->facts.ros2_subscription->ros_distribution, "humble");
    EXPECT_EQ(first.value()->facts.ros2_subscription->rmw_implementation, "rmw_fastrtps_cpp");
    EXPECT_EQ(first.value()->facts.ros2_subscription->subscription_generation, 1);
}

TEST(DescriptorRegistry, OwnsEveryBorrowedFactAndComputesActualSchemaDigest) {
    auto created = pipeline::DescriptorRegistry::create({1});
    ASSERT_TRUE(created.ok());
    auto& registry = created.value();
    Proposal source;
    auto captured = registry.capture(source.view(), 100);
    ASSERT_TRUE(captured.ok());
    auto descriptor = captured.value();
    source.channel = "/changed";
    source.schema.assign(1024, 9);
    EXPECT_EQ(descriptor->facts.channel_name, "/data");
    EXPECT_EQ(descriptor->facts.schema, (std::vector<std::uint8_t>{'a', 'b', 'c'}));
    EXPECT_EQ(
        descriptor->facts.schema_digest,
        "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto native = descriptor->view();
    EXPECT_TRUE(rodaq::storage::abi::valid_descriptor(&native));
    EXPECT_EQ(native.descriptor_id, 100);
    EXPECT_EQ(native.stream_id, 1);
    EXPECT_EQ(std::string(native.channel_name.data, native.channel_name.size), "/data");
    EXPECT_EQ(registry.lookup(1, 100).value().get(), descriptor.get());
    EXPECT_EQ(registry.lookup(2, 100).status().code(), StatusCode::NotFound);
}

TEST(DescriptorRegistry, CanonicalQosIsDeduplicatedSortedAndPreservesExactUnsignedDurations) {
    auto created = pipeline::DescriptorRegistry::create({1});
    ASSERT_TRUE(created.ok());
    auto& registry = created.value();
    Proposal source;
    source.profile = RODAQ_DATA_PROFILE_ROS2;
    source.encoding = "cdr";
    source.schema_encoding = "ros2msg";
    rodaq_ros2_qos_profile_t reliable{};
    reliable.history = RODAQ_ROS2_QOS_HISTORY_KEEP_LAST;
    reliable.reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
    reliable.depth = std::numeric_limits<std::uint64_t>::max();
    reliable.deadline = {std::numeric_limits<std::uint64_t>::max(), 999999999};
    auto best_effort = reliable;
    best_effort.reliability = RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT;
    source.qos = {best_effort, reliable, reliable};
    auto first = registry.capture(source.view(), 1);
    ASSERT_TRUE(first.ok());
    source.qos = {reliable, best_effort};
    auto second = registry.capture(source.view(), 2);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value()->facts.fingerprint, second.value()->facts.fingerprint);
    ASSERT_EQ(first.value()->facts.offered_qos.size(), 2);
    EXPECT_EQ(first.value()->facts.offered_qos[0].reliability, RODAQ_ROS2_QOS_RELIABILITY_RELIABLE);
    EXPECT_EQ(first.value()->facts.offered_qos[0].depth, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        first.value()->facts.offered_qos[0].deadline.sec,
        std::numeric_limits<std::uint64_t>::max());
    source.qos[0].deadline.nsec = 1;
    auto changed = registry.capture(source.view(), 3);
    ASSERT_TRUE(changed.ok());
    EXPECT_NE(first.value()->facts.fingerprint, changed.value()->facts.fingerprint);
    source.qos.clear();
    EXPECT_EQ(first.value()->view().ros2_offered_qos_profile_count, 2);
}

TEST(DescriptorRegistry, LiveReferencesBoundCapacityAndRuntimeSlotGenerationPreventsAba) {
    pipeline::DescriptorRegistryOptions options;
    options.max_descriptors_per_stream = 1;
    auto created = pipeline::DescriptorRegistry::create({1, 2}, options);
    ASSERT_TRUE(created.ok());
    auto& registry = created.value();
    Proposal source;
    auto first = registry.capture(source.view(), 100);
    ASSERT_TRUE(first.ok());
    const auto token = first.value()->token;
    EXPECT_EQ(registry.capture(source.view(), 101).status().code(), StatusCode::ResourceExhausted);
    auto retained_record_ref = first.value();
    first.value().reset();
    EXPECT_EQ(registry.snapshot().live_descriptors, 1);
    retained_record_ref.reset();
    EXPECT_EQ(registry.snapshot().live_descriptors, 0);
    auto next = registry.capture(source.view(), 101);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value()->token.value, token.value);
    EXPECT_GT(next.value()->token.generation, token.generation);
    EXPECT_EQ(registry.capture(source.view(), 100).status().code(), StatusCode::Conflict);
    auto another_stream = registry.capture(source.view(2), 100);
    ASSERT_TRUE(another_stream.ok());
    EXPECT_NE(another_stream.value()->token.value, next.value()->token.value);
}

TEST(DescriptorRegistry, ValidationAndDataBudgetFailuresDoNotCreateUnownedReservations) {
    pipeline::DescriptorRegistryOptions options;
    options.max_owned_data_bytes = 20;
    auto tiny = pipeline::DescriptorRegistry::create({1}, options);
    ASSERT_TRUE(tiny.ok());
    Proposal source;
    EXPECT_EQ(
        tiny.value().capture(source.view(), 1).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(tiny.value().snapshot().owned_data_bytes, 0);
    auto created = pipeline::DescriptorRegistry::create({1});
    ASSERT_TRUE(created.ok());
    auto& registry = created.value();
    auto invalid = source.view();
    invalid.ros2_offered_qos_profile_count = std::numeric_limits<std::uint32_t>::max();
    invalid.ros2_offered_qos_profiles = reinterpret_cast<const rodaq_ros2_qos_profile_t*>(1);
    EXPECT_EQ(registry.capture(invalid, 1).status().code(), StatusCode::InvalidArgument);
    invalid = source.view();
    invalid.schema.size = 1024 * 1024 + 1;
    EXPECT_EQ(registry.capture(invalid, 1).status().code(), StatusCode::InvalidArgument);
    invalid = source.view();
    invalid.reserved0 = 1;
    EXPECT_EQ(registry.capture(invalid, 1).status().code(), StatusCode::InvalidArgument);
    EXPECT_EQ(registry.capture(source.view(), 0).status().code(), StatusCode::InvalidArgument);
    EXPECT_EQ(registry.capture(source.view(2), 1).status().code(), StatusCode::NotFound);
    EXPECT_EQ(registry.snapshot().live_descriptors, 0);
    EXPECT_EQ(registry.snapshot().owned_data_bytes, 0);
}

TEST(DescriptorRegistry, LastRecordReferenceOutlivesRegistryFacadeWithoutOwnershipCycle) {
    std::optional<pipeline::DescriptorRegistry> registry;
    auto created = pipeline::DescriptorRegistry::create({1});
    ASSERT_TRUE(created.ok());
    registry.emplace(std::move(created).value());
    Proposal source;
    auto captured = registry->capture(source.view(), 1);
    ASSERT_TRUE(captured.ok());
    auto record_ref = std::move(captured).value();
    registry.reset();
    const auto view = record_ref->view();
    EXPECT_TRUE(rodaq::storage::abi::valid_descriptor(&view));
    EXPECT_EQ(view.schema.size, 3);
    record_ref.reset();
}

TEST(DescriptorRegistry, ConcurrentIndependentStreamsAndLookupReleaseCannotDeadlockBudgetOwner) {
    auto created = pipeline::DescriptorRegistry::create({1, 2, 3, 4});
    ASSERT_TRUE(created.ok());
    auto& registry = created.value();
    std::vector<std::future<bool>> workers;
    for (rodaq_stream_id_t stream = 1; stream <= 4; ++stream) {
        workers.push_back(std::async(std::launch::async, [&, stream] {
            Proposal source;
            for (rodaq_descriptor_id_t id = 1; id <= 100; ++id) {
                auto descriptor = registry.capture(source.view(stream), id);
                if (!descriptor.ok()) {
                    return false;
                }
                auto found = registry.lookup(stream, id);
                if (!found.ok() || found.value().get() != descriptor.value().get()) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& worker : workers) {
        EXPECT_TRUE(worker.get());
    }
    EXPECT_EQ(registry.snapshot().live_descriptors, 0);
    EXPECT_EQ(registry.snapshot().owned_data_bytes, 0);
    EXPECT_LE(registry.snapshot().high_water_descriptors, 4);
}

TEST(DescriptorRegistry, NonmatchingLookupNeverDropsLastDescriptorOwnerUnderRegistryLock) {
    auto created = pipeline::DescriptorRegistry::create({1});
    ASSERT_TRUE(created.ok());
    auto& registry = created.value();
    std::atomic<bool> finished{false};
    auto lookup = std::async(std::launch::async, [&] {
        bool valid = true;
        while (!finished) {
            auto missing = registry.lookup(1, std::numeric_limits<std::uint64_t>::max());
            valid = valid && !missing.ok() && missing.status().code() == StatusCode::NotFound;
        }
        return valid;
    });
    Proposal source;
    bool valid = true;
    for (rodaq_descriptor_id_t id = 1; id <= 1000; ++id) {
        auto descriptor = registry.capture(source.view(), id);
        if (!descriptor.ok()) {
            valid = false;
            break;
        }
    }
    finished = true;
    EXPECT_TRUE(lookup.get());
    EXPECT_TRUE(valid);
    EXPECT_EQ(registry.snapshot().live_descriptors, 0);
}

TEST(ByteDigest, RawBytesHaveNoDomainOrJsonConversionAndNullPositiveInputIsRejected) {
    auto empty = rodaq::persistence::byte_digest(nullptr, 0);
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(
        empty.value(), "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(
        rodaq::persistence::byte_digest(nullptr, 1).status().code(), StatusCode::InvalidArgument);
    const std::array<unsigned char, 4> binary{0, 255, 128, 1};
    auto hashed = rodaq::persistence::byte_digest(binary.data(), binary.size());
    ASSERT_TRUE(hashed.ok());
    EXPECT_EQ(
        hashed.value(), "sha256:6509423fd9da5c225d2f8619ffae394b40f9f7686fee55a38c54b1424ac65f46");
}

}  // namespace
