#include "descriptor.hpp"

#include <utility>

namespace rodaq::pipeline {
namespace {

rodaq_string_t text(const std::string& value) noexcept {
    return {value.empty() ? nullptr : value.data(), static_cast<std::uint32_t>(value.size())};
}

}  // namespace

Descriptor::Descriptor(
    DescriptorReservation reservation, rodaq_stream_id_t stream, rodaq_descriptor_id_t identity,
    acquisition::DescriptorToken runtime_token, DescriptorFacts owned_facts) noexcept
: reservation_(std::move(reservation)),
  stream_id(stream),
  descriptor_id(identity),
  token(runtime_token),
  facts(std::move(owned_facts)) {}

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
