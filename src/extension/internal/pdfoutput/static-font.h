// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_PDF_STATIC_FONT_H
#define PROOF_PDF_STATIC_FONT_H

#include <memory>
#include <string>

class FontInstance;

namespace Inkscape::Extension::Internal::PdfBuilder {

/// A private PDF-only font instance; the installed variable font is never modified.
class StaticFont
{
public:
    explicit StaticFont(std::string path) : path(std::move(path)) {}
    ~StaticFont();
    StaticFont(StaticFont const &) = delete;
    StaticFont &operator=(StaticFont const &) = delete;
    std::string const path;
};

/// Freeze all actual loaded axes, retaining glyph IDs; CFF2 becomes embeddable CFF1.
/// Throws on failure rather than embedding the default instance or outlining the text.
std::unique_ptr<StaticFont> static_font(FontInstance const &font);

} // namespace Inkscape::Extension::Internal::PdfBuilder
#endif
