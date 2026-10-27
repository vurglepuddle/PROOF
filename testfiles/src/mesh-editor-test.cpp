// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Mesh previews follow their own resource without rebuilding the list.
 * GTK cases use INKSCAPE_TEST_GUI=1 and dispatch queued frames without opening windows.
 */

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <glibmm/main.h>
#include <gtest/gtest.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/init.h>

#include "document.h"
#include "extension/init.h"
#include "inkscape.h"
#include "object/sp-defs.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-stop.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/mesh-editor.h"
#include "xml/repr.h"

namespace Inkscape::UI::Widget {
namespace {

std::unique_ptr<SPDocument> mesh_document()
{
    std::string const svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50">
      <defs>
        <meshgradient id="first" x="0" y="0" gradientUnits="userSpaceOnUse"><meshrow><meshpatch>
          <stop id="first-stop" path="l 40,0" style="stop-color:#ff0000"/>
          <stop path="l 0,40" style="stop-color:#00ff00"/>
          <stop path="l -40,0" style="stop-color:#0000ff"/>
          <stop path="l 0,-40" style="stop-color:#ffffff"/>
        </meshpatch></meshrow></meshgradient>
        <meshgradient id="second" x="50" y="0" gradientUnits="userSpaceOnUse"><meshrow><meshpatch>
          <stop id="second-stop" path="l 40,0" style="stop-color:#00ffff"/>
          <stop path="l 0,40" style="stop-color:#ff00ff"/>
          <stop path="l -40,0" style="stop-color:#ffff00"/>
          <stop path="l 0,-40" style="stop-color:#000000"/>
        </meshpatch></meshrow></meshgradient>
      </defs>
      <rect width="40" height="40" fill="url(#first)"/>
      <rect x="50" width="40" height="40" fill="url(#second)"/>
    </svg>)svg";
    auto document = SPDocument::createNewDocFromMem(svg);
    if (document) document->ensureUpToDate();
    return document;
}

void change_stop(SPDocument &document, char const *color)
{
    auto stop = document.getObjectById("first-stop");
    ASSERT_TRUE(stop);
    stop->getRepr()->setAttribute("style", (std::string("stop-color:") + color).c_str());
    document.ensureUpToDate();
}

std::vector<unsigned char> pixels(Glib::RefPtr<Gdk::Texture> const &image)
{
    std::vector<unsigned char> data(image->get_width() * image->get_height() * 4);
    image->download(data.data(), image->get_width() * 4);
    return data;
}

void pump()
{
    auto context = Glib::MainContext::get_default();
    for (int i = 0; i < 500 && context->pending(); ++i) context->iteration(false);
}

class MeshPreviewDocumentTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
        document = mesh_document();
        ASSERT_TRUE(document);
        first = cast<SPMeshGradient>(document->getObjectById("first"));
        second = cast<SPMeshGradient>(document->getObjectById("second"));
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
    }

    std::unique_ptr<SPDocument> document;
    SPMeshGradient *first = nullptr;
    SPMeshGradient *second = nullptr;
};

TEST_F(MeshPreviewDocumentTest, StopChangeNotifiesItsMeshAndDefsSeparately)
{
    unsigned first_flags = 0, second_flags = 0, defs_flags = 0;
    sigc::scoped_connection a = first->connectModified([&](SPObject *object, unsigned flags) {
        EXPECT_EQ(object, first);
        first_flags |= flags;
    });
    sigc::scoped_connection b = second->connectModified([&](SPObject*, unsigned flags) { second_flags |= flags; });
    sigc::scoped_connection defs = document->getDefs()->connectModified([&](SPObject *object, unsigned flags) {
        EXPECT_EQ(object, document->getDefs());
        EXPECT_FALSE(cast<SPMeshGradient>(object));
        defs_flags |= flags;
    });

    change_stop(*document, "#0000ff");
    EXPECT_TRUE(first_flags & SP_OBJECT_CHILD_MODIFIED_FLAG);
    EXPECT_EQ(second_flags, 0u);
    EXPECT_TRUE(defs_flags & SP_OBJECT_CHILD_MODIFIED_FLAG);
}

TEST_F(MeshPreviewDocumentTest, GeometryAndRootChangesNotifyOnlyTheirMesh)
{
    unsigned first_flags = 0, second_flags = 0;
    sigc::scoped_connection a = first->connectModified([&](SPObject*, unsigned flags) { first_flags |= flags; });
    sigc::scoped_connection b = second->connectModified([&](SPObject*, unsigned flags) { second_flags |= flags; });

    document->getObjectById("first-stop")->getRepr()->setAttribute("path", "C 10,-20 30,-20 40,0");
    document->ensureUpToDate();
    EXPECT_TRUE(first_flags & SP_OBJECT_CHILD_MODIFIED_FLAG);
    EXPECT_EQ(second_flags, 0u);
    first_flags = 0;
    first->getRepr()->setAttribute("x", "5");
    document->ensureUpToDate();
    EXPECT_TRUE(first_flags & SP_OBJECT_MODIFIED_FLAG);
    EXPECT_EQ(second_flags, 0u);
}

} // namespace

class MeshEditorTest : public MeshPreviewDocumentTest
{
protected:
    void SetUp() override
    {
        auto gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GTK widget test: set INKSCAPE_TEST_GUI=1 (no windows are opened)";
        }
        if (!Application::exists()) Application::create(false);
        static bool const initialized = [] {
            gtk_init();
            Gtk::init_gtkmm_internals();
            register_all();
            Extension::init();
            return true;
        }();
        (void)initialized;
        MeshPreviewDocumentTest::SetUp();
    }

    // A real frame clock needs a mapped native surface. Dispatch the queued work explicitly
    // so these tests exercise the tick's update path without showing a window.
    static bool frame(MeshEditor &editor)
    {
        if (!editor._tick_callback) return false;
        editor.remove_tick_callback(editor._tick_callback);
        editor._tick_callback = 0;
        editor.update();
        return true;
    }

    static auto store(MeshEditor &editor) { return editor._store; }
    static auto selection(MeshEditor &editor) { return editor._selection_model; }
    static auto scroll(MeshEditor &editor) { return editor._scroll.get_vadjustment(); }
    static auto pending(MeshEditor &editor) { return editor._tick_callback; }
    static auto dirty(MeshEditor &editor) { return editor._dirty_meshes.size(); }
    static auto connections(MeshEditor &editor) { return editor._mesh_connections.size(); }
};

TEST_F(MeshEditorTest, ChangedMeshAloneRefreshesAndKeepsSelectionAndScroll)
{
    MeshEditor editor;
    unsigned selection_changes = 0, model_changes = 0;
    editor.signal_changed().connect([&](auto) { ++selection_changes; });
    editor.set_document(document.get());
    editor.select_mesh(second);
    ASSERT_TRUE(frame(editor));
    ASSERT_EQ(editor.get_selected_mesh(), second);
    auto list = store(editor);
    auto row0 = list->get_object(0), row1 = list->get_object(1);
    sigc::scoped_connection changes = list->signal_items_changed().connect([&](auto, auto, auto) { ++model_changes; });
    scroll(editor)->configure(75, 0, 400, 1, 10, 100);
    auto old_first = editor.get_preview(first), old_second = editor.get_preview(second);
    ASSERT_TRUE(old_first);
    ASSERT_TRUE(old_second);
    auto before_pixels = pixels(old_first);

    change_stop(*document, "#0000ff");
    auto xml = sp_repr_save_buf(document->getReprDoc());
    EXPECT_EQ(editor.get_preview(first), old_first);
    ASSERT_TRUE(frame(editor));
    EXPECT_NE(editor.get_preview(first), old_first);
    EXPECT_NE(pixels(editor.get_preview(first)), before_pixels);
    EXPECT_EQ(editor.get_preview(second), old_second);
    EXPECT_EQ(store(editor), list);
    EXPECT_EQ(list->get_object(0), row0);
    EXPECT_EQ(list->get_object(1), row1);
    EXPECT_EQ(model_changes, 0u);
    EXPECT_EQ(selection_changes, 0u);
    EXPECT_EQ(editor.get_selected_mesh(), second);
    EXPECT_DOUBLE_EQ(scroll(editor)->get_value(), 75);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
}

TEST_F(MeshEditorTest, TwoListsRefreshTheirOwnChangedPreview)
{
    MeshEditor dock, sidebar;
    dock.set_document(document.get());
    ASSERT_TRUE(frame(dock));
    change_stop(*document, "#0000ff");
    sidebar.set_document(document.get());
    ASSERT_TRUE(frame(sidebar));
    auto old_dock = dock.get_preview(first), old_sidebar = sidebar.get_preview(first);
    EXPECT_NE(pixels(old_dock), pixels(old_sidebar));
    ASSERT_TRUE(frame(dock));
    EXPECT_EQ(pixels(dock.get_preview(first)), pixels(old_sidebar));
    auto dock_other = dock.get_preview(second), sidebar_other = sidebar.get_preview(second);

    change_stop(*document, "#ffffff");
    ASSERT_TRUE(frame(dock));
    ASSERT_TRUE(frame(sidebar));
    EXPECT_NE(sidebar.get_preview(first), old_sidebar);
    EXPECT_EQ(pixels(dock.get_preview(first)), pixels(sidebar.get_preview(first)));
    EXPECT_EQ(dock.get_preview(second), dock_other);
    EXPECT_EQ(sidebar.get_preview(second), sidebar_other);
}

TEST_F(MeshEditorTest, BurstCoalescesIntoOneQueuedFrame)
{
    MeshEditor editor;
    editor.set_document(document.get());
    ASSERT_TRUE(frame(editor));
    auto image = editor.get_preview(first);
    change_stop(*document, "#0000ff");
    auto tick = pending(editor);
    ASSERT_NE(tick, 0u);
    for (auto color : {"#00ffff", "#ff00ff", "#ffffff"}) {
        change_stop(*document, color);
        EXPECT_EQ(pending(editor), tick);
        EXPECT_EQ(dirty(editor), 1u);
        EXPECT_EQ(editor.get_preview(first), image);
    }
    ASSERT_TRUE(frame(editor));
    EXPECT_NE(editor.get_preview(first), image);
    EXPECT_EQ(dirty(editor), 0u);
    EXPECT_FALSE(frame(editor));
}

TEST_F(MeshEditorTest, HiddenChangesWaitForTheNextFrame)
{
    MeshEditor editor;
    editor.set_document(document.get());
    EXPECT_FALSE(editor.get_mapped());
    pump();
    EXPECT_FALSE(editor.get_preview(first));
    change_stop(*document, "#0000ff");
    ASSERT_TRUE(frame(editor));
    auto image = editor.get_preview(first);

    change_stop(*document, "#ffffff");
    pump();
    EXPECT_EQ(editor.get_preview(first), image);
    EXPECT_EQ(dirty(editor), 1u);
    EXPECT_NE(pending(editor), 0u);
    ASSERT_TRUE(frame(editor));
    EXPECT_NE(editor.get_preview(first), image);
}

TEST_F(MeshEditorTest, GeometryAndRootChangesRefreshOnlyTheirPreview)
{
    MeshEditor editor;
    editor.set_document(document.get());
    ASSERT_TRUE(frame(editor));
    auto image = editor.get_preview(first), other = editor.get_preview(second);
    document->getObjectById("first-stop")->getRepr()->setAttribute("path", "C 10,-20 30,-20 40,0");
    document->ensureUpToDate();
    ASSERT_TRUE(frame(editor));
    EXPECT_NE(editor.get_preview(first), image);
    EXPECT_EQ(editor.get_preview(second), other);
    image = editor.get_preview(first);
    first->getRepr()->setAttribute("x", "5");
    document->ensureUpToDate();
    ASSERT_TRUE(frame(editor));
    EXPECT_NE(editor.get_preview(first), image);
    EXPECT_EQ(editor.get_preview(second), other);
}

TEST_F(MeshEditorTest, ResourceRebuildKeepsUserSelectionAndReconnectsOnce)
{
    MeshEditor editor;
    editor.set_document(document.get());
    ASSERT_TRUE(frame(editor));
    for (unsigned i = 0; i < store(editor)->get_n_items(); ++i) {
        selection(editor)->set_selected(i);
        if (editor.get_selected_mesh() == second) break;
    }
    ASSERT_EQ(editor.get_selected_mesh(), second);
    unsigned changes = 0;
    editor.signal_changed().connect([&](auto) { ++changes; });
    auto copy = second->getRepr()->duplicate(document->getReprDoc());
    copy->setAttribute("id", "third");
    copy->firstChild()->firstChild()->firstChild()->setAttribute("id", "third-stop");
    document->getDefs()->getRepr()->appendChild(copy);
    GC::release(copy);
    document->ensureUpToDate();
    ASSERT_TRUE(frame(editor));
    EXPECT_EQ(store(editor)->get_n_items(), 3u);
    EXPECT_EQ(connections(editor), 6u);
    EXPECT_EQ(editor.get_selected_mesh(), second);
    EXPECT_EQ(changes, 0u);
    change_stop(*document, "#0000ff");
    EXPECT_EQ(dirty(editor), 1u);
    ASSERT_TRUE(frame(editor));
    EXPECT_EQ(connections(editor), 6u);
}

TEST_F(MeshEditorTest, DocumentSwitchDropsOldConnectionsAndDirtyWork)
{
    MeshEditor editor;
    editor.set_document(document.get());
    ASSERT_TRUE(frame(editor));
    change_stop(*document, "#0000ff");
    auto other = mesh_document();
    ASSERT_TRUE(other);
    editor.set_document(other.get());
    EXPECT_EQ(dirty(editor), 0u);
    EXPECT_EQ(connections(editor), 0u);
    EXPECT_FALSE(editor.get_preview(first));
    ASSERT_TRUE(frame(editor));
    change_stop(*document, "#ffffff");
    EXPECT_EQ(pending(editor), 0u);
    change_stop(*other, "#00ffff");
    EXPECT_NE(pending(editor), 0u);
    editor.set_document(nullptr);
    EXPECT_EQ(pending(editor), 0u);
    EXPECT_EQ(connections(editor), 0u);
    EXPECT_EQ(store(editor)->get_n_items(), 0u);
    change_stop(*other, "#ffffff");
    EXPECT_EQ(pending(editor), 0u);
}

TEST_F(MeshEditorTest, RemovingDirtyMeshRebuildsWithoutRenderingReleasedObject)
{
    MeshEditor editor;
    editor.set_document(document.get());
    editor.select_mesh(second);
    ASSERT_TRUE(frame(editor));
    change_stop(*document, "#0000ff");
    first->deleteObject();
    first = nullptr;
    document->ensureUpToDate();
    EXPECT_EQ(dirty(editor), 0u);
    ASSERT_TRUE(frame(editor));
    EXPECT_EQ(store(editor)->get_n_items(), 1u);
    EXPECT_EQ(connections(editor), 2u);
    EXPECT_EQ(editor.get_selected_mesh(), second);
    EXPECT_TRUE(editor.get_preview(second));
}

TEST_F(MeshEditorTest, DocumentReleaseClearsPendingWork)
{
    MeshEditor editor;
    auto other = mesh_document();
    ASSERT_TRUE(other);
    editor.set_document(other.get());
    ASSERT_TRUE(frame(editor));
    change_stop(*other, "#0000ff");
    other.reset();
    EXPECT_EQ(pending(editor), 0u);
    EXPECT_EQ(connections(editor), 0u);
    EXPECT_EQ(store(editor)->get_n_items(), 0u);
    EXPECT_FALSE(editor.get_selected_mesh());
}

TEST_F(MeshEditorTest, EditorDestructionDisconnectsPendingWork)
{
    auto editor = std::make_unique<MeshEditor>();
    editor->set_document(document.get());
    ASSERT_TRUE(frame(*editor));
    change_stop(*document, "#0000ff");
    EXPECT_NE(pending(*editor), 0u);
    editor.reset();
    change_stop(*document, "#ffffff");
    pump();
}

} // namespace Inkscape::UI::Widget
