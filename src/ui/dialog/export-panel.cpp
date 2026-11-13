// SPDX-License-Identifier: GPL-2.0-or-later
#include "export-panel.h"
#include "export-panel-plan.h"
#include "export.h"
#include "export-batch.h"
#include "link-missing.h"
#include <array>
#include <algorithm>
#include <stdexcept>
#include <gtkmm/error.h>
#include <glibmm/convert.h>
#include <cmath>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/recentmanager.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <glibmm/fileutils.h>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "extension/output.h"
#include "inkscape-window.h"
#include "layer-manager.h"
#include "object/sp-image.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "selection.h"
#include "ui/dialog-run.h"
#include "ui/widget/color-picker.h"
#include "ui/widget/export-lists.h"
#include "ui/widget/unit-menu.h"
#include "util/scope_exit.h"

namespace Inkscape::UI::Dialog {
namespace P = ExportPlanning;
struct ExportPanel::Impl {
    ExportPanel &root;
    SPDesktop *desktop = nullptr;
    SPDocument *doc = nullptr;
    Preferences *prefs = Preferences::get();
    Gtk::ScrolledWindow scroll;
    Gtk::Box body{Gtk::Orientation::VERTICAL, 12}, formats{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box footer{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::ComboBoxText what;
    Gtk::FlowBox tiles;
    BatchItems tile_items;
    P::Mode tile_mode = P::Mode::Document;
    std::map<P::Mode, std::set<std::string>> ticked;
    Gtk::CheckButton each{_("Each selected object separately")}, hide{_("Hide all except selected")}, show{_("Show preview")};
    Gtk::Grid custom;
    std::array<Gtk::SpinButton, 4> coords;
    UI::Widget::UnitMenu units;
    Util::Unit const *old_unit = nullptr;
    ExportPreview preview;
    std::shared_ptr<PreviewDrawing> drawing;
    UI::Widget::ColorPicker background{_("Background color"), _("Export background color"), Colors::Color(0xffffff00), false};
    Gtk::Entry folder, name;
    Gtk::Label filenames;
    Gtk::Button add{_("+ Add format")}, browse{_("Browse...")}, run{_("Export")}, cancel{_("Cancel")};
    Gtk::ProgressBar progress;
    bool updating = false, busy = false, stopped = false, edited = false;
    std::size_t current = 0, total = 1;
    sigc::scoped_connection pages_changed, page_modified;
    struct Row : Gtk::Box {
        Impl &panel;
        ExtensionList format;
        Gtk::SpinButton dpi, width, height;
        Gtk::Label dpi_label{_("DPI")}, w_label{_("W")}, h_label{_("H")};
        Gtk::Entry suffix;
        Gtk::CheckButton one_pdf{_("One PDF with all pages")};
        Gtk::Button remove{"\u2212"};
        Gtk::Box top{Gtk::Orientation::HORIZONTAL, 6}, size{Gtk::Orientation::HORIZONTAL, 6}, naming{Gtk::Orientation::HORIZONTAL, 6};
        Row(Impl &p) : Gtk::Box(Gtk::Orientation::VERTICAL, 6), panel(p) {
            format.setup();
            format.set_active_id(p.prefs->getString("/dialogs/export/format", SP_MODULE_KEY_RASTER_PNG));
            if (!format.getExtension()) format.set_active_id(SP_MODULE_KEY_RASTER_PNG);
            append(top); append(size); append(naming); format.set_hexpand(true);
            top.append(format); top.append(*format.getPrefButton()); top.append(remove);
            size.append(dpi_label); size.append(dpi); size.append(w_label); size.append(width); size.append(h_label); size.append(height);
            labelled(naming, _("_Suffix"), suffix); append(one_pdf); remove.set_size_request(36, 36);
            gtk_accessible_update_property(GTK_ACCESSIBLE(format.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Format"), -1);
            for (auto const &[spin, text] : std::initializer_list<std::pair<Gtk::SpinButton *, char const *>>{{&dpi, _("DPI")}, {&width, _("Pixel width")}, {&height, _("Pixel height")}})
                gtk_accessible_update_property(GTK_ACCESSIBLE(spin->gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
            dpi.set_range(1, 100000); dpi.set_increments(1, 10); dpi.set_digits(2);
            dpi.set_value(p.prefs->getDouble("/dialogs/export/defaultxdpi/value", 96));
            for (auto *spin : {&dpi, &width, &height}) spin->set_width_chars(5);
            for (auto *spin : {&width, &height}) { spin->set_range(1, 2147483647); spin->set_increments(1, 10); }
            suffix.set_width_chars(4); suffix.set_placeholder_text(_("Suffix")); suffix.set_tooltip_text(_("Filename suffix"));
            remove.set_tooltip_text(_("Remove this format")); one_pdf.set_active(true);
            format.signal_changed().connect([this] { panel.refresh(); });
            dpi.signal_value_changed().connect([this] { panel.refresh(); });
            suffix.signal_changed().connect([this] { panel.refresh(); });
            one_pdf.signal_toggled().connect([this] { panel.refresh(); });
            width.signal_value_changed().connect([this] { resize(true); });
            height.signal_value_changed().connect([this] { resize(false); });
            remove.signal_clicked().connect([this] { panel.remove(this); });
        }
        void resize(bool horizontal) {
            if (panel.updating) return;
            auto items = P::items(panel.input());
            if (items.size() == 1) dpi.set_value((horizontal ? width.get_value() / items[0].area.width() :
                                                                         height.get_value() / items[0].area.height()) * 96);
        }
    };
    std::vector<std::unique_ptr<Row>> rows;

    explicit Impl(ExportPanel &r) : root(r) {
        scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scroll.set_vexpand(true); scroll.set_child(body); body.set_margin(12);
        root.append(scroll); root.append(footer);
        heading(_("What"));
        for (auto const &s : {_("Document"), _("Pages"), _("Selection"), _("Layers"), _("Custom")}) what.append(s);
        gtk_accessible_update_property(GTK_ACCESSIBLE(what.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, _("What to export"), -1);
        auto legacy = prefs->getString("/dialogs/export/exportarea/value");
        int initial = legacy == "page" ? 1 : legacy == "selection" ? 2 : legacy == "custom" ? 4 : 0;
        what.set_active(prefs->getIntLimited("/dialogs/export/what", initial, 0, 4)); body.append(what);
        body.append(each); body.append(hide);
        tiles.set_selection_mode(Gtk::SelectionMode::MULTIPLE); body.append(tiles);
        preview.setSize(160); body.append(preview);
        custom.set_column_spacing(6); custom.set_row_spacing(6);
        int i = 0;
        for (auto const *text : {"X", "Y", "W", "H"}) {
            auto *label = Gtk::make_managed<Gtk::Label>(text);
            custom.attach(*label, 0, i); custom.attach(coords[i], 1, i);
            coords[i].set_range(i < 2 ? -1e9 : 0.001, 1e9); coords[i].set_digits(3); coords[i].set_increments(1, 10);
            label->set_mnemonic_widget(coords[i]);
            gtk_accessible_update_property(GTK_ACCESSIBLE(coords[i].gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1); coords[i].signal_value_changed().connect([this] { refresh(); }); ++i;
        }
        units.setUnitType(Util::UNIT_TYPE_LINEAR); units.setUnit("px"); old_unit = units.getUnit();
        custom.attach(units, 1, 4); body.append(custom);
        units.signal_changed().connect([this] {
            if (updating) return;
            auto *next = units.getUnit(); updating = true;
            for (auto &c : coords) c.set_value(old_unit->convert(c.get_value(), next));
            old_unit = next; updating = false; refresh();
        });
        body.append(show); show.set_active(true); body.append(background);
        heading(_("Formats")); body.append(formats); body.append(add);
        heading(_("Where"));
        auto *where = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        labelled(*where, _("_Folder"), folder); where->append(browse); body.append(*where);
        auto *names = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        labelled(*names, _("_Name"), name); body.append(*names);
        filenames.set_wrap(true); filenames.set_xalign(0); filenames.set_selectable(true); body.append(filenames);
        progress.set_show_text(true); progress.set_hexpand(true); footer.set_margin(12);
        footer.append(progress); footer.append(cancel); footer.append(run); cancel.set_visible(false);
        for (auto *button : {&add, &browse, &run, &cancel}) button->set_size_request(-1, 36);
        add.signal_clicked().connect([this] { add_row(); });
        browse.signal_clicked().connect([this] { choose_folder(); });
        run.signal_clicked().connect([this] { write(); }); cancel.signal_clicked().connect([this] { stopped = true; });
        what.signal_changed().connect([this] { if (!updating) { edited = false; refresh(true, true); } });
        for (auto *check : {&each, &hide, &show}) check->signal_toggled().connect([this] { refresh(); });
        tiles.signal_selected_children_changed().connect([this] { refresh(true); });
        for (auto *entry : {&folder, &name}) entry->signal_changed().connect([this] { if (!updating) { edited = true; refresh(); } });
        background.connectChanged([this](auto const &color) {
            if (doc && !updating) set_export_bg_color(doc->getNamedView(), color);
            refresh();
        });
        add_row();
    }
    void heading(Glib::ustring const &text) {
        auto *label = Gtk::make_managed<Gtk::Label>(text); label->set_xalign(0); label->add_css_class("heading"); body.append(*label);
    }
    static void labelled(Gtk::Box &box, Glib::ustring const &text, Gtk::Widget &widget) {
        auto *label = Gtk::make_managed<Gtk::Label>(); label->set_text_with_mnemonic(text); label->set_mnemonic_widget(widget);
        box.append(*label); box.append(widget); widget.set_hexpand(true);
    }
    void add_row() { rows.push_back(std::make_unique<Row>(*this)); formats.append(*rows.back()); refresh(); }
    void remove(Row *row) {
        if (rows.size() == 1) return;
        formats.remove(*row);
        std::erase_if(rows, [row](auto const &r) { return r.get() == row; }); refresh();
    }
    P::Mode mode() const { return static_cast<P::Mode>(std::max(0, what.get_active_row_number())); }
    static P::Candidate candidate(SPItem *item) {
        auto bounds = item->documentVisualBounds();
        auto const *id = item->getId(), *label = item->label();
        return {id ? id : "", label && *label ? label : id ? id : "Object", bounds ? *bounds : Geom::Rect()};
    }
    P::Input input() {
        P::Input in; in.mode = mode(); in.each = each.get_active(); in.hide = hide.get_active();
        if (!doc || !desktop) return in;
        doc->ensureUpToDate();
        auto bounds = doc->getRoot()->documentVisualBounds(); if (!bounds) bounds = doc->preferredBounds();
        in.document = {"", _("Document"), bounds ? *bounds : Geom::Rect()};
        for (auto *item : desktop->getSelection()->items()) in.selection.push_back(candidate(item));
        auto px = [&](int i) { return units.getUnit()->convert(coords[i].get_value(), "px"); };
        in.custom = {"", _("Custom"), Geom::Rect(px(0), px(1), px(0) + px(2), px(1) + px(3))};
        for (auto const &[id, tile] : tile_items) {
            if (!tile->is_selected()) continue;
            if (auto *page = tile->getPage()) in.pages.push_back({id, page->getLabel(), page->getDocumentRect()});
            if (auto *item = tile->getItem()) {
                auto layer = candidate(item);
                for (auto *selected : desktop->getSelection()->items()) {
                    if (item == selected || item->isAncestorOf(selected)) layer.selected_children.push_back(selected->getId());
                }
                in.layers.push_back(layer);
            }
        }
        // Tile maps use IDs; pages must be exported in document order.
        std::sort(in.pages.begin(), in.pages.end(), [this](auto const &a, auto const &b) {
            return cast<SPPage>(doc->getObjectById(a.id))->getPageIndex() < cast<SPPage>(doc->getObjectById(b.id))->getPageIndex();
        });
        return in;
    }
    std::vector<P::Format> format_specs() {
        std::vector<P::Format> out;
        for (auto const &row : rows) {
            if (auto *ext = row->format.getExtension()) out.push_back({ext->get_extension(), row->suffix.get_text(), row->dpi.get_value(), ext->is_raster(), row->one_pdf.get_active()});
        }
        return out;
    }
    std::vector<P::File> plan() {
        if (!doc) return {};
        return P::files(P::items(input()), format_specs(), mode(),
            Glib::filename_to_utf8(Glib::canonicalize_filename(Export::absolutizePath(doc, Glib::filename_from_utf8(folder.get_text())))), name.get_text(),
            [](auto const &f) { return Glib::file_test(Glib::filename_from_utf8(f), Glib::FileTest::EXISTS); });
    }
    void load_hints(P::Input const &in) {
        SPObject *target = doc->getRoot();
        if (in.mode == P::Mode::Selection && in.selection.size() == 1) target = doc->getObjectById(in.selection[0].id);
        if (in.mode == P::Mode::Pages && in.pages.size() == 1) target = doc->getObjectById(in.pages[0].id);
        if (in.mode == P::Mode::Layers && in.layers.size() == 1) target = doc->getObjectById(in.layers[0].id);
        if (!target) target = doc->getRoot();
        std::string path = target->getExportFilename();
        if (path.empty()) path = Export::defaultFilename(doc, "", ".png");
        path = Glib::filename_to_utf8(Export::absolutizePath(doc, Glib::filename_from_utf8(path)));
        folder.set_text(Glib::path_get_dirname(path));
        auto stem = Glib::path_get_basename(path); rows.front()->format.removeExtension(stem);

        name.set_text(stem);
        if (target == doc->getRoot()) {
            if (auto previous = target->getAttribute("inkscape:export-batch-path")) folder.set_text(previous);
            else if (target->getExportFilename().empty()) {
                auto saved = prefs->getString("/dialogs/export/batch/path"); if (!saved.empty()) folder.set_text(saved);
            }
            if (auto previous = target->getAttribute("inkscape:export-batch-name")) name.set_text(previous);
        }
        if (auto dpi = target->getExportDpi().x(); dpi > 0) rows.front()->dpi.set_value(dpi);
    }
    void refresh(bool hints = false, bool rebuild = false) {
        if (updating || busy) return;
        updating = true; auto reset = scope_exit([&] { updating = false; });
        bool const tiled = mode() == P::Mode::Pages || mode() == P::Mode::Layers;
        if (rebuild) {
            bool changed_mode = tile_mode != mode();
            if (changed_mode && !tile_items.empty()) {
                auto &saved = ticked[tile_mode]; saved.clear();
                for (auto const &[id, tile] : tile_items) if (tile->is_selected()) saved.insert(id);
            }
            std::map<std::string, SPObject *> objects;
            if (doc && desktop) {
                if (mode() == P::Mode::Pages) for (auto *p : doc->getPageManager().getPages()) objects[p->getId()] = p;
                if (mode() == P::Mode::Layers) for (auto *p : desktop->layerManager().getAllLayers()) if (p->getId() && p->documentVisualBounds()) objects[p->getId()] = p;
            }
            BatchItem::syncItems(tile_items, objects, tiles, drawing, mode() == P::Mode::Layers);
            if (changed_mode && ticked.contains(mode())) for (auto const &[id, tile] : tile_items) tile->set_selected(ticked[mode()].contains(id));
            tile_mode = mode();
        }
        tiles.set_visible(tiled); preview.set_visible(!tiled && show.get_active());
        each.set_visible(mode() == P::Mode::Selection); custom.set_visible(mode() == P::Mode::Custom);
        auto in = input(); if (doc && hints && !edited) load_hints(in);
        auto items = P::items(in);
        for (auto const &row : rows) {
            auto *ext = row->format.getExtension(); bool raster = ext && ext->is_raster();
            row->size.set_visible(raster); row->dpi.set_visible(raster); row->dpi_label.set_visible(raster);
            bool one = raster && items.size() == 1;
            for (auto *widget : std::initializer_list<Gtk::Widget *>{&row->width, &row->height, &row->w_label, &row->h_label}) widget->set_visible(one);
            row->one_pdf.set_visible(ext && mode() == P::Mode::Pages && std::string(ext->get_extension()) == ".pdf");
            row->remove.set_sensitive(rows.size() > 1);
            if (one) { row->width.set_value(std::round(items[0].area.width() * row->dpi.get_value() / 96)); row->height.set_value(std::round(items[0].area.height() * row->dpi.get_value() / 96)); }
        }
        auto files = plan(); std::string text;
        for (std::size_t i = 0; i < std::min<std::size_t>(files.size(), 4); ++i) text += (i ? "\n" : "") + Glib::path_get_basename(files[i].filename);
        if (files.size() > 4) text += "\n" + Glib::ustring::compose(_("and %1 more"), files.size() - 4);
        filenames.set_text(text.empty() ? _("Nothing to export.") : text); run.set_sensitive(doc && !files.empty());
        if (drawing) {
            std::vector<SPItem const *> selected;
            if (hide.get_active() && desktop) for (auto *p : desktop->getSelection()->items()) selected.push_back(p);
            drawing->set_shown_items(std::move(selected));
            for (auto const &[id, tile] : tile_items) tile->refresh(!show.get_active(), background.get_current_color().toRGBA());
            if (!items.empty()) { preview.setBox(items[0].area); preview.setBackgroundColor(background.get_current_color().toRGBA()); if (show.get_active()) preview.queueRefresh(); }
        }
    }
    void set_context(SPDesktop *d, SPDocument *document) {
        if (busy) { stopped = true; return; }
        if (desktop == d && doc == document) return;
        pages_changed.disconnect(); page_modified.disconnect();
        updating = true;
        while (auto *child = tiles.get_first_child()) tiles.remove(*child);
        tile_items.clear(); ticked.clear(); tile_mode = P::Mode::Document; preview.setDrawing({}); drawing.reset(); desktop = d; doc = document; edited = false;
        if (doc) {
            drawing = std::make_shared<PreviewDrawing>(doc); preview.setDrawing(drawing);
            background.setColor(get_export_bg_color(doc->getNamedView(), Colors::Color(0xffffff00)));
            auto bounds = doc->preferredBounds();
            if (bounds) { coords[0].set_value(bounds->left()); coords[1].set_value(bounds->top()); coords[2].set_value(bounds->width()); coords[3].set_value(bounds->height()); }
            units.setUnit("px"); old_unit = units.getUnit();
            pages_changed = doc->getPageManager().connectPagesChanged([this](auto *) { refresh(false, true); });
            page_modified = doc->getPageManager().connectPageModified([this](auto *) { refresh(); });
        }
        updating = false; refresh(true, true);
    }
    void choose_folder() {
        if (!desktop) return;
        auto dialog = Gtk::FileDialog::create();
        if (!folder.get_text().empty()) dialog->set_initial_folder(Gio::File::create_for_path(Glib::filename_from_utf8(folder.get_text())));
        dialog->select_folder(*desktop->getInkscapeWindow(), sigc::track_object([this, dialog](auto &result) {
            try { if (auto f = dialog->select_folder_finish(result)) folder.set_text(Glib::filename_to_utf8(f->get_path())); }
            catch (Gtk::DialogError const &) {}
        }, root), {});
    }
    void message(Glib::ustring const &title, Glib::ustring const &text = "") {
        Gtk::MessageDialog dialog(*desktop->getInkscapeWindow(), title, false, Gtk::MessageType::INFO, Gtk::ButtonsType::CLOSE);
        dialog.set_secondary_text(text); UI::dialog_run(dialog);
    }
    static unsigned progress_callback(float value, void *data) {
        auto &p = *static_cast<Impl *>(data); p.progress.set_fraction((p.current + value) / p.total);
        Glib::MainContext::get_default()->iteration(false); return !p.stopped;
    }
    void write() {
        if (!doc || !desktop || busy) return;
        busy = true; stopped = false; body.set_sensitive(false); run.set_sensitive(false); cancel.set_visible(true);
        auto finish = scope_exit([&] { busy = false; body.set_sensitive(true); cancel.set_visible(false); progress.set_fraction(0); progress.set_text(""); refresh(false, true); });
        auto links = Extension::MissingLinkAction::RequirePresent;
        bool needs = false; for (auto const &r : rows) if (auto *e = r->format.getExtension()) needs |= e->needs_picture_bytes();
        if (needs && !sp_images_missing(doc).empty()) { links = resolveMissingLinksForSave(desktop, doc); if (links == Extension::MissingLinkAction::Cancel) return; }
        auto files = plan(); if (files.empty()) { message(_("Nothing to export.")); return; }
        if (!Export::checkOrCreateDirectory(Glib::filename_from_utf8(files.front().filename))) return;
        std::string conflicts; unsigned count = 0;
        for (auto const &file : files) if (file.conflict) { ++count; if (count <= 10) conflicts += Glib::path_get_basename(file.filename) + "\n"; }
        if (count) {
            Gtk::MessageDialog dialog(*desktop->getInkscapeWindow(), _("Replace existing files?"), false, Gtk::MessageType::QUESTION, Gtk::ButtonsType::NONE);
            dialog.set_secondary_text((count > 10 ? Glib::ustring::compose(_("%1 files already exist.\n"), count).raw() : conflicts) + _("They will be replaced. Cancel exports nothing."));
            dialog.add_button(_("Cancel"), Gtk::ResponseType::CANCEL); dialog.add_button(_("Replace"), Gtk::ResponseType::ACCEPT); dialog.set_default_response(Gtk::ResponseType::CANCEL);
            if (UI::dialog_run(dialog) != Gtk::ResponseType::ACCEPT) return;
        }
        auto specs = format_specs(); total = files.size(); std::string failures; P::File const *first_written = nullptr;
        for (current = 0; current < total && !stopped; ++current) {
            auto const &file = files[current]; auto const &spec = specs[file.format]; auto *ext = rows[file.format]->format.getExtension();
            progress.set_text(Glib::ustring::compose(_("File %1 of %2: %3"), current + 1, total, Glib::path_get_basename(file.filename)));
            progress_callback(0, this); if (stopped) break;
            bool ok = false;
            auto failure = [&](std::string const &reason) { failures += file.item.label + " (" + file.filename + "): " + reason + "\n"; };
            try {
                std::vector<SPItem const *> selected;
                for (auto const &id : file.item.objects) { auto *p = cast<SPItem>(doc->getObjectById(id)); if (!p) throw std::runtime_error("An export object is no longer available."); selected.push_back(p); }
                if (spec.raster) {
                    double w = std::round(file.item.area.width() * spec.dpi / 96), h = std::round(file.item.area.height() * spec.dpi / 96);
                    if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 2147483647 || h > 2147483647) throw std::runtime_error("The pixel size is out of range.");
                    ok = Export::exportRaster(file.item.area, static_cast<unsigned long>(w), static_cast<unsigned long>(h), spec.dpi, background.get_current_color(), file.filename, true, progress_callback, this, ext, selected.empty() ? nullptr : &selected);
                } else {
                    auto copy = doc->copy(); std::vector<SPPage const *> pages;
                    for (auto const &id : file.item.pages) { auto *p = cast<SPPage>(copy->getObjectById(id)); if (!p) throw std::runtime_error("An export page is no longer available."); pages.push_back(p); }
                    if (pages.empty()) { auto &pm = copy->getPageManager(); pm.disablePages(); copy->fitToRect(file.item.area, false); pm.enablePages(); pages.push_back(pm.getFirstPage()); }
                    ok = Export::exportVector(ext, copy.get(), file.filename, true, selected, pages, links);
                }
                if (!ok && !stopped) failure(_("Export failed."));
            } catch (std::exception const &e) { failure(e.what()); }
              catch (...) { failure(_("Unexpected export error.")); }
            if (ok) {
                if (!first_written) first_written = &file;
                if (auto recent = Gtk::RecentManager::get_default()) recent->add_item(Glib::filename_to_uri(Glib::filename_from_utf8(file.filename)));
                SPObject *target = doc->getRoot();
                if (file.item.pages.size() == 1) target = doc->getObjectById(file.item.pages[0]);
                else if (!file.item.hint_id.empty()) target = doc->getObjectById(file.item.hint_id);
                else if (mode() == P::Mode::Selection && desktop->getSelection()->size() == 1) target = desktop->getSelection()->firstItem();
                if (target) { target->setExportFilename(file.filename); if (spec.raster) target->setExportDpi(Geom::Point(spec.dpi, spec.dpi)); }
            }
        }
        if (first_written) {
            edited = false;
            doc->getRoot()->setExportFilename(first_written->filename);
            auto const &spec = specs[first_written->format];
            if (spec.raster) doc->getRoot()->setExportDpi(Geom::Point(spec.dpi, spec.dpi));
            doc->getRoot()->setAttribute("inkscape:export-batch-path", folder.get_text()); doc->getRoot()->setAttribute("inkscape:export-batch-name", name.get_text());
            prefs->setString("/dialogs/export/batch/path", folder.get_text());
            if (specs[0].raster) prefs->setDouble("/dialogs/export/defaultxdpi/value", specs[0].dpi);
            prefs->setString("/dialogs/export/format", rows.front()->format.get_active_id()); prefs->setInt("/dialogs/export/what", what.get_active_row_number());
            DocumentUndo::done(doc, RC_("Undo", "Set Export Options"), "export");
        }
        if (!failures.empty()) message(_("Some files could not be exported."), failures);
    }
};
ExportPanel::ExportPanel() : Gtk::Box(Gtk::Orientation::VERTICAL, 8), _impl(std::make_unique<Impl>(*this)) {}
ExportPanel::~ExportPanel() = default;
void ExportPanel::setContext(SPDesktop *desktop, SPDocument *doc) { _impl->set_context(desktop, doc); }
void ExportPanel::refresh(bool hints) { _impl->refresh(hints, true); }
} // namespace Inkscape::UI::Dialog
