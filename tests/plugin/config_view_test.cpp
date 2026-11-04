#include "plugin/config_view.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using rodaq::ConfigNodeView;
using rodaq::plugin::ConfigTree;

template <typename T>
T take(rodaq::Result<T> result) {
    if (!result.has_value()) {
        throw std::runtime_error("Expected successful Config operation");
    }
    return std::move(result.value());
}

ConfigTree make_nested_tree() {
    ConfigTree nested = take(ConfigTree::Map({
        {"name", ConfigTree::String("node")},
        {"level", ConfigTree::Int64(2)},
    }));
    ConfigTree list = take(ConfigTree::List({
        ConfigTree::Int64(1),
        ConfigTree::Int64(2),
        ConfigTree::String("three"),
    }));
    return take(ConfigTree::Map({
        {"enabled", ConfigTree::Boolean(true)},
        {"limit", ConfigTree::Int64(std::numeric_limits<std::int64_t>::max())},
        {"gain", ConfigTree::Double(0.25)},
        {"mode", ConfigTree::String("fixture")},
        {"empty", ConfigTree::String("")},
        {"nested", std::move(nested)},
        {"items", std::move(list)},
    }));
}

}  // namespace

TEST(ConfigViewTest, SupportsNullScalarsAndEmptyContainers) {
    EXPECT_EQ(RODAQ_CONFIG_NULL, ConfigTree::Null().view().kind());

    EXPECT_EQ(true, take(ConfigTree::Boolean(true).view().as_bool()));
    EXPECT_EQ(false, take(ConfigTree::Boolean(false).view().as_bool()));
    EXPECT_EQ(
        std::numeric_limits<std::int64_t>::min(),
        take(ConfigTree::Int64(std::numeric_limits<std::int64_t>::min()).view().as_int64()));
    EXPECT_EQ(
        std::numeric_limits<std::int64_t>::max(),
        take(ConfigTree::Int64(std::numeric_limits<std::int64_t>::max()).view().as_int64()));
    EXPECT_DOUBLE_EQ(1.5, take(ConfigTree::Double(1.5).view().as_double()));
    EXPECT_EQ("", take(ConfigTree::String("").view().as_string()));
    EXPECT_EQ(
        "r\xC3\xB6"
        "daq",
        take(ConfigTree::String("r\xC3\xB6"
                                "daq")
                 .view()
                 .as_string()));

    const ConfigTree empty_map = take(ConfigTree::Map({}));
    const ConfigTree empty_list = take(ConfigTree::List({}));
    EXPECT_EQ(RODAQ_CONFIG_MAP, empty_map.view().kind());
    EXPECT_EQ(RODAQ_CONFIG_LIST, empty_list.view().kind());
    EXPECT_EQ(0U, empty_map.view().size());
    EXPECT_EQ(0U, empty_list.view().size());
}

TEST(ConfigViewTest, SupportsNestedMapListMapAndStableOrdering) {
    ConfigTree tree = make_nested_tree();
    const ConfigNodeView root = tree.view();
    EXPECT_EQ(RODAQ_CONFIG_MAP, root.kind());
    EXPECT_EQ(7U, root.size());

    const ConfigNodeView items = take(root.find("items"));
    EXPECT_EQ(RODAQ_CONFIG_LIST, items.kind());
    ASSERT_EQ(3U, items.size());
    EXPECT_EQ(1, take(take(items.at(0)).as_int64()));
    EXPECT_EQ(2, take(take(items.at(1)).as_int64()));
    EXPECT_EQ("three", take(take(items.at(2)).as_string()));

    const ConfigNodeView nested = take(root.find("nested"));
    EXPECT_EQ("name", take(nested.key_at(0)));
    EXPECT_EQ("node", take(take(nested.at(0)).as_string()));
    EXPECT_EQ("level", take(nested.key_at(1)));
    EXPECT_EQ(2, take(take(nested.at(1)).as_int64()));
    EXPECT_EQ("enabled", take(root.key_at(0)));
}

TEST(ConfigViewTest, ReportsNotFoundAndInvalidStateWithoutImplicitConversion) {
    ConfigTree tree = make_nested_tree();
    const ConfigNodeView root = tree.view();

    EXPECT_EQ(rodaq::ErrorCode::NotFound, root.find("missing").error().code());
    EXPECT_EQ(rodaq::ErrorCode::NotFound, take(root.find("items")).at(3).error().code());
    EXPECT_EQ(rodaq::ErrorCode::NotFound, root.key_at(root.size()).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument, root.find(std::string_view("\xFF", 1)).error().code());

    const ConfigNodeView limit = take(root.find("limit"));
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, limit.as_double().error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, limit.find("child").error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, limit.at(0).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, limit.key_at(0).error().code());

    const ConfigNodeView items = take(root.find("items"));
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, items.find("child").error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, items.key_at(0).error().code());
}

TEST(ConfigViewTest, AllowsEmptyAndUtf8MapKeys) {
    const ConfigTree tree = take(ConfigTree::Map({
        {"", ConfigTree::Int64(1)},
        {"r\xC3\xB6"
         "le",
         ConfigTree::Int64(2)},
    }));
    const ConfigNodeView root = tree.view();
    EXPECT_EQ(1, take(take(root.find("")).as_int64()));
    EXPECT_EQ(
        2, take(take(root.find("r\xC3\xB6"
                               "le"))
                    .as_int64()));
}

TEST(ConfigViewTest, RejectsInvalidOrDuplicateMapEntries) {
    const auto invalid_key = ConfigTree::Map({
        {std::string("\xFF", 1), ConfigTree::Null()},
    });
    ASSERT_FALSE(invalid_key.has_value());
    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, invalid_key.error().code());

    const auto duplicate = ConfigTree::Map({
        {"key", ConfigTree::Int64(1)},
        {"key", ConfigTree::Int64(2)},
    });
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(rodaq::ErrorCode::Conflict, duplicate.error().code());
}

TEST(ConfigViewTest, BorrowedViewsOutliveTreeOwner) {
    ConfigNodeView view;
    ConfigTree tree = make_nested_tree();
    {
        const ConfigTree copy = tree;
        view = take(copy.view().find("mode"));
    }
    tree = ConfigTree::Null();
    EXPECT_EQ("fixture", take(view.as_string()));
}

TEST(ConfigViewTest, AbiAdapterSupportsContainersAndExactScalars) {
    ConfigTree tree = make_nested_tree();
    const rodaq_config_view root = tree.view().AbiView();
    ASSERT_NE(nullptr, root.context);
    ASSERT_NE(nullptr, root.vtable);
    EXPECT_EQ(RODAQ_CONFIG_PROTOCOL_VERSION, root.vtable->protocol_version);
    EXPECT_EQ(RODAQ_CONFIG_VTABLE_MIN_STRUCT_SIZE, root.vtable->struct_size);
    EXPECT_EQ(RODAQ_CONFIG_MAP, root.vtable->kind(root.context));
    EXPECT_EQ(7U, root.vtable->size(root.context));

    rodaq_config_view items{};
    rodaq_error error{};
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
    const rodaq_config_view root = tree.view().AbiView();
    rodaq_error error{};

    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        root.vtable->find(root.context, rodaq_string_view{"items", 5}, nullptr, &error));
    EXPECT_NE(0U, error.message_size);

    rodaq_config_view child = take(tree.view().find("items")).AbiView();
    EXPECT_EQ(RODAQ_STATUS_NOT_FOUND, root.vtable->at(root.context, 99, &child, &error));
    EXPECT_EQ(nullptr, child.context);
    EXPECT_EQ(nullptr, child.vtable);

    double value = 42.0;
    EXPECT_EQ(RODAQ_STATUS_INVALID_STATE, root.vtable->as_double(root.context, &value, &error));
    EXPECT_DOUBLE_EQ(0.0, value);
}
