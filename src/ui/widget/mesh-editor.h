// SPDX-License-Identifier: GPL-2.0-or-later
//
// Created by Michael Kowalski on 9/9/24.
//

#ifndef MESH_EDITOR_H
#define MESH_EDITOR_H

#include <unordered_map>
#include <unordered_set>

#include <gtkmm/box.h>
#include <gtkmm/gridview.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/singleselection.h>
#include <ui/iconview-item-factory.h>
#include <ui/operation-blocker.h>

#include "ui/filtered-store.h"

class SPGradient;
class SPMeshGradient;
class SPDocument;

namespace Inkscape::UI::Widget {

class MeshEditor : public Gtk::Box {
public:
    MeshEditor();
    ~MeshEditor() override;

    void set_document(SPDocument* document);
    void select_mesh(SPGradient* mesh);
    SPGradient* get_selected_mesh() const;
    Glib::RefPtr<Gdk::Texture> get_preview(SPMeshGradient* mesh) const;
    sigc::signal<void (SPGradient*)>& signal_changed() { return _signal_changed; }

private:
    friend class MeshEditorTest;

    std::vector<SPMeshGradient*> rebuild_list();
    void schedule_update();
    void update();
    void rebuild_store(const std::vector<SPMeshGradient*>& list);
    void update_previews();

    Gtk::ScrolledWindow _scroll;
    Gtk::GridView _gridview;
    std::unique_ptr<IconViewItemFactory> _item_factory;
    Glib::RefPtr<Gio::ListStoreBase> _store;
    Glib::RefPtr<Gtk::SingleSelection> _selection_model;
    SPDocument* _document = nullptr;
    unsigned int _tick_callback = 0;
    sigc::scoped_connection _gradients;
    sigc::scoped_connection _defs_release;
    std::vector<sigc::scoped_connection> _mesh_connections;
    std::unordered_map<SPMeshGradient*, unsigned> _mesh_indices;
    std::unordered_set<SPMeshGradient*> _dirty_meshes;
    bool _gradients_changed = false;
    SPGradient* _selected = nullptr;
    std::string _selected_id;
    OperationBlocker _update;
    sigc::signal<void (SPGradient*)> _signal_changed;
};

} // namespace

#endif //MESH_EDITOR_H
