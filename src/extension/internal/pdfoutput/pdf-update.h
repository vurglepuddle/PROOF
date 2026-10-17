// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: objects added to a PDF just written, in an update appended to it (a PDF's incremental
 * update), with cross-references of the kind the file has. Placed pages put their pages in this
 * way (placed-pages.h), and the .ai writer Illustrator's private data (ai/ai-native-output.h).
 */
#ifndef SEEN_EXTENSION_INTERNAL_PDFOUTPUT_PDF_UPDATE_H
#define SEEN_EXTENSION_INTERNAL_PDFOUTPUT_PDF_UPDATE_H

#ifdef WITH_POPPLER
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <Object.h>
#include <Stream.h>

class XRef;

namespace Inkscape::Extension::Internal::PdfBuilder {

/// What is appended to a PDF, kept here until it is whole. Positions count on from the file's end.
class Appended : public OutStream
{
public:
    explicit Appended(Goffset start)
        : _start(start)
    {}
    void close() override {}
    Goffset getPos() override { return _start + Goffset(text.size()); }
    void put(char c) override { text += c; }
    size_t write(std::span<unsigned char const> data) override;
    void printf(char const *format, ...) override;
    Appended &operator<<(std::string_view s)
    {
        text += s;
        return *this;
    }

    std::string text;

private:
    Goffset _start;
};

/// An update to the PDF at `path`, which `xref` reads: the objects written into it, then its
/// cross-references and trailer, then the file appended to.
class PdfUpdate
{
public:
    /// When the file can't be found or has no cross-references, `ok()` is false and `error()`
    /// says which.
    PdfUpdate(std::string path, XRef *xref);

    bool ok() const { return _error.empty(); }
    std::string const &error() const { return _error; }

    /// Where the object's text goes.
    Appended &text() { return _text; }
    /// Starts object `r`: a new one, or a newer version of one in the file.
    void begin(Ref const &r);
    void end();

    /// Ends the update with its cross-references and trailer. `next` is the first number no object
    /// has. The XRef must still be open.
    void finish(int next);
    /// Appends the update to the file, which is touched only now. Whatever reads it should be
    /// closed first.
    bool write();

private:
    std::string _path;
    XRef *_xref;
    Goffset _size; ///< The file's, or -1.
    Goffset _prev = -1;
    Appended _text;
    /// Object number, generation, where it is.
    std::vector<std::tuple<int, int, Goffset>> _entries;
    std::string _error;
};

} // namespace Inkscape::Extension::Internal::PdfBuilder

#endif // WITH_POPPLER

#endif
