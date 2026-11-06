#ifndef RODAQ_ADMISSION_SUBMIT_RESULT_HPP_
#define RODAQ_ADMISSION_SUBMIT_RESULT_HPP_

#include <rodaq/plugin/abi/acquisition.h>

namespace rodaq {

inline rodaq_acquisition_submit_result make_submit_result(
    rodaq_admission_outcome outcome, rodaq_admission_rejection_reason reason,
    rodaq_payload_disposition disposition) noexcept {
    return rodaq_acquisition_submit_result{
        sizeof(rodaq_acquisition_submit_result), outcome, reason, disposition, {0, 0},
    };
}

}  // namespace rodaq

#endif  // RODAQ_ADMISSION_SUBMIT_RESULT_HPP_
