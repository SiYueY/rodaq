#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rodaq/acquisition/types.hpp>

#include "admission.hpp"

namespace rodaq::pipeline {

struct StreamPreparation {
    StreamId stream_id{0};
    std::string stream_key;
    std::uint64_t max_record_bytes{0};
};

// One Recording Instance's immutable structure. All entries/key backing are
// prepared before publishing callbacks; no lookup/insertion growth or table
// lock exists on the hot path.
class StreamTable {
public:
    static Result<StreamTable> create(
        std::vector<StreamPreparation> streams, std::size_t max_streams = 64);
    StreamTable(const StreamTable&) = delete;
    StreamTable& operator=(const StreamTable&) = delete;
    StreamTable(StreamTable&&) noexcept = default;
    StreamTable& operator=(StreamTable&&) noexcept = default;

    StreamAdmission* find(StreamId id) noexcept;
    const std::vector<acquisition::StreamBinding>& bindings() const noexcept { return bindings_; }

private:
    struct Entry {
        std::string key;
        StreamAdmission admission;
        Entry(StreamId id, std::string name, std::uint64_t max_bytes)
        : key(std::move(name)), admission(id, max_bytes) {}
    };
    StreamTable() = default;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::vector<acquisition::StreamBinding> bindings_;
};

}  // namespace rodaq::pipeline
