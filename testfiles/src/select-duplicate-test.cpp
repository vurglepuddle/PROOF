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
#include <cairomm/surface.h>

#include "display/drawing.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "preferences.h"
#include "selection-drag.h"
#include "selection.h"
#include "ui/modifiers.h"
#include "xml/node.h"
#include "xml/node-observer.h"
#include "xml/repr.h"

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

class ReprChanges : public XML::NodeObserver
{
public:
    explicit ReprChanges(XML::Node &root) : root(root) { root.addSubtreeObserver(*this); }
    ~ReprChanges() override { root.removeSubtreeObserver(*this); }
    void notifyAttributeChanged(XML::Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override { ++count; }
    void notifyChildAdded(XML::Node &, XML::Node &, XML::Node *) override { ++count; }
    void notifyChildRemoved(XML::Node &, XML::Node &, XML::Node *) override { ++count; }
    void notifyContentChanged(XML::Node &, Util::ptr_shared, Util::ptr_shared) override { ++count; }
    XML::Node &root;
    unsigned count = 0;
};

struct DragCase {
    bool press;
    bool midway;
    bool release;
    char const *name;
};

class SelectDuplicateReleaseTest : public SelectDuplicateTest, public ::testing::WithParamInterface<DragCase> {};

TEST_P(SelectDuplicateReleaseTest, ReleaseDecidesWithoutWritingDuringTheDrag)
{
    auto const state = GetParam();
    auto original = item("source");
    auto const initial = original->i2dt_affine();
    auto const before = *original->desktopGeometricBounds();
    auto const parent = original->parent;
    auto const count = parent->children.size();
    original->setAttribute("fill", "red");
    DocumentUndo::done(doc.get(), RC_("Undo", "Set fill"), "");
    auto const xml = sp_repr_save_buf(doc->getReprDoc());
    auto const serial = doc->serial();
    auto const modified = doc->isModifiedSinceSave();
    ReprChanges changes(*doc->getReprRoot());
    ObjectSet selection(doc.get());
    selection.set(original);
    SelectionDrag drag(selection);
    drag.setDuplicate(state.press);
    EXPECT_EQ(drag.isDuplicate(), state.press);
    ASSERT_EQ(drag.bounds().size(), 1u);
    auto const origin_bounds = drag.bounds().front();
    auto const move = Geom::Affine(Geom::Translate(45, 20));
    original->set_i2d_affine(initial * move);
    drag.setDuplicate(state.midway);
    EXPECT_EQ(drag.isDuplicate(), state.midway);
    drag.setDuplicate(state.release);
    EXPECT_EQ(drag.isDuplicate(), state.release);
    EXPECT_EQ(selection.singleItem(), original);
    EXPECT_EQ(parent->children.size(), count);
    EXPECT_EQ(drag.bounds().front(), origin_bounds);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    EXPECT_EQ(doc->serial(), serial);
    EXPECT_EQ(doc->isModifiedSinceSave(), modified);
    EXPECT_EQ(changes.count, 0u);

    drag.finish(move);
    auto result = selection.singleItem();
    ASSERT_TRUE(result);
    std::string const result_id = result->getId();
    EXPECT_EQ(item("source"), original);
    EXPECT_EQ(result->parent, parent);
    EXPECT_EQ(parent->children.size(), count + unsigned(state.release));
    expect_bounds(result, before * move);
    if (state.release) {
        EXPECT_NE(result, original);
        EXPECT_STRNE(result->getId(), "source");
        EXPECT_EQ(parent->lastChild(), result);
        expect_bounds(original, before);
    } else {
        EXPECT_EQ(result, original);
    }
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    expect_bounds(item("source"), before);
    EXPECT_EQ(parent->children.size(), count);
    EXPECT_STREQ(item("source")->getRepr()->attribute("fill"), "red");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(item("source")->getRepr()->attribute("fill"));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    expect_bounds(cast<SPItem>(doc->getObjectById(result_id)), before * move);
}

INSTANTIATE_TEST_SUITE_P(AltTiming, SelectDuplicateReleaseTest, ::testing::Values(
    DragCase{false, true, true, "AltDuringDragCopies"},
    DragCase{false, true, false, "AltReleasedBeforeDropMoves"},
    DragCase{true, true, true, "AltThroughoutCopies"},
    DragCase{true, false, false, "AltAtPressReleasedBeforeDropMoves"}),
    [](auto const &info) { return info.param.name; });

TEST_F(SelectDuplicateTest, RepeatedAltTogglesDoNotAccumulateCopiesOrMotion)
{
    ObjectSet selection(doc.get());
    auto original = item("source");
    auto const initial = original->i2dt_affine();
    auto const before = *original->desktopGeometricBounds();
    auto const count = original->parent->children.size();
    selection.set(original);
    SelectionDrag move(selection);
    for (int i = 0; i < 10; ++i) {
        original->set_i2d_affine(initial * Geom::Translate(i * 5, 10));
        move.setDuplicate(i % 2 == 0);
        EXPECT_EQ(original->parent->children.size(), count);
    }
    move.setDuplicate(true);
    move.finish(Geom::Translate(45, 10));
    ASSERT_EQ(original->parent->children.size(), count + 1);
    expect_bounds(original, before);
    expect_bounds(selection.singleItem(), before * Geom::Translate(45, 10));
}

TEST_F(SelectDuplicateTest, CancelKeepsThePreviousUndoAndTheOriginalSelection)
{
    for (bool duplicate : {false, true}) {
        ObjectSet selection(doc.get());
        auto original = item("source");
        auto const initial = original->i2dt_affine();
        auto const before = *original->desktopGeometricBounds();
        original->setAttribute("fill", "red");
        DocumentUndo::done(doc.get(), RC_("Undo", "Set fill"), "");
        auto const xml = sp_repr_save_buf(doc->getReprDoc());
        selection.set(original);
        SelectionDrag move(selection);
        original->set_i2d_affine(initial * Geom::Translate(45, 20));
        move.setDuplicate(duplicate);
        move.finish(Geom::Translate(45, 20), true);
        expect_bounds(original, before);
        EXPECT_EQ(selection.singleItem(), original);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        EXPECT_FALSE(item("source")->getRepr()->attribute("fill"));
    }
}

TEST_F(SelectDuplicateTest, DestroyingAnUnfinishedDragRestoresItsOriginals)
{
    ObjectSet selection(doc.get());
    auto original = item("source");
    auto const before = *original->desktopGeometricBounds();
    selection.set(original);
    {
        SelectionDrag drag(selection);
        original->set_i2d_affine(original->i2dt_affine() * Geom::Translate(30, 40));
        drag.setDuplicate(true);
    }
    expect_bounds(original, before);
    EXPECT_EQ(selection.singleItem(), original);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
}

TEST_F(SelectDuplicateTest, ConstrainedDropUsesTheSameHorizontalAndVerticalMove)
{
    for (auto delta : {Geom::Point(40, 10), Geom::Point(10, 40)}) {
        ObjectSet selection(doc.get());
        auto original = item("source");
        auto const before = *original->desktopGeometricBounds();
        selection.set(original);
        SelectionDrag drag(selection);
        auto const constrained = constrain_selection_move(delta);
        EXPECT_EQ(constrained, delta.x() > delta.y() ? Geom::Point(40, 0) : Geom::Point(0, 40));
        auto const affine = Geom::Affine(Geom::Translate(constrained));
        original->set_i2d_affine(original->i2dt_affine() * affine);
        drag.setDuplicate(true);
        drag.finish(affine);
        expect_bounds(original, before);
        expect_bounds(selection.singleItem(), before * affine);
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    }
}

TEST_F(SelectDuplicateTest, DeferredStampsAndCopyAreOneTransactionOrAllCancel)
{
    for (bool cancel : {true, false}) {
        ObjectSet selection(doc.get());
        auto original = item("source");
        auto const before = *original->desktopGeometricBounds();
        auto const count = original->parent->children.size();
        selection.set(original);
        SelectionDrag drag(selection);
        ObjectSet stamp(doc.get());
        stamp.set(original);
        stamp.duplicate(true);
        stamp.applyAffine(Geom::Translate(20, 10));
        std::string const stamp_id = stamp.singleItem()->getId();
        original->set_i2d_affine(original->i2dt_affine() * Geom::Translate(40, 20));
        drag.setDuplicate(true);
        drag.finish(Geom::Translate(40, 20), cancel);
        expect_bounds(original, before);
        if (cancel) {
            EXPECT_FALSE(doc->getObjectById(stamp_id));
            EXPECT_EQ(selection.singleItem(), original);
            EXPECT_EQ(original->parent->children.size(), count);
            EXPECT_FALSE(DocumentUndo::undo(doc.get()));
        } else {
            EXPECT_TRUE(doc->getObjectById(stamp_id));
            EXPECT_EQ(original->parent->children.size(), count + 2);
            EXPECT_NE(selection.singleItem(), original);
            ASSERT_TRUE(DocumentUndo::undo(doc.get()));
            EXPECT_FALSE(doc->getObjectById(stamp_id));
            EXPECT_EQ(original->parent->children.size(), count);
        }
    }
}

TEST_F(SelectDuplicateTest, CopyAtZeroTranslationIsStillOneUndoableDrop)
{
    ObjectSet selection(doc.get());
    auto original = item("source");
    selection.set(original);
    SelectionDrag drag(selection);
    drag.setDuplicate(true);
    drag.finish(Geom::identity());
    ASSERT_NE(selection.singleItem(), original);
    std::string const id = selection.singleItem()->getId();
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(doc->getObjectById(id));
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
}

TEST_F(SelectDuplicateTest, ReleaseCopiesAcrossLayersAndRelinksDependencies)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    prefs->setBool("/options/relinkclonesonduplicate/value", true);
    std::vector<SPItem *> originals{item("source"), item("clone"), item("path"), item("offset"), item("other")};
    std::vector<Geom::Rect> bounds;
    std::vector<Geom::Affine> initial;
    ObjectSet selection(doc.get());
    selection.setList(originals);
    SelectionDrag drag(selection);
    for (auto original : originals) {
        bounds.push_back(*original->desktopGeometricBounds());
        initial.push_back(original->i2dt_affine());
    }
    for (unsigned i = 0; i < originals.size(); ++i) originals[i]->set_i2d_affine(initial[i] * Geom::Translate(35, 25));
    drag.setDuplicate(true);
    drag.finish(Geom::Translate(35, 25));
    ASSERT_EQ(selection.size(), originals.size());
    for (unsigned i = 0; i < originals.size(); ++i) {
        EXPECT_FALSE(selection.includes(originals[i]));
        expect_bounds(originals[i], bounds[i]);
    }
    SPUse *clone = nullptr;
    SPItem *offset = nullptr;
    for (auto copy : selection.items()) {
        if (auto use = cast<SPUse>(copy)) clone = use;
        else if (copy->getRepr()->attribute("xlink:href")) offset = copy;
        EXPECT_TRUE(copy->parent == item("layer1") || copy->parent == item("layer2"));
    }
    ASSERT_TRUE(clone);
    EXPECT_TRUE(selection.includes(clone->get_original()));
    ASSERT_TRUE(offset);
    auto href = offset->getRepr()->attribute("xlink:href");
    ASSERT_TRUE(href);
    EXPECT_TRUE(selection.includes(doc->getObjectById(href + 1)));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    for (unsigned i = 0; i < originals.size(); ++i) expect_bounds(originals[i], bounds[i]);
}

TEST_F(SelectDuplicateTest, SelectionNotificationsCannotResetTheDropTransform)
{
    Selection selection(doc.get());
    auto original = item("source");
    auto const before = *original->desktopGeometricBounds();
    selection.set(original);
    SelectionDrag drag(selection);
    Geom::Affine affine = Geom::Translate(45, 20);
    original->set_i2d_affine(original->i2dt_affine() * affine);
    auto connection = selection.connectChanged([&](Selection *) { affine.setIdentity(); });
    drag.setDuplicate(true);
    drag.finish(affine);
    connection.disconnect();
    EXPECT_TRUE(affine.isIdentity());
    expect_bounds(original, before);
    expect_bounds(selection.singleItem(), before * Geom::Translate(45, 20));
}

class Display
{
public:
    explicit Display(SPDocument &doc) : root(doc.getRoot()), key(SPItem::display_key_new(1))
    {
        drawing.setRoot(root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY));
        drawing.setExact();
    }
    ~Display() { root->invoke_hide(key); }

    std::vector<unsigned char> pixels()
    {
        drawing.update(Geom::IntRect::infinite());
        auto area = Geom::IntRect::from_xywh(0, 0, 400, 300);
        auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, area.width(), area.height());
        auto ds = DrawingSurface(surface->cobj(), area.min());
        auto dc = DrawingContext(ds);
        drawing.render(dc, area);
        surface->flush();
        auto data = surface->get_data();
        return {data, data + surface->get_stride() * surface->get_height()};
    }

    Drawing drawing;
    SPRoot *root;
    unsigned key;
};

class SelectDuplicatePreviewTest : public SelectDuplicateTest, public ::testing::WithParamInterface<int> {};

TEST_P(SelectDuplicatePreviewTest, CompletionClearsStrokePreviewFromOriginalsAndCopies)
{
    item("source")->setAttribute("style", "fill:none;stroke:#000;stroke-width:6;stroke-dasharray:4,2");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), RC_("Undo", "Set stroke"), "");
    Display display(*doc);
    auto const before = display.pixels();
    ObjectSet selection(doc.get());
    auto original = item("source");
    selection.set(original);
    SelectionDrag drag(selection, display.key);
    auto const affine = Geom::Affine(Geom::Translate(45, 20));
    original->set_i2d_affine(original->i2dt_affine() * affine);
    selection.setTransformPreview(affine, display.key);
    // A deliberately different override makes a forgotten cleanup visible in the renderer.
    auto stroke = original->getStrokeTransform(Geom::Scale(3));
    original->setTransformPreview(display.key, stroke);
    EXPECT_NE(display.pixels(), before);
    drag.setDuplicate(true);
    drag.setDuplicate(false);
    drag.setDuplicate(GetParam() == 1);
    drag.finish(affine, GetParam() == -1);
    doc->ensureUpToDate();
    if (GetParam() == -1) {
        EXPECT_EQ(display.pixels(), before);
    } else {
        auto reopened = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
        ASSERT_TRUE(reopened);
        reopened->ensureUpToDate();
        Display clean(*reopened);
        EXPECT_EQ(display.pixels(), clean.pixels());
    }
}

INSTANTIATE_TEST_SUITE_P(DropOrCancel, SelectDuplicatePreviewTest, ::testing::Values(-1, 0, 1),
    [](auto const &info) { return info.param == -1 ? "Cancel" : info.param == 0 ? "Move" : "Copy"; });

} // namespace
