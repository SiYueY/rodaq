#ifndef RODAQ_ADMISSION_ACCEPTANCE_SEQUENCE_HPP_
#define RODAQ_ADMISSION_ACCEPTANCE_SEQUENCE_HPP_

#include <rodaq/result.hpp>
#include <canonical/handle.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rodaq {

class AcceptanceSequenceAllocator final {
public:
    static Result<AcceptanceSequenceAllocator> create(
        std::size_t max_streams, std::uint64_t first_sequence = 0);

    AcceptanceSequenceAllocator(const AcceptanceSequenceAllocator&) = delete;
    AcceptanceSequenceAllocator& operator=(const AcceptanceSequenceAllocator&) = delete;
    AcceptanceSequenceAllocator(AcceptanceSequenceAllocator&& other) noexcept;
    AcceptanceSequenceAllocator& operator=(AcceptanceSequenceAllocator&&) = delete;

    bool can_allocate(StreamHandle stream) noexcept;
    Result<std::uint64_t> allocate(StreamHandle stream);
    std::size_t capacity() const noexcept { return entries_.size(); }
    std::size_t registered_stream_count() const;

private:
    struct Entry {
        StreamHandle stream;
        std::uint64_t next_sequence = 0;
        bool exhausted = false;
    };

    AcceptanceSequenceAllocator(std::size_t max_streams, std::uint64_t first_sequence);
    Entry* find_or_insert_locked(StreamHandle stream);
    Entry* find_locked(StreamHandle stream) noexcept;

    std::vector<Entry> entries_;
    std::size_t entry_count_ = 0;
    std::uint64_t first_sequence_ = 0;
    mutable std::mutex mutex_;
};

}  // namespace rodaq

#endif  // RODAQ_ADMISSION_ACCEPTANCE_SEQUENCE_HPP_
