// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_MESH_TOOl_H
#define INKSCAPE_UI_TOOLS_MESH_TOOl_H

/*
 * Mesh drawing and editing tool
 *
 * Authors:
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Jon A. Cruz <jon@joncruz.org.
 *   Tavmjong Bah <tavmjong@free.fr>
 *
 * Copyright (C) 2012 Tavmjong Bah
 * Copyright (C) 2007 Johan Engelen
 * Copyright (C) 2005,2010 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "gradient-drag.h"
#include "ui/tools/tool-base.h"

namespace Inkscape {
class Selection;
class CanvasItemCurve;
} // namespace Inkscape

namespace Inkscape::UI::Tools {

class MeshTool : public ToolBase
{
public:
    MeshTool(SPDesktop *desktop);
    ~MeshTool() override;

    Geom::Point origin;

    Geom::Point mousepoint_doc; // stores mousepoint when over_line in doc coords

    sigc::connection *selcon;

    void set(Preferences::Entry const &val) override;
    bool root_handler(CanvasEvent const &event) override;
    void fit_mesh_in_bbox();
    void corner_operation(MeshCornerOperation operation);

private:
    bool cursor_addnode;
    bool created_on_press = false; // a double-click's second press made the mesh
    bool show_handles;
    bool edit_fill;
    bool edit_stroke;

    Modifiers::Modifier *mod_gradient_create;
    Modifiers::Modifier *mod_node_insert;
    Modifiers::Modifier *mod_select_add_to;
    Modifiers::Modifier *mod_select_force_drag;
    Modifiers::Modifier *mod_select_remove_from;

    void selection_changed(Inkscape::Selection *sel);
    void select_next();
    void select_prev();
    void new_default();
    void split_near_point(SPItem *item, Geom::Point mouse_p);
    std::vector<GrDrag::ItemCurve*> over_curve(Geom::Point event_p, bool first = true);
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_MESH_TOOl_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
