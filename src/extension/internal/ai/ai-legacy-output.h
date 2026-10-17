// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_LEGACY_OUTPUT_H
#define SEEN_AI_LEGACY_OUTPUT_H

#include "extension/implementation/implementation.h"

namespace Inkscape::Extension::Internal {

/// Copy-only Illustrator 7 export, using the same artwork writer as modern native Save As.
class AiLegacyOutput : public Implementation::Implementation
{
public:
    bool check(Inkscape::Extension::Extension *) override { return true; }
    void save(Inkscape::Extension::Output *mod, SPDocument *doc, char const *filename) override;
    static void init();
};

} // namespace Inkscape::Extension::Internal

#endif // SEEN_AI_LEGACY_OUTPUT_H
