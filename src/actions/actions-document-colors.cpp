// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * PROOF: File > Document Color Mode. Switching converts process colors to the mode's
 * working profile and makes the mode the default for new documents.
 */
#include "actions-document-colors.h"

#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/i18n.h>

#include "actions/actions-extra-data.h"
#include "colors/document-cms.h"
#include "colors/document-colors.h"
#include "colors/spaces/enum.h"
#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "message-stack.h"
#include "xml/node.h"

namespace {
namespace DC = Inkscape::Colors::DocumentColors;
using Inkscape::Colors::Space::Type;

Glib::ustring state_for(SPDocument *document) { return DC::mode(document) == Type::CMYK ? "CMYK" : "RGB"; }

void set_document_color_mode(SPDocument *document, Glib::ustring const &value)
{
    auto target = value == "CMYK" ? Type::CMYK : Type::RGB;
    bool converting = DC::mode(document) != target;
    auto root = document->getReprRoot();
    auto attribute = [root](char const *key) { return std::string(root->attribute(key) ? root->attribute(key) : ""); };
    auto before = attribute("proof:color-mode") + "|" + attribute("proof:color-profile");
    auto error = DC::switchMode(document, target);
    if (!error.empty()) {
        g_warning("%s", error.c_str());
        if (auto desktop = SP_ACTIVE_DESKTOP; desktop && desktop->getDocument() == document) {
            desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _(error.c_str()));
        }
        return;
    }
    if (before == attribute("proof:color-mode") + "|" + attribute("proof:color-profile")) {
        return; // already in this mode; an empty undo step would still clear redo
    }
    Inkscape::DocumentUndo::done(document, converting ? RC_("Undo", "Change document color mode")
                                                      : RC_("Undo", "Assign document color profile"),
                                 "document-properties");
}

std::vector<std::vector<Glib::ustring>> raw_data_document_colors = {
    // clang-format off
    {"doc.document-color-mode", N_("Document Color Mode"), NC_("Action Section", "Document"),
     N_("Convert the document to CMYK or RGB using that mode's working profile")},
    // clang-format on
};
} // namespace

void add_actions_document_colors(SPDocument *document)
{
    auto group = document->getActionGroup();
    auto action = group->add_action_radio_string(
        "document-color-mode", [document](Glib::ustring const &value) { set_document_color_mode(document, value); },
        state_for(document));
    // Undo, Document Properties and loading change the mode too; the menu follows the document.
    document->getDocumentCMS().connectAssignmentChanged([document, action] {
        action->set_state(Glib::Variant<Glib::ustring>::create(state_for(document)));
    });

    if (auto app = InkscapeApplication::instance()) {
        app->get_action_extra_data().add_data(raw_data_document_colors);
    }
}
