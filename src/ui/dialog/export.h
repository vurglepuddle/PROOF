// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Anshudhar Kumar Singh <anshudhar2001@gmail.com>
 *
 * Copyright (C) 1999-2007, 2021 Authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SP_EXPORT_H
#define SP_EXPORT_H

#include "ui/dialog/dialog-base.h"
#include <optional>
#include <memory>

namespace Gtk {
class Box;
class Builder;
class Notebook;
} // namespace Gtk

class SPObject;
class SPPage;

namespace Inkscape {

class Preferences;
namespace Colors {
class Color;
}

namespace Extension {
enum class MissingLinkAction;
class Output;
} // namespace Extension

namespace UI::Dialog {

class ExportPanel;

void set_export_bg_color(SPObject* object, Inkscape::Colors::Color const &color);
Inkscape::Colors::Color get_export_bg_color(SPObject* object, Inkscape::Colors::Color const &default_color);

class Export final : public DialogBase
{
public:
    Export();
    ~Export() final;

private:
    Glib::RefPtr<Gtk::Builder> builder;
    Gtk::Box      &container;       // Main Container
    std::unique_ptr<ExportPanel> panel;

    void documentReplaced() override;
    void desktopReplaced() override;
    void selectionChanged(Inkscape::Selection *selection) override;
    void selectionModified(Inkscape::Selection *selection, unsigned flags) override;

public:
    static std::string absolutizePath(SPDocument *doc, const std::string &filename);
    static bool unConflictFilename(SPDocument *doc, std::string &filename, std::string const extension);
    static std::string filePathFromObject(SPDocument *doc, SPObject *obj, const std::string &file_entry_text);
    static std::string prependDirectory(const std::string &name, const std::string &orig, SPDocument *doc = nullptr);
    static std::string defaultFilename(SPDocument *doc, const std::string &filename_entry_text,
                                       const std::string &extension);
    static bool checkOrCreateDirectory(std::string const &filename);

    static bool exportRaster(
        Geom::Rect const &area, unsigned long int const &width, unsigned long int const &height,
        float const &dpi, Inkscape::Colors::Color const &bgcolor, Glib::ustring const &filename, bool overwrite,
        unsigned (*callback)(float, void *), void *data,
        Inkscape::Extension::Output *extension, std::vector<SPItem const *> *items = nullptr);
  
    // No preset asks for this export; a batch passes its one decision to every item.
    static bool exportVector(
        Inkscape::Extension::Output *extension, SPDocument *doc, Glib::ustring const &filename,
        bool overwrite, Geom::Rect const &area,
        std::optional<Extension::MissingLinkAction> links = std::nullopt);
    static bool exportVector(
        Inkscape::Extension::Output *extension, SPDocument *doc, Glib::ustring const &filename,
        bool overwrite, const std::vector<SPItem const *> &items, SPPage const *page,
        std::optional<Extension::MissingLinkAction> links = std::nullopt);
    static bool exportVector(
        Inkscape::Extension::Output *extension, SPDocument *doc, Glib::ustring const &filename,
        bool overwrite, const std::vector<SPItem const *> &items, const std::vector<SPPage const *> &pages,
        std::optional<Extension::MissingLinkAction> links = std::nullopt);
};

} // namespace UI::Dialog

} // namespace Inkscape

#endif // SP_EXPORT_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
