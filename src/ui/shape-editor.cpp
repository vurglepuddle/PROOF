// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Inkscape::ShapeEditor
 * This is a container class which contains a knotholder for shapes.
 * It is attached to a single item.
 *//*
 * Authors: see git history
 *   bulia byak <buliabyak@users.sf.net>
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "shape-editor.h"

#include "desktop.h"
#include "seltrans.h"
#include "live_effects/effect.h"
#include "object/sp-lpe-item.h"
#include "shape-editor-knotholders.h"
#include "ui/knot/knot-holder.h"

namespace Inkscape {
namespace UI {

bool ShapeEditor::_blockSetItem = false;

ShapeEditor::ShapeEditor(SPDesktop *dt, Geom::Affine edit_transform, double edit_rotation, int edit_marker_mode)
    : desktop(dt)
    , _edit_transform(edit_transform)
    , _edit_rotation(edit_rotation)
    , _edit_marker_mode(edit_marker_mode)
{
}

ShapeEditor::~ShapeEditor() {
    unset_item();
}

void ShapeEditor::useTransformBox()
{
    unset_item();
    _transform_box = std::make_unique<SelTrans>(desktop);
}

void ShapeEditor::setDrawing(bool drawing)
{
    if (_transform_box) _transform_box->setBoxesHidden(drawing);
}

void ShapeEditor::setPointer(Geom::Point const &point)
{
    if (_transform_box) _transform_box->setPointer(point);
}

void ShapeEditor::unset_item(bool keep_knotholder) {
    if (knotholder) {
        Inkscape::XML::Node *old_repr = knotholder->repr;
        if (old_repr && old_repr == knotholder_listener_attached_for) {
            old_repr->removeObserver(*this);
            Inkscape::GC::release(old_repr);
            knotholder_listener_attached_for = nullptr;
        }

        if (!keep_knotholder) {
            knotholder.reset();
        }
    }
    if (lpeknotholder) {
        Inkscape::XML::Node *old_repr = lpeknotholder->repr;
        bool remove = false;
        if (old_repr && old_repr == lpeknotholder_listener_attached_for) {
            old_repr->removeObserver(*this);
            Inkscape::GC::release(old_repr);
            remove = true;
        }

        if (!keep_knotholder) {
            lpeknotholder.reset();
        }
        if (remove) {
            lpeknotholder_listener_attached_for = nullptr;
        }
    }
}

void ShapeEditor::update_knotholder() {
    if (knotholder) {
        knotholder->update_knots();
    }
    if (lpeknotholder) {
        lpeknotholder->update_knots();
    }
}

bool ShapeEditor::has_local_change() const
{
    return (knotholder && knotholder->local_change) || (lpeknotholder && lpeknotholder->local_change);
}

void ShapeEditor::decrement_local_change() {
    if (knotholder) {
        knotholder->local_change = false;
    }
    if (lpeknotholder) {
        lpeknotholder->local_change = false;
    }
}

void ShapeEditor::notifyAttributeChanged(Inkscape::XML::Node&, GQuark,
                                         Inkscape::Util::ptr_shared,
                                         Inkscape::Util::ptr_shared)
{
    bool changed_kh = false;

    if (has_knotholder()) {
        changed_kh = !has_local_change();
        decrement_local_change();
        if (changed_kh) {
            reset_item();
        }
    }
}


void ShapeEditor::set_item(SPItem *item) {
    if (_blockSetItem) {
        return;
    }
    // this happens (and should only happen) when for an LPEItem having both knotholder and
    // nodepath the knotholder is adapted; in this case we don't want to delete the knotholder
    // since this freezes the handles
    unset_item(true);

    if (item) {
        Inkscape::XML::Node *repr;
        if (!knotholder && !_transform_box) {
            // only recreate knotholder if none is present
            knotholder = create_knot_holder(item, desktop, _edit_rotation, _edit_marker_mode);
        }
        auto lpe = cast<SPLPEItem>(item);
        if (!(lpe &&
            lpe->getCurrentLPE() &&
            lpe->getCurrentLPE()->isVisible() &&
            lpe->getCurrentLPE()->providesKnotholder()))
        {
            lpeknotholder.reset();
        }
        if (!lpeknotholder) {
            // only recreate knotholder if none is present
            lpeknotholder = create_LPE_knot_holder(item, desktop);
        }
        if (knotholder) {
            knotholder->install_modification_watch(); // let knotholder know item's attribute may have changed
            knotholder->setEditTransform(_edit_transform);
            knotholder->update_knots();
            // setting new listener
            repr = knotholder->repr;
            if (repr != knotholder_listener_attached_for) {
                Inkscape::GC::anchor(repr);
                repr->addObserver(*this);
                knotholder_listener_attached_for = repr;
            }
        }
        if (this->lpeknotholder) {
            this->lpeknotholder->setEditTransform(_edit_transform);
            this->lpeknotholder->update_knots();
            // setting new listener
            repr = this->lpeknotholder->repr;
            if (repr != lpeknotholder_listener_attached_for) {
                Inkscape::GC::anchor(repr);
                repr->addObserver(*this);
                lpeknotholder_listener_attached_for = repr;
            }
        }
    }
}


/** FIXME: This thing is only called when the item needs to be updated in response to repr change.
   Why not make a reload function in KnotHolder? */
void ShapeEditor::reset_item()
{
    if (knotholder) {
        SPObject *obj = desktop->getDocument()->getObjectByRepr(knotholder_listener_attached_for); /// note that it is not certain that this is an SPItem; it could be a LivePathEffectObject.
        set_item(cast<SPItem>(obj));
    } else if (lpeknotholder) {
        SPObject *obj = desktop->getDocument()->getObjectByRepr(lpeknotholder_listener_attached_for); /// note that it is not certain that this is an SPItem; it could be a LivePathEffectObject.
        set_item(cast<SPItem>(obj));
    }
}

/**
 * Returns true if this ShapeEditor has a knot above which the mouse currently hovers.
 */
bool ShapeEditor::knot_mouseover() const {
    if (this->knotholder) {
        return knotholder->knot_mouseover();
    }
    if (this->lpeknotholder) {
        return lpeknotholder->knot_mouseover();
    }

    return false;
}

} // namespace UI
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
