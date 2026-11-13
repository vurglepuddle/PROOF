// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_EXTENSION_MISSING_LINK_SAVE_H
#define INKSCAPE_EXTENSION_MISSING_LINK_SAVE_H

#include "document.h"
#include "export-report.h"
#include "object/sp-image.h"
#include "object/sp-clippath.h"
#include "object/sp-mask.h"
#include "xml/repr.h"
#include <stdexcept>

namespace Inkscape::Extension {

inline bool can_discard_missing_picture(SPImage const *image)
{
    if (!image || !image->linkMissing() || !image->getRepr()->parent()) return false;
    for (auto *parent = image->parent; parent; parent = parent->parent) {
        if (is<SPClipPath>(parent) || is<SPMask>(parent)) return false;
    }
    return true;
}

inline bool discard_missing_picture(SPImage *image)
{
    if (!can_discard_missing_picture(image)) return false;
    // Only remove the image XML node. Do not cascade through containers or clone references.
    sp_repr_unparent(image->getRepr());
    return true;
}

// Edit a copy. Cancel never edits the source or its undo log.
class MissingLinkSave {
public:
    explicit MissingLinkSave(SPDocument *source)
        : _original(sp_images_missing(source)), _copy(source->copy()), _staged(sp_images_missing(_copy.get())),
          _discarded(_staged.size(), false)
    {
        if (_original.size() != _staged.size()) throw std::runtime_error("Missing links changed while preparing save.");
    }
    SPDocument *document() const { return _copy.get(); }
    bool can_discard(SPImage const *image) const
    {
        for (std::size_t i = 0; i < _staged.size(); ++i) {
            if (_staged[i] == image && !_discarded[i]) {
                return _original[i]->getId() && can_discard_missing_picture(image);
            }
        }
        return false;
    }
    bool discard(SPImage *image)
    {
        if (!can_discard(image)) return false;
        for (std::size_t i = 0; i < _staged.size(); ++i) {
            if (_staged[i] == image) {
                _discarded[i] = true;
                _staged[i] = nullptr; // The SPImage may be destroyed by unparenting its node.
                return discard_missing_picture(image);
            }
        }
        return false;
    }
    bool accept(MissingLinkAction action, SPDocument *working_document = nullptr)
    {
        if (_accepted || action == MissingLinkAction::Cancel ||
            (action == MissingLinkAction::RequirePresent && !sp_images_missing(_copy.get()).empty())) return false;
        // Validate every mirror before changing either document. A missing ID cannot be
        // matched safely across an Export-panel copy and the working document.
        std::vector<SPImage *> mirrors(_original.size(), nullptr);
        for (std::size_t i = 0; i < _staged.size(); ++i) {
            if (!_discarded[i] && _staged[i]->linkMissing()) continue;
            if (working_document && working_document != _original[i]->document) {
                auto const *id = _original[i]->getId();
                if (!id || !(mirrors[i] = cast<SPImage>(working_document->getObjectById(id)))) return false;
            }
            if (_discarded[i] && (!can_discard_missing_picture(_original[i]) ||
                (mirrors[i] && !can_discard_missing_picture(mirrors[i])))) return false;
        }
        for (std::size_t i = 0; i < _staged.size(); ++i) {
            if (_discarded[i]) {
                discard_missing_picture(_original[i]);
                if (mirrors[i]) discard_missing_picture(mirrors[i]);
                changed = true;
            } else if (!_staged[i]->linkMissing()) {
                auto const file = _staged[i]->linkedFile();
                _original[i]->relink(file);
                if (mirrors[i]) mirrors[i]->relink(file);
                changed = true;
            }
        }
        _accepted = true;
        return true;
    }
    bool changed = false;
private:
    std::vector<SPImage *> _original;
    std::unique_ptr<SPDocument> _copy;
    std::vector<SPImage *> _staged;
    std::vector<bool> _discarded;
    bool _accepted = false;
};
} // namespace Inkscape::Extension
#endif
