#include "descriptor_registry.hpp"

#include "../persistence/digest.hpp"
#include "../persistence/json.hpp"
#include "../ros2_qos.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

#include <rodaq/acquisition/validation.hpp>

namespace rodaq::pipeline {
namespace {

using persistence::Json;

std::string copy_text(rodaq_string_t text) {
    return text.size == 0 ? std::string{} : std::string(text.data, text.size);
}

Result<DescriptorFacts> copy_facts(const acquisition::DescriptorProposal& proposal) {
    DescriptorFacts facts;
    facts.data_profile = proposal.data_profile;
    facts.channel_name = copy_text(proposal.channel_name);
    facts.message_encoding = copy_text(proposal.message_encoding);
    facts.schema_name = copy_text(proposal.schema_name);
    facts.schema_encoding = copy_text(proposal.schema_encoding);
    facts.ros2_type_description_hash = copy_text(proposal.ros2_type_description_hash);
    if (proposal.schema.size != 0) {
        facts.schema.assign(proposal.schema.data, proposal.schema.data + proposal.schema.size);
    }
    if (proposal.ros2_offered_qos_profile_count != 0) {
        facts.offered_qos.assign(
            proposal.ros2_offered_qos_profiles,
            proposal.ros2_offered_qos_profiles + proposal.ros2_offered_qos_profile_count);
        std::sort(
            facts.offered_qos.begin(), facts.offered_qos.end(),
            [](const auto& left, const auto& right) {
                return canonical::qos_key(left) < canonical::qos_key(right);
            });
        facts.offered_qos.erase(
            std::unique(
                facts.offered_qos.begin(), facts.offered_qos.end(),
                [](const auto& left, const auto& right) {
                    return canonical::qos_key(left) == canonical::qos_key(right);
                }),
            facts.offered_qos.end());
    }
    auto schema_digest = persistence::byte_digest(facts.schema.data(), facts.schema.size());
    if (!schema_digest.ok()) {
        return std::move(schema_digest).status();
    }
    facts.schema_digest = std::move(schema_digest).value();
    if (const auto* subscription = acquisition::abi::subscription_facts(proposal)) {
        facts.ros2_subscription = canonical::copy_subscription_facts(*subscription);
    }
    Json qos_set = Json::array();
    for (const auto& qos : facts.offered_qos) {
        qos_set.push_back(
            {{"history", qos.history},
             {"reliability", qos.reliability},
             {"durability", qos.durability},
             {"liveliness", qos.liveliness},
             {"depth", std::to_string(qos.depth)},
             {"deadline_sec", std::to_string(qos.deadline.sec)},
             {"deadline_nsec", std::to_string(qos.deadline.nsec)},
             {"lifespan_sec", std::to_string(qos.lifespan.sec)},
             {"lifespan_nsec", std::to_string(qos.lifespan.nsec)},
             {"lease_sec", std::to_string(qos.liveliness_lease_duration.sec)},
             {"lease_nsec", std::to_string(qos.liveliness_lease_duration.nsec)},
             {"avoid_ros_namespace_conventions", qos.avoid_ros_namespace_conventions}});
    }
    Json document{
        {"data_profile", facts.data_profile},
        {"channel_name", facts.channel_name},
        {"message_encoding", facts.message_encoding},
        {"schema_name", facts.schema_name},
        {"schema_encoding", facts.schema_encoding},
        {"schema_digest", facts.schema_digest},
        {"ros2_type_description_hash", facts.ros2_type_description_hash},
        {"offered_qos", std::move(qos_set)}};
    if (facts.ros2_subscription) {
        document["ros2_subscription"] = canonical::subscription_document(*facts.ros2_subscription);
    }
    persistence::JsonLimits limits{1024 * 1024, 4, 1024, 16, 256, 4096};
    auto fingerprint = persistence::semantic_digest(document, "rodaq.descriptor-facts.v1", limits);
    if (!fingerprint.ok()) {
        return std::move(fingerprint).status();
    }
    facts.fingerprint = std::move(fingerprint).value();
    return facts;
}

Result<std::size_t> input_bytes(
    const acquisition::DescriptorProposal& proposal, std::size_t budget) {
    const auto* subscription = acquisition::abi::subscription_facts(proposal);
    const std::array<std::size_t, 8> dimensions{
        proposal.schema.size,
        proposal.channel_name.size,
        proposal.message_encoding.size,
        proposal.schema_name.size,
        proposal.schema_encoding.size,
        proposal.ros2_type_description_hash.size,
        static_cast<std::size_t>(proposal.ros2_offered_qos_profile_count) *
            sizeof(rodaq_ros2_qos_profile_t),
        subscription ? sizeof(*subscription) + subscription->ros_distribution.size +
                           subscription->rmw_implementation.size
                     : 0};
    std::size_t charge = 0;
    for (const auto dimension : dimensions) {
        if (dimension > budget - charge) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        charge += dimension;
    }
    return charge;
}

}  // namespace

struct DescriptorRegistryState {
    struct Slot {
        std::weak_ptr<const Descriptor> descriptor;
        rodaq_descriptor_id_t descriptor_id{0};
        std::uint64_t generation{0};
        bool occupied{false};
    };

    struct Stream {
        rodaq_stream_id_t stream_id{0};
        rodaq_descriptor_id_t last_descriptor_id{0};
        std::vector<Slot> slots;
    };

    DescriptorRegistryOptions options;
    mutable std::mutex mutex;
    std::vector<Stream> streams;
    DescriptorRegistrySnapshot counters;
};

DescriptorReservation::DescriptorReservation(
    std::shared_ptr<DescriptorRegistryState> state, std::size_t stream_index,
    std::size_t slot_index, std::size_t bytes) noexcept
: state_(std::move(state)), stream_index_(stream_index), slot_index_(slot_index), bytes_(bytes) {}

DescriptorReservation::DescriptorReservation(DescriptorReservation&& other) noexcept
: state_(std::move(other.state_)),
  stream_index_(other.stream_index_),
  slot_index_(other.slot_index_),
  bytes_(other.bytes_) {}

DescriptorReservation::~DescriptorReservation() noexcept {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto& slot = state_->streams[stream_index_].slots[slot_index_];
        slot.descriptor.reset();
        slot.occupied = false;
        --state_->counters.live_descriptors;
        state_->counters.owned_data_bytes -= bytes_;
    }
    // state_ is released after the accounting lock; no descriptor/state cycle.
}

DescriptorRegistry::DescriptorRegistry(std::shared_ptr<DescriptorRegistryState> state) noexcept
: state_(std::move(state)) {}

Result<DescriptorRegistry> DescriptorRegistry::create(
    std::vector<rodaq_stream_id_t> streams, DescriptorRegistryOptions options) {
    if (streams.empty() || streams.size() > 1024 || options.max_descriptors_per_stream == 0 ||
        options.max_descriptors_per_stream > 65536 / streams.size() ||
        options.max_schema_bytes == 0 || options.max_offered_qos_profiles > 256 ||
        options.max_owned_data_bytes == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        std::sort(streams.begin(), streams.end());
        if (streams.front() == 0 ||
            std::adjacent_find(streams.begin(), streams.end()) != streams.end()) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto state = std::make_shared<DescriptorRegistryState>();
        state->options = options;
        state->streams.reserve(streams.size());
        for (const auto id : streams) {
            state->streams.push_back(
                {id, 0,
                 std::vector<DescriptorRegistryState::Slot>(options.max_descriptors_per_stream)});
        }
        return DescriptorRegistry(std::move(state));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<DescriptorRef> DescriptorRegistry::capture(
    const acquisition::DescriptorProposal& proposal, rodaq_descriptor_id_t durable_descriptor_id) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    // Count/size bounds precede the SDK's deep validation or any borrowed copy.
    if (!plugin::abi::readable(
            &proposal,
            offsetof(acquisition::DescriptorProposal, reserved1) + sizeof(proposal.reserved1)) ||
        proposal.schema.size > state_->options.max_schema_bytes ||
        proposal.ros2_offered_qos_profile_count > state_->options.max_offered_qos_profiles ||
        durable_descriptor_id == 0 || !acquisition::abi::valid_descriptor(&proposal)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto charge = input_bytes(proposal, state_->options.max_owned_data_bytes);
    if (!charge.ok()) {
        return std::move(charge).status();
    }
    const auto bytes = charge.value();
    std::size_t stream_index = 0;
    std::size_t slot_index = 0;
    acquisition::DescriptorToken token{};
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto stream = std::lower_bound(
            state_->streams.begin(), state_->streams.end(), proposal.stream_id,
            [](const auto& entry, rodaq_stream_id_t id) { return entry.stream_id < id; });
        if (stream == state_->streams.end() || stream->stream_id != proposal.stream_id) {
            return Status(StatusCode::NotFound, {});
        }
        if (durable_descriptor_id <= stream->last_descriptor_id) {
            return Status(StatusCode::Conflict, "Descriptor identity was already consumed");
        }
        if (bytes > state_->options.max_owned_data_bytes - state_->counters.owned_data_bytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        const auto slot =
            std::find_if(stream->slots.begin(), stream->slots.end(), [](const auto& entry) {
                return !entry.occupied &&
                       entry.generation != std::numeric_limits<std::uint64_t>::max();
            });
        if (slot == stream->slots.end()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        stream_index = static_cast<std::size_t>(stream - state_->streams.begin());
        slot_index = static_cast<std::size_t>(slot - stream->slots.begin());
        ++slot->generation;
        slot->occupied = true;
        slot->descriptor_id = durable_descriptor_id;
        stream->last_descriptor_id = durable_descriptor_id;
        // Token value is a runtime slot, not a persistent descriptor identity.
        token = {
            1 + stream_index * state_->options.max_descriptors_per_stream + slot_index,
            slot->generation};
        ++state_->counters.live_descriptors;
        state_->counters.owned_data_bytes += bytes;
        state_->counters.high_water_descriptors =
            std::max(state_->counters.high_water_descriptors, state_->counters.live_descriptors);
        state_->counters.high_water_data_bytes =
            std::max(state_->counters.high_water_data_bytes, state_->counters.owned_data_bytes);
    }
    DescriptorReservation reservation(state_, stream_index, slot_index, bytes);
    try {
        auto facts = copy_facts(proposal);
        if (!facts.ok()) {
            return std::move(facts).status();
        }
        DescriptorRef descriptor(new Descriptor(
            std::move(reservation), proposal.stream_id, durable_descriptor_id, token,
            std::move(facts).value()));
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->streams[stream_index].slots[slot_index].descriptor = descriptor;
        }
        return descriptor;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<DescriptorRef> DescriptorRegistry::lookup(
    rodaq_stream_id_t stream_id, rodaq_descriptor_id_t descriptor_id) const {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto stream = std::lower_bound(
        state_->streams.begin(), state_->streams.end(), stream_id,
        [](const auto& entry, rodaq_stream_id_t id) { return entry.stream_id < id; });
    if (stream == state_->streams.end() || stream->stream_id != stream_id) {
        return Status(StatusCode::NotFound, {});
    }
    for (const auto& slot : stream->slots) {
        if (slot.occupied && slot.descriptor_id == descriptor_id) {
            auto descriptor = slot.descriptor.lock();
            if (descriptor) {
                // Moving this ref out prevents last-owner destruction under
                // the registry lock; nonmatching slots are not promoted.
                return descriptor;
            }
        }
    }
    return Status(StatusCode::NotFound, {});
}

Result<bool> DescriptorRegistry::equivalent(
    const Descriptor& descriptor, const acquisition::DescriptorProposal& proposal) const {
    if (!state_ ||
        !plugin::abi::readable(
            &proposal,
            offsetof(acquisition::DescriptorProposal, reserved1) + sizeof(proposal.reserved1)) ||
        proposal.schema.size > state_->options.max_schema_bytes ||
        proposal.ros2_offered_qos_profile_count > state_->options.max_offered_qos_profiles ||
        !acquisition::abi::valid_descriptor(&proposal)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto bounded = input_bytes(proposal, state_->options.max_owned_data_bytes);
    if (!bounded.ok()) {
        return bounded.status();
    }
    if (descriptor.stream_id != proposal.stream_id) {
        return false;
    }
    try {
        auto facts = copy_facts(proposal);
        if (!facts.ok()) {
            return facts.status();
        }
        return facts.value().fingerprint == descriptor.facts.fingerprint;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

DescriptorRegistrySnapshot DescriptorRegistry::snapshot() const {
    if (!state_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->counters;
}

}  // namespace rodaq::pipeline
