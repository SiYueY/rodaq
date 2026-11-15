#pragma once

#include <rodaq/acquisition/c/types.h>

namespace rodaq::acquisition {

// Views follow the C ABI borrow duration. Owners must outlive actual calls.
using PrepareConfig = rodaq_acquisition_prepare_config_t;
using StreamBinding = rodaq_acquisition_stream_binding_t;
using RecordAttempt = rodaq_record_attempt_t;
using DescriptorProposal = rodaq_descriptor_proposal_t;
using DescriptorToken = rodaq_descriptor_token_t;
using SubmitResult = rodaq_submit_result_t;
using SourceEvent = rodaq_source_event_t;
using AcquisitionEvent = rodaq_acquisition_event_t;

}  // namespace rodaq::acquisition
