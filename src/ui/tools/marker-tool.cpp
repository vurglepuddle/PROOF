// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Marker edit mode - onCanvas marker editing of marker orientation, position, scale
 *//*
 * Authors:
 * see git history
 * Rachana Podaralla <rpodaralla3@gatech.edu>
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "document.h"
#include "style.h"
#include "message-context.h"
#include "selection.h"

#include "object/sp-marker.h"

#include "ui/shape-editor.h"
#include "ui/tools/marker-tool.h"
#include "ui/widget/events/canvas-event.h"


namespace Inkscape::UI::Tools {

MarkerTool::MarkerTool(SPDesktop *desktop)
    : ToolBase(desktop, "/tools/marker", "select.svg")
    , mod_select_force_drag(Modifiers::Modifier::get(Modifiers::Type::SELECT_FORCE_DRAG))
{
    Inkscape::Selection *selection = desktop->getSelection();

    sel_changed_connection.disconnect();
    sel_changed_connection = selection->connectChanged(
        sigc::mem_fun(*this, &MarkerTool::selection_changed)
    );
    selection_changed(selection);

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (prefs->getBool("/tools/marker/selcue")) enableSelectionCue();
    if (prefs->getBool("/tools/marker/gradientdrag")) enableGrDrag();
}

MarkerTool::~MarkerTool()
{
    ungrabCanvasEvents();

    message_context->clear();
    _shape_editors.clear();

    enableGrDrag(false);
    sel_changed_connection.disconnect();
}

/*
- cycles through all the selected items to see if any have a marker in the right location (based on enterMarkerMode)
- if a matching item is found, loads the corresponding marker on the shape into the shape-editor and exits the loop
- forces user to only edit one marker at a time
*/
void MarkerTool::selection_changed(Inkscape::Selection *selection) {
    using namespace Inkscape::UI;

    g_assert(_desktop != nullptr);

    SPDocument *doc = _desktop->getDocument();
    g_assert(doc != nullptr);

    auto selected_items = selection->items();
    _shape_editors.clear();

    for(auto item : selected_items){
        if(item) {
            auto shape = cast<SPShape>(item);

            if(shape && shape->hasMarkers() && (editMarkerMode != -1)) {
                SPObject *obj = shape->_marker[editMarkerMode];

                if(obj) {

                    auto sp_marker = cast<SPMarker>(obj);
                    g_assert(sp_marker != nullptr);

                    sp_validate_marker(sp_marker, doc);

                    ShapeRecord sr;
                    switch(editMarkerMode) {
                        case SP_MARKER_LOC_START:
                            sr  = get_marker_transform(shape, item, sp_marker, SP_MARKER_LOC_START);
                            break;

                        case SP_MARKER_LOC_MID:
                            sr  = get_marker_transform(shape, item, sp_marker, SP_MARKER_LOC_MID);
                            break;

                        case SP_MARKER_LOC_END:
                            sr  = get_marker_transform(shape, item, sp_marker, SP_MARKER_LOC_END);
                            break;

                        default:
                            break;
                    }

                    auto si = std::make_unique<ShapeEditor>(_desktop, sr.edit_transform, sr.edit_rotation, editMarkerMode);
                    si->set_item(cast<SPItem>(sr.object));

                    _shape_editors.insert({item, std::move(si)});
                    break;                     
                }
            }
        }
    }
}

// handles selection of new items
bool MarkerTool::root_handler(CanvasEvent const &event)
{
    auto selection = _desktop->getSelection();
    bool ret = false;
    
    inspect_event(event,
        [&] (ButtonPressEvent const &event) {
            if (event.num_press == 1 && event.button == 1) {
                auto force_drag = mod_select_force_drag->active(event.modifiers);
                item_to_select = sp_event_context_find_item(_desktop, event.pos, force_drag, true);

                grabCanvasEvents();
                ret = true;
            }
        },
        [&] (ButtonReleaseEvent const &event) {
            if (event.button == 1) {
                if (item_to_select) {
                    // unselect all items, except for newly selected item
                    selection->set(item_to_select.get());
                } else {
                    // clicked into empty space, deselect any selected items
                    selection->clear();
                }

                item_to_select = nullptr;
                ungrabCanvasEvents();
                ret = true;
            }
        },
        [&] (CanvasEvent const &event) {}
    );

    return ret || ToolBase::root_handler(event);
}

/*
- this function uses similar logic that exists in sp_shape_update_marker_view
- however, the tangent angle needs to be saved here and parent_item->i2dt_affine() needs to also be accounted for in the right places
- calculate where the shape-editor knotholders need to go based on the reference shape
*/
ShapeRecord MarkerTool::get_marker_transform(SPShape* shape, SPItem *parent_item, SPMarker *sp_marker, SPMarkerLoc marker_type)
{

    // scale marker transform with parent stroke width
    SPStyle *style = shape->style;
    SPDocument *doc = _desktop->getDocument();
    Geom::Scale scale = doc->getDocumentScale();

    if(sp_marker->markerUnits == SP_MARKER_UNITS_STROKEWIDTH) {
        scale *= Geom::Scale(style->stroke_width.computed);
    }

    Geom::PathVector const &pathv = *shape->curve();
    Geom::Affine ret = Geom::identity(); //edit_transform
    double angle = 0.0; // edit_rotation - tangent angle used for auto orientation
    Geom::Point p;
    
    if(marker_type == SP_MARKER_LOC_START) {

        Geom::Curve const &c = pathv.begin()->front();
        p = c.pointAt(0);
        ret = Geom::Translate(p * parent_item->i2doc_affine());

        if (!c.isDegenerate()) {
            Geom::Point tang = c.unitTangentAt(0);
            angle = Geom::atan2(tang);
            ret = Geom::Rotate(angle) * ret;
        }

    } else if(marker_type == SP_MARKER_LOC_MID) {
        /* 
        - a shape can have multiple mid markers - only one is needed
        - once a valid mid marker is found, save edit_transfom and edit_rotation and break out of loop
        */
        for(Geom::PathVector::const_iterator path_it = pathv.begin(); path_it != pathv.end(); ++path_it) {

            // mid marker start position
            if (path_it != pathv.begin() && ! ((path_it == (pathv.end()-1)) && (path_it->size_default() == 0)))
            {
                Geom::Curve const &c = path_it->front();
                p = c.pointAt(0);
                ret = Geom::Translate(p * parent_item->i2doc_affine());

                if (!c.isDegenerate()) {
                    Geom::Point tang = c.unitTangentAt(0);
                    angle = Geom::atan2(tang);
                    ret = Geom::Rotate(angle) * ret;
                    break;
                }
            }

            // mid marker mid positions
            if ( path_it->size_default() > 1) {
                Geom::Path::const_iterator curve_it1 = path_it->begin();
                Geom::Path::const_iterator curve_it2 = ++(path_it->begin());
                while (curve_it2 != path_it->end_default())
                {
                    Geom::Curve const & c1 = *curve_it1;
                    Geom::Curve const & c2 = *curve_it2;

                    p = c1.pointAt(1);
                    Geom::Curve * c1_reverse = c1.reverse();
                    Geom::Point tang1 = - c1_reverse->unitTangentAt(0);
                    delete c1_reverse;
                    Geom::Point tang2 = c2.unitTangentAt(0);

                    double const angle1 = Geom::atan2(tang1);
                    double const angle2 = Geom::atan2(tang2);

                    angle = .5 * (angle1 + angle2);

                    if ( fabs( angle2 - angle1 ) > M_PI ) {
                        angle += M_PI;
                    }

                    ret = Geom::Rotate(angle) * Geom::Translate(p * parent_item->i2doc_affine());

                    ++curve_it1;
                    ++curve_it2;
                    break;
                }
            }

            // mid marker end position
            if ( path_it != (pathv.end()-1) && !path_it->empty()) {
                Geom::Curve const &c = path_it->back_default();
                p = c.pointAt(1);
                ret = Geom::Translate(p * parent_item->i2doc_affine());

                if ( !c.isDegenerate() ) {
                    Geom::Curve * c_reverse = c.reverse();
                    Geom::Point tang = - c_reverse->unitTangentAt(0);
                    delete c_reverse;
                    angle = Geom::atan2(tang);
                    ret = Geom::Rotate(angle) * ret;
                    break;
                } 
            }
        }

    } else if (marker_type == SP_MARKER_LOC_END) {

        Geom::Path const &path_last = pathv.back();
        unsigned int index = path_last.size_default();
        if (index > 0) index--;

        Geom::Curve const &c = path_last[index];
        p = c.pointAt(1);
        ret = Geom::Translate(p * parent_item->i2doc_affine());

        if ( !c.isDegenerate() ) {
            Geom::Curve * c_reverse = c.reverse();
            Geom::Point tang = - c_reverse->unitTangentAt(0);
            delete c_reverse;
            angle = Geom::atan2(tang);
            ret = Geom::Rotate(angle) * ret;
        } 
    }

    /* scale by stroke width */
    ret = scale * ret;
    /* account for parent transform */
    ret = parent_item->transform.withoutTranslation() * ret;

    ShapeRecord sr;
    sr.object = sp_marker;
    sr.edit_transform = ret;
    sr.edit_rotation = angle * 180.0/M_PI;
    sr.role = SHAPE_ROLE_NORMAL;
    return sr;
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
