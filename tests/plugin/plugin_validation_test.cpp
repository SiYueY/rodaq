#include "plugin/plugin_validation.hpp"

#include <gtest/gtest.h>

#include <string_view>

TEST(PluginIdValidationTest, AcceptsStableNamespacedIdentifiers) {
    EXPECT_TRUE(rodaq::plugin::is_valid_plugin_id("rodaq.acquisition.ros2"));
    EXPECT_TRUE(rodaq::plugin::is_valid_plugin_id("vendor.plugin-2_test"));
    EXPECT_TRUE(rodaq::plugin::is_valid_plugin_id("a.0"));
}

TEST(PluginIdValidationTest, RejectsMalformedIdentifiers) {
    EXPECT_FALSE(rodaq::plugin::is_valid_plugin_id(""));
    EXPECT_FALSE(rodaq::plugin::is_valid_plugin_id("single"));
    EXPECT_FALSE(rodaq::plugin::is_valid_plugin_id(".leading"));
    EXPECT_FALSE(rodaq::plugin::is_valid_plugin_id("trailing."));
    EXPECT_FALSE(rodaq::plugin::is_valid_plugin_id("double..dot"));
    EXPECT_FALSE(rodaq::plugin::is_valid_plugin_id("Uppercase.plugin"));
    EXPECT_FALSE(rodaq::plugin::is_valid_plugin_id("bad.plugin name"));
    EXPECT_FALSE(rodaq::plugin::is_valid_plugin_id("bad.plugin/path"));
}

TEST(SemVerValidationTest, AcceptsSemVerTwo) {
    for (const std::string_view version : {
             "1.0.0",
             "1.0.0-alpha",
             "1.0.0-alpha.1",
             "1.0.0+build.5",
             "1.0.0+build-01",
             "1.0.0-alpha.1+build.5",
             "1.0.0-alpha+build-01",
             "1.0.0+001",
         }) {
        EXPECT_TRUE(rodaq::plugin::is_valid_semantic_version(version)) << version;
    }
}

TEST(SemVerValidationTest, RejectsMalformedVersions) {
    for (const std::string_view version : {
             "",
             "1.0",
             "01.0.0",
             "1.01.0",
             "1.0.01",
             "1.0.0-alpha.01",
             "1.0.0-alpha..1",
             "1.0.0-",
             "1.0.0+",
             "1.0.0-alpha+",
             "1.0.0+bad_metadata",
             "1.0.0+bad..metadata",
         }) {
        EXPECT_FALSE(rodaq::plugin::is_valid_semantic_version(version)) << version;
    }
}

TEST(Utf8ValidationTest, AcceptsUnicodeWithoutNul) {
    EXPECT_TRUE(rodaq::plugin::is_valid_utf8_without_nul("ascii"));
    EXPECT_TRUE(
        rodaq::plugin::is_valid_utf8_without_nul("r\xC3\xB6"
                                                 "daq"));
    EXPECT_TRUE(rodaq::plugin::is_valid_utf8_without_nul("\xF0\x9F\xA5\x80"));
}

TEST(Utf8ValidationTest, RejectsNulAndMalformedSequences) {
    EXPECT_FALSE(rodaq::plugin::is_valid_utf8_without_nul(std::string_view("a\0b", 3)));
    EXPECT_FALSE(rodaq::plugin::is_valid_utf8_without_nul("\xFF"));
    EXPECT_FALSE(rodaq::plugin::is_valid_utf8_without_nul("\xC0\xAF"));
    EXPECT_FALSE(rodaq::plugin::is_valid_utf8_without_nul("\xED\xA0\x80"));
    EXPECT_FALSE(rodaq::plugin::is_valid_utf8_without_nul("\xF0\x9F\xA5"));
}

TEST(PluginDescriptorValidationTest, AcceptsValidDescriptorWithEmptyDescription) {
    const rodaq_plugin_descriptor descriptor = {
        RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE,
        0,
        {"rodaq.acquisition.fixture", 25},
        {"Fixture", 7},
        {"1.2.3", 5},
        {"", 0},
    };

    const auto metadata = rodaq::plugin::validate_plugin_descriptor(descriptor);
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata.value().id, "rodaq.acquisition.fixture");
    EXPECT_EQ(metadata.value().name, "Fixture");
    EXPECT_EQ(metadata.value().version, "1.2.3");
    EXPECT_TRUE(metadata.value().description.empty());
}

TEST(PluginDescriptorValidationTest, RejectsMissingRequiredFieldsAndReserved) {
    rodaq_plugin_descriptor descriptor = {
        RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE,
        0,
        {"rodaq.acquisition.fixture", 25},
        {"", 0},
        {"1.2.3", 5},
        {"", 0},
    };

    EXPECT_FALSE(rodaq::plugin::validate_plugin_descriptor(descriptor).has_value());
    descriptor.name = {"Fixture", 7};
    descriptor.reserved = 1;
    EXPECT_FALSE(rodaq::plugin::validate_plugin_descriptor(descriptor).has_value());
    descriptor.reserved = 0;
    descriptor.struct_size = RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE - 1;
    EXPECT_FALSE(rodaq::plugin::validate_plugin_descriptor(descriptor).has_value());
}
