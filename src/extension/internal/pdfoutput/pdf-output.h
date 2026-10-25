// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Output an SVG to a PDF using capypdf
 *
 * Authors:
 *   Martin Owens <doctormo@geek-2.com?
 *
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef EXTENSION_INTERNAL_PDFOUTPUT_H
#define EXTENSION_INTERNAL_PDFOUTPUT_H

#include "extension/implementation/implementation.h"

namespace Inkscape::Extension::Internal {

class PdfOutput : public Inkscape::Extension::Implementation::Implementation
{
public:
    bool check(Inkscape::Extension::Extension *module) override;
    std::optional<bool> output_prefs(Inkscape::Extension::Output *module, bool settings) override;
    void save(Inkscape::Extension::Output *mod, SPDocument *doc, char const *filename) override;
    static void init();
    static bool supports_pdf17();
};

} // namespace Inkscape::Extension::Internal

#endif /* !EXTENSION_INTERNAL_PDFOUTPUT_H */

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
