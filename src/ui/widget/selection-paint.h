// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_WIDGET_SELECTION_PAINT_H
#define INKSCAPE_UI_WIDGET_SELECTION_PAINT_H

#include <functional>
#include <vector>
#include <sigc++/connection.h>

class SPDesktop;

namespace Inkscape::UI::Widget {
class PaintSwitch;

// Use the same paint operations in the dialog and sidebar. The caller owns
// these connections and may invalidate its context before closing a popover.
// modified_tag marks the document's modified notifications: 0, or a tag from
// get_next_object_modified_tag(). It doesn't group undo steps (the keys do).
std::vector<sigc::connection> connect_selection_paint(
    PaintSwitch &paint_switch, bool is_fill, std::function<SPDesktop *()> desktop,
    std::function<bool()> blocked, unsigned int modified_tag = 0,
    std::function<void()> changed = {});

// Make a no-selection paint choice the default for newly drawn objects.
void remember_default_paint(SPDesktop *desktop, bool is_fill);
} // namespace Inkscape::UI::Widget
#endif
