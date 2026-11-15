#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rodaq/status.hpp>
#include <rodaq/storage/c/types.h>

namespace rodaq::storage {

using PrepareConfig = rodaq_storage_prepare_config_t;
using CapacityModel = rodaq_storage_capacity_model_t;
using Record = rodaq_storage_record_t;
using BatchResult = rodaq_storage_batch_result_t;
using StreamDescriptor = rodaq_storage_descriptor_t;
using SegmentSpec = rodaq_segment_spec_t;
using SegmentUsage = rodaq_segment_usage_t;

/** Contiguous frozen records and nested views borrowed until actual write return. */
struct Batch {
    const Record* records{nullptr};
    std::uint32_t count{0};
};

struct WriteOutcome {
    Status status;
    BatchResult batch_result;

    // Admission failure: native Storage was not entered and nothing was written.
    WriteOutcome(Status failure)
    : status(std::move(failure)),
      batch_result{{sizeof(BatchResult)}, 0, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN} {}

    WriteOutcome(Status operation_status, BatchResult result)
    : status(std::move(operation_status)), batch_result(result) {}
};

struct SealedFile {
    std::string relative_path;
    std::uint64_t physical_size{0};
};

/** Owning copy; never retains native or caller-temporary path pointers. */
struct SegmentSealResult {
    std::vector<SealedFile> files;
    std::uint64_t total_physical_bytes{0};
};

}  // namespace rodaq::storage
