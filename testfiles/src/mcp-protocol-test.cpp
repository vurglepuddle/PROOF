// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "document.h"
#include "inkscape.h"
#include "mcp-bridge/bridge.h"
#include "mcp-bridge/protocol.h"
#include "mcp-bridge/tools.h"
#include "object/sp-root.h"

namespace {
using namespace Inkscape::MCP;
using Object = Json::Object;

class McpProtocol : public ::testing::Test
{
protected:
    std::shared_ptr<Protocol> protocol = std::make_shared<Protocol>("secret", "nonce", "test", 42);
    Json response;
    bool close = false;
    bool look = true, allowed = true, closing = false;
    Json call(std::string method, Json params = Object{})
    {
        protocol->dispatch(Object{{"id", "request"}, {"method", method}, {"params", params}}, look, closing,
            [](auto const &name, auto const &args) {
                validate_arguments(name, args);
                return tool_result(Object{{"document", nullptr}}, "No document is open in PROOF.");
            },
            [this](auto, auto, auto answer) { answer(allowed); },
            [this](auto result, bool disconnect) { response = std::move(result); close = disconnect; });
        return response;
    }
    Json hello(std::string token = "secret", int version = 1)
    {
        return call("hello", Object{{"token", token}, {"bridge_protocol", version}, {"client", Object{{"name", "test"}, {"version", "1"}}}});
    }
};

TEST_F(McpProtocol, RequiresHello)
{
    EXPECT_TRUE(call("tools/list").contains("error"));
    EXPECT_TRUE(close);
}
TEST_F(McpProtocol, WrongTokenCloses)
{
    EXPECT_EQ(hello("bad").at("error").at("code"), Json(-32001));
    EXPECT_TRUE(close);
}
TEST_F(McpProtocol, WrongVersionCloses)
{
    EXPECT_EQ(hello("secret", 2).at("error").at("code"), Json(-32002));
    EXPECT_TRUE(close);
}
TEST_F(McpProtocol, DeniedConsentCloses)
{
    allowed = false;
    EXPECT_EQ(hello().at("error").at("code"), Json(-32003));
    EXPECT_TRUE(close);
}
TEST_F(McpProtocol, HelloAndPing)
{
    EXPECT_EQ(hello().at("result").at("instance").at("pid"), Json(42));
    EXPECT_FALSE(close);
    EXPECT_TRUE(protocol->authorized());
    EXPECT_EQ(call("ping").at("result"), Json(Object{}));
}
TEST_F(McpProtocol, CurrentPermissionControlsListAndCalls)
{
    hello();
    EXPECT_EQ(call("tools/list").at("result").at("tools").get<Json::Array>().size(), 7);
    look = false;
    EXPECT_TRUE(call("tools/list").at("result").at("tools").get<Json::Array>().empty());
    for (auto const &tool : look_tools()) {
        EXPECT_EQ(call("tools/call", Object{{"name", tool.at("name")}, {"arguments", Object{}}}).at("result").at("isError"), Json(true));
    }
    look = true;
    EXPECT_EQ(call("tools/list").at("result").at("tools").get<Json::Array>().size(), 7);
}
TEST_F(McpProtocol, LookToolsHaveFixedOrderAndReadOnlyAnnotations)
{
    hello();
    auto tools = call("tools/list").at("result").at("tools").get<Json::Array>();
    std::vector<std::string> names;
    for (auto const &tool : tools) {
        names.push_back(tool.at("name").get<std::string>());
        EXPECT_EQ(tool.at("annotations").at("readOnlyHint"), Json(true));
        EXPECT_EQ(tool.at("annotations").at("idempotentHint"), Json(true));
        EXPECT_EQ(tool.at("annotations").at("openWorldHint"), Json(false));
        EXPECT_EQ(tool.at("inputSchema").at("additionalProperties"), Json(false));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"document_info", "list_layers", "list_objects", "get_object", "get_selection", "render_preview", "get_xml"}));
}
TEST_F(McpProtocol, UnknownAndBadToolCallsAreToolErrors)
{
    hello();
    for (auto params : {Json(Object{{"name", "execute_code"}, {"arguments", Object{}}}),
                        Json(Object{{"name", "document_info"}, {"arguments", Object{{"path", "outside"}}}}),
                        Json(Object{{"name", "document_info"}, {"arguments", nullptr}}), Json(Object{})}) {
        EXPECT_EQ(call("tools/call", params).at("result").at("isError"), Json(true));
        EXPECT_FALSE(close);
    }
    EXPECT_EQ(call("unknown").at("error").at("code"), Json(-32601));
}
TEST_F(McpProtocol, NoDocument)
{
    hello();
    EXPECT_EQ(call("tools/call", Object{{"name", "document_info"}, {"arguments", Object{}}}).at("result").at("structuredContent").at("document"), Json());
    Bridge bridge;
    EXPECT_EQ(bridge.document_info().at("structuredContent").at("document"), Json());
}
TEST_F(McpProtocol, DocumentCountsExcludeResources)
{
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    std::string const svg = R"svg(<svg xmlns='http://www.w3.org/2000/svg'
      xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' width='100' height='100'>
      <defs>
        <clipPath id='clip'><path d='M0,0H10V10Z'/></clipPath>
        <mask id='mask'><rect width='10' height='10'/></mask>
        <pattern id='pattern' width='10' height='10'><rect width='10' height='10'/></pattern>
        <marker id='marker'><path d='M0,0L10,10'/></marker>
        <symbol id='symbol'><g><rect width='10' height='10'/></g></symbol>
      </defs>
      <g inkscape:groupmode='layer' id='layer'>
        <defs><clipPath id='nested-clip'><path d='M0,0H10V10Z'/></clipPath></defs>
        <g inkscape:groupmode='layer' id='sublayer'><rect width='10' height='10'/></g>
        <g><path d='M0,0L10,10'/></g>
      </g>
      <rect width='10' height='10'/>
      <text>x<tspan>y</tspan></text>
      </svg>)svg";
    auto document = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(document);
    auto counts = document_counts(*document->getRoot());
    EXPECT_EQ(counts.layers, 2);
    EXPECT_EQ(counts.items, 5);
}
TEST_F(McpProtocol, ClosingAndCancelledQuit)
{
    hello();
    closing = true;
    EXPECT_EQ(call("tools/call", Object{{"name", "document_info"}, {"arguments", Object{}}}).at("result").at("isError"), Json(true));
    closing = false;
    EXPECT_TRUE(call("ping").contains("result"));
}
TEST_F(McpProtocol, HandlerExceptionBecomesToolError)
{
    hello();
    protocol->dispatch(Object{{"id", 1}, {"method", "tools/call"}, {"params", Object{{"name", "document_info"}, {"arguments", Object{}}}}},
        true, false, [](auto const &, auto const &) -> Json { throw std::runtime_error("failed"); }, {},
        [this](auto reply, bool) { response = std::move(reply); });
    EXPECT_EQ(response.at("result").at("isError"), Json(true));
}
TEST_F(McpProtocol, RejectsMalformedEnvelope)
{
    protocol->dispatch(Object{{"id", true}}, true, false, {}, {},
        [this](auto reply, bool) { response = std::move(reply); });
    EXPECT_EQ(response.at("error").at("code"), Json(-32600));
}
} // namespace
