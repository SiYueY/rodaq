#include "ros2/schema.hpp"
#include "ros2/qos.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

namespace support = rodaq::ros2;
namespace fs = std::filesystem;
using rodaq::StatusCode;

class Ros2SchemaTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-schema-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        if (const auto* value = std::getenv("AMENT_PREFIX_PATH")) {
            previous_ = value;
        }
        const auto paths = root_.string() + (previous_ ? ":" + *previous_ : "");
        ASSERT_EQ(setenv("AMENT_PREFIX_PATH", paths.c_str(), 1), 0);
        fs::create_directories(root_ / "share/ament_index/resource_index/rosidl_interfaces");
    }

    void TearDown() override {
        if (previous_) {
            setenv("AMENT_PREFIX_PATH", previous_->c_str(), 1);
        } else {
            unsetenv("AMENT_PREFIX_PATH");
        }
        fs::remove_all(root_);
    }

    void definition(const std::string& file, const std::string& text) {
        const auto path = root_ / "share/schema_pkg" / file;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        std::ofstream(
            root_ / "share/ament_index/resource_index/rosidl_interfaces/schema_pkg", std::ios::app)
            << file << '\n';
    }

    static std::string delimiter(const std::string& name) {
        return "\n" + std::string(80, '=') + "\nMSG: schema_pkg/" + name + "\n";
    }

    fs::path root_;
    std::optional<std::string> previous_;
};

TEST(Ros2TypeName, CanonicalNamesRejectTraversalEmptyTokensAndLegacyRootAliases) {
    EXPECT_TRUE(support::valid_type_name("std_msgs/msg/String"));
    EXPECT_TRUE(support::valid_type_name("schema_pkg/msg/nested/A"));
    for (const auto* value :
         {"std_msgs/String", "../msg/String", "a/msg/..", "a/msg/", "a//X", "a/msg/X.msg",
          "a/msg/X\n", "a/msg//X", "a/other/X"}) {
        EXPECT_FALSE(support::valid_type_name(value)) << value;
    }
}

TEST_F(Ros2SchemaTest, RecursiveMsgGoldenSortsDependenciesDeduplicatesAndNormalizesNewlines) {
    definition("msg/A.msg", "schema_pkg/Z z\r\nschema_pkg/B b\r\n# comment\r\n");
    definition("msg/B.msg", "schema_pkg/C c\n");
    definition("msg/C.msg", "int32 value\n");
    definition("msg/Z.msg", "string<=20 label\n");
    const auto resolved = support::resolve_schema("schema_pkg/msg/A");
    ASSERT_TRUE(resolved.ok()) << resolved.status().message();
    EXPECT_EQ(resolved.value().name, "schema_pkg/msg/A");
    EXPECT_EQ(resolved.value().encoding, "ros2msg");
    const auto expected = "schema_pkg/Z z\nschema_pkg/B b\n# comment\n" + delimiter("B") +
                          "schema_pkg/C c\n" + delimiter("C") + "int32 value\n" + delimiter("Z") +
                          "string<=20 label\n";
    EXPECT_EQ(resolved.value().text, expected);
    EXPECT_EQ(support::resolve_schema("schema_pkg/msg/A").value().text, expected);
}

TEST_F(Ros2SchemaTest, MissingMsgDependencyFallsBackToACompleteIdlTreeAndIgnoresCommentedIncludes) {
    definition("msg/A.msg", "missing_pkg/Missing value\n");
    definition(
        "msg/A.idl",
        "/*\n#include \"missing_pkg/msg/NotReal.idl\"\n*/\n"
        "# include \"schema_pkg/msg/B.idl\"\n"
        "module schema_pkg { module msg { struct A { long value; }; }; };\n");
    definition("msg/B.idl", "module schema_pkg { module msg { struct B { long value; }; }; };\n");
    const auto resolved = support::resolve_schema("schema_pkg/msg/A");
    ASSERT_TRUE(resolved.ok()) << resolved.status().message();
    EXPECT_EQ(resolved.value().encoding, "ros2idl");
    const auto header = std::string(80, '=') + "\nIDL: schema_pkg/msg/A\n";
    EXPECT_EQ(resolved.value().text.substr(0, header.size()), header);
    EXPECT_NE(resolved.value().text.find("IDL: schema_pkg/msg/B\n"), std::string::npos);
}

TEST_F(Ros2SchemaTest, CyclesAndRepeatedDependenciesProduceEachDefinitionOnlyOnce) {
    definition("msg/A.msg", "schema_pkg/B first\nschema_pkg/B second\n");
    definition("msg/B.msg", "schema_pkg/A parent\n");
    const auto resolved = support::resolve_schema("schema_pkg/msg/A", {1024, 2, 2});
    ASSERT_TRUE(resolved.ok());
    EXPECT_EQ(
        resolved.value().text,
        "schema_pkg/B first\nschema_pkg/B second\n" + delimiter("B") + "schema_pkg/A parent\n");
}

TEST_F(Ros2SchemaTest, ResourceLimitsFailBeforeBuildingOversizedOrDeepSchemas) {
    definition("msg/A.msg", "schema_pkg/B value\n");
    definition("msg/B.msg", "schema_pkg/C value\n");
    definition("msg/C.msg", "int32 value\n");
    EXPECT_EQ(
        support::resolve_schema("schema_pkg/msg/A", {1024, 8, 2}).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(
        support::resolve_schema("schema_pkg/msg/A", {1024, 2, 8}).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(
        support::resolve_schema("schema_pkg/msg/A", {16, 8, 8}).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(
        support::resolve_schema("schema_pkg/msg/A", {1024, 0, 8}).status().code(),
        StatusCode::InvalidArgument);
}

TEST_F(Ros2SchemaTest, ExactResourceBeatsNestedBasenamesIndependentlyOfListingOrder) {
    definition("msg/first/A.msg", "int32 wrong\n");
    definition("msg/second/A.msg", "string wrong\n");
    EXPECT_EQ(support::resolve_schema("schema_pkg/msg/A").status().code(), StatusCode::Conflict);
    definition("msg/A.msg", "int32 correct\n");
    const auto resolved = support::resolve_schema("schema_pkg/msg/A");
    ASSERT_TRUE(resolved.ok());
    EXPECT_EQ(resolved.value().text, "int32 correct\n");
}

TEST_F(Ros2SchemaTest, MissingDefinitionsAndSpecialFilesNeverProducePlaceholderSchemas) {
    EXPECT_EQ(
        support::resolve_schema("schema_pkg/msg/Missing").status().code(), StatusCode::Unavailable);
    definition("msg/A.msg", "");
    EXPECT_EQ(support::resolve_schema("schema_pkg/msg/A").value().text, "\n");
    definition("msg/B.msg", "ignored");
    const auto path = root_ / "share/schema_pkg/msg/B.msg";
    fs::remove(path);
    ASSERT_EQ(mkfifo(path.c_str(), 0600), 0);
    EXPECT_EQ(support::resolve_schema("schema_pkg/msg/B").status().code(), StatusCode::Unavailable);
}

TEST_F(Ros2SchemaTest, InstalledStandardMessageResolvesThroughTheActualAmentIndex) {
    const auto schema = support::resolve_schema("sensor_msgs/msg/JointState");
    ASSERT_TRUE(schema.ok()) << schema.status().message();
    EXPECT_EQ(schema.value().encoding, "ros2msg");
    EXPECT_NE(schema.value().text.find("MSG: std_msgs/Header\n"), std::string::npos);
    EXPECT_NE(schema.value().text.find("MSG: builtin_interfaces/Time\n"), std::string::npos);
}

TEST_F(Ros2SchemaTest, EmbeddedNulInResourcePathsCannotSelectADifferentDefinition) {
    definition("msg/Existing.msg", "int32 different_type\n");
    const auto path = root_ / "share/ament_index/resource_index/rosidl_interfaces/schema_pkg";
    const std::string invalid = std::string("msg/Existing.msg") + '\0' + "/Requested.msg\n";
    std::ofstream(path, std::ios::binary) << invalid;
    EXPECT_EQ(
        support::resolve_schema("schema_pkg/msg/Requested").status().code(),
        StatusCode::InvalidArgument);
}

TEST(Ros2Qos, CanonicalSetIgnoresEndpointArrivalOrderAndDuplicateOffers) {
    rodaq_ros2_qos_profile_t reliable{};
    reliable.history = RODAQ_ROS2_QOS_HISTORY_KEEP_LAST;
    reliable.depth = 10;
    reliable.reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
    auto best_effort = reliable;
    best_effort.reliability = RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT;
    const auto a = support::normalize_qos({best_effort, reliable, best_effort}).value();
    const auto b = support::normalize_qos({reliable, best_effort}).value();
    ASSERT_EQ(a.size(), 2);
    ASSERT_EQ(b.size(), 2);
    EXPECT_EQ(a[0].reliability, b[0].reliability);
    EXPECT_EQ(a[1].reliability, b[1].reliability);
    reliable.reserved[0] = 1;
    EXPECT_FALSE(support::normalize_qos({reliable}).ok());
}

TEST(Ros2Qos, AdaptUsesLeastRestrictiveOffersAndKeepsConfiguredBoundedHistoryDepth) {
    const auto empty = support::adapt_qos({}, 10);
    EXPECT_EQ(empty.reliability, RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT);
    EXPECT_EQ(empty.durability, RODAQ_ROS2_QOS_DURABILITY_VOLATILE);
    EXPECT_EQ(empty.depth, 10);
    rodaq_ros2_qos_profile_t offer{};
    offer.reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
    offer.durability = RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL;
    auto mixed = offer;
    mixed.reliability = RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT;
    mixed.durability = RODAQ_ROS2_QOS_DURABILITY_VOLATILE;
    EXPECT_EQ(support::adapt_qos({offer}, 16).reliability, RODAQ_ROS2_QOS_RELIABILITY_RELIABLE);
    EXPECT_EQ(
        support::adapt_qos({offer, mixed}, 16).reliability, RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT);
    EXPECT_EQ(
        support::adapt_qos({offer, mixed}, 16).durability, RODAQ_ROS2_QOS_DURABILITY_VOLATILE);
    EXPECT_EQ(support::adapt_qos({offer, mixed}, 16).depth, 16);
}

TEST(Ros2Qos, AStricterCandidateDoesNotRequireRebuildingAStillCompatibleInstalledRequest) {
    const support::RequestedQos installed;
    rodaq_ros2_qos_profile_t strict{};
    strict.reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
    strict.durability = RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL;
    const auto candidate = support::adapt_qos({strict}, 10);
    EXPECT_EQ(candidate.reliability, RODAQ_ROS2_QOS_RELIABILITY_RELIABLE);
    EXPECT_TRUE(support::qos_compatible(installed, {strict}));
    auto weak = strict;
    weak.reliability = RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT;
    EXPECT_FALSE(support::qos_compatible(candidate, {weak}));
    EXPECT_TRUE(support::qos_compatible(candidate, {}));
}

}  // namespace
