#include <rodaq/plugin/deadline.hpp>

int deadline_header_anchor() { return rodaq::plugin::Deadline::infinite().is_infinite() ? 0 : 1; }
