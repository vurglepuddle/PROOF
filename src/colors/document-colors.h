// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_COLORS_DOCUMENT_COLORS_H
#define INKSCAPE_COLORS_DOCUMENT_COLORS_H

#include "color.h"
#include "spaces/enum.h"

class SPDocument;
namespace Inkscape::XML { class Node; }

namespace Inkscape::Colors {
namespace CMS { class Profile; }
namespace Space { class CMS; }

// Application defaults are used only when requested. Documents retain an
// embedded profile, independent of later preference changes or other windows.
namespace DocumentColors {
std::vector<std::shared_ptr<CMS::Profile>> profiles(Space::Type mode);
std::shared_ptr<CMS::Profile> workingProfile(Space::Type mode);
void setWorkingProfile(Space::Type mode, std::shared_ptr<CMS::Profile> const &profile);
RenderingIntent workingIntent();
Space::Type mode(SPDocument *document);
std::shared_ptr<Space::CMS> assignedSpace(SPDocument *document);
Color interpret(SPDocument *document, Color const &color);
void refresh(SPDocument *document);

// Same model: assign, retaining channel values. Different model: convert
// process paints. A null profile explicitly disables document management.
// Returns an error before changing the document for an incompatible profile.
std::string assign(SPDocument *document, Space::Type mode,
                   std::shared_ptr<CMS::Profile> const &profile, RenderingIntent intent);

// Every document has a mode and, where a profile is available, an assigned profile.
// Mode for new documents: the user's last choice, initially CMYK when a CMYK working profile exists.
Space::Type newDocumentMode();
void setNewDocumentMode(Space::Type mode);
// CMYK if any process paint is CMYK, otherwise RGB.
Space::Type inferMode(SPDocument *document);
// For a document without proof:color-mode only: an embedded profile of that mode is kept
// (as Illustrator preserves embedded profiles), otherwise the working profile is assigned.
// Paint values are not rewritten. Returns false if the document already has a mode.
bool adopt(SPDocument *document, Space::Type mode);
// File > Document Color Mode: converts to the mode with its working profile and remembers it.
// Returns an error before changing the document if no working profile is available.
std::string switchMode(SPDocument *document, Space::Type mode);
// Called when proof:color-mode or proof:color-profile changes on the root.
void assignmentChanged(SPDocument *document);

// Browsers and Illustrator drop a whole icc-color()/device-cmyk() declaration. Saving adds
// the RGB preview as a presentation attribute next to such style paints; the style still
// wins wherever it is understood. Loading removes them so they can never go stale.
void insertFallbacks(SPDocument *document);
void stripFallbacks(XML::Node *root);
}
}
#endif
