// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdkmm/pixbuf.h>
#include <gdkmm/wrap_init.h>

#include "document.h"
#include "inkscape.h"
#include "mcp-bridge/bridge.h"
#include "mcp-bridge/tools.h"
#include "object/object-set.h"
#include "object/sp-item-group.h"
#include "object/sp-namedview.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "preferences.h"
#include "spot-ink.h"
#include "style.h"
#include "util/units.h"
#include "xml/node.h"

namespace {
using namespace Inkscape;
using namespace Inkscape::MCP;
using Object = Json::Object;
using Array = Json::Array;

class McpTools : public ::testing::Test
{
protected:
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<ObjectSet> selection;
    Json identity = Object{{"handle", "nonce:42"}, {"revision", 7}};
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
        Gdk::wrap_init();
        load(R"svg(<defs>
          <clipPath id='clip'><path id='clip-path' d='M0,0H100V100Z'/></clipPath>
          <mask id='mask'><rect id='mask-rect' width='100' height='100' fill='white'/></mask>
          <marker id='marker'><path id='marker-path' d='M0,0L5,5'/></marker>
          <symbol id='symbol'><rect id='symbol-rect' width='10' height='10'/></symbol>
          <linearGradient id='gradient'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>
        </defs>
        <g id='layer' inkscape:groupmode='layer' inkscape:label='Art'>
          <rect id='rect' x='10' y='10' width='20' height='10' fill='device-cmyk(0.1 0.2 0.3 0.4)'/>
          <g id='sub' inkscape:groupmode='layer' inkscape:label='Hidden' style='display:none' sodipodi:insensitive='true'>
            <path id='hidden-path' d='M0,0L10,10'/>
          </g>
          <g id='group' inkscape:label='Objects'>
            <path id='path' d='M40,10L60,10L60,30Z' fill='url(#gradient)'/>
            <text id='text' x='40' y='60' style='font-family:sans-serif;font-size:12px'><tspan sodipodi:role='line'>Hello</tspan><tspan sodipodi:role='line'>world</tspan></text>
          </g>
        </g>
        <use id='use' xlink:href='#rect' x='80'/>
        <image id='image' x='150' y='10' width='10' height='10'
          xlink:href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLttAAAAABJRU5ErkJggg=='/>)svg");
    }
    void load(std::string const &body, std::string attributes = "width='200' height='100'")
    {
        selection.reset();
        std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
            "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' xmlns:xlink='http://www.w3.org/1999/xlink' "
            "xmlns:proof='urn:x-proof:document:1' " + attributes + ">" + body + "</svg>";
        document = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        selection = std::make_unique<ObjectSet>(document.get());
    }
    LookContext context(Geom::Point anchor = {0, 0})
    {
        return {*document, selection.get(), document->getObjectById("sub"), anchor, identity};
    }
    SPItem *item(char const *id) { return cast<SPItem>(document->getObjectById(id)); }
    Json call(std::string const &name, Json args = Object{})
    {
        auto response = call_look_tool(context(), name, args);
        EXPECT_EQ(response.at("structuredContent").at("document"), identity);
        return response;
    }
    Json data(std::string const &name, Json args = Object{})
    {
        auto response = call(name, args);
        EXPECT_EQ(response.at("isError"), Json(false)) << serialize(response);
        return response.at("structuredContent");
    }
    static std::vector<std::string> ids(Json const &array)
    {
        std::vector<std::string> result;
        for (auto const &object : array.get<Array>()) result.push_back(object.at("id").get<std::string>());
        return result;
    }
    static Glib::RefPtr<Gdk::Pixbuf> png(Json const &response)
    {
        auto const &content = response.at("content").get<Array>();
        EXPECT_EQ(content[0].at("type"), Json("image"));
        EXPECT_EQ(content[0].at("mimeType"), Json("image/png"));
        auto const &encoded = content[0].at("data").get<std::string>();
        gsize length;
        auto bytes = g_base64_decode(encoded.c_str(), &length);
        auto loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
        EXPECT_TRUE(gdk_pixbuf_loader_write(loader, bytes, length, nullptr));
        EXPECT_TRUE(gdk_pixbuf_loader_close(loader, nullptr));
        g_free(bytes);
        auto raw = gdk_pixbuf_loader_get_pixbuf(loader);
        auto result = raw ? Glib::wrap(raw, true) : Glib::RefPtr<Gdk::Pixbuf>();
        g_object_unref(loader);
        return result;
    }
};

TEST_F(McpTools, LayerTreeAndCurrent)
{
    auto value = data("list_layers");
    auto const &layers = value.at("layers").get<Array>();
    ASSERT_EQ(layers.size(), 2);
    EXPECT_EQ(ids(value.at("layers")), (std::vector<std::string>{"layer", "sub"}));
    EXPECT_EQ(layers[0].at("parent"), Json());
    EXPECT_EQ(layers[0].at("depth"), Json(0));
    EXPECT_EQ(layers[0].at("items"), Json(2));
    EXPECT_EQ(layers[1].at("parent"), Json("layer"));
    EXPECT_EQ(layers[1].at("depth"), Json(1));
    EXPECT_EQ(layers[1].at("visible"), Json(false));
    EXPECT_EQ(layers[1].at("locked"), Json(true));
    EXPECT_EQ(value.at("current"), Json("sub"));
    auto ctx = context();
    ctx.current_layer = document->getRoot();
    EXPECT_EQ(call_look_tool(ctx, "list_layers", Object{}).at("structuredContent").at("current"), Json());
}
TEST_F(McpTools, ObjectDepthAndZOrder)
{
    EXPECT_EQ(ids(data("list_objects").at("objects")), (std::vector<std::string>{"layer", "use", "image"}));
    auto all = data("list_objects", Object{{"depth", 5}});
    EXPECT_EQ(ids(all.at("objects")), (std::vector<std::string>{"layer", "rect", "sub", "hidden-path", "group", "path", "text", "use", "image"}));
    EXPECT_EQ(all.at("total"), Json(9));
    EXPECT_EQ(all.at("objects").get<Array>()[3].at("depth"), Json(3));
    EXPECT_EQ(data("list_objects", Object{{"parent", "layer"}, {"depth", 1}}).at("total"), Json(3));
    EXPECT_EQ(data("list_objects", Object{{"parent", "group"}}).at("total"), Json(2));
}
TEST_F(McpTools, ObjectPaging)
{
    auto first = data("list_objects", Object{{"depth", 5}, {"limit", 2}});
    EXPECT_EQ(first.at("next_offset"), Json(2));
    auto middle = data("list_objects", Object{{"depth", 5}, {"offset", 2}, {"limit", 2}});
    EXPECT_EQ(ids(middle.at("objects")), (std::vector<std::string>{"sub", "hidden-path"}));
    auto last = data("list_objects", Object{{"depth", 5}, {"offset", 8}, {"limit", 2}});
    EXPECT_EQ(ids(last.at("objects")), (std::vector<std::string>{"image"}));
    EXPECT_EQ(last.at("next_offset"), Json());
    auto beyond = data("list_objects", Object{{"offset", 9007199254740991.0}});
    EXPECT_TRUE(beyond.at("objects").get<Array>().empty());
    EXPECT_EQ(beyond.at("next_offset"), Json());
}
TEST_F(McpTools, ParentsAndDefinitionIdsAreRefused)
{
    for (auto id : {"missing", "rect", "clip", "clip-path", "symbol", "symbol-rect", "marker-path", "mask-rect"}) {
        EXPECT_EQ(call("list_objects", Object{{"parent", id}}).at("isError"), Json(true)) << id;
    }
    for (auto id : {"missing", "clip", "clip-path", "symbol", "symbol-rect", "marker-path", "mask-rect", "gradient"}) {
        EXPECT_EQ(call("get_object", Object{{"id", id}}).at("isError"), Json(true)) << id;
    }
}
TEST_F(McpTools, NamesPaintsAndOwnVisibility)
{
    auto rect = data("get_object", Object{{"id", "rect"}});
    EXPECT_EQ(rect.at("name"), Json());
    EXPECT_EQ(rect.at("label"), Json("#rect"));
    EXPECT_EQ(rect.at("type"), Json("rect"));
    EXPECT_EQ(rect.at("tag"), Json("svg:rect"));
    EXPECT_EQ(rect.at("layer"), Json("layer"));
    EXPECT_EQ(rect.at("fill").at("space"), Json("DeviceCMYK"));
    EXPECT_EQ(rect.at("fill").at("values"), Json(Array{10, 20, 30, 40}));
    auto group = data("get_object", Object{{"id", "group"}});
    EXPECT_EQ(group.at("name"), Json("Objects"));
    EXPECT_EQ(group.at("children"), Json(Array{"path", "text"}));
    auto child = data("get_object", Object{{"id", "hidden-path"}});
    EXPECT_EQ(child.at("visible"), Json(true));
    EXPECT_EQ(child.at("locked"), Json(false));
    EXPECT_EQ(child.at("layer"), Json("sub"));
    auto path = data("get_object", Object{{"id", "path"}});
    EXPECT_EQ(path.at("fill"), Json(Object{{"type", "gradient"}, {"id", "gradient"}}));
    EXPECT_EQ(path.at("nodes"), Json(3));
}
TEST_F(McpTools, SpotStrokeKeepsInkAlternateAndTint)
{
    auto swatch = SpotInk::ensure(document.get(), {"Test ink", Colors::Color(Colors::Space::Type::CMYK, {0.2, 0.4, 0.1, 0}), 0.35});
    item("rect")->setAttribute("style", ("stroke:url(#" + swatch + ");stroke-width:2;fill:none").c_str());
    auto value = data("get_object", Object{{"id", "rect"}});
    auto const &stroke = value.at("stroke");
    EXPECT_EQ(stroke.at("type"), Json("spot"));
    EXPECT_EQ(stroke.at("ink"), Json("Test ink"));
    EXPECT_EQ(stroke.at("tint"), Json(0.35));
    EXPECT_EQ(stroke.at("alternate").at("values"), Json(Array{20, 40, 10, 0}));
    EXPECT_NEAR(stroke.at("width").get<double>(), Util::Quantity::convert(2, "px", document->getDisplayUnit()), 1e-8);
    EXPECT_EQ(value.at("fill"), Json(Object{{"type", "none"}}));
    EXPECT_TRUE(value.contains("geometric"));
}
TEST_F(McpTools, ObjectDetails)
{
    auto text = data("get_object", Object{{"id", "text"}});
    EXPECT_EQ(text.at("text"), Json("Hello\nworld"));
    EXPECT_EQ(text.at("kind"), Json("point"));
    EXPECT_NEAR(text.at("font_size").get<double>(), 9, 1e-8);
    EXPECT_EQ(data("get_object", Object{{"id", "use"}}).at("href"), Json("#rect"));
    auto image = data("get_object", Object{{"id", "image"}});
    EXPECT_EQ(image.at("link"), Json("embedded"));
    EXPECT_EQ(image.at("pixels"), Json(Object{{"width", 1}, {"height", 1}}));
    item("rect")->setAttribute("transform", "matrix(1,0,0,1,5,6)");
    item("rect")->setAttribute("style", "mix-blend-mode:multiply;opacity:0.4");
    item("rect")->setAttribute("clip-path", "url(#clip)");
    item("rect")->setAttribute("mask", "url(#mask)");
    auto rect = data("get_object", Object{{"id", "rect"}});
    EXPECT_EQ(rect.at("transform"), Json(Array{1, 0, 0, 1, 5, 6}));
    EXPECT_EQ(rect.at("blend"), Json("multiply"));
    EXPECT_EQ(rect.at("clip"), Json("clip"));
    EXPECT_EQ(rect.at("mask"), Json("mask"));
    EXPECT_NEAR(rect.at("opacity").get<double>(), 0.4, 1e-5);
}
TEST_F(McpTools, TextReportsItsCharactersPaint)
{
    // As the Text tool leaves it after painting selected characters: the <text> says none.
    load("<text id='text' style='fill:none;stroke:none'><tspan sodipodi:role='line'>"
         "<tspan style='fill:#ffffff;stroke:#000000;stroke-width:2'>Hi</tspan></tspan></text>"
         "<text id='mixed' style='fill:none'>a<tspan style='fill:#ff0000'>b</tspan></text>"
         "<text id='plain'>c</text><rect id='rect' width='1' height='1'/>");
    auto text = data("get_object", Object{{"id", "text"}});
    EXPECT_EQ(text.at("fill"), Json(Object{{"type", "none"}}));
    EXPECT_EQ(text.at("characters").at("fill").at("hex"), Json("#ffffff"));
    EXPECT_EQ(text.at("characters").at("stroke").at("hex"), Json("#000000"));
    EXPECT_NEAR(text.at("characters").at("stroke").at("width").get<double>(), Util::Quantity::convert(2, "px", document->getDisplayUnit()), 1e-8);
    auto mixed = data("list_objects").at("objects").get<Array>().at(1);
    EXPECT_EQ(mixed.at("characters").at("fill"), Json(Object{{"type", "mixed"}}));
    EXPECT_EQ(mixed.at("characters").at("stroke"), Json(Object{{"type", "none"}}));
    // Unset everywhere: the characters are drawn black, the initial fill.
    EXPECT_EQ(data("get_object", Object{{"id", "plain"}}).at("characters").at("fill").at("hex"), Json("#000000"));
    EXPECT_FALSE(data("get_object", Object{{"id", "rect"}}).contains("characters"));
}
TEST_F(McpTools, KindsAndTextTruncation)
{
    load("<rect id='rect'/><circle id='circle'/><ellipse id='ellipse'/><line id='line'/><polyline id='polyline'/>"
         "<polygon id='polygon'/><g id='group'/><switch id='switch'/><path id='path'/>"
         "<path id='star' sodipodi:type='star'/><path id='spiral' sodipodi:type='spiral'/>"
         "<text id='text'>" + std::string(2100, 'x') + "</text>"
         "<text id='pathtext'><textPath xlink:href='#path'>Path</textPath></text>"
         "<flowRoot id='flow'><flowRegion><rect width='100' height='100'/></flowRegion><flowPara>Area</flowPara></flowRoot>");
    for (auto id : {"rect", "circle", "ellipse", "line", "polyline", "polygon", "group", "switch", "path", "star", "spiral", "text"}) {
        EXPECT_EQ(data("get_object", Object{{"id", id}}).at("type"), Json(id));
    }
    auto text = data("get_object", Object{{"id", "text"}});
    EXPECT_EQ(text.at("text").get<std::string>().size(), 2000);
    EXPECT_EQ(text.at("truncated"), Json(true));
    EXPECT_EQ(data("get_object", Object{{"id", "pathtext"}}).at("kind"), Json("path"));
    EXPECT_EQ(data("get_object", Object{{"id", "flow"}}).at("kind"), Json("area"));
}
TEST_F(McpTools, SelectionAndCombinedBounds)
{
    EXPECT_EQ(data("get_selection").at("count"), Json(0));
    EXPECT_EQ(data("get_selection").at("bbox"), Json());
    selection->setList(std::vector<SPItem *>{item("rect"), item("path")});
    auto value = data("get_selection");
    EXPECT_EQ(value.at("count"), Json(2));
    EXPECT_EQ(value.at("objects").get<Array>().size(), 2);
    auto bounds = selection->visualBounds();
    EXPECT_NEAR(value.at("bbox").at("width").get<double>(), Util::Quantity::convert(bounds->width(), "px", document->getDisplayUnit()), 1e-8);
}
TEST_F(McpTools, SelectionAndGroupChildrenAreCapped)
{
    std::string body = "<g id='group'>";
    for (int i = 0; i < 210; ++i) body += "<rect id='r" + std::to_string(i) + "' width='1' height='1'/>";
    load(body + "</g>");
    auto group = data("get_object", Object{{"id", "group"}});
    EXPECT_EQ(group.at("children").get<Array>().size(), 200);
    std::vector<SPItem *> items;
    for (int i = 0; i < 210; ++i) items.push_back(item(("r" + std::to_string(i)).c_str()));
    selection->setList(items);
    auto selected = data("get_selection");
    EXPECT_EQ(selected.at("count"), Json(210));
    EXPECT_EQ(selected.at("objects").get<Array>().size(), 200);
    EXPECT_EQ(data("list_objects", Object{{"parent", "group"}, {"limit", 500}}).at("objects").get<Array>().size(), 210);
}
TEST_F(McpTools, DocumentScaleAndDisplayUnitAreApplied)
{
    load("<sodipodi:namedview inkscape:document-units='mm' origin-correction='false'/>"
         "<rect id='rect' x='10' y='20' width='30' height='40' fill='red'/>"
         "<text id='text' style='font-size:12px'>Text</text>",
         "width='200mm' height='100mm' viewBox='0 0 200 100'");
    auto rect = data("get_object", Object{{"id", "rect"}});
    auto const &bbox = rect.at("bbox");
    EXPECT_NEAR(bbox.at("x").get<double>(), 10, 1e-6);
    EXPECT_NEAR(bbox.at("y").get<double>(), 20, 1e-6);
    EXPECT_NEAR(bbox.at("width").get<double>(), 30, 1e-6);
    EXPECT_NEAR(bbox.at("height").get<double>(), 40, 1e-6);
    EXPECT_NEAR(data("get_object", Object{{"id", "text"}}).at("font_size").get<double>(), Util::Quantity::convert(12, "mm", "pt"), 1e-6);
}
TEST_F(McpTools, CoordinatesMatchSelectorWithPageOriginAnchorAndYDirection)
{
    for (auto down : {true, false}) for (auto correction : {true, false}) {
        load("<sodipodi:namedview inkscape:document-units='mm'/><defs><view id='page' viewBox='50 20 80 60'/></defs>"
             "<rect id='rect' x='60' y='30' width='20' height='10' style='fill:red;stroke:black;stroke-width:2'/>");
        document->getNamedView()->set_y_axis_down(down);
        document->getNamedView()->set_origin_follows_page(correction);
        document->ensureUpToDate();
        EXPECT_EQ(document->yaxisdown(), down);
        ASSERT_EQ(document->getPageManager().getPageCount(), 1);
        selection->set(item("rect"));
        auto ctx = context({0.5, 1});
        auto value = call_look_tool(ctx, "get_selection", Object{}).at("structuredContent").at("bbox");
        auto bounds = selection->visualBounds();
        auto x = bounds->left() + bounds->width() * ctx.anchor.x();
        auto y = bounds->top() + bounds->height() * ctx.anchor.y();
        if (correction) {
            auto page = document->getPageManager().getSelectedPageRect();
            x -= page.left();
            y -= page.top();
        }
        EXPECT_NEAR(value.at("x").get<double>(), Util::Quantity::convert(x, "px", document->getDisplayUnit()), 1e-8);
        EXPECT_NEAR(value.at("y").get<double>(), Util::Quantity::convert(y, "px", document->getDisplayUnit()), 1e-8);
        auto object = call_look_tool(ctx, "get_object", Object{{"id", "rect"}}).at("structuredContent");
        EXPECT_EQ(object.at("bbox"), value);
        EXPECT_TRUE(object.contains("geometric"));
    }
}
TEST_F(McpTools, PreviewPageObjectAndSelectionPixels)
{
    load("<rect id='red' width='100' height='100' fill='#ff0000'/><rect id='blue' x='100' width='100' height='100' fill='#0000ff'/>");
    auto page = call("render_preview", Object{{"size", 200}, {"background", "white"}});
    ASSERT_EQ(page.at("isError"), Json(false));
    auto page_image = png(page);
    ASSERT_TRUE(page_image);
    ASSERT_EQ(page_image->get_width(), 200);
    ASSERT_EQ(page_image->get_height(), 100);
    auto pixels = page_image->get_pixels();
    auto red = pixels + 50 * page_image->get_rowstride() + 50 * page_image->get_n_channels();
    EXPECT_EQ(red[0], 255);
    EXPECT_EQ(red[1], 0);
    EXPECT_EQ(red[2], 0);
    auto blue = pixels + 50 * page_image->get_rowstride() + 150 * page_image->get_n_channels();
    EXPECT_EQ(blue[2], 255);
    auto object = call("render_preview", Object{{"area", "object"}, {"id", "red"}, {"size", 64}});
    auto object_image = png(object);
    ASSERT_TRUE(object_image);
    EXPECT_EQ(object_image->get_width(), 64);
    EXPECT_EQ(object_image->get_height(), 64);
    selection->set(item("blue"));
    auto selected = png(call("render_preview", Object{{"area", "selection"}, {"size", 64}}));
    ASSERT_TRUE(selected);
    ASSERT_EQ(selected->get_width(), 64);
    ASSERT_EQ(selected->get_height(), 64);
    auto center = selected->get_pixels() + 32 * selected->get_rowstride() + 32 * selected->get_n_channels();
    EXPECT_EQ(center[0], 0);
    EXPECT_EQ(center[2], 255);
}
TEST_F(McpTools, PreviewHidesUnselectedOverlappingObjects)
{
    load("<rect id='red' width='100' height='100' fill='red'/><rect id='blue' width='100' height='100' fill='blue'/>");
    selection->set(item("red"));
    for (auto args : {Object{{"area", "selection"}, {"size", 64}}, Object{{"area", "object"}, {"id", "red"}, {"size", 64}}}) {
        auto image = png(call("render_preview", args));
        ASSERT_TRUE(image);
        ASSERT_EQ(image->get_width(), 64);
        ASSERT_EQ(image->get_height(), 64);
        auto pixel = image->get_pixels() + 32 * image->get_rowstride() + 32 * image->get_n_channels();
        EXPECT_EQ(pixel[0], 255);
        EXPECT_EQ(pixel[2], 0);
    }
}
TEST_F(McpTools, PreviewUsesCurrentAndExplicitPages)
{
    load("<defs><view id='first' viewBox='0 0 100 100'/><view id='second' viewBox='100 0 100 100'/></defs>"
         "<rect width='100' height='100' fill='red'/><rect x='100' width='100' height='100' fill='blue'/>");
    auto &pages = document->getPageManager();
    ASSERT_EQ(pages.getPageCount(), 2);
    pages.selectPage(1);
    auto response = call("render_preview", Object{{"size", 64}});
    EXPECT_NEAR(response.at("structuredContent").at("area").at("x").get<double>(), 0, 1e-8);
    auto current = png(response);
    ASSERT_TRUE(current);
    ASSERT_EQ(current->get_width(), 64);
    ASSERT_EQ(current->get_height(), 64);
    auto pixel = current->get_pixels() + 32 * current->get_rowstride() + 32 * current->get_n_channels();
    EXPECT_EQ(pixel[0], 0);
    EXPECT_EQ(pixel[2], 255);
    auto first = png(call("render_preview", Object{{"page", 0}, {"size", 64}}));
    ASSERT_TRUE(first);
    pixel = first->get_pixels() + (first->get_height() / 2) * first->get_rowstride() +
            (first->get_width() / 4) * first->get_n_channels();
    EXPECT_EQ(pixel[0], 255);
    EXPECT_EQ(pixel[2], 0);
    auto drawing = png(call("render_preview", Object{{"area", "drawing"}, {"size", 128}}));
    ASSERT_TRUE(drawing);
    EXPECT_EQ(drawing->get_width(), 128);
    EXPECT_EQ(drawing->get_height(), 64);
}
TEST_F(McpTools, PreviewCapsAndDegenerateArea)
{
    load("<rect id='rect' width='100' height='100' fill='red'/>", "width='100' height='100'");
    auto response = call("render_preview", Object{{"size", 4096}});
    auto image = png(response);
    ASSERT_TRUE(image);
    EXPECT_LE(image->get_width(), 2048);
    EXPECT_LE(image->get_height(), 2048);
    EXPECT_LE(image->get_width() * image->get_height(), 4000000);
    EXPECT_GT(image->get_width(), 1900);
    load("<path id='empty' d='M0,0'/>");
    EXPECT_EQ(call("render_preview", Object{{"area", "object"}, {"id", "empty"}}).at("isError"), Json(true));
    EXPECT_EQ(call("render_preview", Object{{"area", "selection"}}).at("isError"), Json(true));
    EXPECT_EQ(call("render_preview", Object{{"page", 10}}).at("isError"), Json(true));
}
TEST_F(McpTools, PreviewBackgrounds)
{
    load("<sodipodi:namedview pagecolor='#00ff00'/>");
    for (auto background : {"page", "white", "transparent"}) {
        auto image = png(call("render_preview", Object{{"background", background}, {"size", 64}}));
        ASSERT_TRUE(image);
        auto pixel = image->get_pixels();
        if (std::string(background) == "transparent") {
            ASSERT_EQ(image->get_n_channels(), 4);
            EXPECT_EQ(pixel[3], 0);
        } else {
            EXPECT_EQ(pixel[1], 255);
            EXPECT_EQ(pixel[0], std::string(background) == "page" ? 0 : 255);
        }
    }
}
TEST_F(McpTools, EveryToolRejectsUnknownKeysAndWrongArgumentTypes)
{
    for (auto name : {"list_layers", "list_objects", "get_object", "get_selection", "render_preview", "get_xml"}) {
        EXPECT_EQ(call(name, Object{{"unknown", 1}}).at("isError"), Json(true));
        EXPECT_EQ(call(name, Array{}).at("isError"), Json(true));
    }
    for (auto args : {Object{{"depth", 0}}, Object{{"depth", 6}}, Object{{"depth", true}}, Object{{"depth", 1.5}},
                      Object{{"offset", -1}}, Object{{"offset", "1"}}, Object{{"limit", 501}}, Object{{"limit", 0}}, Object{{"parent", 3}}}) {
        EXPECT_EQ(call("list_objects", args).at("isError"), Json(true));
    }
    for (auto args : {Object{}, Object{{"id", 1}}, Object{{"id", ""}}, Object{{"id", std::string("a\0b", 3)}}}) {
        EXPECT_EQ(call("get_object", args).at("isError"), Json(true));
    }
    for (auto args : {Object{{"area", "bad"}}, Object{{"area", "object"}}, Object{{"area", 1}}, Object{{"size", 63}},
                      Object{{"size", true}}, Object{{"size", 100.5}}, Object{{"background", "bad"}}, Object{{"page", -1}},
                      Object{{"id", "rect"}}, Object{{"area", "drawing"}, {"page", 0}}}) {
        EXPECT_EQ(call("render_preview", args).at("isError"), Json(true));
    }
}
TEST_F(McpTools, BridgeWithoutActiveDocumentReturnsToolErrors)
{
    Bridge bridge;
    for (auto name : {"list_layers", "list_objects", "get_object", "get_selection", "render_preview", "get_xml"}) {
        auto response = bridge.call_tool(name, Object{});
        EXPECT_EQ(response.at("isError"), Json(true));
        EXPECT_EQ(response.at("structuredContent").at("document"), Json());
    }
}
TEST_F(McpTools, XmlKeepsNestedTextPaintEscapingCommentsAndAttributeOrder)
{
    load("<text id='painted' style='fill:none' inkscape:label='A &amp; &quot;B&quot; &lt;C&gt;'>"
         "<tspan style='fill:#ff0000'>A &amp; &lt;B&gt;<tspan style='stroke:#0000ff'>nested</tspan></tspan>"
         "<!--kept--><?proof-test kept?><![CDATA[<raw>&]]></text>");
    auto response = call("get_xml", Object{{"id", "painted"}});
    ASSERT_EQ(response.at("isError"), Json(false));
    auto const &value = response.at("structuredContent");
    auto const &xml = value.at("xml").get<std::string>();
    EXPECT_EQ(value.at("id"), Json("painted"));
    EXPECT_EQ(response.at("content").get<Array>()[0].at("text"), Json(xml));
    EXPECT_NE(xml.find("style=\"fill:none\""), std::string::npos);
    EXPECT_NE(xml.find("style=\"fill:#ff0000\""), std::string::npos);
    EXPECT_NE(xml.find("style=\"stroke:#0000ff\""), std::string::npos);
    EXPECT_NE(xml.find("A &amp; &quot;B&quot; &lt;C&gt;"), std::string::npos);
    EXPECT_NE(xml.find("A &amp; &lt;B&gt;"), std::string::npos);
    EXPECT_NE(xml.find("<!--kept-->"), std::string::npos);
    EXPECT_NE(xml.find("<?proof-test kept?>"), std::string::npos);
    EXPECT_NE(xml.find("&lt;raw&gt;&amp;"), std::string::npos);
    EXPECT_LT(xml.find("id=\"painted\""), xml.find("style=\"fill:none\""));
    EXPECT_LT(xml.find("style=\"fill:none\""), xml.find("inkscape:label="));
    EXPECT_EQ(value.at("chars"), Json(double(g_utf8_strlen(xml.c_str(), -1))));
    EXPECT_EQ(value.at("truncated"), Json(false));
    EXPECT_EQ(data("get_xml", Object{{"id", "painted"}}), value);
}
TEST_F(McpTools, XmlDepthCountsElementLevelsAndElidesChildrenTogether)
{
    load("<g id='tree'><!--kept--><g id='branch'><text id='leaf'>text</text></g><rect id='sibling'/></g>");
    auto zero = data("get_xml", Object{{"id", "tree"}, {"depth", 0}}).at("xml").get<std::string>();
    EXPECT_NE(zero.find("<!--kept-->"), std::string::npos);
    EXPECT_NE(zero.find("  <!-- 2 children omitted -->\n"), std::string::npos);
    EXPECT_EQ(zero.find("id=\"branch\""), std::string::npos);
    auto one = data("get_xml", Object{{"id", "tree"}, {"depth", 1}}).at("xml").get<std::string>();
    EXPECT_NE(one.find("  <svg:g id=\"branch\">\n    <!-- 1 children omitted -->"), std::string::npos);
    EXPECT_EQ(one.find("id=\"leaf\""), std::string::npos);
    auto all = data("get_xml", Object{{"id", "tree"}, {"depth", 10}}).at("xml").get<std::string>();
    EXPECT_NE(all.find("    <svg:text id=\"leaf\">text</svg:text>"), std::string::npos);
    EXPECT_EQ(all.find("omitted"), std::string::npos);
    EXPECT_EQ(data("get_xml", Object{{"id", "leaf"}, {"depth", 0}}).at("xml"), Json("<svg:text id=\"leaf\">text</svg:text>\n"));
}
TEST_F(McpTools, XmlStopsAtCharacterCapWithoutSplittingUtf8)
{
    std::string text;
    for (int i = 0; i < 20000; ++i) text += "\xc3\xa9";
    load("<text id='long'>" + text + "</text>");
    auto value = data("get_xml", Object{{"id", "long"}, {"max_chars", 1000}});
    auto const &xml = value.at("xml").get<std::string>();
    EXPECT_EQ(value.at("truncated"), Json(true));
    EXPECT_EQ(g_utf8_strlen(xml.c_str(), -1), 1000);
    EXPECT_TRUE(g_utf8_validate(xml.c_str(), xml.size(), nullptr));
    EXPECT_EQ(value.at("chars"), Json(1001));
    auto full = data("get_xml", Object{{"id", "long"}, {"max_chars", 100000}});
    EXPECT_EQ(full.at("truncated"), Json(false));
    EXPECT_GT(full.at("chars").get<double>(), 20000);
    auto length = int(full.at("chars").get<double>());
    EXPECT_EQ(data("get_xml", Object{{"id", "long"}, {"max_chars", length}}).at("truncated"), Json(false));
}
TEST_F(McpTools, XmlRedactsFileAttributesAndDataPayloadsButKeepsInternalReferences)
{
    auto image = item("image")->getRepr();
    image->setAttribute("xlink:href", "file:///C:/mcp-test-fixture/private/logo.png?query#fragment");
    image->setAttribute("sodipodi:absref", "C:\\mcp-test-fixture\\private\\logo.png");
    image->setAttribute("proof:embedded", "data:image/png;base64,YWJj");
    auto xml = data("get_xml", Object{{"id", "image"}}).at("xml").get<std::string>();
    EXPECT_NE(xml.find("xlink:href=\"logo.png\""), std::string::npos);
    EXPECT_NE(xml.find("sodipodi:absref=\"logo.png\""), std::string::npos);
    EXPECT_NE(xml.find("data:image/png;base64,\xe2\x80\xa6(4 bytes)"), std::string::npos);
    EXPECT_EQ(xml.find("private"), std::string::npos);
    EXPECT_EQ(xml.find("query"), std::string::npos);
    EXPECT_EQ(xml.find("YWJj"), std::string::npos);
    auto rect = item("rect")->getRepr();
    rect->setAttribute("proof:source", "FILE:///C:/mcp-test-fixture/private/source.svg?query#fragment");
    rect->setAttribute("proof:windows", "C:\\mcp-test-fixture\\private\\windows.svg");
    rect->setAttribute("proof:unc", "\\\\mcp-test-fixture\\private\\unc.svg");
    rect->setAttribute("proof:encoded", "file:C:%2fmcp-test-fixture%5cprivate%2Fencoded.svg");
    rect->setAttribute("inkscape:export-filename", "relative/private/export.png");
    rect->setAttribute("sodipodi:docname", "relative/private/document.svg");
    rect->setAttribute("proof:data", "data:text/plain,secret");
    rect->setAttribute("proof:reference", "url(#gradient)");
    xml = data("get_xml", Object{{"id", "rect"}}).at("xml").get<std::string>();
    for (auto filename : {"source.svg", "windows.svg", "unc.svg", "encoded.svg", "export.png", "document.svg"}) {
        EXPECT_NE(xml.find(std::string("=\"") + filename + "\""), std::string::npos) << filename;
    }
    EXPECT_EQ(xml.find("private"), std::string::npos);
    EXPECT_EQ(xml.find("secret"), std::string::npos);
    EXPECT_NE(xml.find("data:text/plain,\xe2\x80\xa6(6 bytes)"), std::string::npos);
    EXPECT_NE(xml.find("url(#gradient)"), std::string::npos);
    EXPECT_NE(data("get_xml", Object{{"id", "use"}}).at("xml").get<std::string>().find("xlink:href=\"#rect\""), std::string::npos);
    load("<image id='relative' width='1' height='1' xlink:href='private/relative.png?query#fragment'/>"
         "<use id='outside' xlink:href='../private/other.svg#art'/>");
    EXPECT_NE(data("get_xml", Object{{"id", "relative"}}).at("xml").get<std::string>().find("xlink:href=\"relative.png\""), std::string::npos);
    // Any link outside the document keeps only its file name, not just a picture's.
    auto outside = data("get_xml", Object{{"id", "outside"}}).at("xml").get<std::string>();
    EXPECT_NE(outside.find("xlink:href=\"other.svg\""), std::string::npos) << outside;
    EXPECT_EQ(outside.find("private"), std::string::npos);
}
TEST_F(McpTools, XmlRefusesMissingAndDefinitionIdsAndInvalidArguments)
{
    for (auto id : {"missing", "clip", "clip-path", "symbol", "symbol-rect", "marker-path", "mask-rect", "gradient"}) {
        EXPECT_EQ(call("get_xml", Object{{"id", id}}).at("isError"), Json(true)) << id;
    }
    for (auto args : {Object{}, Object{{"id", 1}}, Object{{"id", ""}}, Object{{"id", std::string("a\0b", 3)}},
                      Object{{"id", "rect"}, {"depth", -1}}, Object{{"id", "rect"}, {"depth", 11}},
                      Object{{"id", "rect"}, {"depth", true}}, Object{{"id", "rect"}, {"depth", 1.5}},
                      Object{{"id", "rect"}, {"depth", "4"}}, Object{{"id", "rect"}, {"max_chars", 999}},
                      Object{{"id", "rect"}, {"max_chars", 100001}}, Object{{"id", "rect"}, {"max_chars", false}},
                      Object{{"id", "rect"}, {"max_chars", 1000.5}}, Object{{"id", "rect"}, {"max_chars", "20000"}},
                      Object{{"id", "rect"}, {"unknown", 0}}}) {
        EXPECT_EQ(call("get_xml", args).at("isError"), Json(true)) << serialize(args);
    }
}
} // namespace
