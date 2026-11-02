include_guard(GLOBAL)

option(RODAQ_BUILD_TESTS "Build RoDAQ tests" ON)
option(RODAQ_BUILD_BENCHMARKS "Build RoDAQ benchmarks" OFF)
option(RODAQ_WARNINGS_AS_ERRORS "Treat RoDAQ-owned compiler warnings as errors" OFF)
option(RODAQ_ENABLE_ASAN "Enable AddressSanitizer for RoDAQ-owned targets" OFF)
option(RODAQ_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for RoDAQ-owned targets" OFF)
option(RODAQ_USE_MIMALLOC "Add the pinned mimalloc dependency to the build" OFF)
