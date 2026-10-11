// SPDX-License-Identifier: GPL-2.0-or-later

#include <glibmm/i18n.h>
#include <gtest/gtest.h>
#include <src/document.h>
#include <src/object/sp-item.h>
#include <src/object/sp-object.h>

#include "document-undo.h"
#include "geom-predicates.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-root.h"
#include "style.h"
#include "util/units.h"

using namespace Inkscape;
using namespace Inkscape::XML;

static std::string const TEST_DIR = INKSCAPE_TESTS_DIR "/data/doc_import/";

struct ImportFileInfo
{
    std::string file;
    Geom::Rect rect_bounds;
    friend void PrintTo(ImportFileInfo const &obj, std::ostream *os) { *os << obj.file; }
};
static ImportFileInfo const ImportTestFiles1[] = {
    {"p210x297_1mm_tl.svg", Geom::Rect(30, 11, 40, 31)},
    {"p200x287_5mm_tl.svg", Geom::Rect(33, 34, 45, 56)},
    {"p190x277_1mm_bl.svg", Geom::Rect(34, 2, 49, 28)},
    {"p180x267_5mm_bl.svg", Geom::Rect(37, 17, 58, 44)},
    {"p160x247_6mm_tl_shift.svg", Geom::Rect(3, 4, 44, 46)},
    {"p150x237_8mm_bl_shift.svg", Geom::Rect(42, 156, 81, 194)},
};

// Opening a file stamps it with this version. A commit that changes nothing must then find
// nothing to record: otherwise choosing default paints with nothing selected marks a document
// just opened as changed, and closing it asks to save.
TEST(SPDocumentOpening, WhatOpeningDoesIsNotAnEdit)
{
    Application::create(false);
    static char const svg[] =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect width="5" height="5"/></svg>)";
    auto doc = SPDocument::createNewDocFromMem(svg, "");
    ASSERT_TRUE(doc);
    ASSERT_NE(doc->getReprRoot()->attribute("inkscape:version"), nullptr);

    DocumentUndo::done(doc.get(), RC_("Undo", "Nothing"), "");
    EXPECT_FALSE(doc->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    EXPECT_NE(doc->getReprRoot()->attribute("inkscape:version"), nullptr);
}

class SPDocumentTestFilePair : public ::testing::TestWithParam<std::tuple<ImportFileInfo, ImportFileInfo>>
{
public:
    void SetUp() override
    {
        Application::create(false);

        auto target_file = std::get<0>(GetParam());
        target_doc = SPDocument::createNewDoc((TEST_DIR + target_file.file).c_str());
        ASSERT_NE(target_doc.get(), nullptr);
        source_file = std::get<1>(GetParam());
        source_doc = SPDocument::createNewDoc((TEST_DIR + source_file.file).c_str());
        ASSERT_NE(source_doc.get(), nullptr);
    }
    std::unique_ptr<SPDocument> target_doc;
    ImportFileInfo source_file;
    std::unique_ptr<SPDocument> source_doc;
};
class SPDocumentTestRectPairs : public SPDocumentTestFilePair
{
};
TEST_P(SPDocumentTestRectPairs, ImportTransform)
{
    // Test that in every combination of source/target the source physical size is maintained
    auto mm = Util::UnitTable::get().getUnit(SVGLength::MM);
    ASSERT_NE(mm, nullptr);

    this->target_doc->getRoot()->lastChild()->deleteObject();

    target_doc->import(*source_doc, nullptr, nullptr, Geom::Affine());

    auto rect = target_doc->getObjectsBySelector("rect");
    EXPECT_EQ(rect.size(), 1);
    auto item = cast<SPItem>(rect[0]);
    auto bounds = item->documentGeometricBounds();
    auto bounds_expected = source_file.rect_bounds * Geom::Scale(mm->factor);
    EXPECT_RECT_NEAR(bounds.value(), bounds_expected, 0.001);
}

TEST_P(SPDocumentTestRectPairs, ImportAdditionalTransform)
{
    // Test that extra transformation is applied in correct coordinate system (correct transformation center and
    // translation units)
    auto mm = Util::UnitTable::get().getUnit(SVGLength::MM);
    ASSERT_NE(mm, nullptr);

    target_doc->getRoot()->lastChild()->deleteObject();

    auto transform =
        Geom::Rotate(Geom::Angle::from_degrees(-90)) * Geom::Translate(mm->convert(1, "px"), mm->convert(2, "px"));
    target_doc->import(*source_doc, nullptr, nullptr, transform);

    auto rect = target_doc->getObjectsBySelector("rect");
    EXPECT_EQ(rect.size(), 1);

    auto item = cast<SPItem>(rect[0]);
    auto bounds = item->documentGeometricBounds().value() * Geom::Scale(1 / mm->factor);
    auto bounds_expected = source_file.rect_bounds;
    EXPECT_NEAR(bounds.left(), bounds_expected.top() + 1, 0.001);
    EXPECT_NEAR(bounds.bottom(), -bounds_expected.left() + 2, 0.001);
}

INSTANTIATE_TEST_SUITE_P(SPDocumentTestImport1, SPDocumentTestRectPairs,
                         testing::Combine(testing::ValuesIn(ImportTestFiles1), testing::ValuesIn(ImportTestFiles1)));

class SPDocumentTestImportTransform : public SPDocumentTestFilePair
{
};
INSTANTIATE_TEST_SUITE_P(
    SPDocumentTest, SPDocumentTestImportTransform,
    testing::Combine(testing::Values(ImportFileInfo{"import_target_parents.svg"}),
                     testing::Values(ImportFileInfo{"p160x247_6mm_tl_shift.svg", Geom::Rect(3, 4, 44, 46)},
                                     ImportFileInfo{"p150x237_8mm_bl_shift.svg", Geom::Rect(42, 156, 81, 194)})));
TEST_P(SPDocumentTestImportTransform, ImportParentTransform)
{
    // make sure parent transform is canceled out by default
    auto mm = Util::UnitTable::get().getUnit(SVGLength::MM);
    ASSERT_NE(mm, nullptr);

    std::vector<SPItem *> parents;
    parents.push_back(target_doc->getRoot());
    {
        auto parent = target_doc->getObjectById("parent1");
        parents.push_back(cast<SPItem>(parent));
        parent = target_doc->getObjectById("parent2_b");
        parents.push_back(cast<SPItem>(parent));
    }

    for (auto parent : parents) {
        ASSERT_NE(parent, nullptr);
        Geom::Point translation(mm->convert(1, "px"), mm->convert(2, "px"));
        auto transform = Geom::Translate(translation);
        std::vector<Inkscape::XML::Node *> result;
        target_doc->import(*source_doc, parent->getRepr(), nullptr, transform, &result);

        EXPECT_FALSE(result.empty());
        for (auto item : result) {
            EXPECT_EQ(item->parent(), parent->getRepr());
        }
        target_doc->ensureUpToDate();
        Inkscape::ObjectSet imported_items(target_doc.get());
        imported_items.setReprList(result);
        target_doc->ensureUpToDate();

        auto maybe_bounds = imported_items.documentBounds(SPItem::GEOMETRIC_BBOX);
        ASSERT_TRUE(maybe_bounds.has_value()) << parent->getId();
        auto bounds = maybe_bounds.value() * Geom::Scale(1 / mm->factor);
        auto bounds_expected = source_file.rect_bounds;
        bounds_expected += Geom::Point(1, 2);
        EXPECT_RECT_NEAR(bounds, bounds_expected, 0.001);
    }
}

class SPDocumentTestSVGPair : public ::testing::Test
{
public:
    void Init(char const *target, char const *source)
    {
        target_doc = SPDocument::createNewDoc((TEST_DIR + target).c_str());
        ASSERT_NE(target_doc.get(), nullptr) << target;
        source_doc = SPDocument::createNewDoc((TEST_DIR + source).c_str());
        ASSERT_NE(source_doc.get(), nullptr) << source;
    }
    bool InitCheck(char const *target, char const *source)
    {
        Init(target, source);
        return !HasFatalFailure();
    }
    void SetUp() override { Application::create(false); }
    std::unique_ptr<SPDocument> target_doc;
    std::unique_ptr<SPDocument> source_doc;
};

char const *const IMPORT_GROUP_TESTS[] = {"multi_content_single.svg", "multi_content_4.svg", "multi_content_groups.svg",
                                          "multi_content_group_single.svg"};

class SPDocumentTestSVGPairGroupFixture
    : public SPDocumentTestSVGPair
    , public testing::WithParamInterface<char const *>
{
public:
    void SetUp() override
    {
        Application::create(false);
        Init("import_target_parents.svg", GetParam());
    }
};
INSTANTIATE_TEST_SUITE_P(SPDocumentTest, SPDocumentTestSVGPairGroupFixture, testing::ValuesIn(IMPORT_GROUP_TESTS));

TEST_P(SPDocumentTestSVGPairGroupFixture, ImportParentAlwaysGroup)
{
    std::vector<Node *> result;
    target_doc->import(*source_doc, nullptr, nullptr, Geom::Affine(), &result, SPDocument::ImportRoot::AlwaysGroup);
    ASSERT_EQ(result.size(), 1);
    EXPECT_STREQ(result[0]->name(), "svg:g");
    auto id = result[0]->attribute("id");
    EXPECT_STRNE(id, "g5");
    EXPECT_STRNE(id, "g4");
}

TEST_P(SPDocumentTestSVGPairGroupFixture, ImportParentRootSingle)
{
    std::vector<Node *> result;
    target_doc->import(*source_doc, nullptr, nullptr, Geom::Affine(), &result, SPDocument::ImportRoot::Single);
    ASSERT_EQ(result.size(), 1);
}

TEST_F(SPDocumentTestSVGPair, ImportParentGroupUngroup)
{
    struct Case
    {
        char const *source;
        size_t count;
        char const *tag;
        char const *id;
    } cases[] = {
        {"multi_content_single.svg", 1, "svg:rect", "rect1"},
        {"multi_content_groups.svg", 1, "svg:g", "g4"},
        {"multi_content_group_single.svg", 1, "svg:rect", "rect1"},
        {"multi_content_4.svg", 1, "svg:g", nullptr},
    };
    for (auto &subcase : cases) {
        std::vector<Node *> result;
        if (!InitCheck("import_target_parents.svg", subcase.source)) {
            return;
        }
        target_doc->import(*source_doc, nullptr, nullptr, Geom::Affine(), &result,
                           SPDocument::ImportRoot::UngroupSingle);
        ASSERT_EQ(result.size(), subcase.count) << subcase.source;
        if (subcase.count > 1) {
            continue;
        }
        EXPECT_STREQ(result[0]->name(), subcase.tag) << subcase.source;
        if (subcase.id) {
            EXPECT_STREQ(result[0]->attribute("id"), subcase.id) << subcase.source;
        }
    }
}

TEST_F(SPDocumentTestSVGPair, ImportParentGroupWhenNeeded)
{
    // While not strictly required to create a parent group to handle root svg style, doing it that way produces simpler
    // svg structure closer to original file.
    // Regardless of whether parent group was added resolved style should include the property from input svg root.
    struct Case
    {
        char const *source;
        int expectedResultsize;
    } cases[] = {
        {"multi_content_4.svg", 4},
        {"multi_content_4_root_style.svg", 1},
    };
    for (auto &subcase : cases) {
        std::vector<Node *> result;
        if (!InitCheck("import_target_parents.svg", subcase.source)) {
            return;
        }
        target_doc->import(*source_doc, nullptr, nullptr, Geom::Affine(), &result, SPDocument::ImportRoot::WhenNeeded);
        ASSERT_EQ(result.size(), subcase.expectedResultsize) << subcase.source;
        if (subcase.expectedResultsize > 1) {
            continue;
        }
        auto rect = target_doc->getObjectById("rect1");
        auto style = rect->style;
        ASSERT_NE(style, nullptr);
        EXPECT_EQ(style->fill.getColor(), Colors::Color(0xff000000, false));
    }
}
