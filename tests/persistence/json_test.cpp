#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace persistence = rodaq::persistence;
using persistence::Json;

TEST(StrictJson, RejectsDuplicateDecodedKeysInvalidUnicodeAndNonJsonTokens) {
    for (const std::string_view value :
         {R"({"a":1,"\u0061":2})", R"({"nested":{"x":true,"x":false}})", R"([NaN])",
          R"([Infinity])", R"([1e9999])", R"([01])", R"([1,])", R"({/*comment*/"a":1})",
          R"("\ud800")", R"("\udfff")", "null true"}) {
        EXPECT_EQ(
            persistence::parse_json(value).status().code(), rodaq::StatusCode::InvalidArgument)
            << value;
    }
    const std::string invalid_utf8{'"', static_cast<char>(0xc0), static_cast<char>(0x80), '"'};
    EXPECT_FALSE(persistence::parse_json(invalid_utf8).ok());
    EXPECT_TRUE(persistence::parse_json(R"({"a":{"a":1},"b":{"a":2}})").ok());
    EXPECT_TRUE(persistence::parse_json(R"("\ud83d\ude00")").ok());
}

TEST(StrictJson, ChecksDepthCardinalityStringsNodesAndInputBytesDuringParsing) {
    persistence::JsonLimits limits;
    limits.max_depth = 2;
    limits.max_object_members = 1;
    limits.max_array_elements = 2;
    limits.max_string_bytes = 3;
    limits.max_nodes = 4;
    limits.max_bytes = 64;
    EXPECT_TRUE(persistence::parse_json(R"({"x":[1,2]})", limits).ok());
    for (const std::string_view value :
         {R"({"x":[[1]]})", R"({"x":1,"y":2})", "[1,2,3]", R"("long")", R"({"long":1})",
          R"([{"x":1},{"y":2}])"}) {
        EXPECT_FALSE(persistence::parse_json(value, limits).ok()) << value;
    }
    limits.max_bytes = 3;
    EXPECT_FALSE(persistence::parse_json("null", limits).ok());
    EXPECT_TRUE(persistence::parse_json("1.0", limits).ok());
}

TEST(CanonicalJson, SortsUtf16CodeUnitsPreservesArraysAndUsesExactJsonEscapes) {
    // Astral U+1F600 sorts before BMP U+E000 in UTF-16, unlike UTF-8 byte order.
    Json object = {
        {"\xee\x80\x80", 1}, {"\xf0\x9f\x98\x80", 2}, {"a", Json::array({false, nullptr, true})}};
    auto encoded = persistence::canonical_json(object);
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(
        encoded.value(), "{\"a\":[false,null,true],\"\xf0\x9f\x98\x80\":2,\"\xee\x80\x80\":1}");
    const std::string controls{'\0', '\b', '\t', '\n', '\f', '\r', static_cast<char>(0x0f),
                               '"',  '\\', '/'};
    auto quoted = persistence::canonical_json(Json(controls));
    ASSERT_TRUE(quoted.ok());
    EXPECT_EQ(quoted.value(), R"("\u0000\b\t\n\f\r\u000f\"\\/")");
    EXPECT_EQ(persistence::canonical_json(Json("e\xcc\x81")).value(), "\"e\xcc\x81\"");
    EXPECT_NE(
        persistence::canonical_json(Json("e\xcc\x81")).value(),
        persistence::canonical_json(Json("\xc3\xa9")).value());
}

TEST(CanonicalJson, MatchesRfc8785AppendixBNumberVectors) {
    // Mathematical test vectors: https://www.rfc-editor.org/rfc/rfc8785#appendix-B
    const std::pair<std::uint64_t, std::string_view> vectors[]{
        {0x0000000000000000, "0"},
        {0x8000000000000000, "0"},
        {0x0000000000000001, "5e-324"},
        {0x8000000000000001, "-5e-324"},
        {0x7fefffffffffffff, "1.7976931348623157e+308"},
        {0xffefffffffffffff, "-1.7976931348623157e+308"},
        {0x4340000000000000, "9007199254740992"},
        {0xc340000000000000, "-9007199254740992"},
        {0x4430000000000000, "295147905179352830000"},
        {0x44b52d02c7e14af5, "9.999999999999997e+22"},
        {0x44b52d02c7e14af6, "1e+23"},
        {0x44b52d02c7e14af7, "1.0000000000000001e+23"},
        {0x444b1ae4d6e2ef4e, "999999999999999700000"},
        {0x444b1ae4d6e2ef4f, "999999999999999900000"},
        {0x444b1ae4d6e2ef50, "1e+21"},
        {0x3eb0c6f7a0b5ed8c, "9.999999999999997e-7"},
        {0x3eb0c6f7a0b5ed8d, "0.000001"},
        {0x41b3de4355555553, "333333333.3333332"},
        {0x41b3de4355555554, "333333333.33333325"},
        {0x41b3de4355555555, "333333333.3333333"},
        {0x41b3de4355555556, "333333333.3333334"},
        {0x41b3de4355555557, "333333333.33333343"},
        {0xbecbf647612f3696, "-0.0000033333333333333333"},
        {0x43143ff3c1cb0959, "1424953923781206.2"}};
    for (const auto& [bits, expected] : vectors) {
        double number = 0;
        static_assert(sizeof(number) == sizeof(bits));
        std::memcpy(&number, &bits, sizeof(number));
        const auto canonical = persistence::canonical_json(Json(number));
        ASSERT_TRUE(canonical.ok());
        EXPECT_EQ(canonical.value(), expected) << std::hex << bits;
        auto reparsed = persistence::parse_json(canonical.value());
        ASSERT_TRUE(reparsed.ok());
        EXPECT_EQ(persistence::canonical_json(reparsed.value()).value(), expected);
    }
}

TEST(CanonicalJson, RejectsNonFiniteProgrammaticUnicodeAndOutputExpansionOverBudget) {
    for (const auto number :
         {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_FALSE(persistence::canonical_json(Json(number)).ok());
    }
    EXPECT_FALSE(persistence::canonical_json(Json(std::string("\xed\xa0\x80", 3))).ok());
    EXPECT_FALSE(persistence::canonical_json(Json{{std::string("\xff", 1), 1}}).ok());
    persistence::JsonLimits limits;
    limits.max_bytes = 8;
    EXPECT_TRUE(
        persistence::canonical_json(Json(std::string(1, '\0')), limits).value() == R"("\u0000")");
    limits.max_bytes = 7;
    EXPECT_FALSE(persistence::canonical_json(Json(std::string(1, '\0')), limits).ok());
}

TEST(PersistentIntegrity, ExcludesOnlyTopLevelDigestAndSeparatesDomainsIncludingNul) {
    const Json semantic{
        {"format_version", "1.0"},
        {"generation", "18446744073709551615"},
        {"nested", {{"content_digest", "business fact"}}}};
    auto digest = persistence::semantic_digest(semantic, "rodaq.task-control.v1");
    ASSERT_TRUE(digest.ok());
    EXPECT_EQ(
        digest.value(), "sha256:9e269b75b81b4f02dcfd46c8ba858fa9e34637942a1beb0f10a1e8d6c7c292de");
    EXPECT_NE(
        digest.value(), persistence::semantic_digest(semantic, "rodaq.task-recovery.v1").value());
    auto encoded = persistence::encode_integrity(semantic, "rodaq.task-control.v1");
    ASSERT_TRUE(encoded.ok());
    auto parsed = persistence::parse_json(encoded.value());
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(persistence::verify_integrity(parsed.value(), "rodaq.task-control.v1").ok());
    parsed.value()["nested"]["content_digest"] = "corrupted business fact";
    EXPECT_EQ(
        persistence::verify_integrity(parsed.value(), "rodaq.task-control.v1").status().code(),
        rodaq::StatusCode::DataLoss);
    parsed.value()["content_digest"] = "invalid";
    EXPECT_FALSE(persistence::verify_integrity(parsed.value(), "rodaq.task-control.v1").ok());
    EXPECT_FALSE(persistence::semantic_digest(semantic, std::string_view("domain\0", 7)).ok());
}

TEST(StrictJson, DeclaredDepthPolicySupportsDeepPrivateObjectsWithinSafeRecursionBound) {
    persistence::JsonLimits limits;
    limits.max_depth = 128;
    const std::string input = std::string(128, '[') + "0" + std::string(128, ']');
    auto parsed = persistence::parse_json(input, limits);
    ASSERT_TRUE(parsed.ok());
    auto encoded = persistence::canonical_json(parsed.value(), limits);
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(encoded.value(), input);
    limits.max_depth = 127;
    EXPECT_FALSE(persistence::parse_json(input, limits).ok());
    EXPECT_FALSE(persistence::canonical_json(parsed.value(), limits).ok());
}

}  // namespace
