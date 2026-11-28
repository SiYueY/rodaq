#include "descriptor.hpp"

#include <algorithm>
#include <new>
#include <utility>

#include <rodaq/acquisition/validation.hpp>
#include <rodaq/canonical/validation.hpp>

#include "../persistence/digest.hpp"

namespace rodaq::pipeline {
namespace {

rodaq_string_t text(const std::string& value) noexcept {
    return {value.empty() ? nullptr : value.data(), static_cast<std::uint32_t>(value.size())};
}

std::string copy_text(rodaq_string_t value) {
    return value.size == 0 ? std::string{} : std::string(value.data, value.size);
}

}  // namespace

Descriptor::Descriptor(
    rodaq_stream_id_t stream, rodaq_descriptor_id_t identity, DescriptorFacts owned) noexcept
: stream_id(stream), descriptor_id(identity), facts(std::move(owned)) {}

Result<DescriptorRef> Descriptor::create(
    const acquisition::Descriptor& proposal, rodaq_descriptor_id_t descriptor_id,
    std::uint32_t max_schema_bytes, std::uint32_t max_qos_profiles) {
    if (descriptor_id == 0 || proposal.schema.size > max_schema_bytes || max_qos_profiles > 256 ||
        proposal.ros2_offered_qos_profile_count > max_qos_profiles ||
        !acquisition::abi::valid_descriptor(&proposal)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        DescriptorFacts owned;
        owned.data_profile = proposal.data_profile;
        owned.channel_name = copy_text(proposal.channel_name);
        owned.message_encoding = copy_text(proposal.message_encoding);
        owned.schema_name = copy_text(proposal.schema_name);
        owned.schema_encoding = copy_text(proposal.schema_encoding);
        if (proposal.schema.size != 0) {
            owned.schema.assign(proposal.schema.data, proposal.schema.data + proposal.schema.size);
        }
        auto digest = persistence::byte_digest(proposal.schema.data, proposal.schema.size);
        if (!digest.ok()) {
            return digest.status();
        }
        owned.schema_digest = std::move(digest.value());
        owned.ros2_type_description_hash = copy_text(proposal.ros2_type_description_hash);
        if (proposal.ros2_offered_qos_profile_count != 0) {
            owned.offered_qos.assign(
                proposal.ros2_offered_qos_profiles,
                proposal.ros2_offered_qos_profiles + proposal.ros2_offered_qos_profile_count);
            std::sort(
                owned.offered_qos.begin(), owned.offered_qos.end(),
                [](const auto& left, const auto& right) {
                    return canonical::qos_key(left) < canonical::qos_key(right);
                });
            owned.offered_qos.erase(
                std::unique(
                    owned.offered_qos.begin(), owned.offered_qos.end(),
                    [](const auto& left, const auto& right) {
                        return canonical::qos_key(left) == canonical::qos_key(right);
                    }),
                owned.offered_qos.end());
        }
        if (proposal.ros2_subscription != nullptr) {
            owned.ros2_subscription =
                canonical::copy_subscription_facts(*proposal.ros2_subscription);
        }
        return std::shared_ptr<const Descriptor>(
            new Descriptor(proposal.stream_id, descriptor_id, std::move(owned)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

storage::StreamDescriptor Descriptor::view() const noexcept {
    storage::StreamDescriptor result{};
    result.header.struct_size = sizeof(result);
    result.descriptor_id = descriptor_id;
    result.stream_id = stream_id;
    result.data_profile = facts.data_profile;
    result.channel_name = text(facts.channel_name);
    result.message_encoding = text(facts.message_encoding);
    result.schema_name = text(facts.schema_name);
    result.schema_encoding = text(facts.schema_encoding);
    result.schema = {
        facts.schema.empty() ? nullptr : facts.schema.data(),
        static_cast<std::uint32_t>(facts.schema.size())};
    result.ros2_type_description_hash = text(facts.ros2_type_description_hash);
    result.ros2_offered_qos_profiles =
        facts.offered_qos.empty() ? nullptr : facts.offered_qos.data();
    result.ros2_offered_qos_profile_count = static_cast<std::uint32_t>(facts.offered_qos.size());
    return result;
}

}  // namespace rodaq::pipeline
