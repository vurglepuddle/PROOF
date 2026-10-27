// SPDX-License-Identifier: GPL-2.0-or-later
#include "selection-drag.h"

#include <cmath>

#include "document-undo.h"
#include "document.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-lpe-item.h"
#include "ui/icon-names.h"

namespace Inkscape {

Geom::Point constrain_selection_move(Geom::Point delta)
{
    if (std::abs(delta[Geom::X]) > std::abs(delta[Geom::Y])) {
        delta[Geom::Y] = 0;
    } else {
        delta[Geom::X] = 0;
    }
    return delta;
}

SelectionDrag::SelectionDrag(ObjectSet &selection, unsigned display_key)
    : _selection(selection)
    , _display_key(display_key)
{
    for (auto item : selection.items()) {
        _items.push_back(static_cast<SPItem *>(sp_object_ref(item, nullptr)));
        _affines.push_back(item->i2dt_affine());
        if (auto bounds = item->desktopVisualBounds()) {
            _bounds.push_back(*bounds);
        }
    }
}

SelectionDrag::~SelectionDrag()
{
    if (!_finished) {
        finish(Geom::identity(), true);
    }
    for (auto item : _items) {
        sp_object_unref(item, nullptr);
    }
}

void SelectionDrag::finish(Geom::Affine affine, bool cancel)
{
    if (_finished) return;
    _finished = true;

    std::vector<SPItem *> originals;
    for (unsigned i = 0; i < _items.size(); ++i) {
        auto item = _items[i];
        item->setTransformPreview(_display_key, std::nullopt);
        if (item->document != _selection.document()) continue;
        item->set_i2d_affine(_affines[i]);
        originals.push_back(item);
    }
    auto doc = _selection.document();
    if (cancel) {
        // Stamps and any live-effect writes are still part of this drag's transaction.
        DocumentUndo::cancel(doc);
        _selection.setList(originals);
        return;
    }

    _selection.setList(originals);
    if (_duplicate && !originals.empty()) {
        _selection.duplicate(true);
        doc->ensureUpToDate();
    }
    if (!affine.isIdentity()) {
        _selection.applyAffine(affine);
        // As SelTrans's own commit does: path effects recompute from the new position.
        for (auto item : _selection.items()) {
            if (auto lpeitem = cast<SPLPEItem>(item)) {
                sp_lpe_item_update_patheffect(lpeitem, true, true);
            }
        }
    }
    DocumentUndo::done(doc, _duplicate ? RC_("Undo", "Duplicate") : RC_("Undo", "Move"),
                       _duplicate ? INKSCAPE_ICON("edit-duplicate") : INKSCAPE_ICON("tool-pointer"));
}

} // namespace Inkscape
