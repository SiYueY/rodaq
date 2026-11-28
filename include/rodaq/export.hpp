#pragma once

// RoDAQ supports Linux ELF. Mark only out-of-line public SDK entry points.
#define RODAQ_API __attribute__((visibility("default")))
