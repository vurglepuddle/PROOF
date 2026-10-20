// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Saving as Illustrator's own .ai. See ai-native-output.h.
 */

#include "ai-native-output.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/i18n.h>
#include <glibmm/ustring.h>
#include <memory>

#ifdef WITH_POPPLER
#include <zlib.h>
#include <zstd.h>
#include <GlobalParams.h>
#include <Object.h>
#include <PDFDoc.h>
#include <Page.h>
#include <XRef.h>

#include "extension/internal/pdfinput/poppler-transition-api.h"
#include "extension/internal/pdfoutput/pdf-update.h"
#endif

#include "ai-native-writer.h"
#include "colors/cms/profile.h"
#include "colors/document-colors.h"
#include "colors/spaces/cms.h"
#include "document.h"
#include "extension/db.h"
#include "extension/output.h"
#include "extension/system.h"
#include "extension/internal/pdfoutput/pdf-output.h"
#include "util/scope_exit.h"

namespace Inkscape::Extension::Internal {

namespace AiNative {

#ifdef WITH_POPPLER
namespace {

constexpr std::string_view MARKER = "%AI24_ZStandard_Data";
/// Illustrator's block size.
constexpr std::size_t BLOCK = 65536;
/// What Illustrator 29.2.1 writes beside its records (`ContainerVersion` ... `RoundtripVersion`).
constexpr std::string_view VERSIONS =
    " /ContainerVersion 12 /CreatorVersion 29 /RoundtripStreamType 2 /RoundtripVersion 24";

std::string ref(Ref const &r)
{
    return std::to_string(r.num) + " " + std::to_string(r.gen) + " R";
}

/// A PDF name for `key`.
std::string name(std::string_view key)
{
    std::string out = "/";
    for (unsigned char c : key) {
        if (c > 0x20 && c < 0x7f && std::string_view("#()<>[]{}/%").find(char(c)) == std::string_view::npos) {
            out += char(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "#%02X", c);
            out += hex;
        }
    }
    return out;
}

/// Now, as a PDF date in local time: D:YYYYMMDDHHmmSS+HH'mm'.
std::string pdf_date()
{
    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(now, "D:%Y%m%d%H%M%S");
    auto const minutes = g_date_time_get_utc_offset(now) / G_TIME_SPAN_MINUTE;
    g_date_time_unref(now);
    std::string out = stamp;
    g_free(stamp);
    auto const away = std::llabs(minutes);
    char zone[16];
    std::snprintf(zone, sizeof zone, "%c%02lld'%02lld'", minutes < 0 ? '-' : '+', away / 60, away % 60);
    return out + zone;
}

} // namespace

bool add_private_data(std::string const &path, std::string const &records, std::string &error,
                      std::string const &profile)
{
    error.clear();
    // The profile's own header says its colour space (ICC.1, 7.2.6).
    int channels = 0;
    if (profile.size() >= 132 && profile.compare(36, 4, "acsp") == 0) {
        auto const space = profile.substr(16, 4);
        channels = space == "CMYK" ? 4 : space == "RGB " ? 3 : space == "GRAY" ? 1 : 0;
    }
    if (!profile.empty() && !channels) {
        error = "the document's colour profile can't be read";
        return false;
    }
    // Illustrator keeps the header comments beside the records too, as they are.
    auto const comments = records.find("%%EndComments");
    if (comments == std::string::npos) {
        error = "the records have no header";
        return false;
    }
    auto const line_end = records.find('\n', comments);
    std::string_view const meta(records.data(), line_end == std::string::npos ? records.size() : line_end + 1);

    std::string packed(ZSTD_compressBound(records.size()), '\0');
    auto const size = ZSTD_compress(packed.data(), packed.size(), records.data(), records.size(), 9);
    if (ZSTD_isError(size)) {
        error = std::string("the records couldn't be compressed: ") + ZSTD_getErrorName(size);
        return false;
    }
    packed.resize(size);
    std::string const data = std::string(MARKER) + packed;

    if (!globalParams) globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    auto pdf = _POPPLER_MAKE_SHARED_PDFDOC(path.c_str());
    if (!pdf || !pdf->isOk() || pdf->getNumPages() < 1) {
        error = "the PDF just written can't be read back";
        return false;
    }
    XRef *xref = pdf->getXRef();
    PdfBuilder::PdfUpdate update(path, xref);
    if (!update.ok()) {
        error = update.error();
        return false;
    }
    auto &text = update.text();
    int next = xref->getNumObjects();
    auto stream = [&](std::string_view bytes) {
        Ref const r{next++, 0};
        update.begin(r);
        text << "<< /Length " << std::to_string(bytes.size()) << " >>\nstream\n" << bytes << "\nendstream";
        update.end();
        return r;
    };

    std::vector<Ref> blocks;
    for (std::size_t at = 0; at < data.size(); at += BLOCK) {
        blocks.push_back(stream(std::string_view(data).substr(at, BLOCK)));
    }
    Ref const metadata = stream(meta);
    Ref const private_data{next++, 0};
    update.begin(private_data);
    text << "<< /AIMetaData " << ref(metadata);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        text << " /AIPrivateData" << std::to_string(i + 1) << " " << ref(blocks[i]);
    }
    text << " /NumBlock " << std::to_string(blocks.size()) << VERSIONS << " >>";
    update.end();

    auto const date = pdf_date();
    Ref const holder{next++, 0};
    update.begin(holder);
    text << "<< /LastModified (" << date << ") /Private " << ref(private_data) << " >>";
    update.end();

    // The document's profile, once, for every page to name.
    Ref icc{-1, 0};
    if (channels) {
        uLongf length = compressBound(profile.size());
        std::string packed_profile(length, '\0');
        if (compress2(reinterpret_cast<Bytef *>(packed_profile.data()), &length,
                      reinterpret_cast<Bytef const *>(profile.data()), profile.size(), 9) != Z_OK) {
            error = "the document's colour profile couldn't be compressed";
            return false;
        }
        packed_profile.resize(length);
        icc = Ref{next++, 0};
        update.begin(icc);
        text << "<< /N " << std::to_string(channels) << " /Alternate "
             << (channels == 4 ? "/DeviceCMYK" : channels == 3 ? "/DeviceRGB" : "/DeviceGray")
             << " /Filter /FlateDecode /Length " << std::to_string(packed_profile.size()) << " >>\nstream\n"
             << packed_profile << "\nendstream";
        update.end();
    }

    // Each page, again, naming the one copy, as Illustrator's files do.
    for (int i = 1; i <= pdf->getNumPages(); ++i) {
        auto *page = pdf->getPage(i);
        if (!page) {
            error = "a page of the PDF just written can't be read back";
            return false;
        }
        Ref const at = page->getRef();
        Object const object = xref->fetch(at);
        if (!object.isDict()) {
            error = "a page of the PDF just written can't be read back";
            return false;
        }
        update.begin(at);
        text << "<<";
        auto write = [&](Object const &value) {
            Object copy = value.copy();
            PDFDoc::writeObject(&copy, &text, xref, 0, nullptr, cryptRC4, 0, at, nullptr);
        };
        Dict *dict = object.getDict();
        for (int k = 0; k < dict->getLength(); ++k) {
            std::string_view const key = dict->getKey(k);
            if (key == "PieceInfo" || key == "LastModified" || (icc.num >= 0 && key == "Resources")) continue;
            text << " " << name(key) << " ";
            write(dict->getValNF(k));
        }
        if (icc.num >= 0) {
            // The page's resources, inherited ones resolved, its colour spaces and the profile's.
            text << " /Resources <<";
            if (Dict *resources = page->getResourceDict()) {
                for (int k = 0; k < resources->getLength(); ++k) {
                    std::string_view const key = resources->getKey(k);
                    if (key == "ColorSpace") continue;
                    text << " " << name(key) << " ";
                    write(resources->getValNF(k));
                }
                text << " /ColorSpace <<";
                Object const spaces = resources->lookup("ColorSpace");
                if (spaces.isDict()) {
                    Dict *named = spaces.getDict();
                    for (int k = 0; k < named->getLength(); ++k) {
                        std::string_view const key = named->getKey(k);
                        if (key == "CSDocument") continue;
                        text << " " << name(key) << " ";
                        write(named->getValNF(k));
                    }
                }
            } else {
                text << " /ColorSpace <<";
            }
            text << " /CSDocument [/ICCBased " << ref(icc) << "] >> >>";
        }
        text << " /LastModified (" << date << ") /PieceInfo << /Illustrator " << ref(holder) << " >> >>";
        update.end();
    }

    update.finish(next);
    pdf.reset();
    if (!update.write()) {
        error = "Illustrator's data couldn't be written into the file";
        return false;
    }
    return true;
}
#else
bool add_private_data(std::string const &, std::string const &, std::string &error)
{
    error = "this build can't write .ai files";
    return false;
}
#endif

} // namespace AiNative

namespace {

constexpr char const *PDF_OUTPUT = "org.inkscape.output.pdf.capypdf";

} // namespace

bool AiNativeOutput::check(Inkscape::Extension::Extension *)
{
#ifdef WITH_POPPLER
    return PdfOutput::supports_pdf17() && Inkscape::Extension::db.get(PDF_OUTPUT);
#else
    return false;
#endif
}

void AiNativeOutput::save(Inkscape::Extension::Output *mod, SPDocument *doc, char const *filename)
{
    auto const written = AiNative::write_records(doc);
    if (!written.refused.empty()) {
        std::vector<std::string> names;
        for (auto const &r : written.refused) {
            if (std::find(names.begin(), names.end(), r) == names.end()) names.push_back(r);
        }
        std::string list;
        for (auto const &n : names) list += (list.empty() ? "" : ", ") + n;
        throw std::runtime_error(Glib::ustring::compose(_("PROOF can't write these into an Illustrator file yet: %1. "
                                                          "Nothing was saved; save as SVG or PDF to keep them."),
                                                        list)
                                     .raw());
    }
    auto *pdf = dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(PDF_OUTPUT));
    if (!pdf) throw std::runtime_error(_("PROOF's PDF export, which writes the .ai file's page, isn't available."));

    // Written beside the file and put in its place only when whole, so a failure leaves any file
    // that was there as it was.
    std::string part = std::string(filename) + ".proof-XXXXXX";
    int const fd = g_mkstemp(part.data());
    if (fd < 0) {
        throw std::runtime_error(_("The .ai file couldn't be written beside its destination."));
    }
    g_close(fd, nullptr);
    auto cleanup = scope_exit([&] { g_unlink(part.c_str()); });
    // Leave the working document as it was when the PDF exporter enables pages.
    // Use this format's fixed options, independent of earlier PDF/LaTeX exports.
    auto page_doc = doc->copy();
    page_doc->ensureUpToDate();
    PdfOutput().save(mod, page_doc.get(), part.c_str());
    // The document's own profile, when it has one: in Illustrator's files every page names it.
    std::string profile;
    if (auto const space = Colors::DocumentColors::assignedSpace(doc)) {
        if (auto const icc = space->getProfile()) {
            auto const bytes = icc->dumpData();
            profile.assign(bytes.begin(), bytes.end());
        }
    }
    std::string error;
    if (!AiNative::add_private_data(part, written.records, error, profile)) {
        throw std::runtime_error(Glib::ustring::compose(_("The .ai file couldn't be written: %1."), error).raw());
    }
    if (g_rename(part.c_str(), filename) != 0) {
        throw std::runtime_error(_("The .ai file couldn't be put in place of the old one."));
    }
}

#include "../clear-n_.h"

void AiNativeOutput::init()
{
    if (!PdfOutput::supports_pdf17()) return;
    // clang-format off
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("Adobe Illustrator (native)") "</name>\n"
            "<id>org.inkscape.output.ai.native</id>\n"
            "<param gui-hidden='true' name='PDFversion' type='optiongroup'>\n"
                "<option value='PDF-1.7'>PDF 1.7</option>\n"
            "</param>\n"
            "<param gui-hidden='true' name='blurToBitmap' type='bool'>false</param>\n"
            "<param gui-hidden='true' name='resolution' type='int'>300</param>\n"
            "<param gui-hidden='true' name='textToPath' type='optiongroup'>\n"
                "<option value='embed'>Embed</option>\n"
            "</param>\n"
            "<output is_exported='true' priority='7'>\n"
                "<extension>.ai</extension>\n"
                "<mimetype>application/illustrator</mimetype>\n"
                "<filetypename>" N_("Adobe Illustrator (native) (*.ai)") "</filetypename>\n"
                "<filetypetooltip>" N_("Illustrator's own editable data with PROOF's PDF page; what can't be written yet is named and nothing is saved") "</filetypetooltip>\n"
                "<dataloss>true</dataloss>\n"
            "</output>\n"
        "</inkscape-extension>", std::make_unique<AiNativeOutput>());
    // clang-format on
}

} // namespace Inkscape::Extension::Internal
