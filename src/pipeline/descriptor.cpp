#include "descriptor.hpp"

#include <new>
#include <utility>

#include <rodaq/acquisition/validation.hpp>

#include "../artifact/manifest.hpp"
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
    std::uint32_t max_schema_bytes, std::uint32_t max_metadata_bytes) {
    if (descriptor_id == 0 || max_metadata_bytes == 0 ||
        !acquisition::abi::valid_descriptor(&proposal)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (proposal.schema.size > max_schema_bytes) {
        return Status(StatusCode::ResourceExhausted, "Descriptor schema exceeds byte limit");
    }
    try {
        DescriptorFacts owned;
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
        if (proposal.metadata.format.size != 0) {
            owned.metadata_format = copy_text(proposal.metadata.format);
            auto normalized = artifact::normalize_descriptor_metadata(
                owned.metadata_format, {proposal.metadata.json.data, proposal.metadata.json.size},
                max_metadata_bytes);
            if (!normalized.ok()) {
                return normalized.status();
            }
            owned.metadata_json = std::move(normalized.value());
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
    result.channel_name = text(facts.channel_name);
    result.message_encoding = text(facts.message_encoding);
    result.schema_name = text(facts.schema_name);
    result.schema_encoding = text(facts.schema_encoding);
    result.schema = {
        facts.schema.empty() ? nullptr : facts.schema.data(),
        static_cast<std::uint32_t>(facts.schema.size())};
    result.metadata = {text(facts.metadata_format), text(facts.metadata_json)};
    return result;
}

}  // namespace rodaq::pipeline
