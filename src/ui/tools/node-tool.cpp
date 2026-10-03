// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * New node tool - implementation.
 */
/* Authors:
 *   Krzysztof Kosiński <tweenk@gmail.com>
 *   Abhishek Sharma
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/tools/node-tool.h"

#include <iomanip>
#include <glib/gi18n.h>

#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-group.h"
#include "display/control/snap-indicator.h"
#include "document-undo.h"
#include "path/path-curve.h"
#include "live_effects/effect.h"
#include "message-context.h"
#include "object/sp-clippath.h"
#include "object/sp-mask.h"
#include "object/sp-namedview.h"
#include "object/sp-shape.h"
#include "rubberband.h"
#include "selection-chemistry.h"
#include "selection.h"
#include "ui/knot/knot-holder.h"
#include "ui/modifiers.h"
#include "ui/shape-editor.h" // temporary!
#include "ui/tool/control-point-selection.h"
#include "ui/tool/curve-drag-point.h"
#include "ui/tool/multi-path-manipulator.h"
#include "ui/tool/path-manipulator.h"
#include "ui/tool/selectable-control-point.h"
#include "ui/icon-names.h"
#include "util/units.h"
#include "ui/widget/events/canvas-event.h"
#include "util-string/ustring-format.h"

using Inkscape::Modifiers::Modifier;

/** @struct NodeTool
 *
 * Node tool event context.
 *
 * @par Architectural overview of the tool
 * @par
 * Here's a breakdown of what each object does.
 * - Handle: shows a handle and keeps the node type constraint (smooth / symmetric) by updating
 *   the other handle's position when dragged. Its move() method cannot violate the constraints.
 * - Node: keeps node type constraints for auto nodes and smooth nodes at ends of linear segments.
 *   Its move() method cannot violate constraints. Handles linear grow and dispatches spatial grow
 *   to MultiPathManipulator. Keeps a reference to its NodeList.
 * - NodeList: exposes an iterator-based interface to nodes. It is possible to obtain an iterator
 *   to a node from the node. Keeps a reference to its SubpathList.
 * - SubpathList: list of NodeLists that represents an editable pathvector. Keeps a reference
 *   to its PathManipulator.
 * - PathManipulator: performs most of the single-path actions like reverse subpaths,
 *   delete segment, shift selection, etc. Keeps a reference to MultiPathManipulator.
 * - MultiPathManipulator: performs additional operations for actions that are not per-path,
 *   for example node joins and segment joins. Tracks the control transforms for PMs that edit
 *   clipping paths and masks. It is more or less equivalent to ShapeEditor and in the future
 *   it might handle all shapes. Handles XML commit of actions that affect all paths or
 *   the node selection and removes PathManipulators that have no nodes left after e.g. node
 *   deletes.
 * - ControlPointSelection: keeps track of node selection and a set of nodes that can potentially
 *   be selected. There can be more than one selection. Performs actions that require no
 *   knowledge about the path, only about the nodes, like dragging and transforms. It is not
 *   specific to nodes and can accommodate any control point derived from SelectableControlPoint.
 *   Transforms nodes in response to transform handle events.
 * - TransformHandleSet: displays nodeset transform handles and emits transform events. The aim
 *   is to eventually use a common class for object and control point transforms.
 * - SelectableControlPoint: base for any type of selectable point. It can belong to only one
 *   selection.
 *
 * @par Functionality that resides in weird places
 * @par
 *
 * This list is probably incomplete.
 * - Curve dragging: CurveDragPoint, controlled by PathManipulator
 * - Single handle shortcuts: MultiPathManipulator::event(), ModifierTracker
 * - Linear and spatial grow: Node, spatial grow routed to ControlPointSelection
 * - Committing handle actions performed with the mouse: PathManipulator
 * - Sculpting: ControlPointSelection
 *
 * @par Plans for the future
 * @par
 * - MultiPathManipulator should become a generic shape editor that manages all active manipulator,
 *   more or less like the old ShapeEditor.
 * - Knotholder should be rewritten into one manipulator class per shape, using the control point
 *   classes. Interesting features like dragging rectangle sides could be added along the way.
 * - Better handling of clip and mask editing, particularly in response to undo.
 * - High level refactoring of the event context hierarchy. All aspects of tools, like toolbox
 *   controls, icons, event handling should be collected in one class, though each aspect
 *   of a tool might be in an separate class for better modularity. The long term goal is to allow
 *   tools to be defined in extensions or shared library plugins.
 */

namespace Inkscape::UI::Tools {

Inkscape::CanvasItemGroup *create_control_group(SPDesktop *desktop)
{
    auto group = new Inkscape::CanvasItemGroup(desktop->getCanvasControls());
    group->set_name("CanvasItemGroup:NodeTool");
    return group;
}

NodeTool::NodeTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/nodes", "node.svg")
    , mod_move_no_snapping(Modifiers::Modifier::get(Modifiers::Type::MOVE_NO_SNAPPING))
    , mod_node_invert(Modifiers::Modifier::get(Modifiers::Type::NODE_INVERT))
    , mod_node_remove_from(Modifiers::Modifier::get(Modifiers::Type::NODE_REMOVE_FROM))
    , mod_select_add_to(Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO))
    , mod_select_force_drag(Modifiers::Modifier::get(Modifiers::Type::SELECT_FORCE_DRAG))
    , mod_select_touch_path(Modifiers::Modifier::get(Modifiers::Type::SELECT_TOUCH_PATH))
{
    this->_path_data = new Inkscape::UI::PathSharedData();

    Inkscape::UI::PathSharedData &data = *this->_path_data;
    data.node_data.desktop = desktop;

    // Prepare canvas groups for controls. This guarantees correct z-order, so that
    // for example a dragpoint won't obscure a node
    data.outline_group          = create_control_group(desktop);
    data.node_data.handle_line_group = new Inkscape::CanvasItemGroup(desktop->getCanvasControls());
    data.dragpoint_group        = create_control_group(desktop);
    _transform_handle_group     = create_control_group(desktop);
    data.node_data.node_group   = create_control_group(desktop);
    data.node_data.handle_group = create_control_group(desktop);

    data.node_data.handle_line_group->set_name("CanvasItemGroup:NodeTool:handle_line_group");

    Inkscape::Selection *selection = desktop->getSelection();

    this->_selection_changed_connection.disconnect();
    this->_selection_changed_connection =
        selection->connectChanged(sigc::mem_fun(*this, &NodeTool::selection_changed));

    this->_mouseover_changed_connection.disconnect();
    this->_mouseover_changed_connection =
        Inkscape::UI::ControlPoint::signal_mouseover_change.connect(sigc::mem_fun(*this, &NodeTool::mouseover_changed));

    if (this->_transform_handle_group) {
        this->_selected_nodes = new Inkscape::UI::ControlPointSelection(desktop, this->_transform_handle_group);
    }
    data.node_data.selection = this->_selected_nodes;

    this->_multipath = new Inkscape::UI::MultiPathManipulator(data);

    this->_multipath->signal_coords_changed.connect([this] {
        _desktop->emit_control_point_selected(_selected_nodes);
    });

    _selected_nodes->signal_selection_changed.connect([this] (auto, auto) { update_tip(); });

    // read prefs before adding items to selection to prevent momentarily showing the outline
    sp_event_context_read(this, "show_handles");
    sp_event_context_read(this, "show_outline");
    sp_event_context_read(this, "live_outline");
    sp_event_context_read(this, "live_objects");
    sp_event_context_read(this, "show_path_direction");
    sp_event_context_read(this, "show_transform_handles");
    sp_event_context_read(this, "single_node_transform_handles");
    sp_event_context_read(this, "edit_clipping_paths");
    sp_event_context_read(this, "edit_masks");

    selection_changed(selection);
    update_tip();

    auto prefs = Preferences::get();

    if (prefs->getBool("/tools/nodes/selcue")) {
        this->enableSelectionCue();
    }

    if (prefs->getBool("/tools/nodes/gradientdrag")) {
        this->enableGrDrag();
    }

    desktop->emit_control_point_selected(_selected_nodes); // sets the coord entry fields to inactive
    sp_update_helperpath(desktop);
}

NodeTool::~NodeTool()
{
    this->_selected_nodes->clear();
    this->get_rubberband()->stop();

    this->enableGrDrag(false);

    if (this->flash_tempitem) {
        _desktop->remove_temporary_canvasitem(this->flash_tempitem);
    }
    for (auto hp : this->_helperpath_tmpitem) {
        _desktop->remove_temporary_canvasitem(hp);
    }
    this->_selection_changed_connection.disconnect();
    // this->_selection_modified_connection.disconnect();
    this->_mouseover_changed_connection.disconnect();

    delete this->_multipath;
    delete this->_selected_nodes;

    _path_data->node_data.node_group->unlink();
    _path_data->node_data.handle_group->unlink();
    _path_data->node_data.handle_line_group->unlink();
    _path_data->outline_group->unlink();
    _path_data->dragpoint_group->unlink();
    _transform_handle_group->unlink();
}

Inkscape::Rubberband *NodeTool::get_rubberband() const
{
    return Inkscape::Rubberband::get(_desktop);
}

void NodeTool::deleteSelected()
{
    auto prefs = Preferences::get();
    // This takes care of undo internally
    _multipath->deleteNodes((NodeDeleteMode)prefs->getInt("/tools/node/delete-mode-default", (int)NodeDeleteMode::automatic));
}

// show helper paths of the applied LPE, if any
void sp_update_helperpath(SPDesktop *desktop)
{
    if (!desktop) {
        return;
    }

    auto nt = dynamic_cast<Tools::NodeTool*>(desktop->getTool());
    if (!nt) {
        // We remove this warning and just stop execution
        // because we are updating helper paths also from LPE dialog so we not unsure the tool used
        // std::cerr << "sp_update_helperpath called when Node Tool not active!" << std::endl;
        return;
    }

    Inkscape::Selection *selection = desktop->getSelection();
    for (auto hp : nt->_helperpath_tmpitem) {
        desktop->remove_temporary_canvasitem(hp);
    }
    nt->_helperpath_tmpitem.clear();
    std::vector<std::pair<Geom::PathVector, Geom::Affine>> cs;
    for (auto item : selection->items_vector()) {
        auto lpeitem = cast<SPLPEItem>(item);
        if (lpeitem && lpeitem->hasPathEffectRecursive()) {
            Inkscape::LivePathEffect::Effect *lpe = lpeitem->getCurrentLPE();
            if (lpe && lpe->isVisible()/* && lpe->showOrigPath()*/) {
                std::vector<Geom::Point> selectedNodesPositions;
                if (nt->_selected_nodes) {
                    Inkscape::UI::ControlPointSelection *selectionNodes = nt->_selected_nodes;
                    for (auto selectionNode : *selectionNodes) {
                        Inkscape::UI::Node *n = dynamic_cast<Inkscape::UI::Node *>(selectionNode);
                        selectedNodesPositions.push_back(n->position());
                    }
                }
                lpe->setSelectedNodePoints(selectedNodesPositions);
                lpe->setCurrentZoom(desktop->current_zoom());
                Geom::PathVector c;
                std::vector<Geom::PathVector> cs = lpe->getCanvasIndicators(lpeitem);
                for (auto &p : cs) {
                    p *= desktop->dt2doc();
                    pathvector_append(c, p);
                }
                if (!c.empty()) {
                    auto helperpath = new Inkscape::CanvasItemBpath(desktop->getCanvasTemp(), c, true);
                    helperpath->set_stroke(0x0000ff9a);
                    helperpath->set_fill(0x0, SP_WIND_RULE_NONZERO); // No fill
                    nt->_helperpath_tmpitem.emplace_back(desktop->add_temporary_canvasitem(helperpath, 0));
                }
            }
        }
    }
}

void NodeTool::set(const Inkscape::Preferences::Entry& value) {
    Glib::ustring entry_name = value.getEntryName();

    if (entry_name == "show_handles") {
        this->show_handles = value.getBool(true);
        this->_multipath->showHandles(this->show_handles);
    } else if (entry_name == "show_outline") {
        this->show_outline = value.getBool();
        this->_multipath->showOutline(this->show_outline);
    } else if (entry_name == "live_outline") {
        this->live_outline = value.getBool();
        this->_multipath->setLiveOutline(this->live_outline);
    } else if (entry_name == "live_objects") {
        this->live_objects = value.getBool();
        this->_multipath->setLiveObjects(this->live_objects);
    } else if (entry_name == "show_path_direction") {
        this->show_path_direction = value.getBool();
        this->_multipath->showPathDirection(this->show_path_direction);
    } else if (entry_name == "show_transform_handles") {
        this->show_transform_handles = value.getBool(true);
        this->_selected_nodes->showTransformHandles(
            this->show_transform_handles, this->single_node_transform_handles);
    } else if (entry_name == "single_node_transform_handles") {
        this->single_node_transform_handles = value.getBool();
        this->_selected_nodes->showTransformHandles(
            this->show_transform_handles, this->single_node_transform_handles);
    } else if (entry_name == "edit_clipping_paths") {
        this->edit_clipping_paths = value.getBool();
        this->selection_changed(_desktop->getSelection());
    } else if (entry_name == "edit_masks") {
        this->edit_masks = value.getBool();
        this->selection_changed(_desktop->getSelection());
    } else {
        ToolBase::set(value);
    }
}

/** Recursively collect ShapeRecords */
static
void gather_items(NodeTool *nt, SPItem *base, SPObject *obj, Inkscape::UI::ShapeRole role,
    std::set<Inkscape::UI::ShapeRecord> &s)
{
    using namespace Inkscape::UI;

    if (!obj) {
        return;
    }

    //XML Tree being used directly here while it shouldn't be.
    if (role != SHAPE_ROLE_NORMAL && (is<SPGroup>(obj) || is<SPObjectGroup>(obj))) {
        for (auto& c: obj->children) {
            gather_items(nt, base, &c, role, s);
        }
    } else if (auto item = cast<SPItem>(obj)) {
        ShapeRecord r;
        r.object = obj;
        r.role = role;

        // TODO add support for objectBoundingBox
        if (role != SHAPE_ROLE_NORMAL && base) {
            r.edit_transform = base->i2doc_affine();
        }

        if (s.insert(r).second) {
            // this item was encountered the first time
            if (nt->edit_clipping_paths) {
                gather_items(nt, item, item->getClipObject(), SHAPE_ROLE_CLIPPING_PATH, s);
            }

            if (nt->edit_masks) {
                gather_items(nt, item, item->getMaskObject(), SHAPE_ROLE_MASK, s);
            }
        }
    }
}

void NodeTool::selection_changed(Inkscape::Selection *sel)
{
    std::set<ShapeRecord> shapes;

    for (auto item : sel->items()) {
        if (item) {
            gather_items(this, nullptr, item, SHAPE_ROLE_NORMAL, shapes);
        }
    }

    // use multiple ShapeEditors for now, to allow editing many shapes at once
    // needs to be rethought
    std::erase_if(_shape_editors, [&] (auto const &i) {
        ShapeRecord s;
        s.object = i.first;
        return !shapes.contains(s);
    });

    for (auto const &r : shapes) {
        auto item = cast<SPItem>(r.object);
        auto [it, inserted] = _shape_editors.try_emplace(item);
        if (inserted) {
            auto si = std::make_unique<ShapeEditor>(_desktop, r.edit_transform);
            si->set_item(item);
            it->second = std::move(si);
        }
    }

    _multipath->setItems(shapes);
    update_tip();
    sp_update_helperpath(_desktop);
}

bool NodeTool::root_handler(CanvasEvent const &event)
{
    /* things to handle here:
     * 1. selection of items
     * 2. passing events to manipulators
     * 3. some keybindings
     */

    auto selection = _desktop->getSelection();
    auto prefs = Preferences::get();
    auto rubberband = get_rubberband();

    // checks if the modifier for touch path selection is active and sets the cursor accordingly
    auto switch_rubberband_mode = [&](int state) {
        if (mod_select_touch_path->active(state)){
            set_cursor("node-lasso.svg");
            if(rubberband->isStarted()){
                rubberband->setMode(Rubberband::Mode::TOUCHPATH);
                rubberband->setHandle(RUBBERBAND_TOUCHPATH);
            }
        } else {
            set_cursor("node.svg");
            if(rubberband->isStarted()){
                rubberband->setMode(Rubberband::Mode::RECT);
                rubberband->setHandle(RUBBERBAND_RECT);
             }
        }
    };

    if (!rubberband->isStarted()) {
        if (_multipath->event(this, event) || _selected_nodes->event(this, event)) {
            return true;
        }
    }

    bool ret = false;

    inspect_event(event,
    [&] (MotionEvent const &event) {
        if (_direct_press && (event.modifiers & GDK_BUTTON1_MASK)) {
            _directDrag(event);
            ret = true;
            return;
        }

        sp_update_helperpath(_desktop);
        auto over_item = sp_event_context_find_item(_desktop, event.pos, false, true);

        auto const motion_w = event.pos;
        auto const motion_dt = _desktop->w2d(motion_w);

        if (event.modifiers & GDK_BUTTON1_MASK) {
            if (rubberband->isStarted()) {
                rubberband->move(motion_dt);
                _updateSelectionColor(event);
            }

            auto touch_path = mod_select_touch_path->get_label();
            if (rubberband->getMode() == Rubberband::Mode::TOUCHPATH) {
                defaultMessageContext()->setF(Inkscape::NORMAL_MESSAGE,
                    _("<b>Draw over</b> lines to select their nodes; release <b>%s</b> to switch to rubberband selection"), touch_path.c_str());
            } else {
                defaultMessageContext()->setF(Inkscape::NORMAL_MESSAGE,
                    _("<b>Drag around</b> nodes to select them; press <b>%s</b> to switch to box selection"), touch_path.c_str());
            }
            ret = true;
            return;
        } else if (rubberband->isMoved()) {
            // Mouse button is up, but rubberband is still kicking.
            rubberband->stop();
        }

        auto &m = _desktop->getNamedView()->snap_manager;

        // We will show a pre-snap indication for when the user adds a node through double-clicking
        // Adding a node will only work when a path has been selected; if that's not the case then snapping is useless
        if (!_desktop->getSelection()->isEmpty()) {
            if (!mod_move_no_snapping->active(event.modifiers)) {
                m.setup(_desktop);
                auto scp = Inkscape::SnapCandidatePoint(motion_dt, Inkscape::SNAPSOURCE_OTHER_HANDLE);
                m.preSnap(scp, true);
                m.unSetup();
            }
        }

        if (over_item && over_item != _last_over) {
            _last_over = over_item;
            update_tip(event);
        }
        // create pathflash outline

        if (prefs->getBool("/tools/nodes/pathflash_enabled")) {
            if (over_item == flashed_item) {
                return;
            }

            if (!prefs->getBool("/tools/nodes/pathflash_selected") && over_item && selection->includes(over_item)) {
                return;
            }

            if (flash_tempitem) {
                _desktop->remove_temporary_canvasitem(flash_tempitem);
                flash_tempitem = nullptr;
                flashed_item = nullptr;
            }

            auto shape = cast<SPShape>(over_item);
            if (!shape) {
                return; // for now, handle only shapes
            }

            flashed_item = over_item;
            if (!shape->curveForEdit()) {
                return; // break out when curve doesn't exist
            }

            auto c = *shape->curveForEdit() * over_item->i2dt_affine();

            auto flash = new Inkscape::CanvasItemBpath(_desktop->getCanvasTemp(), c, true);
            flash->set_stroke(over_item->highlight_color().toRGBA());
            flash->set_fill(0x0, SP_WIND_RULE_NONZERO); // No fill.
            flash_tempitem =
                _desktop->add_temporary_canvasitem(flash, prefs->getInt("/tools/nodes/pathflash_timeout", 500));
        }
        // do not return true, because we need to pass this event to the parent context
        // otherwise some features cease to work
    },

    [&] (KeyPressEvent const &event) {
        _updateSelectionColor(event);
        switch_rubberband_mode(event.modifiersAfter());
        rubberband->move(_desktop->point());

        // Unconfigurable shortcuts
        switch (get_latin_keyval(event)) {
        case GDK_KEY_Escape: // deselect everything
            if (_direct_press) {
                _directCancel();
                ret = true;
                return;
            }
            if (_selected_nodes->empty()) {
                Inkscape::SelectionHelper::selectNone(_desktop);
            } else {
                _selected_nodes->clear();
            }
            update_tip(event);
            ret = true;
            return;

        case GDK_KEY_a:
        case GDK_KEY_A:
            if (mod_ctrl(event) && mod_alt(event)) {
                _selected_nodes->selectAll();
                // Ctrl+A is handled in selection-chemistry.cpp via verb
                update_tip(event);
                ret = true;
                return;
            }
            break;

        case GDK_KEY_h:
        case GDK_KEY_H:
            if (mod_ctrl_only(event)) {
                prefs->setBool("/tools/nodes/show_handles", !show_handles);
                ret = true;
                return;
            }
            break;

        case GDK_KEY_Tab:
            _multipath->shiftSelection(1);
            ret = true;
            return;
        case GDK_KEY_ISO_Left_Tab:
            _multipath->shiftSelection(-1);
            ret = true;
            return;

        default:
            break;
        }
        update_tip(event);
    },

    [&] (KeyReleaseEvent const &event) {
        _updateSelectionColor(event);
        switch_rubberband_mode(event.modifiersAfter());
        rubberband->move(_desktop->point());
        update_tip(event);

    },

    [&] (ButtonPressEvent const &event) {
        if (event.button != 1) {
            return;
        }

        auto const event_pt = event.pos;
        auto const desktop_pt = _desktop->w2d(event_pt);

        if (event.num_press == 1) {
            if (_directPress(event)) {
                ret = true;
                return;
            }
            rubberband->start(_desktop, desktop_pt, true);
            switch_rubberband_mode(event.modifiers);
            ret = true;
            return;

        }
    },

    [&] (ButtonReleaseEvent const &event) {

        if (event.button != 1) {
            return;
        }

        if (_direct_press) {
            _directRelease();
            ret = true;
            return;
        }

        if (rubberband->isStarted() && rubberband->isMoved()) {
            select_area(rubberband->getPath(), event);
        } else {
            select_point(event);
        }
        rubberband->stop();
        switch_rubberband_mode(event.modifiersAfter());
        ret = true;
        return;
    },

    [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::root_handler(event);
}

bool NodeTool::item_handler(SPItem *item, CanvasEvent const &event)
{
    if (ToolBase::item_handler(item, event)) {
        return true;
    }

    bool ret = false;

    // Node shape editors are handled differently than shape tools
    inspect_event(event,
    [&] (ButtonPressEvent const &event) {
        if (event.num_press != 1 || event.button != 1) {
            return;
        }
        for (auto &se : _shape_editors) {
            // This allows users to select an arbitary position in a pattern to edit on canvas.
            if (auto &knotholder = se.second->knotholder) {
                auto const point = event.pos;

                // This allows us to dive into groups and find what the real item is
                if (_desktop->getItemAtPoint(point, true) != knotholder->getItem()) {
                    continue;
                }

                ret |= knotholder->set_item_clickpos(_desktop->w2d(point) * _desktop->dt2doc());
            }
        }
    },
    [&] (CanvasEvent const &event) {}
    );

    return ret;
}

void NodeTool::update_tip(CanvasEvent const &event)
{
    if (event.type() == EventType::KEY_PRESS || event.type() == EventType::KEY_RELEASE) {
        auto modifiers_change = event.modifiersChange();

        if (modifiers_change == 0) {
            return;
        }

        auto modifiers_after = event.modifiers ^ modifiers_change;

        if (mod_select_add_to->active(modifiers_after)) {
            if (_last_over) {
                message_context->setF(Inkscape::NORMAL_MESSAGE,
                    C_("Node tool tip", "<b>%s</b>: drag to add nodes to the selection, "
                    "click to toggle object selection"), mod_select_add_to->get_label().c_str());
            } else {
                message_context->setF(Inkscape::NORMAL_MESSAGE,
                    C_("Node tool tip", "<b>%s</b>: drag to add nodes to the selection"),
                    mod_select_add_to->get_label().c_str());
            }

            return;
        }
    }

    update_tip();
}

void NodeTool::update_tip()
{
    unsigned sz = _selected_nodes->size();
    unsigned total = _selected_nodes->allPoints().size();

    if (sz != 0) {
        // TRANSLATORS: %1 is the number of nodes selected. %2 is the total number of nodes.
        auto nodestring = Glib::ustring::compose(
            ngettext("<b>%1 of %2</b> node selected.", "<b>%1 of %2</b> nodes selected.", total),
            sz, total);

        if (sz == 2) {
            // if there are only two nodes selected, display the angle
            // of a line going through them relative to the X axis.
            Inkscape::UI::ControlPointSelection::Set &selection_nodes = this->_selected_nodes->allPoints();
            std::vector<Geom::Point> positions;
            for (auto selection_node : selection_nodes) {
                if (selection_node->selected()) {
                    Inkscape::UI::Node *n = dynamic_cast<Inkscape::UI::Node *>(selection_node);
                    positions.push_back(n->position());
                }
            }
            g_assert(positions.size() == 2);
            const double angle = Geom::deg_from_rad(Geom::Line(positions[0], positions[1]).angle());
            nodestring += " ";
            // TRANSLATORS: %1 is an angle in degrees, formatted with two decimal places.
            nodestring += Glib::ustring::compose(_("Angle: %1°."),
                                                 Inkscape::ustring::format_classic(std::fixed, std::setprecision(2), angle));
        }

        if (this->_last_over) {
            // TRANSLATORS: The %1 below is where the previous "%1 of %2 nodes selected" sentence gets put.
            auto const dyntip = Glib::ustring::compose(C_("Node tool tip",
                                                          "%1 Drag to select nodes, click to edit only this object (more: Shift)"),
                                                       nodestring);
            this->message_context->set(Inkscape::NORMAL_MESSAGE, dyntip.c_str());
        } else {
            // TRANSLATORS: The %1 below is where the previous "%1 of %2 nodes selected" sentence gets put.
            auto const dyntip = Glib::ustring::compose(C_("Node tool tip",
                                                          "%1 Drag to select nodes, click to clear the selection"),
                                                       nodestring);
            this->message_context->set(Inkscape::NORMAL_MESSAGE, dyntip.c_str());
        }
    } else if (!this->_multipath->empty()) {
        if (this->_last_over) {
            this->message_context->set(Inkscape::NORMAL_MESSAGE, C_("Node tool tip",
                                                                    "Drag to select nodes, click to edit only this object"));
        } else {
            this->message_context->set(Inkscape::NORMAL_MESSAGE, C_("Node tool tip",
                                                                    "Drag to select nodes, click to clear the selection"));
        }
    } else {
        if (this->_last_over) {
            this->message_context->set(Inkscape::NORMAL_MESSAGE, C_("Node tool tip",
                                                                    "Drag to select objects to edit, click to edit this object (more: Shift)"));
        } else {
            this->message_context->set(Inkscape::NORMAL_MESSAGE, C_("Node tool tip",
                                                                    "Drag to select objects to edit"));
        }
    }
}

Inkscape::UI::SelectableControlPoint *NodeTool::_nodeNear(Geom::Point const &window_point, double radius) const
{
    Inkscape::UI::SelectableControlPoint *nearest = nullptr;
    double best = radius;
    for (auto point : _selected_nodes->allPoints()) {
        double const d = Geom::distance(_desktop->d2w(point->position()), window_point);
        if (d <= best) {
            best = d;
            nearest = point;
        }
    }
    return nearest;
}

/**
 * PROOF: Illustrator's Direct Selection press, for a press on an object outside any handle with
 * no modifier. It selects that object alone, inside groups too. With the pointer on one of its
 * anchors only that anchor is selected; otherwise all of them are, and a drag moves the whole
 * object. An unselected object's anchor can so be selected and dragged in one gesture, where
 * Inkscape needed a click to show the anchors first and dragged a selection box over the fill.
 */
bool NodeTool::_directPress(ButtonPressEvent const &event)
{
    if (event.modifiers & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK)) {
        return false;
    }
    auto item = sp_event_context_find_item(_desktop, event.pos, false, true);
    if (!item) {
        return false;
    }

    auto selection = _desktop->getSelection();
    if (selection->single() != item) {
        selection->set(item); // Builds the anchors at once.
    }

    DirectPress press;
    press.origin = _desktop->w2d(event.pos);
    press.origin_w = event.pos;
    press.reference = press.origin;

    auto const tolerance = Preferences::get()->getIntLimited("/options/grabsensitivity/value", 8, 1, 100);
    if (auto node = _nodeNear(event.pos, tolerance)) {
        _selected_nodes->clear();
        _selected_nodes->insert(node);
        press.reference = node->position();
    } else if (!_selected_nodes->allPoints().empty()) {
        _selected_nodes->selectAll();
        if (auto nearest = _nodeNear(event.pos, Geom::infinity())) {
            press.reference = nearest->position();
        }
    } else {
        press.whole_items = true;
    }
    _direct_press = press;
    return true;
}

void NodeTool::_directDrag(MotionEvent const &event)
{
    auto &press = *_direct_press;
    if (!press.moved) {
        auto const tolerance = Preferences::get()->getIntLimited("/options/dragtolerance/value", 0, 0, 100);
        if (Geom::LInfty(event.pos - press.origin_w) < tolerance) {
            return;
        }
        press.moved = true;
    }

    Geom::Point delta = _desktop->w2d(event.pos) - press.origin;
    if (event.modifiers & GDK_SHIFT_MASK) {
        // Horizontal, vertical or diagonal, as in Illustrator.
        double const step = M_PI / 4;
        double const angle = std::round(Geom::atan2(delta) / step) * step;
        delta = Geom::Point::polar(angle, Geom::dot(delta, Geom::Point::polar(angle)));
        _desktop->getSnapIndicator()->remove_snaptarget();
    } else if (!mod_move_no_snapping->active(event.modifiers)) {
        auto &m = _desktop->getNamedView()->snap_manager;
        m.setupIgnoreSelection(_desktop, true);
        Inkscape::SnapCandidatePoint scp(press.reference + delta, Inkscape::SNAPSOURCE_NODE_CUSP);
        auto const sp = m.freeSnap(scp, Geom::OptRect(), false);
        m.unSetup();
        if (sp.getSnapped()) {
            delta = sp.getPoint() - press.reference;
            _desktop->getSnapIndicator()->set_new_snaptarget(sp);
        } else {
            _desktop->getSnapIndicator()->remove_snaptarget();
        }
    }

    auto const step = delta - press.applied;
    if (press.whole_items) {
        _desktop->getSelection()->moveRelative(step);
    } else {
        _selected_nodes->transform(Geom::Translate(step));
    }
    press.applied = delta;

    auto const units = _desktop->getNamedView()->display_units;
    defaultMessageContext()->setF(Inkscape::NORMAL_MESSAGE, _("<b>Move</b> by %s, %s; with <b>Shift</b> to restrict the angle"),
                                  Inkscape::Util::Quantity(delta.x(), "px").string(units).c_str(),
                                  Inkscape::Util::Quantity(delta.y(), "px").string(units).c_str());
    gobble_motion_events(GDK_BUTTON1_MASK);
}

void NodeTool::_directRelease()
{
    auto const press = *_direct_press;
    _direct_press.reset();
    _desktop->getSnapIndicator()->remove_snaptarget();
    if (!press.moved || press.applied == Geom::Point()) {
        return;
    }
    if (press.whole_items) {
        DocumentUndo::done(_desktop->getDocument(), RC_("Undo", "Move"), INKSCAPE_ICON("tool-node-editor"));
    } else {
        _selected_nodes->signal_commit.emit(Inkscape::UI::COMMIT_MOUSE_MOVE);
    }
    update_tip();
}

void NodeTool::_directCancel()
{
    auto const press = *_direct_press;
    _direct_press.reset();
    _desktop->getSnapIndicator()->remove_snaptarget();
    if (press.whole_items) {
        _desktop->getSelection()->moveRelative(-press.applied);
    } else {
        _selected_nodes->transform(Geom::Translate(-press.applied));
    }
    _desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Move canceled."));
}

void NodeTool::select_area(Geom::Path const &path, ButtonReleaseEvent const &event)
{
    if (_multipath->empty()) {
        // if multipath is empty, select rubberbanded items rather than nodes
        auto selection = _desktop->getSelection();
        auto sel_doc = _desktop->dt2doc() * *path.boundsFast();
        auto items = _desktop->getDocument()->getItemsInBox(_desktop->dkey, sel_doc);
        selection->setList(items);
    } else {
        bool add_to = mod_select_add_to->active(event.modifiers);
        bool remove_from = mod_node_remove_from->active(event.modifiers);
        bool invert = mod_node_invert->active(event.modifiers);

        if (remove_from) {
            // D. Shift+Ctrl pressed, removes nodes under box from existing selection.
            _selected_nodes->selectArea(path, true);
        } else {
            if (!add_to) {
                // A/C. No modifier, selects all nodes, or selects all other nodes.
                _selected_nodes->clear();
            }
            // A/B/C. Adds nodes under box to existing selection.
            _selected_nodes->selectArea(path);
            if (invert) {
                // C. Selects the inverse of all nodes under the box.
                _selected_nodes->invertSelection();
            }
        }
    }
}

void NodeTool::select_point(ButtonReleaseEvent const &event)
{
    if (event.button != 1) {
        return;
    }

    auto selection = _desktop->getSelection();

    auto const force_drag = mod_select_force_drag->active(event.modifiers);
    auto item_clicked = sp_event_context_find_item(_desktop, event.pos, force_drag, true);

    if (!item_clicked) { // nothing under cursor
        // if no Shift, deselect
        // if there are nodes selected, the first click should deselect the nodes
        // and the second should deselect the items
        if (!mod_shift(event)) {
            if (_selected_nodes->empty()) {
                selection->clear();
            } else {
                _selected_nodes->clear();
            }
        }
    } else {
        if (mod_shift(event)) {
            selection->toggle(item_clicked);
        } else if (!selection->includes(item_clicked)) {
            selection->set(item_clicked);
        }
    }
}

void NodeTool::mouseover_changed(Inkscape::UI::ControlPoint *p) {
    using Inkscape::UI::CurveDragPoint;

    CurveDragPoint *cdp = dynamic_cast<CurveDragPoint*>(p);

    if (cdp && !this->cursor_drag) {
        this->set_cursor("node-mouseover.svg");
        this->cursor_drag = true;
    } else if (!cdp && this->cursor_drag) {
        this->set_cursor("node.svg");
        this->cursor_drag = false;
    }
}

void NodeTool::handleControlUiStyleChange() {
    this->_multipath->updateHandles();
}

void NodeTool::_updateSelectionColor(CanvasEvent const &event)
{
    auto rubberband = get_rubberband();
    if (mod_node_remove_from->active(event.modifiersAfter())) {
        // if Ctrl+Shift is pressed, change rubberband operation to remove
        rubberband->setOperation(Rubberband::Operation::REMOVE);
    } else if (mod_node_invert->active(event.modifiersAfter())) {
        // Ctrl pressed, it's an invert option
        rubberband->setOperation(Rubberband::Operation::INVERT);
    } else {
        // Nothing pressed, switch back to add
        rubberband->setOperation(Rubberband::Operation::ADD);
    }
}

} // namespace Inkscape::UI::Tools

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
