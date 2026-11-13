// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_EXPORT_PANEL_H
#define INKSCAPE_UI_DIALOG_EXPORT_PANEL_H
#include <gtkmm/box.h>
#include <memory>
class SPDesktop;
class SPDocument;
namespace Inkscape::UI::Dialog {
class ExportPanel final : public Gtk::Box {
public:
    ExportPanel();
    ~ExportPanel() override;
    void setContext(SPDesktop *, SPDocument *);
    void refresh(bool hints = false);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
#endif
