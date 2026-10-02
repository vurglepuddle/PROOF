// SPDX-License-Identifier: GPL-2.0-or-later
#include "stroke-panel.h"

namespace Inkscape::UI::Dialog {

StrokePanel::StrokePanel() : DialogBase("/dialogs/stroke", "Stroke")
{
    add_css_class("workspace-properties");
    _stroke.set_margin(8);
    append(_stroke);
}

StrokePanel::~StrokePanel() { _stroke.setDesktop(nullptr); }
void StrokePanel::desktopReplaced() { _stroke.setDesktop(getDesktop()); }
void StrokePanel::documentReplaced() { _stroke.setDesktop(getDesktop()); }
void StrokePanel::selectionChanged(Selection *) { _stroke.selectionChangedCB(); }
void StrokePanel::selectionModified(Selection *, unsigned flags) { _stroke.selectionModifiedCB(flags); }

} // namespace Inkscape::UI::Dialog
