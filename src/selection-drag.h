// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_SELECTION_DRAG_H
#define INKSCAPE_SELECTION_DRAG_H

#include <vector>
#include <2geom/affine.h>
#include <2geom/rect.h>

class SPItem;

namespace Inkscape {

class ObjectSet;

Geom::Point constrain_selection_move(Geom::Point delta);

// A move previews the original items in memory; only the drop decides whether to copy them.
class SelectionDrag
{
public:
    explicit SelectionDrag(ObjectSet &selection, unsigned display_key = 0);
    ~SelectionDrag();
    SelectionDrag(SelectionDrag const &) = delete;
    SelectionDrag &operator=(SelectionDrag const &) = delete;

    void setDuplicate(bool duplicate) { _duplicate = duplicate; }
    bool isDuplicate() const { return _duplicate; }
    std::vector<Geom::Rect> const &bounds() const { return _bounds; }
    void finish(Geom::Affine affine, bool cancel = false);

private:
    ObjectSet &_selection;
    unsigned _display_key;
    std::vector<SPItem *> _items;
    std::vector<Geom::Affine> _affines;
    std::vector<Geom::Rect> _bounds;
    bool _duplicate = false;
    bool _finished = false;
};

} // namespace Inkscape

#endif // INKSCAPE_SELECTION_DRAG_H
