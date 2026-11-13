// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_EXTENSION_MISSING_LINK_SAVE_H
#define INKSCAPE_EXTENSION_MISSING_LINK_SAVE_H

#include "document.h"
#include "export-report.h"
#include "object/sp-image.h"
#include <stdexcept>

namespace Inkscape::Extension {

// Relink a copy with the existing picker logic. Cancel never edits the source or its undo log.
class MissingLinkSave {
public:
    explicit MissingLinkSave(SPDocument *source)
        : _original(sp_images_missing(source)), _copy(source->copy()), _staged(sp_images_missing(_copy.get()))
    {
        if (_original.size() != _staged.size()) throw std::runtime_error("Missing links changed while preparing save.");
    }
    SPDocument *document() const { return _copy.get(); }
    bool accept(MissingLinkAction action, SPDocument *working_document = nullptr)
    {
        if (action == MissingLinkAction::Cancel ||
            (action == MissingLinkAction::RequirePresent && !sp_images_missing(_copy.get()).empty())) return false;
        for (std::size_t i = 0; i < _staged.size(); ++i) {
            if (!_staged[i]->linkMissing()) {
                auto const file = _staged[i]->linkedFile();
                _original[i]->relink(file);
                // Export panel callers supply a document copy. Keep accepted relinks in the
                // working document too, where the caller records one ordinary undo step.
                if (working_document && working_document != _original[i]->document && _original[i]->getId()) {
                    if (auto *image = cast<SPImage>(working_document->getObjectById(_original[i]->getId()))) {
                        image->relink(file);
                    }
                }
                changed = true;
            }
        }
        return true;
    }
    bool changed = false;
private:
    std::vector<SPImage *> _original;
    std::unique_ptr<SPDocument> _copy;
    std::vector<SPImage *> _staged;
};
} // namespace Inkscape::Extension
#endif
