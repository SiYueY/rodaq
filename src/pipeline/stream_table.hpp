#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include <rodaq/acquisition/types.hpp>

#include "admission.hpp"

namespace rodaq::pipeline {

struct StreamPreparation {
    StreamId stream_id{0};
    std::string stream_key;
    std::uint64_t max_record_bytes{0};
    // Supplied only after StreamIdentityStore actually published these ranges.
    // Construction failure burns those reserved tails; it does not roll back
    // persistent identity authority or manufacture replacement ranges.
    IdentityRange sequences;
    IdentityRange descriptors;
};

// One Recording Instance's immutable structure. All entries/key backing are
// prepared before publishing callbacks; no lookup/insertion growth or table
// lock exists on the hot path. Retain this owner through actual callback and
// Acquisition prepare return. Per-Stream mutable facts use their own mutex.
class StreamTable {
public:
    static Result<StreamTable> create(
        std::vector<StreamPreparation> streams, std::size_t max_streams = 64);
    StreamTable(const StreamTable&) = delete;
    StreamTable& operator=(const StreamTable&) = delete;
    StreamTable(StreamTable&&) noexcept = default;
    StreamTable& operator=(StreamTable&&) noexcept = default;

    StreamAdmission* find(StreamId id) noexcept;
    // Native views stay valid while this owner/actual prepare frame is held.
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
