#include "plugin/config_view.hpp"
#include "config/abi_bridge.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using rodaq::ConfigNodeRef;
using rodaq::plugin::ConfigTree;

template <typename T>
T take(rodaq::Result<T> result) {
    if (!result.ok()) {
        throw std::runtime_error("Expected successful Config operation");
    }
    return std::move(result.value());
}

ConfigTree make_nested_tree() {
    ConfigTree nested = take(ConfigTree::map({
        {"name", ConfigTree::string("node")},
        {"level", ConfigTree::int64(2)},
    }));
    ConfigTree list = take(ConfigTree::list({
        ConfigTree::int64(1),
        ConfigTree::int64(2),
        ConfigTree::string("three"),
    }));
    return take(ConfigTree::map({
        {"enabled", ConfigTree::boolean(true)},
        {"limit", ConfigTree::int64(std::numeric_limits<std::int64_t>::max())},
        {"gain", ConfigTree::float64(0.25)},
        {"mode", ConfigTree::string("fixture")},
        {"empty", ConfigTree::string("")},
        {"nested", std::move(nested)},
        {"items", std::move(list)},
    }));
}

}  // namespace

TEST(ConfigViewTest, SupportsNullScalarsAndEmptyContainers) {
    EXPECT_EQ(rodaq::ConfigKind::Null, ConfigTree::null().root().kind());

    EXPECT_EQ(true, take(ConfigTree::boolean(true).root().as_bool()));
    EXPECT_EQ(false, take(ConfigTree::boolean(false).root().as_bool()));
    EXPECT_EQ(
        std::numeric_limits<std::int64_t>::min(),
        take(ConfigTree::int64(std::numeric_limits<std::int64_t>::min()).root().as_int64()));
    EXPECT_EQ(
        std::numeric_limits<std::int64_t>::max(),
        take(ConfigTree::int64(std::numeric_limits<std::int64_t>::max()).root().as_int64()));
    EXPECT_DOUBLE_EQ(1.5, take(ConfigTree::float64(1.5).root().as_double()));
    EXPECT_EQ("", take(ConfigTree::string("").root().as_string()));
    EXPECT_EQ(
        "r\xC3\xB6"
        "daq",
        take(ConfigTree::string("r\xC3\xB6"
                                "daq")
                 .root()
                 .as_string()));

    const ConfigTree empty_map = take(ConfigTree::map({}));
    const ConfigTree empty_list = take(ConfigTree::list({}));
    EXPECT_EQ(rodaq::ConfigKind::Map, empty_map.root().kind());
    EXPECT_EQ(rodaq::ConfigKind::List, empty_list.root().kind());
    EXPECT_EQ(0U, empty_map.root().size());
    EXPECT_EQ(0U, empty_list.root().size());
}

TEST(ConfigViewTest, SupportsNestedMapListMapAndStableOrdering) {
    ConfigTree tree = make_nested_tree();
    const ConfigNodeRef root = tree.root();
    EXPECT_EQ(rodaq::ConfigKind::Map, root.kind());
    EXPECT_EQ(7U, root.size());

    const ConfigNodeRef items = take(root.find("items"));
    EXPECT_EQ(rodaq::ConfigKind::List, items.kind());
    ASSERT_EQ(3U, items.size());
    EXPECT_EQ(1, take(take(items.at(0)).as_int64()));
    EXPECT_EQ(2, take(take(items.at(1)).as_int64()));
    EXPECT_EQ("three", take(take(items.at(2)).as_string()));

    const ConfigNodeRef nested = take(root.find("nested"));
    EXPECT_EQ("name", take(nested.key_at(0)));
    EXPECT_EQ("node", take(take(nested.at(0)).as_string()));
    EXPECT_EQ("level", take(nested.key_at(1)));
    EXPECT_EQ(2, take(take(nested.at(1)).as_int64()));
    EXPECT_EQ("enabled", take(root.key_at(0)));
}

TEST(ConfigViewTest, ReportsNotFoundAndInvalidStateWithoutImplicitConversion) {
    ConfigTree tree = make_nested_tree();
    const ConfigNodeRef root = tree.root();

    EXPECT_EQ(rodaq::StatusCode::NotFound, root.find("missing").status().code());
    EXPECT_EQ(rodaq::StatusCode::NotFound, take(root.find("items")).at(3).status().code());
    EXPECT_EQ(rodaq::StatusCode::NotFound, root.key_at(root.size()).status().code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument, root.find(std::string_view("\xFF", 1)).status().code());

    const ConfigNodeRef limit = take(root.find("limit"));
    EXPECT_EQ(rodaq::StatusCode::InvalidState, limit.as_double().status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, limit.find("child").status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, limit.at(0).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, limit.key_at(0).status().code());

    const ConfigNodeRef items = take(root.find("items"));
    EXPECT_EQ(rodaq::StatusCode::InvalidState, items.find("child").status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, items.key_at(0).status().code());
}

TEST(ConfigViewTest, AllowsEmptyAndUtf8MapKeys) {
    const ConfigTree tree = take(ConfigTree::map({
        {"", ConfigTree::int64(1)},
        {"r\xC3\xB6"
         "le",
         ConfigTree::int64(2)},
    }));
    const ConfigNodeRef root = tree.root();
    EXPECT_EQ(1, take(take(root.find("")).as_int64()));
    EXPECT_EQ(
        2, take(take(root.find("r\xC3\xB6"
                               "le"))
                    .as_int64()));
}

TEST(ConfigViewTest, RejectsInvalidOrDuplicateMapEntries) {
    const auto invalid_key = ConfigTree::map({
        {std::string("\xFF", 1), ConfigTree::null()},
    });
    ASSERT_FALSE(invalid_key.ok());
    EXPECT_EQ(rodaq::StatusCode::InvalidArgument, invalid_key.status().code());

    const auto duplicate = ConfigTree::map({
        {"key", ConfigTree::int64(1)},
        {"key", ConfigTree::int64(2)},
    });
    ASSERT_FALSE(duplicate.ok());
    EXPECT_EQ(rodaq::StatusCode::Conflict, duplicate.status().code());
}

TEST(ConfigViewTest, BorrowedViewsOutliveTreeOwner) {
    ConfigNodeRef view;
    ConfigTree tree = make_nested_tree();
    {
        const ConfigTree copy = tree;
        view = take(copy.root().find("mode"));
    }
    tree = ConfigTree::null();
    EXPECT_EQ("fixture", take(view.as_string()));
}

TEST(ConfigViewTest, AbiAdapterSupportsContainersAndExactScalars) {
    ConfigTree tree = make_nested_tree();
    const rodaq_config_view root = rodaq::config_abi_view(tree.root());
    ASSERT_NE(nullptr, root.context);
    ASSERT_NE(nullptr, root.vtable);
    EXPECT_EQ(RODAQ_CONFIG_PROTOCOL_VERSION, root.vtable->protocol_version);
    EXPECT_EQ(RODAQ_CONFIG_VTABLE_MIN_STRUCT_SIZE, root.vtable->struct_size);
    EXPECT_EQ(RODAQ_CONFIG_MAP, root.vtable->kind(root.context));
    EXPECT_EQ(7U, root.vtable->size(root.context));

    rodaq_config_view items{};
    rodaq_status error{};
    EXPECT_EQ(
        RODAQ_STATUS_OK,
        root.vtable->find(root.context, rodaq_string_view{"items", 5}, &items, &error));
    ASSERT_NE(nullptr, items.vtable);
    EXPECT_EQ(RODAQ_CONFIG_LIST, items.vtable->kind(items.context));
    EXPECT_EQ(3U, items.vtable->size(items.context));

    rodaq_string_view key{};
    EXPECT_EQ(RODAQ_STATUS_OK, root.vtable->key_at(root.context, 0, &key, &error));
    EXPECT_EQ(std::string_view("enabled"), std::string_view(key.data, key.size));

    rodaq_config_view limit{};
    EXPECT_EQ(
        RODAQ_STATUS_OK,
        root.vtable->find(root.context, rodaq_string_view{"limit", 5}, &limit, &error));
    std::int64_t value = 0;
    EXPECT_EQ(RODAQ_STATUS_OK, limit.vtable->as_int64(limit.context, &value, &error));
    EXPECT_EQ(std::numeric_limits<std::int64_t>::max(), value);
    double wrong = 0.0;
    EXPECT_EQ(RODAQ_STATUS_INVALID_STATE, limit.vtable->as_double(limit.context, &wrong, &error));
    EXPECT_DOUBLE_EQ(0.0, wrong);
}

TEST(ConfigViewTest, AbiAdapterRejectsNullOutputsAndClearsFailedOutputs) {
    ConfigTree tree = make_nested_tree();
    const rodaq_config_view root = rodaq::config_abi_view(tree.root());
    rodaq_status error{};

    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        root.vtable->find(root.context, rodaq_string_view{"items", 5}, nullptr, &error));
    EXPECT_NE(0U, error.message_size);

    rodaq_config_view child = rodaq::config_abi_view(take(tree.root().find("items")));
    EXPECT_EQ(RODAQ_STATUS_NOT_FOUND, root.vtable->at(root.context, 99, &child, &error));
    EXPECT_EQ(nullptr, child.context);
    EXPECT_EQ(nullptr, child.vtable);

    double value = 42.0;
    EXPECT_EQ(RODAQ_STATUS_INVALID_STATE, root.vtable->as_double(root.context, &value, &error));
    EXPECT_DOUBLE_EQ(0.0, value);
}
