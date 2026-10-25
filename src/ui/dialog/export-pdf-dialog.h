// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_UI_DIALOG_EXPORT_PDF_DIALOG_H
#define SEEN_UI_DIALOG_EXPORT_PDF_DIALOG_H

#include <string>

namespace Inkscape::Extension { class Output; }

namespace Inkscape::UI::Dialog {

struct PdfExportSettings
{
    std::string version = "PDF-1.7";
    std::string text = "embed";
    bool strokes = false;
    bool rasterize = true;
    int resolution = 300;

    static PdfExportSettings load(Extension::Output const &output);
    void apply(Extension::Output &output) const;
    bool has_warnings() const { return version == "PDF-2.0" || text != "embed" || strokes; }
};

bool export_pdf_dialog(Extension::Output &output, bool settings);

} // namespace Inkscape::UI::Dialog

#endif
