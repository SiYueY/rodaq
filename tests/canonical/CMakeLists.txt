rodaq_add_test(rodaq_handle_registry_test canonical.registry registry_test.cpp)
rodaq_add_test(
    rodaq_canonical_registration_test
    canonical.registration
    registration_test.cpp
)
rodaq_add_test(
    rodaq_canonical_availability_test
    canonical.availability
    availability_test.cpp
)
rodaq_add_test(rodaq_descriptor_test canonical.descriptor descriptor_test.cpp)
