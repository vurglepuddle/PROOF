// SPDX-License-Identifier: GPL-2.0-or-later
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <libxml/parser.h>
#include <libxml/xinclude.h>
#include <2geom/transforms.h>

#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-use.h"
#include "preferences.h"
#include "ui/modifiers.h"
#include "xml/node.h"

using namespace Inkscape;
using namespace Inkscape::Modifiers;

namespace {

using Bindings = std::map<std::string, std::string>;

void collect_bindings(xmlNode *node, Bindings &bindings)
{
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST "modifier")) {
            auto action = xmlGetProp(node, BAD_CAST "action");
            auto keys = xmlGetProp(node, BAD_CAST "modifiers");
            auto disabled = xmlGetProp(node, BAD_CAST "disabled");
            if (action) {
                bindings[reinterpret_cast<char const *>(action)] =
                    disabled && xmlStrEqual(disabled, BAD_CAST "true") ? "disabled" :
                    keys ? reinterpret_cast<char const *>(keys) : "";
            }
            xmlFree(action);
            xmlFree(keys);
            xmlFree(disabled);
        }
        collect_bindings(node->children, bindings);
    }
}

Bindings read_bindings(char const *file)
{
    auto path = std::string(INKSCAPE_SHARE_DIR) + "/keys/" + file;
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(xmlReadFile(path.c_str(), nullptr, XML_PARSE_NONET), xmlFreeDoc);
    EXPECT_TRUE(doc);
    Bindings bindings;
    if (doc) {
        EXPECT_GE(xmlXIncludeProcessFlags(doc.get(), XML_PARSE_NONET), 0);
        collect_bindings(xmlDocGetRootElement(doc.get()), bindings);
    }
    return bindings;
}

TEST(SelectDuplicateModifiers, ShippedDefaultUsesAltAndShift)
{
    auto const bindings = read_bindings("default.xml");
    ASSERT_EQ(bindings.count("select-duplicate"), 1u);
    EXPECT_EQ(bindings.at("select-duplicate"), "Alt");
    EXPECT_EQ(bindings.at("select-touch-path"), "Alt");
    EXPECT_EQ(bindings.at("select-force-drag"), "Alt");
    EXPECT_EQ(bindings.at("move-confine"), "Shift");
    EXPECT_EQ(bindings.at("move-snapping"), "Ctrl");
}

TEST(SelectDuplicateModifiers, InkscapeProfileKeepsItsExistingMapping)
{
    auto const bindings = read_bindings("inkscape.xml");
    EXPECT_EQ(bindings.count("select-duplicate"), 0u);
    EXPECT_EQ(bindings.at("select-force-drag"), "Alt");
    EXPECT_EQ(bindings.at("move-confine"), "Ctrl");
    auto duplicate = *Modifier::get(Type::SELECT_DUPLICATE);
    duplicate.unset_user();
    duplicate.unset_keys();
    EXPECT_FALSE(duplicate.active(ALT));
}

TEST(SelectDuplicateModifiers, DuplicateCombinesWithConfinementAndRespectsOverrides)
{
    auto duplicate = *Modifier::get("select-duplicate");
    auto confine = *Modifier::get("move-confine");
    duplicate.unset_user();
    confine.unset_user();
    duplicate.set_keys(ALT, NOT_SET);
    confine.set_keys(SHIFT, NOT_SET);
    EXPECT_TRUE(duplicate.active(ALT));
    EXPECT_TRUE(duplicate.active(ALT | SHIFT | GDK_BUTTON1_MASK));
    EXPECT_TRUE(confine.active(ALT | SHIFT | GDK_BUTTON1_MASK));
    EXPECT_FALSE(duplicate.active(SHIFT));
    EXPECT_FALSE(duplicate.active(CTRL));
    duplicate.set_user(NEVER, NOT_SET);
    EXPECT_FALSE(duplicate.active(ALT | SHIFT));
    duplicate.set_user(SUPER, CTRL);
    EXPECT_FALSE(duplicate.active(ALT));
    EXPECT_TRUE(duplicate.active(SUPER | SHIFT));
    EXPECT_FALSE(duplicate.active(SUPER | CTRL));
}

constexpr std::string_view SVG = R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
    xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
    xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
    xmlns:xlink="http://www.w3.org/1999/xlink" width="400" height="300" viewBox="0 0 400 300">
  <g id="layer1" inkscape:groupmode="layer" transform="translate(12,18)">
    <rect id="source" x="10" y="20" width="30" height="40" transform="rotate(30)"/>
    <use id="clone" xlink:href="#source" transform="translate(80,0)"/>
    <g id="group" transform="scale(1.5)"><rect id="child" width="12" height="8"/></g>
    <path id="path" d="M 0,0 H 20 V 20 H 0 Z"/>
    <path id="offset" sodipodi:type="inkscape:offset" inkscape:radius="2"
      inkscape:original="M 0,0 H 20 V 20 H 0 Z" xlink:href="#path" d="M -2,-2 H 22 V 22 H -2 Z"/>
  </g>
  <g id="layer2" inkscape:groupmode="layer" transform="matrix(1.2,0.2,-0.1,0.8,10,70)">
    <rect id="other" x="80" y="30" width="20" height="15"/>
  </g>
</svg>)SVG";

class SelectDuplicateTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
        doc = SPDocument::createNewDocFromMem(SVG);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        DocumentUndo::setUndoSensitive(doc.get(), true);
        DocumentUndo::clearUndo(doc.get());
        DocumentUndo::clearRedo(doc.get());
    }

    SPItem *item(char const *id) { return cast<SPItem>(doc->getObjectById(id)); }

    void expect_bounds(SPItem *object, Geom::Rect const &expected)
    {
        auto const bounds = object->desktopGeometricBounds();
        ASSERT_TRUE(bounds);
        for (auto dim : {Geom::X, Geom::Y}) {
            EXPECT_NEAR(bounds->min()[dim], expected.min()[dim], 1e-6);
            EXPECT_NEAR(bounds->max()[dim], expected.max()[dim], 1e-6);
        }
    }

    void finish_move()
    {
        DocumentUndo::done(doc.get(), RC_("Undo", "Move"), "tool-pointer");
        DocumentUndo::done(doc.get(), RC_("Undo", "Duplicate"), "edit-duplicate");
    }

    std::unique_ptr<SPDocument> doc;
};

TEST_F(SelectDuplicateTest, MovesOnlyTheCopyOfATransformedObject)
{
    ObjectSet selection(doc.get());
    auto original = item("source");
    auto const before = *original->desktopGeometricBounds();
    auto const transform = original->transform;
    auto const id = std::string(original->getId());
    selection.set(original);
    selection.duplicate(true);
    doc->ensureUpToDate();
    auto copy = selection.singleItem();
    ASSERT_TRUE(copy);
    EXPECT_NE(copy, original);
    EXPECT_STRNE(copy->getId(), id.c_str());
    EXPECT_EQ(copy->parent, original->parent);
    selection.applyAffine(Geom::Translate(40, -15));
    finish_move();
    expect_bounds(original, before);
    EXPECT_EQ(original->transform, transform);
    expect_bounds(copy, before * Geom::Translate(40, -15));
    EXPECT_EQ(selection.singleItem(), copy);
}

TEST_F(SelectDuplicateTest, MultipleCopiesKeepTheirLayersAndStackingOrder)
{
    ObjectSet selection(doc.get());
    auto first = item("source");
    auto second = item("other");
    auto const first_bounds = *first->desktopGeometricBounds();
    auto const second_bounds = *second->desktopGeometricBounds();
    selection.setList(std::vector<SPItem *>{first, second});
    selection.duplicate(true);
    doc->ensureUpToDate();
    selection.applyAffine(Geom::Translate(25, 30));
    finish_move();
    ASSERT_EQ(selection.size(), 2u);
    EXPECT_FALSE(selection.includes(first));
    EXPECT_FALSE(selection.includes(second));
    for (auto copy : selection.items()) {
        EXPECT_EQ(copy->parent->lastChild(), copy);
        expect_bounds(copy, (copy->parent == first->parent ? first_bounds : second_bounds) * Geom::Translate(25, 30));
    }
    expect_bounds(first, first_bounds);
    expect_bounds(second, second_bounds);
}

TEST_F(SelectDuplicateTest, DuplicatedGroupKeepsChildrenWithFreshIds)
{
    ObjectSet selection(doc.get());
    auto original = item("group");
    selection.set(original);
    selection.duplicate(true);
    auto copy = selection.singleItem();
    ASSERT_TRUE(copy);
    ASSERT_TRUE(copy->firstChild());
    EXPECT_NE(copy, original);
    EXPECT_STRNE(copy->getId(), original->getId());
    EXPECT_STRNE(copy->firstChild()->getId(), item("child")->getId());
    EXPECT_EQ(copy->transform, original->transform);
}

TEST_F(SelectDuplicateTest, CloneAloneKeepsItsOriginalLink)
{
    ObjectSet selection(doc.get());
    selection.set(item("clone"));
    selection.duplicate(true);
    auto copy = cast<SPUse>(selection.singleItem());
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->get_original(), item("source"));
    EXPECT_STRNE(copy->getId(), item("clone")->getId());
    auto const source_bounds = *item("source")->desktopGeometricBounds();
    auto const clone_bounds = *item("clone")->desktopGeometricBounds();
    selection.applyAffine(Geom::Translate(20, 10));
    finish_move();
    expect_bounds(item("source"), source_bounds);
    expect_bounds(item("clone"), clone_bounds);
    expect_bounds(copy, clone_bounds * Geom::Translate(20, 10));
}

TEST_F(SelectDuplicateTest, CloneRelinkingFollowsEditDuplicatePreference)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    for (bool relink : {false, true}) {
        prefs->setBool("/options/relinkclonesonduplicate/value", relink);
        ObjectSet selection(doc.get());
        selection.setList(std::vector<SPItem *>{item("source"), item("clone")});
        selection.duplicate(true);
        auto copies = selection.items_vector();
        ASSERT_EQ(copies.size(), 2u);
        auto clone = cast<SPUse>(copies[0]);
        auto source = copies[1];
        if (!clone) {
            clone = cast<SPUse>(copies[1]);
            source = copies[0];
        }
        ASSERT_TRUE(clone);
        EXPECT_EQ(clone->get_original(), relink ? source : item("source"));
    }
}

TEST_F(SelectDuplicateTest, LinkedOffsetsRelinkToTheCopiedSource)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    prefs->setBool("/options/relinkclonesonduplicate/value", true);
    ObjectSet selection(doc.get());
    selection.setList(std::vector<SPItem *>{item("path"), item("offset")});
    selection.duplicate(true);
    auto copies = selection.items_vector();
    ASSERT_EQ(copies.size(), 2u);
    auto offset = copies[0];
    auto source = copies[1];
    if (!offset->getRepr()->attribute("xlink:href")) {
        std::swap(offset, source);
    }
    EXPECT_EQ(std::string(offset->getRepr()->attribute("xlink:href")), std::string("#") + source->getId());
    EXPECT_STREQ(item("offset")->getRepr()->attribute("xlink:href"), "#path");
}

TEST_F(SelectDuplicateTest, OneUndoRemovesTheWholeDuplicateAndMove)
{
    ObjectSet selection(doc.get());
    auto original = item("source");
    auto const before = *original->desktopGeometricBounds();
    original->setAttribute("fill", "red");
    DocumentUndo::done(doc.get(), RC_("Undo", "Set fill"), "");
    selection.set(original);
    selection.duplicate(true);
    std::string const copy_id = selection.singleItem()->getId();
    selection.applyAffine(Geom::Translate(60, 25));
    finish_move();
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(doc->getObjectById(copy_id));
    EXPECT_STREQ(item("source")->getRepr()->attribute("fill"), "red");
    expect_bounds(item("source"), before);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(item("source")->getRepr()->attribute("fill"));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    auto copy = cast<SPItem>(doc->getObjectById(copy_id));
    ASSERT_TRUE(copy);
    expect_bounds(copy, before * Geom::Translate(60, 25));
}

TEST_F(SelectDuplicateTest, ReturnToOriginStillCommitsOneUndoableCopy)
{
    ObjectSet selection(doc.get());
    selection.set(item("source"));
    selection.duplicate(true);
    std::string const copy_id = selection.singleItem()->getId();
    // SelTrans::ungrab() skips the Move commit for an identity transform.
    DocumentUndo::done(doc.get(), RC_("Undo", "Duplicate"), "edit-duplicate");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(doc->getObjectById(copy_id));
    EXPECT_TRUE(item("source"));
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_TRUE(doc->getObjectById(copy_id));
}

} // namespace
