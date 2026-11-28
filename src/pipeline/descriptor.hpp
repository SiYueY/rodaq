#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rodaq/acquisition/types.hpp>
#include <rodaq/result.hpp>
#include <rodaq/storage/types.hpp>

namespace rodaq::pipeline {

struct DescriptorFacts {
    std::string channel_name;
    std::string message_encoding;
    std::string schema_name;
    std::string schema_encoding;
    std::vector<std::uint8_t> schema;
    std::string schema_digest;
    std::string metadata_format;
    std::string metadata_json;

    friend bool operator==(const DescriptorFacts& left, const DescriptorFacts& right) noexcept {
        return left.channel_name == right.channel_name &&
               left.message_encoding == right.message_encoding &&
               left.schema_name == right.schema_name &&
               left.schema_encoding == right.schema_encoding &&
               left.schema_digest == right.schema_digest && left.schema == right.schema &&
               left.metadata_format == right.metadata_format &&
               left.metadata_json == right.metadata_json;
    }

    friend bool operator!=(const DescriptorFacts& left, const DescriptorFacts& right) noexcept {
        return !(left == right);
    }
};

// Immutable bind-once Stream Descriptor owned by a Recording Instance and
// retained by accepted Records through actual Storage return.
class Descriptor {
public:
    static Result<std::shared_ptr<const Descriptor>> create(
        const acquisition::Descriptor& proposal, rodaq_descriptor_id_t descriptor_id,
        std::uint32_t max_schema_bytes = 1024 * 1024,
        std::uint32_t max_metadata_bytes = 128 * 1024);

    const rodaq_stream_id_t stream_id;
    const rodaq_descriptor_id_t descriptor_id;
    const DescriptorFacts facts;

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;
    ~Descriptor() = default;

    // All views remain valid while this immutable owner remains referenced,
    // including beyond an observer timeout until actual Storage call return.
    storage::StreamDescriptor view() const noexcept;

private:
    Descriptor(
        rodaq_stream_id_t stream, rodaq_descriptor_id_t identity, DescriptorFacts owned) noexcept;
};

using DescriptorRef = std::shared_ptr<const Descriptor>;

}  // namespace rodaq::pipeline
