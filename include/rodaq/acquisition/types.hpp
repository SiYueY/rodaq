#pragma once

#include <rodaq/c/acquisition.h>

namespace rodaq::acquisition {

// Views follow the C ABI borrow duration. Owners must outlive actual calls.
using PrepareConfig = rodaq_acquisition_prepare_config_t;
using StreamBinding = rodaq_acquisition_stream_binding_t;
using RecordAttempt = rodaq_acquisition_record_attempt_t;
using RecordDisposition = rodaq_record_disposition_t;
using Descriptor = rodaq_acquisition_descriptor_t;
using AcquisitionEvent = rodaq_acquisition_event_t;
using CallbacksTable = rodaq_acquisition_callbacks_t;

}  // namespace rodaq::acquisition
