// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Inkscape::ShapeEditor
 * This is a container class which contains a knotholder for shapes.
 * It is attached to a single item.
 *//*
 * Authors: see git history
 *   bulia byak <buliabyak@users.sf.net>
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_SHAPE_EDITOR_H
#define SEEN_SHAPE_EDITOR_H

#include <2geom/affine.h>
#include <memory>

#include "xml/node-observer.h"

class KnotHolder;
class LivePathEffectObject;
class SPDesktop;
class SPItem;

namespace Inkscape { class SelTrans; namespace XML { class Node; }
namespace UI {

class ShapeEditor : private XML::NodeObserver
{
public:

    ShapeEditor(SPDesktop *desktop, Geom::Affine edit_transform = Geom::identity(), double edit_rotation = 0.0, int edit_marker_mode = -1);
    ~ShapeEditor() override;

    void set_item(SPItem *item);
    void unset_item(bool keep_knotholder = false);
    /// Shape drawing tools share the selector's box; other editors keep native knots.
    void useTransformBox();
    void setDrawing(bool drawing);
    void setPointer(Geom::Point const &point);

    void update_knotholder(); //((deprecated))

    bool has_local_change() const;
    void decrement_local_change();

    bool knot_mouseover() const;
    std::unique_ptr<KnotHolder> knotholder;
    std::unique_ptr<KnotHolder> lpeknotholder;
    bool has_knotholder() const { return knotholder || lpeknotholder; }

    static void blockSetItem(bool b) { _blockSetItem = b; } // kludge
private:
    void reset_item();
    static bool _blockSetItem;
    std::unique_ptr<SelTrans> _transform_box;

    SPDesktop *desktop;
    Inkscape::XML::Node *knotholder_listener_attached_for{nullptr};
    Inkscape::XML::Node *lpeknotholder_listener_attached_for{nullptr};
    Geom::Affine _edit_transform;
    double _edit_rotation;
    int _edit_marker_mode;

    void notifyAttributeChanged(Inkscape::XML::Node &node, GQuark key, Inkscape::Util::ptr_shared oldvalue, Inkscape::Util::ptr_shared newval) final;
};

} // namespace UI
} // namespace Inkscape

#endif // SEEN_SHAPE_EDITOR_H


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

