// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Mesh drawing and editing tool
 *
 * Authors:
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Abhishek Sharma
 *   Tavmjong Bah <tavmjong@free.fr>
 *
 * Copyright (C) 2012 Tavmjong Bah
 * Copyright (C) 2007 Johan Engelen
 * Copyright (C) 2005 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

//#define DEBUG_MESH

#include "mesh-tool.h"

// Libraries
#include <glibmm/i18n.h>

// General
#include "document-undo.h"
#include "document.h"
#include "gradient-chemistry.h"
#include "message-context.h"
#include "rubberband.h"
#include "selection.h"

#include "display/control/canvas-item-curve.h"

#include "object/sp-defs.h"
#include "object/sp-mesh-gradient.h"
#include "object/mesh-object.h"
#include "object/sp-path.h"
#include "object/sp-namedview.h"
#include "object/sp-text.h"
#include "style.h"

#include "ui/icon-names.h"
#include "ui/widget/events/canvas-event.h"

using Inkscape::DocumentUndo;

namespace Inkscape {
namespace UI {
namespace Tools {

// TODO: The gradient tool class looks like a 1:1 copy.

MeshTool::MeshTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/mesh", "mesh.svg")
// TODO: Why are these connections stored as pointers?
    , selcon(nullptr)
    , cursor_addnode(false)
    , show_handles(true)
    , edit_fill(true)
    , edit_stroke(true)
    , mod_gradient_create(Modifiers::Modifier::get(Modifiers::Type::GRADIENT_CREATE))
    , mod_node_insert(Modifiers::Modifier::get(Modifiers::Type::NODE_INSERT))
    , mod_select_add_to(Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO))
    , mod_select_force_drag(Modifiers::Modifier::get(Modifiers::Type::SELECT_FORCE_DRAG))
    , mod_select_remove_from(Modifiers::Modifier::get(Modifiers::Type::SELECT_REMOVE_FROM))
{
    // TODO: This value is overwritten in the root handler
    this->tolerance = 6;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (prefs->getBool("/tools/mesh/selcue", true)) {
        this->enableSelectionCue();
    }

    this->enableGrDrag();
    Inkscape::Selection *selection = desktop->getSelection();

    this->selcon = new sigc::connection(selection->connectChanged(
        sigc::mem_fun(*this, &MeshTool::selection_changed)
    ));

    sp_event_context_read(this, "show_handles");
    sp_event_context_read(this, "edit_fill");
    sp_event_context_read(this, "edit_stroke");

    this->selection_changed(selection);
}

MeshTool::~MeshTool() {
    this->enableGrDrag(false);

    this->selcon->disconnect();
    delete this->selcon;
}

// This must match GrPointType enum sp-gradient.h
// We should move this to a shared header (can't simply move to gradient.h since that would require
// including <glibmm/i18n.h> which messes up "N_" in extensions... argh!).
const gchar *ms_handle_descr [] = {
    N_("Linear gradient <b>start</b>"), //POINT_LG_BEGIN
    N_("Linear gradient <b>end</b>"),
    N_("Linear gradient <b>mid stop</b>"),
    N_("Radial gradient <b>center</b>"),
    N_("Radial gradient <b>radius</b>"),
    N_("Radial gradient <b>radius</b>"),
    N_("Radial gradient <b>focus</b>"), // POINT_RG_FOCUS
    N_("Radial gradient <b>mid stop</b>"),
    N_("Radial gradient <b>mid stop</b>"),
    N_("Mesh gradient <b>corner</b>"),
    N_("Mesh gradient <b>handle</b>"),
    N_("Mesh gradient <b>tensor</b>")
};

void MeshTool::selection_changed(Inkscape::Selection *)
{
    Inkscape::Selection *selection = _desktop->getSelection();

    if (!selection) {
        return;
    }

    guint n_obj = std::ranges::distance(selection->items());

    if (!_grdrag->isNonEmpty() || selection->isEmpty()) {
        return;
    }

    guint n_tot = _grdrag->numDraggers();
    guint n_sel = _grdrag->numSelected();

    //The use of ngettext in the following code is intentional even if the English singular form would never be used
    if (n_sel == 1) {
        if (_grdrag->singleSelectedDraggerNumDraggables() == 1) {
            gchar * message = g_strconcat(
                //TRANSLATORS: %s will be substituted with the point name (see previous messages); This is part of a compound message
                _("%s selected"),
                //TRANSLATORS: Mind the space in front. This is part of a compound message
                ngettext(" out of %d mesh handle"," out of %d mesh handles",n_tot),
                ngettext(" on %d selected object"," on %d selected objects",n_obj),nullptr);
            this->message_context->setF(Inkscape::NORMAL_MESSAGE, message,
                                       _(ms_handle_descr[_grdrag->singleSelectedDraggerSingleDraggableType()]), n_tot, n_obj);
        } else {
            gchar * message =
                g_strconcat(
                    //TRANSLATORS: This is a part of a compound message (out of two more indicating: grandint handle count & object count)
                    ngettext("One handle merging %d stop (drag with <b>Shift</b> to separate) selected",
                             "One handle merging %d stops (drag with <b>Shift</b> to separate) selected",
                             _grdrag->singleSelectedDraggerNumDraggables()),
                    ngettext(" out of %d mesh handle"," out of %d mesh handles",n_tot),
                    ngettext(" on %d selected object"," on %d selected objects",n_obj),nullptr);
            this->message_context->setF(Inkscape::NORMAL_MESSAGE, message, _grdrag->singleSelectedDraggerNumDraggables(), n_tot, n_obj);
        }
    } else if (n_sel > 1) {
        //TRANSLATORS: The plural refers to number of selected mesh handles. This is part of a compound message (part two indicates selected object count)
        gchar * message =
            g_strconcat(ngettext("<b>%d</b> mesh handle selected out of %d","<b>%d</b> mesh handles selected out of %d",n_sel),
                        //TRANSLATORS: Mind the space in front. (Refers to gradient handles selected). This is part of a compound message
                        ngettext(" on %d selected object"," on %d selected objects",n_obj),nullptr);
        this->message_context->setF(Inkscape::NORMAL_MESSAGE, message, n_sel, n_tot, n_obj);
    } else if (n_sel == 0) {
        this->message_context->setF(Inkscape::NORMAL_MESSAGE,
                                   //TRANSLATORS: The plural refers to number of selected objects
                                   ngettext("<b>No</b> mesh handles selected out of %d on %d selected object",
                                            "<b>No</b> mesh handles selected out of %d on %d selected objects",n_obj), n_tot, n_obj);
    }

    // FIXME
    // We need to update mesh gradient handles.
    // Get gradient this drag belongs too..
}

void MeshTool::set(const Inkscape::Preferences::Entry& value) {
    Glib::ustring entry_name = value.getEntryName();
    if (entry_name == "show_handles") {
        this->show_handles = value.getBool(true);
    } else if (entry_name == "edit_fill") {
        this->edit_fill = value.getBool(true);
    } else if (entry_name == "edit_stroke") {
        this->edit_stroke = value.getBool(true);
    } else {
        ToolBase::set(value);
    }
}

void MeshTool::select_next()
{
    g_assert(_grdrag);
    GrDragger *d = _grdrag->select_next();
    _desktop->scroll_to_point(d->point);
}

void MeshTool::select_prev()
{
    g_assert(_grdrag);
    GrDragger *d = _grdrag->select_prev();
    _desktop->scroll_to_point(d->point);
}

/**
 * Returns vector of control curves mouse is over. Returns only first if 'first' is true.
 * event_p is in canvas (world) units.
 */
std::vector<GrDrag::ItemCurve*> MeshTool::over_curve(Geom::Point event_p, bool first)
{
    // Translate mouse point into proper coord system: needed later.
    mousepoint_doc = _desktop->w2d(event_p);
    std::vector<GrDrag::ItemCurve*> selected;

    for (auto &it : _grdrag->item_curves) {
        if (it.curve->contains(event_p, tolerance)) {
            selected.emplace_back(&it);
            if (first) {
                break;
            }
        }
    }
    return selected;
}

/**
Split row/column near the mouse point.
*/
void MeshTool::split_near_point(SPItem *item, Geom::Point mouse_p)
{
#ifdef DEBUG_MESH
    std::cout << "split_near_point: entrance: " << mouse_p << std::endl;
#endif

    // item is the selected item. mouse_p the location in doc coordinates of where to add the stop
    get_drag()->addStopNearPoint(item, mouse_p, tolerance / _desktop->current_zoom());
    DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Split mesh row/column"), INKSCAPE_ICON("mesh-gradient"));
    get_drag()->updateDraggers();
}

/**
Wrapper for various mesh operations that require a list of selected corner nodes.
 */
void MeshTool::corner_operation(MeshCornerOperation operation)
{

#ifdef DEBUG_MESH
    std::cout << "sp_mesh_corner_operation: entrance: " << operation << std::endl;
#endif

    SPDocument *doc = nullptr;

    std::map<SPMeshGradient*, std::vector<guint> > points;
    std::map<SPMeshGradient*, SPItem*> items;
    std::map<SPMeshGradient*, Inkscape::PaintTarget> fill_or_stroke;

    // Get list of selected draggers for each mesh.
    // For all selected draggers (a dragger may include draggerables from different meshes).
    for (auto dragger : _grdrag->selected) {
        // For all draggables of dragger (a draggable corresponds to a unique mesh).
        for (auto d : dragger->draggables) { 
            // Only mesh corners
            if( d->point_type != POINT_MG_CORNER ) continue;

            // Find the gradient
            auto gradient = cast<SPMeshGradient>( getGradient (d->item, d->fill_or_stroke) );

            // Collect points together for same gradient
            points[gradient].push_back( d->point_i );
            items[gradient] = d->item;
            fill_or_stroke[gradient] = d->fill_or_stroke ? Inkscape::FOR_FILL: Inkscape::FOR_STROKE;
        }
    }

    // Loop over meshes.
    for( std::map<SPMeshGradient*, std::vector<guint> >::const_iterator iter = points.begin(); iter != points.end(); ++iter) {
        SPMeshGradient *mg = iter->first;
        if( iter->second.size() > 0 ) {
            guint noperation = 0;
            switch (operation) {

                case MG_CORNER_SIDE_TOGGLE:
                    // std::cout << "SIDE_TOGGLE" << std::endl;
                    noperation += mg->array.side_toggle( iter->second );
                    break;

                case MG_CORNER_SIDE_ARC:
                    // std::cout << "SIDE_ARC" << std::endl;
                    noperation += mg->array.side_arc( iter->second );
                    break;

                case MG_CORNER_TENSOR_TOGGLE:
                    // std::cout << "TENSOR_TOGGLE" << std::endl;
                    noperation += mg->array.tensor_toggle( iter->second );
                    break;

                case MG_CORNER_COLOR_SMOOTH:
                    // std::cout << "COLOR_SMOOTH" << std::endl;
                    noperation += mg->array.color_smooth( iter->second );
                    break;

                case MG_CORNER_COLOR_PICK:
                    // std::cout << "COLOR_PICK" << std::endl;
                    noperation += mg->array.color_pick( iter->second, mg, items[iter->first] );
                    break;

                case MG_CORNER_INSERT:
                    // std::cout << "INSERT" << std::endl;
                    noperation += mg->array.insert( iter->second );
                    break;

                default:
                    std::cerr << "sp_mesh_corner_operation: unknown operation" << std::endl;
            }                    

            if( noperation > 0 ) {
                mg->array.write( mg );
                mg->requestModified(SP_OBJECT_MODIFIED_FLAG);
                doc = mg->document;

                switch (operation) {

                    case MG_CORNER_SIDE_TOGGLE:
                        DocumentUndo::done(doc, RC_("Undo", "Toggled mesh path type."), INKSCAPE_ICON("mesh-gradient"));
                        _grdrag->local_change = true; // Don't create new draggers.
                        break;

                    case MG_CORNER_SIDE_ARC:
                        DocumentUndo::done(doc, RC_("Undo", "Approximated arc for mesh side."), INKSCAPE_ICON("mesh-gradient"));
                        _grdrag->local_change = true; // Don't create new draggers.
                        break;

                    case MG_CORNER_TENSOR_TOGGLE:
                        DocumentUndo::done(doc, RC_("Undo", "Toggled mesh tensors."), INKSCAPE_ICON("mesh-gradient"));
                        _grdrag->local_change = true; // Don't create new draggers.
                        break;

                    case MG_CORNER_COLOR_SMOOTH:
                        DocumentUndo::done(doc, RC_("Undo", "Smoothed mesh corner color."), INKSCAPE_ICON("mesh-gradient"));
                        _grdrag->local_change = true; // Don't create new draggers.
                        break;

                    case MG_CORNER_COLOR_PICK:
                        DocumentUndo::done(doc, RC_("Undo", "Picked mesh corner color."), INKSCAPE_ICON("mesh-gradient"));
                        _grdrag->local_change = true; // Don't create new draggers.
                        break;

                    case MG_CORNER_INSERT:
                        DocumentUndo::done(doc, RC_("Undo", "Inserted new row or column."), INKSCAPE_ICON("mesh-gradient"));
                        break;

                    default:
                        std::cerr << "sp_mesh_corner_operation: unknown operation" << std::endl;
                }
            }
        }
    }
}


/**
 * Scale mesh to just fit into bbox of selected items.
 */
void MeshTool::fit_mesh_in_bbox()
{

#ifdef DEBUG_MESH
    std::cout << "fit_mesh_in_bbox: entrance: Entrance" << std::endl;
#endif

    Inkscape::Selection *selection = _desktop->getSelection();
    if (selection == nullptr) {
        return;
    }

    bool changed = false;
    auto itemlist = selection->items();
    for (auto item : itemlist) {

        SPStyle *style = item->style;

        if (style) {

            if (style->fill.isPaintserver()) {
                SPPaintServer *server = item->style->getFillPaintServer();
                if ( is<SPMeshGradient>(server) ) {

                    Geom::OptRect item_bbox = item->geometricBounds();
                    auto gradient = cast<SPMeshGradient>(server);
                    if (gradient->array.fill_box(gradient, item_bbox)) {
                        changed = true;
                    }
                }
            }

            if (style->stroke.isPaintserver()) {
                SPPaintServer *server = item->style->getStrokePaintServer();
                if ( is<SPMeshGradient>(server) ) {

                    Geom::OptRect item_bbox = item->visualBounds();
                    auto gradient = cast<SPMeshGradient>(server);
                    if (gradient->array.fill_box(gradient, item_bbox)) {
                        changed = true;
                    }
                }
            }

        }
    }
    if (changed) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Fit mesh inside bounding box"), INKSCAPE_ICON("mesh-gradient"));
    }
}

// Helper function to determine if an item has a mesh or not.
static bool has_mesh(SPItem *item)
{
    if (!item) {
        return false;
    }

    auto fill_or_stroke_pref =
        static_cast<Inkscape::PaintTarget>(Inkscape::Preferences::get()->getInt("/tools/mesh/newfillorstroke"));

    if (auto style = item->style) {
        auto server =
            (fill_or_stroke_pref == Inkscape::FOR_FILL) ? style->getFillPaintServer() : style->getStrokePaintServer();
        if (is<SPMeshGradient>(server)) {
            return true;
        }
    }

    return false;
}

/**
Handles all keyboard and mouse input for meshs.
Note: node/handle events are take care of elsewhere.
*/
bool MeshTool::root_handler(CanvasEvent const &event)
{
    auto selection = _desktop->getSelection();
    auto prefs = Inkscape::Preferences::get();

    tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

    bool contains_mesh = false;
    if (!selection->isEmpty()) {
        contains_mesh = has_mesh(selection->items().front());
    }

    bool ret = false;

    g_assert(_grdrag);

    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (event.num_press == 2 && event.button == 1) {

#ifdef DEBUG_MESH
                std::cout << "root_handler: GDK_2BUTTON_PRESS" << std::endl;
#endif

                // Double click:
                //  If over a mesh line, divide mesh row/column
                //  If not over a line and no mesh, create new mesh for top selected object.

                // Are we over a mesh line? (Should replace by CanvasItem event.)
                auto over_curve = this->over_curve(event.pos);

                if (!over_curve.empty() && contains_mesh) {
                    // We take the first item in selection, because with doubleclick, the first click
                    // always resets selection to the single object under cursor
                    split_near_point(selection->items().front(), mousepoint_doc);
                } else if (!contains_mesh) {
                    // Create a new gradient with default coordinates.

                    // Check if object already has mesh... if it does,
                    // don't create new mesh with click-drag.
                    new_default();
                    created_on_press = true;
                }

                ret = true;
            }

            if (event.num_press == 1 && event.button == 1) {
                created_on_press = false;

#ifdef DEBUG_MESH
                std::cout << "root_handler: GDK_BUTTON_PRESS" << std::endl;
#endif

                // Button down
                //  If mesh already exists, do rubber band selection.
                //  Else set origin for drag which will create a new gradient.

                // Are we over a mesh curve?
                auto over_curve = this->over_curve(event.pos, false);

                if (!over_curve.empty() && contains_mesh) {
                    for (auto it : over_curve) {
                        Inkscape::PaintTarget fill_or_stroke = it->is_fill ? Inkscape::FOR_FILL : Inkscape::FOR_STROKE;
                        GrDragger *dragger0 = _grdrag->getDraggerFor(it->item, POINT_MG_CORNER, it->corner0, fill_or_stroke);
                        GrDragger *dragger1 = _grdrag->getDraggerFor(it->item, POINT_MG_CORNER, it->corner1, fill_or_stroke);
                        bool add    = mod_select_add_to->active(event.modifiers);
                        bool toggle = mod_select_remove_from->active(event.modifiers);
                        if ( !add && !toggle ) {
                            _grdrag->deselectAll();
                        }
                        _grdrag->setSelected( dragger0, true, !toggle );
                        _grdrag->setSelected( dragger1, true, !toggle );
                    }
                    ret = true;

                } else {
                    Geom::Point button_w(event.pos);

                    // Save drag origin
                    saveDragOrigin(button_w);

                    dragging = true;

                    Geom::Point button_dt = _desktop->w2d(button_w);
                    // Check if object already has mesh... if it does,
                    // don't create new mesh with click-drag.
                    if (contains_mesh && !mod_gradient_create->active(event.modifiers)) {
                        Inkscape::Rubberband::get(_desktop)->start(_desktop, button_dt);
                    }

                    // remember clicked item, disregarding groups, honoring Alt; but do nothing
                    // if we might be doing a create-doubleclick of exactly the selected item(s)
                    if (!mod_gradient_create->active(event.modifiers)) {
                        auto force_drag = mod_select_force_drag->active(event.modifiers);
                        item_to_select = sp_event_context_find_item(_desktop, button_w, force_drag, true);
                    }

                    if (!selection->isEmpty()) {
                        SnapManager &m = _desktop->getNamedView()->snap_manager;
                        m.setup(_desktop);
                        m.freeSnapReturnByRef(button_dt, Inkscape::SNAPSOURCE_NODE_HANDLE);
                        m.unSetup();
                    }

                    origin = button_dt;

                    ret = true;
                }
            }
        },
        [&] (MotionEvent const &event) {
            // Mouse move
            if (dragging && (event.modifiers & GDK_BUTTON1_MASK)) {
                if (!checkDragMoved(event.pos)) {
                    return;
                }
 
#ifdef DEBUG_MESH
                std::cout << "root_handler: GDK_MOTION_NOTIFY: Dragging" << std::endl;
#endif

                Geom::Point const motion_dt = _desktop->w2d(event.pos);

                if (Inkscape::Rubberband::get(_desktop)->isStarted()) {
                    Inkscape::Rubberband::get(_desktop)->move(motion_dt);
                    this->defaultMessageContext()->set(Inkscape::NORMAL_MESSAGE, _("<b>Draw around</b> handles to select them"));
                } else {
                    // Do nothing. For a linear/radial gradient we follow the drag, updating the
                    // gradient as the end node is dragged. For a mesh gradient, the gradient is always
                    // created to fill the object when the drag ends.
                }

                gobble_motion_events(GDK_BUTTON1_MASK);

                ret = true;
            } else {
                // Not dragging

                // Do snapping
                if (!_grdrag->mouseOver() && !selection->isEmpty()) {
                    auto &m = _desktop->getNamedView()->snap_manager;
                    m.setup(_desktop);

                    auto const motion_dt = _desktop->w2d(event.pos);
                    m.preSnap(SnapCandidatePoint(motion_dt, SNAPSOURCE_OTHER_HANDLE));
                    m.unSetup();
                }

                // Highlight corner node corresponding to side or tensor node
                if (_grdrag->mouseOver()) {
                    // MESH FIXME: Light up corresponding corner node corresponding to node we are over.
                    // See "pathflash" in ui/tools/node-tool.cpp for ideas.
                    // Use _desktop->add_temporary_canvasitem( SPCanvasItem, milliseconds );
                }

                // Change cursor shape if over line
                auto over_curve = this->over_curve(event.pos);

                if (cursor_addnode && over_curve.empty()) {
                    set_cursor("mesh.svg");
                    cursor_addnode = false;
                } else if (!cursor_addnode && !over_curve.empty() && contains_mesh) {
                    set_cursor("mesh-add.svg");
                    cursor_addnode = true;
                }
            }
        },
        [&] (ButtonReleaseEvent const &event) {
            xyp = {};
            if (event.button == 1) {

#ifdef DEBUG_MESH
                std::cout << "root_handler: GDK_BUTTON_RELEASE" << std::endl;
#endif

                // Check if over line
                auto over_curve = this->over_curve(event.pos);

                if (mod_node_insert->active(event.modifiers)) {
                    if (!over_curve.empty() && has_mesh(over_curve[0]->item)) {
                        split_near_point(over_curve[0]->item, mousepoint_doc);
                        ret = true;
                    }
                } else {
                    dragging = false;

                    // Unless clicked with intent to create (Ctrl+doubleclick by default).
                    if (mod_gradient_create->active(event.modifiers) && !mod_select_add_to->active(event.modifiers)) {
                        Inkscape::Rubberband::get(_desktop)->stop();
                        ret = true;
                    } else {

                        if (created_on_press) {
                            // The double-click made the mesh, replacing a shape by a mesh object:
                            // keep that selected rather than treat this as a click on the canvas.
                            created_on_press = false;
                        } else if (!within_tolerance) {

                            // Check if object already has mesh... if it does,
                            // don't create new mesh with click-drag.
                            if (!contains_mesh) {
                                new_default();
                            } else {

                                // we've been dragging, either create a new gradient
                                // or rubberband-select if we have rubberband
                                Inkscape::Rubberband *r = Inkscape::Rubberband::get(_desktop);

                                if (r->isStarted() && !this->within_tolerance) {
                                    // this was a rubberband drag
                                    if (r->getMode() == Rubberband::Mode::RECT) {
                                        Geom::OptRect const b = r->getRectangle();
                                        if (!mod_select_add_to->active(event.modifiers)) {
                                            _grdrag->deselectAll();
                                        }
                                        _grdrag->selectRect(*b);
                                    }
                                }
                            }

                        } else if (this->item_to_select) {
                            if (!over_curve.empty()) {
                                // Clicked on an existing mesh line, don't change selection. This stops
                                // possible change in selection during a double click with overlapping objects.
                            } else {
                                // No dragging, select clicked item if any.
                                if (mod_select_add_to->active(event.modifiers)) {
                                    selection->toggle(item_to_select.get());
                                } else {
                                    _grdrag->deselectAll();
                                    selection->set(item_to_select.get());
                                }
                            }
                        } else {
                            if (!over_curve.empty()) {
                                // Clicked on an existing mesh line, don't change selection. This stops
                                // possible change in selection during a double click with overlapping objects.
                            } else {
                                // Click in an empty space; do the same as Esc.
                                if (!_grdrag->selected.empty()) {
                                    _grdrag->deselectAll();
                                } else {
                                    selection->clear();
                                }
                            }
                        }

                        item_to_select = nullptr;
                        ret = true;
                    }
                }
                Inkscape::Rubberband::get(_desktop)->stop();
            }
        },
        [&] (KeyPressEvent const &event) {

#ifdef DEBUG_MESH
            std::cout << "root_handler: GDK_KEY_PRESS" << std::endl;
#endif

            auto keyval = get_latin_keyval(event);

            // FIXME: tip
            if (Modifiers::keyval_is_a_modifier(keyval)) {
                // sp_event_show_modifier_tip (this->defaultMessageContext(), event,
                //                             _("FIXME<b>Ctrl</b>: snap mesh angle"),
                //                             _("FIXME<b>Shift</b>: draw mesh around the starting point"),
                //                             NULL);
            }

            switch (keyval) {
                case GDK_KEY_A:
                case GDK_KEY_a:
                    if (mod_ctrl_only(event) && _grdrag->isNonEmpty()) {
                        _grdrag->selectAll();
                        ret = true;
                    }
                    break;

                case GDK_KEY_Escape:
                    if (!_grdrag->selected.empty()) {
                        _grdrag->deselectAll();
                    } else {
                        selection->clear();
                    }

                    ret = true;
                    //TODO: make dragging escapable by Esc
                    break;

                    // Mesh Operations --------------------------------------------

                case GDK_KEY_Insert:
                case GDK_KEY_KP_Insert:
                    // with any modifiers:
                    corner_operation(MG_CORNER_INSERT);
                    ret = true;
                    break;

                case GDK_KEY_i:
                case GDK_KEY_I:
                    if (mod_shift_only(event)) {
                        // Shift+I - insert corners (alternate keybinding for keyboards
                        //           that don't have the Insert key)
                        this->corner_operation(MG_CORNER_INSERT);
                        ret = true;
                    }
                    break;

                case GDK_KEY_Delete:
                case GDK_KEY_KP_Delete:
                case GDK_KEY_BackSpace:
                    if (!_grdrag->selected.empty()) {
                        ret = true;
                    }
                    break;

                case GDK_KEY_b:  // Toggle mesh side between lineto and curveto.
                case GDK_KEY_B:
                    if (mod_alt(event) && _grdrag->isNonEmpty() && _grdrag->hasSelection()) {
                        corner_operation(MG_CORNER_SIDE_TOGGLE);
                        ret = true;
                    }
                    break;

                case GDK_KEY_c:  // Convert mesh side from generic Bezier to Bezier approximating arc,
                case GDK_KEY_C:  // preserving handle direction.
                    if (mod_alt(event) && _grdrag->isNonEmpty() && _grdrag->hasSelection()) {
                        corner_operation(MG_CORNER_SIDE_ARC);
                        ret = true;
                    }
                    break;

                case GDK_KEY_g:  // Toggle mesh tensor points on/off
                case GDK_KEY_G:
                    if (mod_alt(event) && _grdrag->isNonEmpty() && _grdrag->hasSelection()) {
                        corner_operation(MG_CORNER_TENSOR_TOGGLE);
                        ret = true;
                    }
                    break;

                case GDK_KEY_j:  // Smooth corner color
                case GDK_KEY_J:
                    if (mod_alt(event) && _grdrag->isNonEmpty() && _grdrag->hasSelection()) {
                        corner_operation(MG_CORNER_COLOR_SMOOTH);
                        ret = true;
                    }
                    break;

                case GDK_KEY_k:  // Pick corner color
                case GDK_KEY_K:
                    if (mod_alt(event) && _grdrag->isNonEmpty() && _grdrag->hasSelection()) {
                        corner_operation(MG_CORNER_COLOR_PICK);
                        ret = true;
                    }
                    break;

                default:
                    ret = _grdrag->key_press_handler(event);
                    break;
            }
        },
        [&] (KeyReleaseEvent const &event) {
            auto keyval = get_latin_keyval(event);

            if (Modifiers::keyval_is_a_modifier(keyval)) {
                defaultMessageContext()->clear();
            }
        },
        [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::root_handler(event);
}

// Creates a new mesh gradient.
void MeshTool::new_default()
{
    Inkscape::Selection *selection = _desktop->getSelection();
    SPDocument *document = _desktop->getDocument();

    if (!selection->isEmpty()) {

        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        Inkscape::PaintTarget fill_or_stroke_pref =
            static_cast<Inkscape::PaintTarget>(prefs->getInt("/tools/mesh/newfillorstroke"));
        Inkscape::Preferences::Entry _show_handles_value = prefs->getEntry("/tools/mesh/show_handles");

        // Ensure that the preference show_handles is enable by default 
        if (!_show_handles_value.isSet()) {
            prefs->setBool("/tools/mesh/show_handles", true);
        }
        // Ensure mesh is immediately editable.
        // Editing both fill and stroke at same time doesn't work well so avoid.
        if (fill_or_stroke_pref == Inkscape::FOR_FILL) {
            prefs->setBool("/tools/mesh/edit_fill",   true );
            prefs->setBool("/tools/mesh/edit_stroke", false);
        } else {
            prefs->setBool("/tools/mesh/edit_fill",   false);
            prefs->setBool("/tools/mesh/edit_stroke", true );
        }

// HACK: reset fill-opacity - that 0.75 is annoying; BUT remove this when we have an opacity slider for all tabs
        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "fill-opacity", "1.0");

        Inkscape::XML::Document *xml_doc = document->getReprDoc();
        SPDefs *defs = document->getDefs();

        auto items = selection->items_vector();
        std::vector<SPItem *> mesh_items;
        for(auto item : items){

            //FIXME: see above
            sp_repr_css_change_recursive(item->getRepr(), css, "style");

            // Create mesh element
            Inkscape::XML::Node *repr = xml_doc->createElement("svg:meshgradient");

            // privates are garbage-collectable
            repr->setAttribute("inkscape:collect", "always");

            // Attach to document
            defs->getRepr()->appendChild(repr);
            Inkscape::GC::release(repr);

            // Get corresponding object
            SPMeshGradient *mg = static_cast<SPMeshGradient *>(document->getObjectByRepr(repr));
            mg->array.create(mg, item, (fill_or_stroke_pref == Inkscape::FOR_FILL) ?
                             item->geometricBounds() : item->visualBounds());

            bool isText = is<SPText>(item);
            sp_style_set_property_url(item,
                                      ((fill_or_stroke_pref == Inkscape::FOR_FILL) ? "fill":"stroke"),
                                      mg, isText);

            if (fill_or_stroke_pref == Inkscape::FOR_FILL &&
                prefs->getInt("/tools/mesh/mesh_geometry", SP_MESH_GEOMETRY_NORMAL) == SP_MESH_GEOMETRY_NORMAL) {
                if (auto path = Inkscape::MeshObject::create(item, mg)) {
                    item = path;
                }
            }
            mesh_items.push_back(item);

            item->requestModified(SP_OBJECT_MODIFIED_FLAG|SP_OBJECT_STYLE_MODIFIED_FLAG);
        }
        selection->setList(mesh_items);

        if (css) {
            sp_repr_css_attr_unref(css);
            css = nullptr;
        }

        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Create mesh"), INKSCAPE_ICON("mesh-gradient"));

        // status text; we do not track coords because this branch is run once, not all the time
        // during drag
        int n_objects = std::ranges::distance(selection->items());
        message_context->setF(Inkscape::NORMAL_MESSAGE,
                                  ngettext("<b>Gradient</b> for %d object; with <b>Ctrl</b> to snap angle",
                                           "<b>Gradient</b> for %d objects; with <b>Ctrl</b> to snap angle", n_objects),
                                  n_objects);
    } else {
        _desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>objects</b> on which to create gradient."));
    }
}

}
}
}

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
