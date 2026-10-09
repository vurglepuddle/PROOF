// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Per-desktop selection container
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   MenTaLguY <mental@rydia.net>
 *   bulia byak <buliabyak@users.sf.net>
 *   Andrius R. <knutux@gmail.com>
 *   Abhishek Sharma
 *   Adrian Boguszewski
 *
 * Copyright (C) 2016 Adrian Boguszewski
 * Copyright (C) 2006 Andrius R.
 * Copyright (C) 2004-2005 MenTaLguY
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "selection.h"

#include <2geom/path-sink.h>
#include <cmath>
#include <glibmm/i18n.h>

#include "box-frame.h"
#include "desktop.h"
#include "ui/widget/canvas/framecheck.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "layer-manager.h"
#include "page-manager.h"

#include "object/sp-defs.h"
#include "object/sp-page.h"
#include "object/sp-shape.h"
#include "ui/icon-names.h"
#include "ui/tool/control-point-selection.h"
#include "ui/tool/path-manipulator.h"
#include "ui/tools/node-tool.h"

static constexpr auto SP_SELECTION_UPDATE_PRIORITY = G_PRIORITY_HIGH_IDLE + 1;

namespace Inkscape {

Selection::Selection(SPDesktop *desktop)
    : ObjectSet(desktop)
{}

Selection::Selection(SPDocument *document)
    : ObjectSet(document)
{}

Selection::~Selection() {
    if (_idle) {
        g_source_remove(_idle);
        _idle = 0;
    }
}

/* Handler for selected objects "modified" signal */

void Selection::_schedule_modified(SPObject */*obj*/, guint flags) {
    if (!this->_idle) {
        /* Request handling to be run in _idle loop */
        this->_idle = g_idle_add_full(SP_SELECTION_UPDATE_PRIORITY, GSourceFunc(&Selection::_emit_modified), this, nullptr);
    }

    /* Collect all flags */
    this->_flags |= flags;
}

gboolean Selection::_emit_modified(Selection *selection)
{
    /* force new handler to be created if requested before we return */
    selection->_idle = 0;
    guint flags = selection->_flags;
    selection->_flags = 0;

    selection->_emitModified(flags);

    /* drop this handler */
    return FALSE;
}

void Selection::_emitModified(guint flags)
{
    auto framecheck = FrameCheck::maybe("selection_modified");
    _modified_signal.emit(this, flags);

    if (!_desktop || isEmpty()) {
        return;
    }

    auto &pm = _document->getPageManager();

    // If the selected items have been moved to a new page...
    if (auto item = singleItem()) {
        pm.selectPage(item, false);
    } else {
        SPPage *page = pm.getPageFor(firstItem(), true);
        for (auto this_item : this->items()) {
            if (page != pm.getPageFor(this_item, true)) {
                return;
            }
        }
        pm.selectPage(page);
    }
}

void Selection::_emitChanged(bool persist_selection_context)
{
    ObjectSet::_emitChanged();
    if (persist_selection_context) {
        if (nullptr == _selection_context) {
            _selection_context = _desktop->layerManager().currentLayer();
            sp_object_ref(_selection_context, nullptr);
            _context_release_connection = _selection_context->connectRelease(sigc::mem_fun(*this, &Selection::_releaseContext));
        }
    } else {
        _releaseContext(_selection_context);
    }

    /** Change the layer selection to the item selection
      * Only change if there's a single object
      */
    if (_document && _desktop) {
        if (auto item = singleItem()) {
            // whether to change the layer with the selection
            // defaults to true (see src/ui/tools/tool-base)
            if (_change_layer) {
                auto layer = _desktop->layerManager().layerForObject(item);
                if (layer && layer != _selection_context) {
                    _desktop->layerManager().setCurrentLayer(layer);
                }
            }
            // whether to change the page with the selection
            // defaults to true (see src/ui/tools/tool-base)
            if (_change_page) {
                // This could be more complex if we want to be smarter.
                _document->getPageManager().selectPage(item, false);
            }
        }
        DocumentUndo::resetKey(_document);
    }

    _changed_signal.emit(this);
}

void Selection::_releaseContext(SPObject *obj)
{
    if (!_selection_context || _selection_context != obj) {
        return;
    }

    _context_release_connection.disconnect();

    sp_object_unref(_selection_context, nullptr);
    _selection_context = nullptr;
}

SPObject *Selection::activeContext()
{
    if (_selection_context) {
        return _selection_context;
    }
    return _desktop->layerManager().currentLayer();
}

std::vector<Inkscape::SnapCandidatePoint> Selection::getSnapPoints(SnapPreferences const *snapprefs) const {
    std::vector<Inkscape::SnapCandidatePoint> p;

    if (snapprefs != nullptr){
        SnapPreferences snapprefs_dummy = *snapprefs; // create a local copy of the snapping prefs
        snapprefs_dummy.setTargetSnappable(Inkscape::SNAPTARGET_ROTATION_CENTER, false); // locally disable snapping to the item center
        auto items = const_cast<Selection *>(this)->items();
        for (auto this_item : items) {
            this_item->getSnappoints(p, &snapprefs_dummy);

            //Include the transformation origin for snapping
            //For a selection or group only the overall center is considered, not for each item individually
            if (snapprefs->isTargetSnappable(Inkscape::SNAPTARGET_ROTATION_CENTER)) {
                p.emplace_back(this_item->getCenter(), SNAPSOURCE_ROTATION_CENTER);
            }
        }
    }

    return p;
}

void Selection::setAnchor(double x, double y, bool set)
{
    constexpr double epsilon = 1e-12;
    auto const pt = Geom::Point{x, y};
    if (Geom::LInfty(anchor - pt) > epsilon || set != has_anchor) {
        anchor = pt;
        has_anchor = set;
        _emitModified(SP_OBJECT_MODIFIED_FLAG);

        // This allows each anchored-event to have it's own maybeDone
        DocumentUndo::resetKey(document());
    }
}

void Selection::scaleAnchored(double amount, bool fixed)
{
    if (Geom::OptRect bbox = visualBounds()) {
        // Scale the amount by the size to get the final scale amount
        if (fixed) {
            double const max_len = bbox->maxExtent();
            if (max_len + amount <= 1e-3) {
                return;
            }
            amount = 1.0 + amount / max_len;
        }

        auto center = has_anchor ? bbox->min() + bbox->dimensions() * Geom::Scale(anchor) : bbox->midpoint();
        scaleRelative(center, Geom::Scale(amount, amount));

        DocumentUndo::maybeDone(document(),
                                ((amount > 0) ? "selector:grow:larger" : "selector:grow:smaller" ),
                                ((amount > 0) ? RC_("Undo", "Grow") : RC_("Undo", "Shrink")), INKSCAPE_ICON("tool-pointer"));
    }
}

void Selection::rotateAnchored(double angle_degrees, double zoom)
{
    if (Geom::OptRect bbox = visualBounds()) {
        auto actionkey = document()->action_key();

        auto const type = Preferences::get()->getInt("/tools/bounding_box") == 0
                            ? SPItem::VISUAL_BBOX : SPItem::GEOMETRIC_BBOX;
        auto const mid = box_rotation_center(items_vector(), type).value_or(bbox->midpoint());
        auto center = has_anchor ? bbox->min() + bbox->dimensions() * Geom::Scale(anchor) : mid;

        // Remember the center for previous rotations with the same undo action
        if (has_anchor && (actionkey == "selector:rotate:ccw" || actionkey == "selector:rotate:cw")) {
            center = _previous_rotate_anchor;
        }

        if (zoom != 1.0) {
            Geom::Point m = bbox->midpoint();
            unsigned i = 0;
            if (center[Geom::X] < m[Geom::X]) { 
                i = 1;
            }
            if (center[Geom::Y] < m[Geom::Y]) {
                i = 3 - i;
            }

            double const r = Geom::L2(bbox->corner(i) - center);
            angle_degrees = 180 * atan2(angle_degrees / zoom, r) / M_PI;
        }

        rotateRelative(center, angle_degrees);

        // Remember the rotation anchor for multiple rotation events.
        _previous_rotate_anchor = center;

        // Visual angle might be different from actual angle,
        // when y axis points up.
        double visual_angle = angle_degrees * document()->yaxisdir();

        if (visual_angle == 90.0) {
            DocumentUndo::maybeDone(document(), "selector:rotate:cw", RC_("Undo", "Rotate 90\xc2\xb0 CW"), INKSCAPE_ICON("object-rotate-right"));
        } else if (visual_angle == -90.0) {
            DocumentUndo::maybeDone(document(), "selector:rotate:ccw", RC_("Undo", "Rotate 90\xc2\xb0 CCW"), INKSCAPE_ICON("object-rotate-left"));
        } else {
            DocumentUndo::maybeDone(document(), ((visual_angle > 0) ? "selector:rotate:ccw" : "selector:rotate:cw"),
                                    RC_("Undo", "Rotate"), INKSCAPE_ICON("tool-pointer"));
        }
    }
}


SPObject *Selection::_objectForXMLNode(Inkscape::XML::Node *repr) const {
    g_return_val_if_fail(repr != nullptr, NULL);
    auto object = _document->getObjectByRepr(repr);
    assert(object == _document->getObjectById(repr->attribute("id")));
    return object;
}

std::pair<size_t, size_t> Selection::selectionDistinctLayerAndParentCounts() {
    // Set to count unique parents and layers only.
    std::unordered_set<SPObject*> layers;
    std::unordered_set<SPObject*> parents;
    auto items = this->items();
    auto &layerManager = _desktop->layerManager();

    for (auto const item : items) {
        auto const parent = item->parent;
        auto const group = cast<SPGroup>(parent);
        parents.insert(parent);

        if (group && group->isLayer()) {
            layers.insert(parent);
        } else {
            SPObject *layer = layerManager.layerForObject(item);
            layers.insert(layer);
        }

    }

    return {layers.size(), parents.size()};
}

void Selection::_connectSignals(SPObject *object) {
    _modified_connections[object] = object->connectModified(sigc::mem_fun(*this, &Selection::_schedule_modified));
}

void Selection::_releaseSignals(SPObject *object) {
    _modified_connections.erase(object);
}

SelectionState Selection::getState()
{
    SelectionState state;

    // Get IDs of selected objects
    for (auto const * const item : items()) {
        if (auto id = item->getId()) {
            state.selected_ids.emplace_back(id);
        }
    }

    // If node tool is active, get selected nodes
    if (SPDesktop *desktop = this->desktop()) {
        if (auto tool = dynamic_cast<Inkscape::UI::Tools::NodeTool *>(desktop->getTool())) {
            for (auto const point : tool->_selected_nodes->_points_list) {
                auto const node = dynamic_cast<Inkscape::UI::Node const *>(point);
                if (!node)
                    continue;

                auto const &nodeList = node->nodeList();
                auto const &subpathList = nodeList.subpathList();

                // Find subpath index
                int sp = 0;
                bool found_sp = false;
                for (auto i = subpathList.begin(), e = subpathList.end(); i != e; ++i, ++sp) {
                    if (&**i == &nodeList) {
                        found_sp = true;
                        break;
                    }
                }

                // Find node index
                int nl = 0;
                bool found_nl = false;
                for (auto j = nodeList.begin(), e = nodeList.end(); j != e; ++j, ++nl) {
                    if (&*j == node) {
                        found_nl = true;
                        break;
                    }
                }

                if (!(found_nl && found_sp)) {
                    g_warning("Something went wrong while trying to get node info. Please report a bug.");
                    continue;
                }

                if (auto id = subpathList.pm().item()->getId()) {
                    state.selected_nodes.emplace_back(id, sp, nl);
                }
            }
        }
    }

    return state;
}

void Selection::setState(SelectionState const &state)
{
    SPDesktop *desktop = this->desktop();
    SPDocument *document = SP_ACTIVE_DOCUMENT;
    SPDefs * defs = document->getDefs();
    Inkscape::UI::Tools::NodeTool *tool = nullptr;
    if (desktop) {
        if (auto nt = dynamic_cast<Inkscape::UI::Tools::NodeTool*>(desktop->getTool())) {
            tool = nt;
        }
    }

    // update selection
    std::vector<SPItem *> new_selection;
    for (auto const &selected_id : state.selected_ids) {
        auto const item = cast<SPItem>(document->getObjectById(selected_id.c_str()));
        if (item && !defs->isAncestorOf(item)) {
            new_selection.push_back(item);
        }
    }
    if (size())
        clear();
    add(new_selection.begin(), new_selection.end());
    new_selection.clear();

    if (!tool) return;

    auto const cps = tool->_selected_nodes;
    cps->selectAll();
    auto const point = !cps->_points_list.empty() ? cps->_points_list.front() : nullptr;
    cps->clear();
    if (!point) return;

    auto const node = dynamic_cast<Inkscape::UI::Node const *>(point);
    if (!node) return;

    auto const &sp = node->nodeList().subpathList();
    for (auto const &node_state : state.selected_nodes) {
        int sp_count = 0;
        for (auto j = sp.begin(); j != sp.end(); ++j, ++sp_count) {
            if (sp_count != node_state.subpath_index)
                continue;

            int nt_count = 0;
            for (auto k = (*j)->begin(); k != (*j)->end(); ++k, ++nt_count) {
                if (nt_count == node_state.node_index) {
                    cps->insert(k.ptr());
                    break;
                }
            }
            break;
        }
    }
}

} // namespace Inkscape

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
