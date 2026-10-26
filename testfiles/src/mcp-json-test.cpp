// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <limits>
#include "mcp-bridge/json.h"

using namespace Inkscape::MCP;

TEST(McpJson, ValuesAndEscapesRoundTrip)
{
    for (auto input : {"null", "true", "false", "0", "-0", "-2.5e+30", "1E-9", "[]", "{}",
                       "[1,true,null,{\"x\":\"a\\b\\f\\n\\r\\t\\/\\\\\\\"\"}]",
                       "\"\\u0000\\u00e9\\u6f22\\ud83d\\ude00\""}) {
        auto value = parse(input);
        EXPECT_EQ(parse(serialize(value)), value) << input;
    }
    EXPECT_EQ(parse("\"\\ud83d\\ude00\"").get<std::string>(), "\xf0\x9f\x98\x80");
    EXPECT_EQ(parse("\"\\u0000\"").get<std::string>(), std::string(1, '\0'));
}

TEST(McpJson, RejectsMalformedValuesAndNumbers)
{
    for (auto input : {"", " ", "[", "{", "[1,]", "{\"a\":1,}", "{a:1}", "null true",
                       "NaN", "Infinity", "-Infinity", "1e999", "01", "-01", "+1", ".1", "1.", "1e", "--1",
                       "\"\\x20\"", "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"a\nb\""}) {
        EXPECT_THROW(parse(input), std::exception) << input;
    }
}

TEST(McpJson, RejectsDuplicateKeysIncludingEscapedKeys)
{
    EXPECT_THROW(parse("{\"a\":1,\"a\":2}"), std::exception);
    EXPECT_THROW(parse("{\"a\":1,\"\\u0061\":2}"), std::exception);
    EXPECT_THROW(parse("[{\"a\":0,\"a\":1}]"), std::exception);
}

TEST(McpJson, StrictUtf8)
{
    for (auto bytes : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                       std::string("\x80"), std::string("\xe2\x82"), std::string("\xf0\x80\x80\x80")}) {
        EXPECT_THROW(parse("\"" + bytes + "\""), std::exception);
        EXPECT_THROW(serialize(Json(bytes)), std::exception);
    }
    EXPECT_NO_THROW(parse("\"\xf4\x8f\xbf\xbf\""));
}

TEST(McpJson, LimitsDepthAndInputSize)
{
    auto nested = [](int n) { return std::string(n, '[') + "0" + std::string(n, ']'); };
    EXPECT_NO_THROW(parse(nested(32)));
    EXPECT_THROW(parse(nested(33)), std::exception);
    EXPECT_THROW(parse(std::string(REQUEST_LIMIT + 1, ' ')), std::exception);
}

TEST(McpJson, SerializerRejectsNonFiniteNumbersAndEscapesControls)
{
    EXPECT_THROW(serialize(Json(std::numeric_limits<double>::infinity())), std::exception);
    EXPECT_THROW(serialize(Json(std::numeric_limits<double>::quiet_NaN())), std::exception);
    EXPECT_EQ(serialize(Json(std::string("a\0b", 3))), "\"a\\u0000b\"");
}

TEST(McpJson, CapsSerializedOutput)
{
    EXPECT_THROW(serialize(Json(std::string(RESPONSE_LIMIT, 'a'))), std::exception);
}
