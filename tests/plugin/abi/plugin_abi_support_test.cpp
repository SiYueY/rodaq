#include "test_support.hpp"

#include <rodaq/plugin/c/abi.h>
#include <rodaq/plugin/interface.hpp>

#include <cstdint>
#include <string_view>

#include "abi_support.hpp"

namespace {

struct TestVtable {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*call)(void* context);
};

struct TestInterface {
    using Vtable = TestVtable;

    static constexpr std::string_view kInterfaceId = "com.example.abi-test";
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;
    static constexpr std::uint16_t kMinMinor = 0;

    static constexpr std::size_t required_prefix_size() noexcept { return sizeof(TestVtable); }

    static bool required_slots_valid(const Vtable* vtable) noexcept {
        return vtable != nullptr && vtable->call != nullptr;
    }
};

rodaq_status_code_t call(void*) { return RODAQ_STATUS_OK; }

}  // namespace

int main() {
    using rodaq::StatusCode;

    RODAQ_TEST_CHECK(rodaq::plugin::is_known_status_code(RODAQ_STATUS_INTERNAL));
    RODAQ_TEST_CHECK(!rodaq::plugin::is_known_status_code(99u));
    RODAQ_TEST_CHECK(rodaq::plugin::status_code_from_abi(99u) == StatusCode::Internal);
    const char invalid_string[] = {static_cast<char>(0xc0u), static_cast<char>(0x80u)};
    RODAQ_TEST_CHECK(
        !rodaq::plugin::valid_string(rodaq_string_t{invalid_string, sizeof(invalid_string)}));

    rodaq_error_t empty{};
    RODAQ_TEST_CHECK(rodaq::plugin::valid_error_buffer(&empty));
    RODAQ_TEST_CHECK(rodaq::plugin::valid_error_buffer(nullptr));

    uint8_t storage[8]{};
    rodaq_error_t malformed{};
    malformed.message.data = storage;
    malformed.message.capacity = 2u;
    malformed.message.size = 3u;
    RODAQ_TEST_CHECK(!rodaq::plugin::valid_error_buffer(&malformed));

    malformed.message.size = 0u;
    RODAQ_TEST_CHECK(rodaq::plugin::valid_error_buffer(&malformed));

    malformed.message.capacity = 0u;
    RODAQ_TEST_CHECK(!rodaq::plugin::valid_error_buffer(&malformed));

    const uint8_t invalid_utf8[] = {0xc0u, 0x80u};
    malformed.message.data = const_cast<uint8_t*>(invalid_utf8);
    malformed.message.capacity = sizeof(invalid_utf8);
    malformed.message.size = sizeof(invalid_utf8);
    RODAQ_TEST_CHECK(!rodaq::plugin::valid_error_buffer(&malformed));

    uint8_t text_storage[8]{};
    rodaq_error_t error{};
    error.message.data = text_storage;
    error.message.capacity = sizeof(text_storage);
    RODAQ_TEST_CHECK(rodaq::plugin::result_from_abi_call(RODAQ_STATUS_OK, &error).ok());

    error.message.capacity = 4u;
    rodaq::plugin::write_error(&error, "ab\xe2\x82\xac");
    RODAQ_TEST_CHECK(error.message.size == 2u);
    RODAQ_TEST_CHECK(rodaq::plugin::error_message(&error) == "ab");

    RODAQ_TEST_CHECK(
        rodaq::plugin::result_from_abi_call(RODAQ_STATUS_OK, &error).status().code() ==
        StatusCode::Internal);
    RODAQ_TEST_CHECK(
        rodaq::plugin::result_from_abi_call(RODAQ_STATUS_DATA_LOSS, &error).status().code() ==
        StatusCode::DataLoss);

    malformed.message.size = 9u;
    malformed.message.capacity = 8u;
    RODAQ_TEST_CHECK(
        rodaq::plugin::result_from_abi_call(RODAQ_STATUS_DATA_LOSS, &malformed).status().code() ==
        StatusCode::DataLoss);
    RODAQ_TEST_CHECK(
        rodaq::plugin::result_from_abi_call(99u, nullptr).status().code() == StatusCode::Internal);

    TestVtable vtable{rodaq_struct_header_t{sizeof(TestVtable)}, &call};
    rodaq_interface_reference_t reference{};
    reference.context = reinterpret_cast<rodaq_interface_context_t*>(&vtable);
    reference.vtable = reinterpret_cast<const rodaq_interface_vtable_t*>(&vtable);
    reference.vtable_size = sizeof(TestVtable);
    RODAQ_TEST_CHECK(
        rodaq::plugin::interface_support::validate_typed_reference<TestInterface>(reference).ok());

    reference.vtable_size = sizeof(rodaq_struct_header_t) - 1u;
    RODAQ_TEST_CHECK(
        !rodaq::plugin::interface_support::validate_typed_reference<TestInterface>(reference).ok());

    vtable.header.struct_size = sizeof(rodaq_struct_header_t) - 1u;
    reference.vtable_size = sizeof(TestVtable);
    RODAQ_TEST_CHECK(
        !rodaq::plugin::interface_support::validate_typed_reference<TestInterface>(reference).ok());
    return 0;
}
