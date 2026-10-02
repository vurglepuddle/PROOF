// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_COLORS_DOCUMENT_COLORS_H
#define INKSCAPE_COLORS_DOCUMENT_COLORS_H

#include "color.h"
#include "spaces/enum.h"

class SPDocument;

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
}
}
#endif
