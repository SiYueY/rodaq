# This single fault-injection case reuses the independently installed SDK DSO.
list(FIND rodaq_pipeline_controller_test_TESTS
    "CleanupSchedulingTest.SupersedingStopSerializesCleanupWhileSiblingTaskStops" cleanup_test_index)
if(NOT cleanup_test_index EQUAL -1)
    set_tests_properties(CleanupSchedulingTest.SupersedingStopSerializesCleanupWhileSiblingTaskStops
        PROPERTIES FIXTURES_REQUIRED counter_sdk)
endif()
