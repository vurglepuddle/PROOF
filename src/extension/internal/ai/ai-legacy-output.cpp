// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Illustrator 7's standalone records: one artboard, no PDF, no inherited native data.
 */

#include "ai-legacy-output.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/i18n.h>
#include <glibmm/ustring.h>

#include "ai-native-writer.h"
#include "extension/output.h"
#include "extension/system.h"
#include "util/scope_exit.h"

namespace Inkscape::Extension::Internal {

void AiLegacyOutput::save(Inkscape::Extension::Output *, SPDocument *doc, char const *filename)
{
    auto const written = AiNative::write_records(doc, AiNative::RecordsVersion::Illustrator7);
    if (!written.refused.empty()) {
        std::string list;
        for (auto const &name : written.refused) list += (list.empty() ? "" : ", ") + name;
        throw std::runtime_error(Glib::ustring::compose(
            _("PROOF can't write these into an Illustrator 7 file yet: %1. "
              "Nothing was saved; use SVG or the modern Illustrator format, or export one artboard."), list).raw());
    }

    // Replace the destination only after the standalone records are wholly written.
    std::string part = std::string(filename) + ".proof-XXXXXX";
    int const fd = g_mkstemp(part.data());
    if (fd < 0) throw std::runtime_error(_("The Illustrator 7 file couldn't be written beside its destination."));
    auto cleanup = scope_exit([&] { g_unlink(part.c_str()); });
    if (!g_close(fd, nullptr) ||
        !g_file_set_contents(part.c_str(), written.records.data(), written.records.size(), nullptr)) {
        throw std::runtime_error(_("The Illustrator 7 records couldn't be written."));
    }
    if (g_rename(part.c_str(), filename) != 0) {
        throw std::runtime_error(_("The Illustrator 7 file couldn't be put in place of the old one."));
    }
}

#include "../clear-n_.h"

void AiLegacyOutput::init()
{
    // This format has no PDF backend requirement and must remain an explicit copy choice.
    // clang-format off
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("Adobe Illustrator 7") "</name>\n"
            "<id>org.inkscape.output.ai.legacy7</id>\n"
            "<label>" N_("One artboard, as editable Illustrator 7 records without a PDF page. Save the working document as SVG first. Unsupported artwork is named and nothing is saved.") "</label>\n"
            "<output is_exported='true' priority='8'>\n"
                "<extension>.ai</extension>\n"
                "<mimetype>application/illustrator</mimetype>\n"
                "<filetypename>" N_("Adobe Illustrator 7 (*.ai)") "</filetypename>\n"
                "<filetypetooltip>" N_("Editable paths, layers and inks for one artboard; no PDF preview or artboard names") "</filetypetooltip>\n"
                "<dataloss>true</dataloss>\n"
                "<savecopyonly>true</savecopyonly>\n"
            "</output>\n"
        "</inkscape-extension>", std::make_unique<AiLegacyOutput>());
    // clang-format on
}

} // namespace Inkscape::Extension::Internal
