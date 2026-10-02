// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_STROKE_PANEL_H
#define INKSCAPE_UI_DIALOG_STROKE_PANEL_H

#include "ui/dialog/dialog-base.h"
#include "ui/widget/stroke-style.h"

namespace Inkscape::UI::Dialog {

// A dockable home for stroke geometry, sharing Fill and Stroke's operations.
class StrokePanel final : public DialogBase {
public:
    StrokePanel();
    ~StrokePanel() override;
    void desktopReplaced() override;
    void documentReplaced() override;
    void selectionChanged(Selection *) override;
    void selectionModified(Selection *, unsigned flags) override;
private:
    UI::Widget::StrokeStyle _stroke;
};

} // namespace Inkscape::UI::Dialog
#endif
