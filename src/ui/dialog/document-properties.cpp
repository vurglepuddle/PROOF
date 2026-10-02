// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Document properties dialog, Gtkmm-style.
 */
/* Authors:
 *   bulia byak <buliabyak@users.sf.net>
 *   Bryce W. Harrington <bryce@bryceharrington.org>
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Jon Phillips <jon@rejon.org>
 *   Ralf Stephan <ralf@ark.in-berlin.de> (Gtkmm)
 *   Diederik van Lierop <mail@diedenrezi.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2006-2008 Johan Engelen  <johan@shouraizou.nl>
 * Copyright (C) 2000 - 2008 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "document-properties.h"

#include <giomm/themedicon.h>
#include <glibmm/main.h>
#include <gtkmm/dialog.h>
#include <gtkmm/liststore.h>
#include <gtkmm/spinbutton.h>

#include "colors/cms/profile.h"
#include "colors/document-cms.h"
#include "colors/document-colors.h"
#include "colors/spaces/cms.h"
#include "inkscape-window.h"
#include "object/color-profile.h"
#include "object/sp-guide.h"
#include "object/sp-root.h"
#include "object/sp-script.h"
#include "page-manager.h"
#include "rdf.h"
#include "selection.h"
#include "streq.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/icon-loader.h"
#include "ui/icon-names.h"
#include "ui/pack.h"
#include "ui/popup-menu.h"
#include "ui/util.h"
#include "ui/widget/alignment-selector.h"
#include "ui/widget/entity-entry.h"
#include "ui/widget/notebook-page.h"
#include "ui/widget/page-properties.h"
#include "ui/widget/document-color-settings.h"
#include "ui/widget/generic/popover-menu.h"
#include "ui/widget/generic/spin-button.h"
#include "util/expression-evaluator.h"

namespace Inkscape::UI {

namespace Widget {

class GridWidget final : public Gtk::Box
{
public:
    GridWidget(SPGrid *obj);

    void update();
    void updateModularGridLimits();
    SPGrid *getGrid() { return _grid; }
    XML::Node *getGridRepr() { return _repr; }

private:
    SPGrid *_grid = nullptr;
    XML::Node *_repr = nullptr;

    Gtk::Button* _delete = Gtk::make_managed<Gtk::Button>();
    Gtk::MenuButton* _options = Gtk::make_managed<Gtk::MenuButton>();
    Gtk::Popover* _opt_items = Gtk::make_managed<Gtk::Popover>();
    Gtk::Image* _icon = Gtk::make_managed<Gtk::Image>();
    Gtk::Label* _id = Gtk::make_managed<Gtk::Label>();
    Gtk::MenuButton* _align = Gtk::make_managed<Gtk::MenuButton>();
    Gtk::Popover* _align_popup = Gtk::make_managed<Gtk::Popover>();

    UI::Widget::Registry _wr;
    Inkscape::UI::Widget::IconComboBox _grid_type;
    RegisteredSwitchButton *_enabled = nullptr;
    RegisteredCheckButton *_snap_visible_only = nullptr;
    RegisteredToggleButton *_visible = nullptr;
    RegisteredCheckButton *_dotted = nullptr;
    AlignmentSelector *_alignment = nullptr;

    RegisteredUnitMenu *_units = nullptr;
    RegisteredScalarUnit *_origin_x = nullptr;
    RegisteredScalarUnit *_origin_y = nullptr;
    RegisteredScalarUnit *_spacing_x = nullptr;
    RegisteredScalarUnit *_spacing_y = nullptr;
    RegisteredScalar *_angle_x = nullptr;
    RegisteredScalar *_angle_z = nullptr;
    RegisteredCheckButton *_angle_y_vertical = nullptr;
    RegisteredColorPicker *_grid_color = nullptr;
    RegisteredInteger *_no_of_lines = nullptr;
    RegisteredScalarUnit* _gap_x = nullptr;
    RegisteredScalarUnit* _gap_y = nullptr;
    RegisteredScalarUnit* _margin_x = nullptr;
    RegisteredScalarUnit* _margin_y = nullptr;
    Gtk::MenuButton* _angle_popup = Gtk::make_managed<Gtk::MenuButton>();
    Gtk::Button* _swap_axes = Gtk::make_managed<Gtk::Button>();
    Gtk::Entry* _aspect_ratio = nullptr;

    sigc::scoped_connection _modified_signal;
};

} // namespace Widget

namespace Dialog {

static constexpr int SPACE_SIZE_X = 15;
static constexpr int SPACE_SIZE_Y = 10;

static void docprops_style_button(Gtk::Button& btn, char const* iconName)
{
    GtkWidget *child = sp_get_icon_image(iconName, GTK_ICON_SIZE_NORMAL);
    gtk_widget_set_visible(child, true);
    btn.set_child(*Gtk::manage(Glib::wrap(child)));
    btn.set_has_frame(false);
}

static bool do_remove_popup_menu(PopupMenuOptionalClick const click,
                                 Gtk::TreeView &tree_view, UI::Widget::PopoverBin &pb, sigc::slot<void ()> const &slot)
{
    auto const selection = tree_view.get_selection();
    if (!selection) return false;

    auto const it = selection->get_selected();
    if (!it) return false;

    auto const mi = Gtk::make_managed<UI::Widget::PopoverMenuItem>(_("_Remove"), true);
    mi->signal_activate().connect(slot);
    auto const menu = Gtk::make_managed<UI::Widget::PopoverMenu>(Gtk::PositionType::BOTTOM);
    menu->append(*mi);

    pb.setPopover(menu);

    if (click) {
        menu->popup_at(tree_view, click->x, click->y);
        return true;
    }

    auto const column = tree_view.get_column(0);
    g_return_val_if_fail(column, false);
    auto rect = Gdk::Rectangle{};
    tree_view.get_cell_area(Gtk::TreePath{it}, *column, rect);
    menu->popup_at(tree_view, rect.get_x() + rect.get_width () / 2.0,
                              rect.get_y() + rect.get_height());
    return true;
}

static void connect_remove_popup_menu(Gtk::TreeView &tree_view, UI::Widget::PopoverBin &pb, sigc::slot<void ()> slot)
{
    UI::on_popup_menu(tree_view, sigc::bind(&do_remove_popup_menu, std::ref(tree_view), std::ref(pb), std::move(slot)));
}

DocumentProperties::DocumentProperties()
    : DialogBase("/dialogs/documentoptions", "DocumentProperties")
    , _page_page(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1, true))
    , _page_guides(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1, true))
    , _page_cms(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1, true))
    , _page_scripting(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1, true))
    , _page_external_scripts(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1))
    , _page_embedded_scripts(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1))
    , _page_metadata1(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1, true))
    , _page_metadata2(Gtk::make_managed<UI::Widget::NotebookPage>(1, 1, true))
    //---------------------------------------------------------------
    // General guide options
    , _rcb_sgui(_("Show _guides"), _("Show or hide guides"), "showguides", _wr)
    , _rcb_lgui(_("Lock all guides"), _("Toggle lock of all guides in the document"), "inkscape:lockguides", _wr)
    , _rcp_gui(_("Guide co_lor:"), _("Guideline color"), _("Color of guidelines"), "guidecolor", "guideopacity", _wr)
    , _rcp_hgui(_("_Highlight color:"), _("Highlighted guideline color"),
                _("Color of a guideline when it is under mouse"), "guidehicolor", "guidehiopacity", _wr)
    , _create_guides_btn(_("Create guides around the current page"))
    , _delete_guides_btn(_("Delete all guides"))
    //---------------------------------------------------------------
    , _grids_label_def("", Gtk::Align::START)
    , _grids_vbox(Gtk::Orientation::VERTICAL)
    , _grids_hbox_crea(Gtk::Orientation::HORIZONTAL)
    // Attach nodeobservers to this document
    , _namedview_connection(this)
    , _root_connection(this)
{
    append(_popoverbin);
    _popoverbin.set_expand();
    _popoverbin.setChild(&_notebook);

    _notebook.append_page(*_page_page,      _("Display"));
    _notebook.append_page(*_page_guides,    _("Guides"));
    _notebook.append_page(_grids_vbox,      _("Grids"));
    _notebook.append_page(*_page_cms,       _("Color"));
    _notebook.append_page(*_page_scripting, _("Scripting"));
    _notebook.append_page(*_page_metadata1, _("Metadata"));
    _notebook.append_page(*_page_metadata2, _("License"));
    _notebook.signal_switch_page().connect([this](Gtk::Widget const *, unsigned const page){
        // we cannot use widget argument, as this notification fires during destruction with all pages passed one by one
        // page no 3 - cms
        if (page == 3) {
            // lazy-load color profiles; it can get prohibitively expensive when hundreds are installed
            populate_available_profiles();
        }
    });

    _wr.setUpdating (true);
    build_page();
    build_guides();
    build_gridspage();
    build_cms();
    build_scripting();
    build_metadata();
    _wr.setUpdating (false);
}

DocumentProperties::~DocumentProperties() = default;

//========================================================================

/**
 * Helper function that sets widgets in a 2 by n table.
 * arr has two entries per table row. Each row is in the following form:
 *     widget, widget -> function adds a widget in each column.
 *     nullptr, widget -> function adds a widget that occupies the row.
 *     label, nullptr -> function adds label that occupies the row.
 *     nullptr, nullptr -> function adds an empty box that occupies the row.
 * This used to be a helper function for a 3 by n table
 */
void attach_all(Gtk::Grid &table, Gtk::Widget *const arr[], unsigned const n)
{
    for (unsigned i = 0, r = 0; i < n; i += 2) {
        if (arr[i] && arr[i+1]) {
            arr[i]->set_hexpand();
            arr[i+1]->set_hexpand();
            arr[i]->set_valign(Gtk::Align::CENTER);
            arr[i+1]->set_valign(Gtk::Align::CENTER);
            table.attach(*arr[i],   0, r, 1, 1);
            table.attach(*arr[i+1], 1, r, 1, 1);
        } else {
            if (arr[i+1]) {
                arr[i+1]->set_hexpand();
                arr[i+1]->set_valign(Gtk::Align::CENTER);
                table.attach(*arr[i+1], 0, r, 2, 1);
            } else if (arr[i]) {
                auto &label = dynamic_cast<Gtk::Label &>(*arr[i]);
                label.set_hexpand();
                label.set_halign(Gtk::Align::START);
                label.set_valign(Gtk::Align::CENTER);
                table.attach(label, 0, r, 2, 1);
            } else {
                auto const space = Gtk::make_managed<Gtk::Box>();
                space->set_size_request (SPACE_SIZE_X, SPACE_SIZE_Y);
                space->set_halign(Gtk::Align::CENTER);
                space->set_valign(Gtk::Align::CENTER);
                table.attach(*space, 0, r, 1, 1);
            }
        }
        ++r;
    }
}

void set_namedview_bool(SPDesktop* desktop, Inkscape::Util::Internal::ContextString operation, SPAttr key, bool on) {
    if (!desktop || !desktop->getDocument()) return;

    desktop->getNamedView()->change_bool_setting(key, on);

    desktop->getDocument()->setModifiedSinceSave();
    DocumentUndo::done(desktop->getDocument(), operation, "");
}

void set_color(SPDesktop* desktop, const char* key, Inkscape::Util::Internal::ContextString operation, SPAttr color_key, SPAttr opacity_key, Colors::Color const &color) {
    if (!desktop || !desktop->getDocument()) return;

    desktop->getNamedView()->change_color(color_key, opacity_key, color);
    desktop->getDocument()->setModifiedSinceSave();
    DocumentUndo::maybeDone(desktop->getDocument(), key, operation, "");
}

void set_document_dimensions(SPDesktop* desktop, double width, double height, const Inkscape::Util::Unit* unit) {
    if (!desktop) return;

    auto new_width_q = Inkscape::Util::Quantity(width, unit);
    auto new_height_q = Inkscape::Util::Quantity(height, unit);
    SPDocument* doc = desktop->getDocument();
    Inkscape::Util::Quantity const old_height_q = doc->getHeight();
    auto rect = Geom::Rect(Geom::Point(0, 0), Geom::Point(new_width_q.value("px"), new_height_q.value("px")));
    doc->fitToRect(rect, false);

    // The origin for the user is in the lower left corner; this point should remain stationary when
    // changing the page size. The SVG's origin however is in the upper left corner, so we must compensate for this
    if (!doc->yaxisdown()) {
        auto const vert_offset = Geom::Translate(Geom::Point(0, (old_height_q.value("px") - new_height_q.value("px"))));
        doc->getRoot()->translateChildItems(vert_offset);
    } else {
        // when this yaxisdown is true, we need to translate just the guides
        // the guides simply need their new converted positions
        // in reference to: https://gitlab.com/inkscape/inkscape/-/issues/1230
        for (auto guide : doc->getNamedView()->guides) {
            guide->moveto(guide->getPoint() * Geom::Translate(0, 0), true);
        }
    }

    // units: this is most likely not needed, units are part of document size attributes
    // if (unit) {
        // set_namedview_value(desktop, "", SPAttr::UNITS)
        // write_str_to_xml(desktop, _("Set document unit"), "unit", unit->abbr.c_str());
    // }
    doc->setWidthAndHeight(new_width_q, new_height_q, true);

    DocumentUndo::done(doc, RC_("Undo", "Set page size"), "");
}

void DocumentProperties::set_viewbox_pos(SPDesktop* desktop, double x, double y) {
    if (!desktop) return;

    auto document = desktop->getDocument();
    if (!document) return;

    auto box = document->getViewBox();
    document->setViewBox(Geom::Rect::from_xywh(x, y, box.width(), box.height()));
    DocumentUndo::done(document, RC_("Undo", "Set viewbox position"), "");
    update_scale_ui(desktop);
}

void DocumentProperties::set_viewbox_size(SPDesktop* desktop, double width, double height) {
    if (!desktop) return;

    auto document = desktop->getDocument();
    if (!document) return;

    auto box = document->getViewBox();
    document->setViewBox(Geom::Rect::from_xywh(box.min()[Geom::X], box.min()[Geom::Y], width, height));
    DocumentUndo::done(document, RC_("Undo", "Set viewbox size"), "");
    update_scale_ui(desktop);
}

// helper function to set document scale; uses magnitude of document width/height only, not computed (pixel) values
void set_document_scale_helper(SPDocument& document, double scale) {
    if (scale <= 0) return;

    auto root = document.getRoot();
    auto box = document.getViewBox();
    document.setViewBox(Geom::Rect::from_xywh(
        box.min()[Geom::X], box.min()[Geom::Y],
        root->width.value / scale, root->height.value / scale)
    );
}

void DocumentProperties::set_content_scale(SPDesktop *desktop, double scale)
{
    if (!desktop) return;

    auto document = desktop->getDocument();
    if (!document) return;

    if (scale > 0) {
        auto old_scale = document->getDocumentScale(false);
        auto delta = old_scale * Geom::Scale(scale).inverse();

        // Shapes in the document
        document->scaleContentBy(delta);

        // Pages, margins and bleeds
        document->getPageManager().scalePages(delta);

        // Grids
        if (auto nv = document->getNamedView()) {
            for (auto grid : nv->grids) {
                grid->scale(delta);
            }
        }
    }
}

void DocumentProperties::set_document_scale(SPDesktop* desktop, double scale) {
    if (!desktop) return;

    auto document = desktop->getDocument();
    if (!document) return;

    if (scale > 0) {
        set_document_scale_helper(*document, scale);
        update_viewbox_ui(desktop);
        update_scale_ui(desktop);
        DocumentUndo::done(document, RC_("Undo", "Set page scale"), "");
    }
}

// document scale as a ratio of document size and viewbox size
// as described in Wiki: https://wiki.inkscape.org/wiki/index.php/Units_In_Inkscape
// for example: <svg width="100mm" height="100mm" viewBox="0 0 100 100"> will report 1:1 scale
std::optional<Geom::Scale> get_document_scale_helper(SPDocument& doc) {
    auto root = doc.getRoot();
    if (root &&
        root->width._set  && root->width.unit  != SVGLength::PERCENT &&
        root->height._set && root->height.unit != SVGLength::PERCENT) {
        if (root->viewBox_set) {
            // viewbox and document size present
            auto vw = root->viewBox.width();
            auto vh = root->viewBox.height();
            if (vw > 0 && vh > 0) {
                return Geom::Scale(root->width.value / vw, root->height.value / vh);
            }
        } else {
            // no viewbox, use SVG size in pixels
            auto w = root->width.computed;
            auto h = root->height.computed;
            if (w > 0 && h > 0) {
                return Geom::Scale(root->width.value / w, root->height.value / h);
            }
        }
    }

    // there is no scale concept applicable in the current state
    return std::optional<Geom::Scale>();
}

void DocumentProperties::update_scale_ui(SPDesktop* desktop) {
    if (!desktop) return;

    auto document = desktop->getDocument();
    if (!document) return;

    using UI::Widget::PageProperties;
    if (auto scale = get_document_scale_helper(*document)) {
        auto sx = (*scale)[Geom::X];
        auto sy = (*scale)[Geom::Y];
        double eps = 0.0001; // TODO: tweak this value
        bool uniform = fabs(sx - sy) < eps;
        _page->set_dimension(PageProperties::Dimension::Scale, sx, sx); // only report one, only one "scale" is used
        _page->set_check(PageProperties::Check::NonuniformScale, !uniform);
        _page->set_check(PageProperties::Check::DisabledScale, false);
    } else {
        // no scale
        _page->set_dimension(PageProperties::Dimension::Scale, 1, 1);
        _page->set_check(PageProperties::Check::NonuniformScale, false);
        _page->set_check(PageProperties::Check::DisabledScale, true);
    }
}

void DocumentProperties::update_viewbox_ui(SPDesktop* desktop) {
    if (!desktop) return;

    auto document = desktop->getDocument();
    if (!document) return;

    using UI::Widget::PageProperties;
    Geom::Rect viewBox = document->getViewBox();
    _page->set_dimension(PageProperties::Dimension::ViewboxPosition, viewBox.min()[Geom::X], viewBox.min()[Geom::Y]);
    _page->set_dimension(PageProperties::Dimension::ViewboxSize, viewBox.width(), viewBox.height());
}

void DocumentProperties::build_page()
{
    using UI::Widget::PageProperties;
    _page = Gtk::manage(PageProperties::create());
    _page_page->table().attach(*_page, 0, 0);

    _page->signal_color_changed().connect([this](Colors::Color const &color, PageProperties::Color const element){
        if (_wr.isUpdating() || !_wr.desktop()) return;

        _wr.setUpdating(true);
        switch (element) {
            case PageProperties::Color::Desk:
                set_color(_wr.desktop(), "document-color-desk", RC_("Undo", "Desk color"), SPAttr::INKSCAPE_DESK_COLOR, SPAttr::INKSCAPE_DESK_OPACITY, color);
                break;
            case PageProperties::Color::Background:
                set_color(_wr.desktop(), "document-color-background", RC_("Undo", "Background color"), SPAttr::PAGECOLOR, SPAttr::INKSCAPE_PAGEOPACITY, color);
                break;
            case PageProperties::Color::Border:
                set_color(_wr.desktop(), "document-color-border", RC_("Undo", "Border color"), SPAttr::BORDERCOLOR, SPAttr::BORDEROPACITY, color);
                break;
        }
        _wr.setUpdating(false);
    });

    _page->signal_dimension_changed().connect([this](double const x, double const y,
                                                     auto const unit,
                                                     PageProperties::Dimension const element)
    {
        if (_wr.isUpdating() || !_wr.desktop()) return;

        _wr.setUpdating(true);
        switch (element) {
            case PageProperties::Dimension::PageTemplate:
            case PageProperties::Dimension::PageSize:
                set_document_dimensions(_wr.desktop(), x, y, unit);
                update_viewbox(_wr.desktop());
                break;

            case PageProperties::Dimension::ViewboxSize:
                set_viewbox_size(_wr.desktop(), x, y);
                break;

            case PageProperties::Dimension::ViewboxPosition:
                set_viewbox_pos(_wr.desktop(), x, y);
                break;

            case PageProperties::Dimension::ScaleContent:
                set_content_scale(_wr.desktop(), x);
            case PageProperties::Dimension::Scale:
                set_document_scale(_wr.desktop(), x); // only uniform scale; there's no 'y' in the dialog
                break;
        }
        _wr.setUpdating(false);
    });

    _page->signal_check_toggled().connect([this](bool const checked, PageProperties::Check const element){
        if (_wr.isUpdating() || !_wr.desktop()) return;

        _wr.setUpdating(true);
        switch (element) {
            case PageProperties::Check::Checkerboard:
                set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle checkerboard"), SPAttr::INKSCAPE_DESK_CHECKERBOARD, checked);
                break;
            case PageProperties::Check::Border:
                set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle page border"), SPAttr::SHOWBORDER, checked);
                break;
            case PageProperties::Check::BorderOnTop:
                set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle border on top"), SPAttr::BORDERLAYER, checked);
                break;
            case PageProperties::Check::Shadow:
                set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle page shadow"), SPAttr::SHOWPAGESHADOW, checked);
                break;
            case PageProperties::Check::AntiAlias:
                set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle anti-aliasing"), SPAttr::INKSCAPE_ANTIALIAS_RENDERING, checked);
                break;
            case PageProperties::Check::ClipToPage:
                set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle clip to page mode"), SPAttr::INKSCAPE_CLIP_TO_PAGE_RENDERING, checked);
                break;
            case PageProperties::Check::PageLabelStyle:
                set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle page label style"), SPAttr::PAGELABELSTYLE, checked);
                break;
        case PageProperties::Check::YAxisPointsDown:
            set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle system coordinate Y axis orientation"), SPAttr::INKSCAPE_Y_AXIS_DOWN, checked);
            break;
        case PageProperties::Check::OriginCurrentPage:
            set_namedview_bool(_wr.desktop(), RC_("Undo", "Toggle system coordinate origin correction"), SPAttr::INKSCAPE_ORIGIN_CORRECTION, checked);
            break;
        }
        _wr.setUpdating(false);
    });

    _page->signal_unit_changed().connect([this](Inkscape::Util::Unit const * const unit, PageProperties::Units const element){
        if (_wr.isUpdating() || !_wr.desktop()) return;

        if (element == PageProperties::Units::Display) {
            // display only units
            display_unit_change(unit);
        }
        else if (element == PageProperties::Units::Document) {
            // not used, fired with page size
        }
    });

    _page->signal_resize_to_fit().connect([this]{
        if (_wr.isUpdating() || !_wr.desktop()) return;

        if (auto document = getDocument()) {
            auto &page_manager = document->getPageManager();
            page_manager.selectPage(0);
            // fit page to selection or content, if there's no selection
            page_manager.fitToSelection(_wr.desktop()->getSelection());
            DocumentUndo::done(document, RC_("Undo", "Resize page to fit"), INKSCAPE_ICON("tool-pages"));
            update_widgets();
        }
    });
}

void DocumentProperties::build_guides()
{
    auto const label_gui = Gtk::make_managed<Gtk::Label>();
    label_gui->set_markup (_("<b>Guides</b>"));

    _rcp_gui.set_margin_start(0);
    _rcp_hgui.set_margin_start(0);
    _rcp_gui.set_hexpand();
    _rcp_hgui.set_hexpand();
    _rcb_sgui.set_hexpand();
    auto const inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    inner->append(_rcb_sgui);
    inner->append(_rcb_lgui);
    inner->append(_rcp_gui);
    inner->append(_rcp_hgui);
    auto const spacer = Gtk::make_managed<Gtk::Label>();
    Gtk::Widget *const widget_array[] =
    {
        label_gui, nullptr,
        inner,     spacer,
        nullptr,   nullptr,
        nullptr,   &_create_guides_btn,
        nullptr,   &_delete_guides_btn
    };
    attach_all(_page_guides->table(), widget_array, G_N_ELEMENTS(widget_array));
    inner->set_hexpand(false);

    _create_guides_btn.set_action_name("doc.create-guides-around-page");
    _delete_guides_btn.set_action_name("doc.delete-all-guides");
}

/// Populates the available color profiles combo box
void DocumentProperties::populate_available_profiles(){
    // scanning can be expensive; avoid if possible
    if (!_AvailableProfilesListStore->children().empty()) return;

    _AvailableProfilesListStore->clear(); // Clear any existing items in the combo box

    // Iterate through the list of profiles and add the name to the combo box.
    bool home = true; // initial value doesn't matter, it's just to avoid a compiler warning
    bool first = true;
    auto &cms_system = Inkscape::Colors::CMS::System::get();
    cms_system.refreshProfiles();
    for (auto const &profile: cms_system.getProfiles()) {
        Gtk::TreeModel::Row row;

        // add a separator between profiles from the user's home directory and system profiles
        if (!first && profile->inHome() != home)
        {
          row = *(_AvailableProfilesListStore->append());
          row[_AvailableProfilesListColumns.fileColumn] = "<separator>";
          row[_AvailableProfilesListColumns.nameColumn] = "<separator>";
          row[_AvailableProfilesListColumns.separatorColumn] = true;
        }
        home = profile->inHome();
        first = false;

        row = *(_AvailableProfilesListStore->append());
        row[_AvailableProfilesListColumns.fileColumn] = profile->getPath();
        row[_AvailableProfilesListColumns.nameColumn] = profile->getName();
        row[_AvailableProfilesListColumns.separatorColumn] = false;
    }
}

/**
 * Cleans up name to remove disallowed characters.
 * Some discussion at http://markmail.org/message/bhfvdfptt25kgtmj
 * Allowed ASCII first characters:  ':', 'A'-'Z', '_', 'a'-'z'
 * Allowed ASCII remaining chars add: '-', '.', '0'-'9',
 *
 * @param str the string to clean up.
 *
 * Note: for use with ICC profiles only.
 * This function has been restored to make ICC profiles work, as their names need to be sanitized.
 * BUT, it is not clear to me whether we really need to strip all non-ASCII characters.
 * We do it currently, because sp_svg_read_icc_color cannot parse Unicode.
 */
void sanitizeName(std::string& str) {
    if (str.empty()) return;

    auto val = str.at(0);
    if ((val < 'A' || val > 'Z') && (val < 'a' || val > 'z') && val != '_' && val != ':') {
        str.insert(0, "_");
    }
    for (std::size_t i = 1; i < str.size(); i++) {
        auto val = str.at(i);
        if ((val < 'A' || val > 'Z') && (val < 'a' || val > 'z') && (val < '0' || val > '9') &&
            val != '_' && val != ':' && val != '-' && val != '.') {
            if (str.at(i - 1) == '-') {
                str.erase(i, 1);
                i--;
            } else {
                str.replace(i, 1, "-");
            }
        }
    }
    if (str.at(str.size() - 1) == '-') {
        str.pop_back();
    }
}

/// Links the selected color profile in the combo box to the document
void DocumentProperties::linkSelectedProfile()
{
    //store this profile in the SVG document (create <color-profile> element in the XML)
    if (auto document = getDocument()){
        // Find the index of the currently-selected row in the color profiles combobox
        Gtk::TreeModel::iterator iter = _AvailableProfilesList.get_active();
        if (!iter)
            return;

        // Read the filename and description from the list of available profiles
        Glib::ustring file = (*iter)[_AvailableProfilesListColumns.fileColumn];

        document->getDocumentCMS().attachProfileToDoc(file, ColorProfileStorage::HREF_FILE, Colors::RenderingIntent::AUTO);
        // inform the document, so we can undo
        DocumentUndo::done(document, RC_("Undo", "Link Color Profile"), "");

        populate_linked_profiles_box();
    }
}

template <typename From, typename To>
struct static_caster { To * operator () (From * value) const { return static_cast<To *>(value); } };

void DocumentProperties::populate_linked_profiles_box()
{
    _LinkedProfilesListStore->clear();
    if (auto document = getDocument()) {
        std::vector<SPObject *> current = document->getResourceList( "iccprofile" );

        std::set<Inkscape::ColorProfile *> _current;
        std::transform(current.begin(),
                       current.end(),
                       std::inserter(_current, _current.begin()),
                       static_caster<SPObject, Inkscape::ColorProfile>());

        for (auto const &profile: _current) {
            auto assigned = Colors::DocumentColors::assignedSpace(document);
            if (assigned && assigned->getName() == profile->getName()) continue;
            Gtk::TreeModel::Row row = *(_LinkedProfilesListStore->append());
            row[_LinkedProfilesListColumns.nameColumn] = profile->getName();
        }
    }
}

void DocumentProperties::onColorProfileSelectRow()
{
    Glib::RefPtr<Gtk::TreeSelection> sel = _LinkedProfilesList.get_selection();
    if (sel) {
        _unlink_btn.set_sensitive(sel->count_selected_rows () > 0);
    }
}

void DocumentProperties::removeSelectedProfile(){
    Glib::ustring name;
    if(_LinkedProfilesList.get_selection()) {
        Gtk::TreeModel::iterator i = _LinkedProfilesList.get_selection()->get_selected();

        if(i){
            name = (*i)[_LinkedProfilesListColumns.nameColumn];
        } else {
            return;
        }
    }
    if (auto document = getDocument()) {
        if (auto colorprofile = document->getDocumentCMS().getColorProfileForSpace(name)) {
            auto assigned = Colors::DocumentColors::assignedSpace(document);
            if (assigned && assigned->getName() == name) return;
            colorprofile->deleteObject(true, false);
            DocumentUndo::done(document, RC_("Undo", "Remove linked color profile"), "");
        }
    }

    populate_linked_profiles_box();
    onColorProfileSelectRow();
}

bool DocumentProperties::_AvailableProfilesList_separator(Glib::RefPtr<Gtk::TreeModel> const &model,
                                                          Gtk::TreeModel::const_iterator const &iter)
{
    bool separator = (*iter)[_AvailableProfilesListColumns.separatorColumn];
    return separator;
}

void DocumentProperties::build_cms()
{
    _document_colors = Gtk::make_managed<UI::Widget::DocumentColorSettings>();
    _page_cms->table().attach(*_document_colors, 0, 0, 3, 1);
    _document_colors->set_margin_bottom(20);
    Gtk::Label *label_link= Gtk::make_managed<Gtk::Label>("", Gtk::Align::START);
    label_link->set_markup (_("<b>Additional Linked Profiles:</b>"));
    label_link->set_tooltip_text(_("Profiles available to individual colors. Use Document Color above to assign the document profile."));
    auto const label_avail = Gtk::make_managed<Gtk::Label>("", Gtk::Align::START);
    label_avail->set_markup (_("<b>Available Color Profiles:</b>"));

    _unlink_btn.set_tooltip_text(_("Unlink Profile"));
    docprops_style_button(_unlink_btn, INKSCAPE_ICON("list-remove"));

    int row = 1;

    label_link->set_hexpand();
    label_link->set_halign(Gtk::Align::START);
    label_link->set_valign(Gtk::Align::CENTER);
    _page_cms->table().attach(*label_link, 0, row, 3, 1);

    row++;

    _LinkedProfilesListScroller.set_hexpand();
    _LinkedProfilesListScroller.set_valign(Gtk::Align::CENTER);
    _page_cms->table().attach(_LinkedProfilesListScroller, 0, row, 3, 1);

    row++;

    auto const spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    spacer->set_size_request(SPACE_SIZE_X, SPACE_SIZE_Y);

    spacer->set_hexpand();
    spacer->set_valign(Gtk::Align::CENTER);
    _page_cms->table().attach(*spacer, 0, row, 3, 1);

    row++;

    label_avail->set_hexpand();
    label_avail->set_halign(Gtk::Align::START);
    label_avail->set_valign(Gtk::Align::CENTER);
    _page_cms->table().attach(*label_avail, 0, row, 3, 1);

    row++;

    _AvailableProfilesList.set_hexpand();
    _AvailableProfilesList.set_valign(Gtk::Align::CENTER);
    _page_cms->table().attach(_AvailableProfilesList, 0, row, 1, 1);

    _unlink_btn.set_halign(Gtk::Align::CENTER);
    _unlink_btn.set_valign(Gtk::Align::CENTER);
    _page_cms->table().attach(_unlink_btn, 2, row, 1, 1);

    // Set up the Available Profiles combo box
    _AvailableProfilesListStore = Gtk::ListStore::create(_AvailableProfilesListColumns);
    _AvailableProfilesList.set_model(_AvailableProfilesListStore);
    _AvailableProfilesList.pack_start(_AvailableProfilesListColumns.nameColumn);
    _AvailableProfilesList.set_row_separator_func(sigc::mem_fun(*this, &DocumentProperties::_AvailableProfilesList_separator));
    _AvailableProfilesList.signal_changed().connect( sigc::mem_fun(*this, &DocumentProperties::linkSelectedProfile) );

    //# Set up the Linked Profiles combo box
    _LinkedProfilesListStore = Gtk::ListStore::create(_LinkedProfilesListColumns);
    _LinkedProfilesList.set_model(_LinkedProfilesListStore);
    _LinkedProfilesList.append_column(_("Profile Name"), _LinkedProfilesListColumns.nameColumn);
//    _LinkedProfilesList.append_column(_("Color Preview"), _LinkedProfilesListColumns.previewColumn);
    _LinkedProfilesList.set_headers_visible(false);
// TODO restore?    _LinkedProfilesList.set_fixed_height_mode(true);

    populate_linked_profiles_box();

    _LinkedProfilesListScroller.set_child(_LinkedProfilesList);
    _LinkedProfilesListScroller.set_has_frame(true);
    _LinkedProfilesListScroller.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::ALWAYS);
    _LinkedProfilesListScroller.set_size_request(-1, 90);

    _unlink_btn.signal_clicked().connect(sigc::mem_fun(*this, &DocumentProperties::removeSelectedProfile));

    _LinkedProfilesList.get_selection()->signal_changed().connect( sigc::mem_fun(*this, &DocumentProperties::onColorProfileSelectRow) );

    connect_remove_popup_menu(_LinkedProfilesList, _popoverbin, sigc::mem_fun(*this, &DocumentProperties::removeSelectedProfile));
}

void DocumentProperties::build_scripting()
{
    _page_scripting->table().attach(_scripting_notebook, 0, 0, 1, 1);

    _scripting_notebook.append_page(*_page_external_scripts, _("External scripts"));
    _scripting_notebook.append_page(*_page_embedded_scripts, _("Embedded scripts"));

    //# External scripts tab
    Gtk::Label *label_external= Gtk::make_managed<Gtk::Label>("", Gtk::Align::START);
    label_external->set_markup (_("<b>External script files:</b>"));

    _external_add_btn.set_tooltip_text(_("Add the current file name or browse for a file"));
    docprops_style_button(_external_add_btn, INKSCAPE_ICON("list-add"));

    _external_remove_btn.set_tooltip_text(_("Remove"));
    docprops_style_button(_external_remove_btn, INKSCAPE_ICON("list-remove"));

    int row = 0;

    label_external->set_hexpand();
    label_external->set_halign(Gtk::Align::START);
    label_external->set_valign(Gtk::Align::CENTER);
    _page_external_scripts->table().attach(*label_external, 0, row, 3, 1);

    row++;

    _ExternalScriptsListScroller.set_hexpand();
    _ExternalScriptsListScroller.set_valign(Gtk::Align::CENTER);
    _page_external_scripts->table().attach(_ExternalScriptsListScroller, 0, row, 3, 1);

    row++;

    auto const spacer_external = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    spacer_external->set_size_request(SPACE_SIZE_X, SPACE_SIZE_Y);

    spacer_external->set_hexpand();
    spacer_external->set_valign(Gtk::Align::CENTER);
    _page_external_scripts->table().attach(*spacer_external, 0, row, 3, 1);

    row++;

    _script_entry.set_hexpand();
    _script_entry.set_valign(Gtk::Align::CENTER);
    _page_external_scripts->table().attach(_script_entry, 0, row, 1, 1);

    _external_add_btn.set_halign(Gtk::Align::CENTER);
    _external_add_btn.set_valign(Gtk::Align::CENTER);
    _external_add_btn.set_margin_start(2);
    _external_add_btn.set_margin_end(2);

    _page_external_scripts->table().attach(_external_add_btn, 1, row, 1, 1);

    _external_remove_btn.set_halign(Gtk::Align::CENTER);
    _external_remove_btn.set_valign(Gtk::Align::CENTER);
    _page_external_scripts->table().attach(_external_remove_btn, 2, row, 1, 1);

    //# Set up the External Scripts box
    _ExternalScriptsListStore = Gtk::ListStore::create(_ExternalScriptsListColumns);
    _ExternalScriptsList.set_model(_ExternalScriptsListStore);
    _ExternalScriptsList.append_column(_("Filename"), _ExternalScriptsListColumns.filenameColumn);
    _ExternalScriptsList.set_headers_visible(true);
// TODO restore?    _ExternalScriptsList.set_fixed_height_mode(true);

    //# Embedded scripts tab
    Gtk::Label *label_embedded= Gtk::make_managed<Gtk::Label>("", Gtk::Align::START);
    label_embedded->set_markup (_("<b>Embedded script files:</b>"));

    _embed_new_btn.set_tooltip_text(_("New"));
    docprops_style_button(_embed_new_btn, INKSCAPE_ICON("list-add"));

    _embed_remove_btn.set_tooltip_text(_("Remove"));
    docprops_style_button(_embed_remove_btn, INKSCAPE_ICON("list-remove"));

    _embed_button_box.append(_embed_new_btn);
    _embed_button_box.append(_embed_remove_btn);
    _embed_button_box.set_halign(Gtk::Align::END);

    row = 0;

    label_embedded->set_hexpand();
    label_embedded->set_halign(Gtk::Align::START);
    label_embedded->set_valign(Gtk::Align::CENTER);
    _page_embedded_scripts->table().attach(*label_embedded, 0, row, 3, 1);

    row++;

    _EmbeddedScriptsListScroller.set_hexpand();
    _EmbeddedScriptsListScroller.set_valign(Gtk::Align::CENTER);
    _page_embedded_scripts->table().attach(_EmbeddedScriptsListScroller, 0, row, 3, 1);

    row++;

    _embed_button_box.set_hexpand();
    _embed_button_box.set_valign(Gtk::Align::CENTER);
    _page_embedded_scripts->table().attach(_embed_button_box, 0, row, 1, 1);

    row++;

    auto const spacer_embedded = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    spacer_embedded->set_size_request(SPACE_SIZE_X, SPACE_SIZE_Y);
    spacer_embedded->set_hexpand();
    spacer_embedded->set_valign(Gtk::Align::CENTER);
    _page_embedded_scripts->table().attach(*spacer_embedded, 0, row, 3, 1);

    row++;

    //# Set up the Embedded Scripts box
    _EmbeddedScriptsListStore = Gtk::ListStore::create(_EmbeddedScriptsListColumns);
    _EmbeddedScriptsList.set_model(_EmbeddedScriptsListStore);
    _EmbeddedScriptsList.append_column(_("Script ID"), _EmbeddedScriptsListColumns.idColumn);
    _EmbeddedScriptsList.set_headers_visible(true);
// TODO restore?    _EmbeddedScriptsList.set_fixed_height_mode(true);

    //# Set up the Embedded Scripts content box
    Gtk::Label *label_embedded_content= Gtk::make_managed<Gtk::Label>("", Gtk::Align::START);
    label_embedded_content->set_markup (_("<b>Content:</b>"));

    label_embedded_content->set_hexpand();
    label_embedded_content->set_halign(Gtk::Align::START);
    label_embedded_content->set_valign(Gtk::Align::CENTER);
    _page_embedded_scripts->table().attach(*label_embedded_content, 0, row, 3, 1);

    row++;

    _EmbeddedContentScroller.set_hexpand();
    _EmbeddedContentScroller.set_valign(Gtk::Align::CENTER);
    _page_embedded_scripts->table().attach(_EmbeddedContentScroller, 0, row, 3, 1);

    _EmbeddedContentScroller.set_child(_EmbeddedContent);
    _EmbeddedContentScroller.set_has_frame(true);
    _EmbeddedContentScroller.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    _EmbeddedContentScroller.set_size_request(-1, 140);

    _EmbeddedScriptsList.signal_cursor_changed().connect(sigc::mem_fun(*this, &DocumentProperties::changeEmbeddedScript));
    _EmbeddedScriptsList.get_selection()->signal_changed().connect( sigc::mem_fun(*this, &DocumentProperties::onEmbeddedScriptSelectRow) );

    _ExternalScriptsList.get_selection()->signal_changed().connect( sigc::mem_fun(*this, &DocumentProperties::onExternalScriptSelectRow) );

    _EmbeddedContent.get_buffer()->signal_changed().connect(sigc::mem_fun(*this, &DocumentProperties::editEmbeddedScript));

    _ExternalScriptsListScroller.set_child(_ExternalScriptsList);
    _ExternalScriptsListScroller.set_has_frame(true);
    _ExternalScriptsListScroller.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::ALWAYS);
    _ExternalScriptsListScroller.set_size_request(-1, 90);

    _external_add_btn.signal_clicked().connect(sigc::mem_fun(*this, &DocumentProperties::addExternalScript));

    _EmbeddedScriptsListScroller.set_child(_EmbeddedScriptsList);
    _EmbeddedScriptsListScroller.set_has_frame(true);
    _EmbeddedScriptsListScroller.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::ALWAYS);
    _EmbeddedScriptsListScroller.set_size_request(-1, 90);

    _embed_new_btn.signal_clicked().connect(sigc::mem_fun(*this, &DocumentProperties::addEmbeddedScript));

    _external_remove_btn.signal_clicked().connect(sigc::mem_fun(*this, &DocumentProperties::removeExternalScript));
    _embed_remove_btn.signal_clicked().connect(sigc::mem_fun(*this, &DocumentProperties::removeEmbeddedScript));

    connect_remove_popup_menu(_ExternalScriptsList, _popoverbin, sigc::mem_fun(*this, &DocumentProperties::removeExternalScript));
    connect_remove_popup_menu(_EmbeddedScriptsList, _popoverbin, sigc::mem_fun(*this, &DocumentProperties::removeEmbeddedScript));

    populate_script_lists();
    _scripts_observer.signal_changed().connect([this](auto, auto){populate_script_lists();});

    // Set initial button sensitivity
    onEmbeddedScriptSelectRow();
    onExternalScriptSelectRow();
}

void DocumentProperties::build_metadata()
{
    using Inkscape::UI::Widget::EntityEntry;

    auto const label = Gtk::make_managed<Gtk::Label>();
    label->set_markup (_("<b>Dublin Core Entities</b>"));
    label->set_halign(Gtk::Align::START);
    label->set_valign(Gtk::Align::CENTER);
    _page_metadata1->table().attach (*label, 0,0,2,1);

     /* add generic metadata entry areas */
    int row = 1;
    for (auto entity = rdf_work_entities; entity && entity->name; ++entity, ++row) {
        if (entity->editable == RDF_EDIT_GENERIC) {
            auto w = std::unique_ptr<EntityEntry>{EntityEntry::create(entity, _wr)};

            w->_label.set_halign(Gtk::Align::START);
            w->_label.set_valign(Gtk::Align::CENTER);
            _page_metadata1->table().attach(w->_label, 0, row, 1, 1);

            w->_packable->set_hexpand();
            w->_packable->set_valign(Gtk::Align::CENTER);
            if (streq(entity->name, "description")) {
                // expand description edit box if there is space
                w->_packable->set_valign(Gtk::Align::FILL);
                w->_packable->set_vexpand();
            }
            _page_metadata1->table().attach(*w->_packable, 1, row, 1, 1);

            _rdflist.push_back(std::move(w));
        }
    }

    auto const button_save = Gtk::make_managed<Gtk::Button>(_("_Save as default"),true);
    button_save->set_tooltip_text(_("Save this metadata as the default metadata"));
    auto const button_load = Gtk::make_managed<Gtk::Button>(_("Use _default"),true);
    button_load->set_tooltip_text(_("Use the previously saved default metadata here"));

    auto const box_buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    UI::pack_end(*box_buttons, *button_save, true, true, 6);
    UI::pack_end(*box_buttons, *button_load, true, true, 6);
    _page_metadata1->table().attach(*box_buttons, 0, row++, 2);
    box_buttons->set_halign(Gtk::Align::END);
    box_buttons->set_homogeneous();

    button_save->signal_clicked().connect(sigc::mem_fun(*this, &DocumentProperties::save_default_metadata));
    button_load->signal_clicked().connect(sigc::mem_fun(*this, &DocumentProperties::load_default_metadata));

    row = 0;
    auto const llabel = Gtk::make_managed<Gtk::Label>();
    llabel->set_markup (_("<b>License</b>"));
    llabel->set_halign(Gtk::Align::START);
    llabel->set_valign(Gtk::Align::CENTER);
    _page_metadata2->table().attach(*llabel, 0, row, 2, 1);

    /* add license selector pull-down and URI */
    ++row;
    _licensor.init (_wr);

    _licensor.set_hexpand();
    _licensor.set_valign(Gtk::Align::CENTER);
    _page_metadata2->table().attach(_licensor, 0, row, 2, 1);
    _page_metadata2->table().set_valign(Gtk::Align::START);
}

void DocumentProperties::addExternalScript(){

    auto document = getDocument();
    if (!document)
        return;

    if (_script_entry.get_text().empty() ) {
        // Click Add button with no filename, show a Browse dialog
        browseExternalScript();
    }

    if (!_script_entry.get_text().empty()) {
        Inkscape::XML::Document *xml_doc = document->getReprDoc();
        Inkscape::XML::Node *scriptRepr = xml_doc->createElement("svg:script");
        scriptRepr->setAttributeOrRemoveIfEmpty("xlink:href", _script_entry.get_text());
        _script_entry.set_text("");

        xml_doc->root()->addChild(scriptRepr, nullptr);

        // inform the document, so we can undo
        DocumentUndo::done(document, RC_("Undo", "Add external script..."), "");

        populate_script_lists();
    }
}

void  DocumentProperties::browseExternalScript() {

    // Get the current directory for finding files.
    static std::string open_path;
    Inkscape::UI::Dialog::get_start_directory(open_path, _prefs_path);

    // Create a dialog.
    static std::vector<std::pair<Glib::ustring, Glib::ustring>> const filters {
        {_("JavaScript Files"), "*.js"}
    };

    auto window = getDesktop()->getInkscapeWindow();
    auto file = choose_file_open(_("Select a script to load"),
                                 window,
                                 filters,
                                 open_path);

    if (!file) {
        return; // Cancel
    }

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    prefs->setString(_prefs_path, open_path);

    _script_entry.set_text(file->get_parse_name());
}

void DocumentProperties::addEmbeddedScript(){
    if(auto document = getDocument()) {
        Inkscape::XML::Document *xml_doc = document->getReprDoc();
        Inkscape::XML::Node *scriptRepr = xml_doc->createElement("svg:script");

        xml_doc->root()->addChild(scriptRepr, nullptr);

        // inform the document, so we can undo
        DocumentUndo::done(document, RC_("Undo", "Add embedded script..."), "");
        populate_script_lists();
    }
}

void DocumentProperties::removeExternalScript(){
    Glib::ustring name;
    if(_ExternalScriptsList.get_selection()) {
        Gtk::TreeModel::iterator i = _ExternalScriptsList.get_selection()->get_selected();

        if(i){
            name = (*i)[_ExternalScriptsListColumns.filenameColumn];
        } else {
            return;
        }
    }

    auto document = getDocument();
    if (!document)
        return;
    std::vector<SPObject *> current = document->getResourceList( "script" );
    for (auto obj : current) {
        if (obj) {
            auto script = cast<SPScript>(obj);
            if (script && (name == script->xlinkhref)) {

                //XML Tree being used directly here while it shouldn't be.
                Inkscape::XML::Node *repr = obj->getRepr();
                if (repr){
                    sp_repr_unparent(repr);

                    // inform the document, so we can undo
                    DocumentUndo::done(document, RC_("Undo", "Remove external script"), "");
                }
            }
        }
    }

    populate_script_lists();
}

void DocumentProperties::removeEmbeddedScript(){
    Glib::ustring id;
    if(_EmbeddedScriptsList.get_selection()) {
        Gtk::TreeModel::iterator i = _EmbeddedScriptsList.get_selection()->get_selected();

        if(i){
            id = (*i)[_EmbeddedScriptsListColumns.idColumn];
        } else {
            return;
        }
    }

    if (auto document = getDocument()) {
        if (auto obj = document->getObjectById(id)) {
            //XML Tree being used directly here while it shouldn't be.
            if (auto repr = obj->getRepr()){
                sp_repr_unparent(repr);

                // inform the document, so we can undo
                DocumentUndo::done(document, RC_("Undo", "Remove embedded script"), "");
            }
        }
    }

    populate_script_lists();
}

void DocumentProperties::onExternalScriptSelectRow()
{
    Glib::RefPtr<Gtk::TreeSelection> sel = _ExternalScriptsList.get_selection();
    if (sel) {
        _external_remove_btn.set_sensitive(sel->count_selected_rows () > 0);
    }
}

void DocumentProperties::onEmbeddedScriptSelectRow()
{
    Glib::RefPtr<Gtk::TreeSelection> sel = _EmbeddedScriptsList.get_selection();
    if (sel) {
        _embed_remove_btn.set_sensitive(sel->count_selected_rows () > 0);
    }
}

void DocumentProperties::changeEmbeddedScript(){
    Glib::ustring id;
    if(_EmbeddedScriptsList.get_selection()) {
        Gtk::TreeModel::iterator i = _EmbeddedScriptsList.get_selection()->get_selected();

        if(i){
            id = (*i)[_EmbeddedScriptsListColumns.idColumn];
        } else {
            return;
        }
    }

    auto document = getDocument();
    if (!document)
        return;

    bool voidscript=true;
    std::vector<SPObject *> current = document->getResourceList( "script" );
    for (auto obj : current) {
        if (id == obj->getId()){
            int count = (int) obj->children.size();

            if (count>1)
                g_warning("TODO: Found a script element with multiple (%d) child nodes! We must implement support for that!", count);

            //XML Tree being used directly here while it shouldn't be.
            SPObject* child = obj->firstChild();
            //TODO: shouldn't we get all children instead of simply the first child?

            if (child && child->getRepr()){
                if (auto const content = child->getRepr()->content()) {
                    voidscript = false;
                    _EmbeddedContent.get_buffer()->set_text(content);
                }
            }
        }
    }

    if (voidscript)
        _EmbeddedContent.get_buffer()->set_text("");
}

void DocumentProperties::editEmbeddedScript(){
    Glib::ustring id;
    if(_EmbeddedScriptsList.get_selection()) {
        Gtk::TreeModel::iterator i = _EmbeddedScriptsList.get_selection()->get_selected();

        if(i){
            id = (*i)[_EmbeddedScriptsListColumns.idColumn];
        } else {
            return;
        }
    }

    auto document = getDocument();
    if (!document)
        return;

    for (auto obj : document->getResourceList("script")) {
        if (id == obj->getId()) {
            //XML Tree being used directly here while it shouldn't be.
            Inkscape::XML::Node *repr = obj->getRepr();
            if (repr) {
                auto tmp = obj->children | std::views::transform([] (SPObject &o) { return &o; });
                std::vector<SPObject*> vec(tmp.begin(), tmp.end());
                for (auto const child : vec) {
                    child->deleteObject();
                }
                obj->appendChildRepr(document->getReprDoc()->createTextNode(_EmbeddedContent.get_buffer()->get_text().c_str()));

                //TODO repr->set_content(_EmbeddedContent.get_buffer()->get_text());

                // inform the document, so we can undo
                DocumentUndo::done(document, RC_("Undo", "Edit embedded script"), "");
            }
        }
    }
}

void DocumentProperties::populate_script_lists(){
    _ExternalScriptsListStore->clear();
    _EmbeddedScriptsListStore->clear();
    _scripts_observer.set(nullptr);

    auto document = getDocument();
    if (!document)
        return;

    std::vector<SPObject *> current = getDocument()->getResourceList( "script" );
    if (!current.empty()) {
        SPObject *obj = *(current.begin());
        g_assert(obj != nullptr);
        _scripts_observer.set(obj->parent);
    }
    for (auto obj : current) {
        auto script = cast<SPScript>(obj);
        g_assert(script != nullptr);
        if (script->xlinkhref)
        {
            Gtk::TreeModel::Row row = *(_ExternalScriptsListStore->append());
            row[_ExternalScriptsListColumns.filenameColumn] = script->xlinkhref;
        }
        else // Embedded scripts
        {
            Gtk::TreeModel::Row row = *(_EmbeddedScriptsListStore->append());
            row[_EmbeddedScriptsListColumns.idColumn] = obj->getId();
        }
    }
}

/**
* Called for _updating_ the dialog. DO NOT call this a lot. It's expensive!
* Will need to probably create a GridManager with signals to each Grid attribute
*/
void DocumentProperties::rebuild_gridspage()
{
    _grids_list.remove_all();
    for (auto w : _grids_unified_size->get_widgets()) {
        _grids_unified_size->remove_widget(*w);
    }

    for (auto grid : getDesktop()->getNamedView()->grids) {
        add_grid_widget(grid);
    }

    update_grid_placeholder();
}

void DocumentProperties::update_grid_placeholder() {
    _no_grids.set_visible(_grids_list.get_first_child() == nullptr);
}

void DocumentProperties::add_grid_widget(SPGrid *grid)
{
    auto const widget = Gtk::make_managed<Inkscape::UI::Widget::GridWidget>(grid);
    _grids_list.append(*widget);
    _grids_unified_size->add_widget(*widget);
    // get rid of row highlight - they are not selectable (we just need to change the last one, but there's no API for that)
    int index = 0;
    for (auto row = _grids_list.get_row_at_index(index); row; row = _grids_list.get_row_at_index(++index)) {
        row->property_activatable() = false;
    }

    update_grid_placeholder();
}

void DocumentProperties::remove_grid_widget(XML::Node &node)
{
    // The SPObject is already gone, so we're working from the xml node directly.
    int index = 0;
    for (auto row = _grids_list.get_row_at_index(index); row; row = _grids_list.get_row_at_index(++index)) {
        if (auto widget = dynamic_cast<Inkscape::UI::Widget::GridWidget*>(row->get_child())) {
            if (&node == widget->getGridRepr()) {
                _grids_unified_size->remove_widget(*widget);
                _grids_list.remove(*row);
                break;
            }
        }
    }

    update_grid_placeholder();
}

/**
 * Build grid page of dialog.
 */
void DocumentProperties::build_gridspage()
{
    /// \todo FIXME: gray out snapping when grid is off.
    /// Dissenting view: you want snapping without grid.

    _grids_hbox_crea.set_spacing(5);
    _grids_hbox_crea.set_margin(8);
    _grids_hbox_crea.set_halign(Gtk::Align::CENTER);

    {
        auto btn = Gtk::make_managed<Gtk::Button>();
        btn->set_size_request(120); // make it easier to hit
        auto hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
        hbox->set_halign(Gtk::Align::CENTER);
        hbox->set_valign(Gtk::Align::CENTER);

        auto icon_image = Gtk::make_managed<Gtk::Image>();
        icon_image->set_from_icon_name("plus");
        icon_image->set_icon_size(Gtk::IconSize::NORMAL);
        hbox->append(*icon_image);

        auto btn_label = Gtk::make_managed<Gtk::Label>(_("New Grid"));
        btn_label->set_valign(Gtk::Align::CENTER);
        hbox->append(*btn_label);

        btn->set_child(*hbox);

        UI::pack_start(_grids_hbox_crea, *btn, false, true);
        btn->signal_clicked().connect([this]{ onNewGrid(GridType::RECTANGULAR); });
    }

    UI::pack_start(_grids_vbox, _grids_hbox_crea, false, false);
    _no_grids.set_text(_("There are no grids defined."));
    _no_grids.set_halign(Gtk::Align::CENTER);
    _no_grids.set_hexpand();
    _no_grids.set_margin_top(40);
    _no_grids.add_css_class("informational-text");
    UI::pack_start(_grids_vbox, _no_grids, false, false);
    UI::pack_start(_grids_vbox, _grids_wnd, true, true);
    _grids_wnd.set_child(_grids_list);
    _grids_list.set_show_separators();
    _grids_list.set_selection_mode(Gtk::SelectionMode::NONE);
    _grids_wnd.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _grids_wnd.set_has_frame(false);
}

void DocumentProperties::update_viewbox(SPDesktop* desktop) {
    if (!desktop) return;

    auto* document = desktop->getDocument();
    if (!document) return;

    using UI::Widget::PageProperties;
    SPRoot* root = document->getRoot();
    if (root->viewBox_set) {
        auto& vb = root->viewBox;
        _page->set_dimension(PageProperties::Dimension::ViewboxPosition, vb.min()[Geom::X], vb.min()[Geom::Y]);
        _page->set_dimension(PageProperties::Dimension::ViewboxSize, vb.width(), vb.height());
    }

    update_scale_ui(desktop);
}

/**
 * Update dialog widgets from desktop. Also call updateWidget routines of the grids.
 */
void DocumentProperties::update_widgets()
{
    auto desktop = getDesktop();
    auto document = getDocument();
    if (_document_colors) _document_colors->set_document(document);
    if (_wr.isUpdating() || !document) return;

    auto nv = desktop->getNamedView();
    auto &page_manager = document->getPageManager();

    _wr.setUpdating(true);

    SPRoot *root = document->getRoot();

    double doc_w = root->width.value;
    Glib::ustring doc_w_unit = Util::UnitTable::get().getUnit(root->width.unit)->abbr;
    bool percent = doc_w_unit == "%";
    if (doc_w_unit == "") {
        doc_w_unit = "px";
    } else if (doc_w_unit == "%" && root->viewBox_set) {
        doc_w_unit = "px";
        doc_w = root->viewBox.width();
    }
    double doc_h = root->height.value;
    Glib::ustring doc_h_unit = Util::UnitTable::get().getUnit(root->height.unit)->abbr;
    percent = percent || doc_h_unit == "%";
    if (doc_h_unit == "") {
        doc_h_unit = "px";
    } else if (doc_h_unit == "%" && root->viewBox_set) {
        doc_h_unit = "px";
        doc_h = root->viewBox.height();
    }
    using UI::Widget::PageProperties;
    // dialog's behavior is not entirely correct when document sizes are expressed in '%', so put up a disclaimer
    _page->set_check(PageProperties::Check::UnsupportedSize, percent);

    _page->set_dimension(PageProperties::Dimension::PageSize, doc_w, doc_h);
    _page->set_unit(PageProperties::Units::Document, doc_w_unit);

    update_viewbox_ui(desktop);
    update_scale_ui(desktop);

    if (nv->display_units) {
        _page->set_unit(PageProperties::Units::Display, nv->display_units->abbr);
    }
    _page->set_check(PageProperties::Check::Checkerboard, nv->desk_checkerboard);
    _page->set_color(PageProperties::Color::Desk, nv->getDeskColor());
    _page->set_color(PageProperties::Color::Background, page_manager.getBackgroundColor());
    _page->set_check(PageProperties::Check::Border, page_manager.border_show);
    _page->set_check(PageProperties::Check::BorderOnTop, page_manager.border_on_top);
    _page->set_color(PageProperties::Color::Border, page_manager.getBorderColor());
    _page->set_check(PageProperties::Check::Shadow, page_manager.shadow_show);
    _page->set_check(PageProperties::Check::PageLabelStyle, page_manager.label_style != "default");
    _page->set_check(PageProperties::Check::AntiAlias, nv->antialias_rendering);
    _page->set_check(PageProperties::Check::ClipToPage, nv->clip_to_page);
    _page->set_check(PageProperties::Check::YAxisPointsDown, nv->is_y_axis_down());
    _page->set_check(PageProperties::Check::OriginCurrentPage, nv->get_origin_follows_page());

    //-----------------------------------------------------------guide page

    _rcb_sgui.setActive (nv->getShowGuides());
    _rcb_lgui.setActive (nv->getLockGuides());
    _rcp_gui.setColor(nv->getGuideColor());
    _rcp_hgui.setColor(nv->getGuideHiColor());

    //-----------------------------------------------------------meta pages
    // update the RDF entities; note that this may modify document, maybe doc-undo should be called?
    if (auto document = getDocument()) {
        for (auto const &it : _rdflist) {
            bool read_only = false;
            it->update(document, read_only);
        }
        _licensor.update(document);
    }
    _wr.setUpdating (false);
}

//--------------------------------------------------------------------

void DocumentProperties::on_response (int id)
{
    if (id == Gtk::ResponseType::DELETE_EVENT || id == Gtk::ResponseType::CLOSE)
    {
        _rcp_gui.closeWindow();
        _rcp_hgui.closeWindow();
    }

    if (id == Gtk::ResponseType::CLOSE)
        set_visible(false);
}

void DocumentProperties::load_default_metadata()
{
    /* Get the data RDF entities data from preferences*/
    for (auto const &it : _rdflist) {
        it->load_from_preferences ();
    }
}

void DocumentProperties::save_default_metadata()
{
    /* Save these RDF entities to preferences*/
    if (auto document = getDocument()) {
        for (auto const &it : _rdflist) {
            it->save_to_preferences(document);
        }
    }
}

void DocumentProperties::WatchConnection::connect(Inkscape::XML::Node *node)
{
    disconnect();
    if (!node) return;

    _node = node;
    _node->addObserver(*this);
}

void DocumentProperties::WatchConnection::disconnect() {
    if (_node) {
        _node->removeObserver(*this);
        _node = nullptr;
    }
}

void DocumentProperties::WatchConnection::notifyChildAdded(XML::Node&, XML::Node &child, XML::Node*)
{
    if (auto grid = cast<SPGrid>(_dialog->getDocument()->getObjectByRepr(&child))) {
        _dialog->add_grid_widget(grid);
    }
}

void DocumentProperties::WatchConnection::notifyChildRemoved(XML::Node&, XML::Node &child, XML::Node*)
{
    _dialog->remove_grid_widget(child);
}

void DocumentProperties::WatchConnection::notifyAttributeChanged(XML::Node&, GQuark, Util::ptr_shared, Util::ptr_shared)
{
    _dialog->update_widgets();
}

void DocumentProperties::documentReplaced()
{
    if (_document_colors) _document_colors->set_document(getDocument());
    _root_connection.disconnect();
    _namedview_connection.disconnect();
    _cms_connection.disconnect();

    if (auto desktop = getDesktop()) {
        _wr.setDesktop(desktop);
        _namedview_connection.connect(desktop->getNamedView()->getRepr());
        if (auto document = desktop->getDocument()) {
            _root_connection.connect(document->getRoot()->getRepr());
            _cms_connection = document->getDocumentCMS().connectChanged(sigc::mem_fun(*this, &DocumentProperties::populate_linked_profiles_box));
        }
        populate_linked_profiles_box();
        populate_script_lists();
        update_widgets();
        rebuild_gridspage();
    }
}

void DocumentProperties::update()
{
    update_widgets();
}

/*########################################################################
# BUTTON CLICK HANDLERS    (callbacks)
########################################################################*/

void DocumentProperties::onNewGrid(GridType grid_type)
{
    auto desktop = getDesktop();
    auto document = getDocument();
    if (!desktop || !document) return;

    auto repr = desktop->getNamedView()->getRepr();
    SPGrid::create_new(document, repr, grid_type);

    DocumentUndo::done(document, RC_("Undo", "Create new grid"), INKSCAPE_ICON("document-properties"));

    // scroll to the last (newly added) grid, so we can see it; postponed till idle time, since scrolling
    // range is not yet updated, despite new grid UI being in place already
    _on_idle_scroll = Glib::signal_idle().connect([this](){
        if (auto adj = _grids_wnd.get_vadjustment()) {
            adj->set_value(adj->get_upper());
        }
        return false;
    });
}

/* This should not effect anything in the SVG tree (other than "inkscape:document-units").
   This should only effect values displayed in the GUI. */
void DocumentProperties::display_unit_change(const Inkscape::Util::Unit* doc_unit)
{
    SPDocument *document = getDocument();
    // Don't execute when change is being undone
    if (!document || !DocumentUndo::getUndoSensitive(document)) {
        return;
    }
    // Don't execute when initializing widgets
    if (_wr.isUpdating()) {
        return;
    }

    auto action = document->getActionGroup()->lookup_action("set-display-unit");
    action->activate(doc_unit->abbr);
}

} // namespace Dialog

namespace Widget {

static const auto grid_types = std::to_array({std::tuple
    {C_("Grid", "Rectangular"), GridType::RECTANGULAR, "grid-rectangular"},
    {C_("Grid", "Axonometric"), GridType::AXONOMETRIC, "grid-axonometric"},
    {C_("Grid", "Modular"), GridType::MODULAR, "grid-modular"}
});

GridWidget::GridWidget(SPGrid *grid)
    : Gtk::Box(Gtk::Orientation::VERTICAL)
    , _grid(grid)
    , _repr(grid->getRepr())
{
    set_halign(Gtk::Align::CENTER);
    add_css_class("grid-row-definition");

    Inkscape::XML::Node *repr = grid->getRepr();
    auto doc = grid->document;

    constexpr int SPACE = 4;
    constexpr int POPUP_MARGIN = 8;

    _wr.setUpdating(true);

    for (auto const &[label, type, icon]: grid_types) {
        _grid_type.add_row(icon, label, static_cast<int>(type));
    }
    _grid_type.refilter();

    _enabled = Gtk::make_managed<Inkscape::UI::Widget::RegisteredSwitchButton>("",
            _("Makes the grid available for working with on the canvas."),
            "enabled", _wr, false, repr, doc);

    _snap_visible_only = Gtk::make_managed<Inkscape::UI::Widget::RegisteredCheckButton>(
            _("Snap to visible _grid lines only"),
            _("When zoomed out, not all grid lines will be displayed. Only the visible ones will be snapped to"),
            "snapvisiblegridlinesonly", _wr, false, repr, doc);

    _visible = Gtk::make_managed<Inkscape::UI::Widget::RegisteredToggleButton>("",
            _("Determines whether the grid is displayed or not."),
            "visible", _wr, false, repr, doc,
            "object-visible", "object-hidden");
    _visible->set_child(*Gtk::make_managed<Gtk::Image>(Gio::ThemedIcon::create("object-visible")));

    _alignment = Gtk::make_managed<Inkscape::UI::Widget::AlignmentSelector>();
    _alignment->connectAlignmentClicked([this, grid](int const align) {
        auto dimensions = grid->document->getDimensions();
        dimensions[Geom::X] *= align % 3 * 0.5;
        dimensions[Geom::Y] *= align / 3 * 0.5;
        dimensions *= grid->document->doc2dt();
        dimensions *= grid->document->getDocumentScale().inverse();
        grid->setOrigin(dimensions);
    });

    _dotted = Gtk::make_managed<Inkscape::UI::Widget::RegisteredCheckButton>(
            _("_Show dots instead of lines"), _("If set, displays dots at gridpoints instead of gridlines"),
            "dotted", _wr, false, repr, doc );

    auto vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, SPACE);
    auto align = Gtk::make_managed<Gtk::Label>(_("Align to page:"));
    align->set_margin_top(8);
    vbox->append(*align);
    vbox->append(*_alignment);
    _align_popup->set_child(*vbox);

    auto angle_popover = Gtk::make_managed<Gtk::Popover>();
    angle_popover->set_has_arrow(false);
    _angle_popup->set_popover(*angle_popover);
    _angle_popup->set_valign(Gtk::Align::FILL);
    // set grid angles from given width to height ratio
    auto angle = Gtk::make_managed<Gtk::Label>(_("Set angle from aspect ratio:"));
    angle->set_xalign(0);
    auto subgrid = Gtk::make_managed<Gtk::Grid>();
    subgrid->set_margin(POPUP_MARGIN);
    subgrid->set_row_spacing(SPACE);
    subgrid->set_column_spacing(SPACE);
    _aspect_ratio = Gtk::make_managed<Gtk::Entry>();
    _aspect_ratio->set_max_width_chars(9);
    subgrid->attach(*angle, 0, 0);
    subgrid->attach(*_aspect_ratio, 0, 1);
    auto apply = Gtk::make_managed<Gtk::Button>(_("Set"));
    apply->set_halign(Gtk::Align::CENTER);
    apply->set_size_request(100);
    subgrid->attach(*apply, 0, 2);
    // TRANSLATORS: Axonometric grid looks like a pattern of parallelograms. Their width to height proportions
    // can be manipulated by changing angles in the axonometric grid. This DX/DY ratio does just that.
    // Pressing "Set" button will calculate grid angles to produce parallelograms with requested widh to height ratio.
    apply->set_tooltip_text(_("Automatically calculate angles from width to height ratio\nof a single grid parallelogram"));
    apply->signal_clicked().connect([=, this](){
        try {
            auto const result = ExpressionEvaluator{get_text(*_aspect_ratio)}.evaluate().value;
            if (!std::isfinite(result) || result <= 0) return;

            auto angle = Geom::deg_from_rad(std::atan(1.0 / result));
            if (angle > 0.0 && angle < 90.0) {
                _angle_x->setValue(angle, false);
                _angle_z->setValue(angle, false);
            }
        }
        catch (EvaluatorException& e) {
            // ignoring user input error for now
        }
    });
    angle_popover->set_child(*subgrid);
    angle_popover->signal_show().connect([this](){
        if (!_grid) return;

        auto ax = _grid->getAngleX();
        auto az = _grid->getAngleZ();
        // try to guess ratio if angles are the same, otherwise leave ratio boxes intact
        if (az == ax) {
            auto ratio = std::tan(Geom::rad_from_deg(ax));
            if (ratio > 0) {
                _aspect_ratio->set_text(ratio > 1.0 ?
                    Glib::ustring::format("1 : ", ratio) :
                    Glib::ustring::format(1.0 / ratio, " : 1")
                );
            }
        }
    });

    _units = Gtk::make_managed<RegisteredUnitMenu>(
                _("Grid _units:"), "units", _wr, repr, doc);
    _origin_x = Gtk::make_managed<RegisteredScalarUnit>(
                _("_Origin X:"), _("X coordinate of grid origin"), "originx",
                *_units, _wr, repr, doc, RSU_x, true);
    _origin_y = Gtk::make_managed<RegisteredScalarUnit>(
                _("O_rigin Y:"), _("Y coordinate of grid origin"), "originy",
                *_units, _wr, repr, doc, RSU_y, true);
    _spacing_x = Gtk::make_managed<RegisteredScalarUnit>(
                "-", _("Distance between vertical grid lines"), "spacingx",
                *_units, _wr, repr, doc, RSU_x, true);
    _spacing_y = Gtk::make_managed<RegisteredScalarUnit>(
                "-", _("Distance between horizontal grid lines"), "spacingy",
                *_units, _wr, repr, doc, RSU_y, true);

    _gap_x = Gtk::make_managed<RegisteredScalarUnit>(
                _("Gap _X:"), _("Horizontal distance between blocks"), "gapx",
                *_units, _wr, repr, doc, RSU_x, true);
    _gap_y = Gtk::make_managed<RegisteredScalarUnit>(
                _("Gap _Y:"), _("Vertical distance between blocks"), "gapy",
                *_units, _wr, repr, doc, RSU_y, true);
    _margin_x = Gtk::make_managed<RegisteredScalarUnit>(
                _("_Margin X:"), _("Right and left margins"), "marginx",
                *_units, _wr, repr, doc, RSU_x, true);
    _margin_y = Gtk::make_managed<RegisteredScalarUnit>(
                _("M_argin Y:"), _("Top and bottom margins"), "marginy",
                *_units, _wr, repr, doc, RSU_y, true);

    _angle_x = Gtk::make_managed<RegisteredScalar>(
        _("An_gle of X:"), _("Angle of x-axis relative to horizontal direction"), "gridanglex", _wr, repr, doc);
    _angle_z = Gtk::make_managed<RegisteredScalar>(
        _("Ang_le of Z:"), _("Angle of z-axis relative to horizontal direction"), "gridanglez", _wr, repr, doc);
    _angle_y_vertical = Gtk::make_managed<RegisteredCheckButton>("Angle Y vertical",
            _("If set, y-axis will be vertical. Otherwise, it will be calculated from x and z angles."),
            "angleyvertical", _wr, false, repr, doc);
    _grid_color = Gtk::make_managed<RegisteredColorPicker>(
                "", _("Grid color"),
                _("Color of the grid lines"),
                "empcolor", "empopacity", _wr, repr, doc);
    _grid_color->setCustomSetter([](Inkscape::XML::Node* node, Colors::Color color) {
        // major color
        node->setAttribute("empcolor", color.toString(false));
        node->setAttributeCssDouble("empopacity", color.getOpacity());

        // minor color at half opacity
        color.addOpacity(0.5);
        node->setAttribute("color", color.toString(false));
        node->setAttributeCssDouble("opacity", color.getOpacity());
    });
    _grid_color->set_spacing(0);
    _no_of_lines = Gtk::make_managed<RegisteredInteger>(
                _("Major grid line e_very:"), _("Number of lines"),
                "empspacing", _wr, repr, doc);

    // All of these undo settings are the same, refactor this later if possible.
    _units->set_undo_parameters(RC_("Undo", "Change grid units"), "show-grid", "grid-settings");
    _angle_x->set_undo_parameters(RC_("Undo", "Change grid dimensions"), "show-grid", "grid-settings");
    _angle_z->set_undo_parameters(RC_("Undo", "Change grid dimensions"), "show-grid", "grid-settings");
    _angle_y_vertical->set_undo_parameters(RC_("Undo", "Change grid dimensions"), "show-grid", "grid-settings");
    _grid_color->set_undo_parameters(RC_("Undo", "Change grid color"), "show-grid", "grid-settings");
    _no_of_lines->set_undo_parameters(RC_("Undo", "Change grid number of lines"), "show-grid", "grid-settings");
    for (auto widget : {_origin_x, _origin_y, _spacing_x, _spacing_y, _gap_x, _gap_y, _margin_x, _margin_y}) {
        widget->set_undo_parameters(RC_("Undo", "Change grid dimensions"), "show-grid", "grid-settings");
    }

    for (auto labelled : std::to_array<Labelled*>(
        {_units, _origin_x, _origin_y, _spacing_x, _spacing_y, _gap_x, _gap_y, _margin_x, _margin_y,
            _angle_x, _angle_z, _no_of_lines})) {
        labelled->getLabel()->set_hexpand();
    }

    _units->set_hexpand();
    _angle_x->set_hexpand();
    _angle_z->set_hexpand();
    _angle_y_vertical->set_hexpand();
    _no_of_lines->set_hexpand();
    _no_of_lines->setWidthChars(5);

    _swap_axes->set_label(_("Swap axes"));
    _swap_axes->set_tooltip_text(_("Swap axonometric grid axes"));
    _swap_axes->signal_clicked().connect([this](){
        auto cur_angle_x = _grid->getAngleX();
        auto cur_angle_z = _grid->getAngleZ();
        auto new_angle_x = 90 - cur_angle_x;
        auto new_angle_z = 90 - cur_angle_z;
        auto y_vertical = _grid->isAngleYVertical();
        auto y_spacing = _grid->getSpacing()[Geom::Y];

        auto rad_x = Geom::rad_from_deg(new_angle_x);
        auto rad_z = Geom::rad_from_deg(new_angle_z);

        auto b = y_spacing * sin(rad_x) / sin(rad_x + rad_z);
        auto diag_size = 2.0 * sqrt(y_spacing * y_spacing / 4.0 + b * b - y_spacing * b * cos(rad_z));

        _grid->setAngleYVertical(!y_vertical);
        _grid->setAngleX(new_angle_x);
        _grid->setAngleZ(new_angle_z);
        _grid->setSpacing(Geom::Point(diag_size, diag_size));
    });

    _origin_x->setProgrammatically = false;
    _origin_y->setProgrammatically = false;

    auto main_grid = Gtk::make_managed<Gtk::Grid>();
    main_grid->set_column_homogeneous();
    main_grid->set_column_spacing(4 * SPACE);

    auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    buttons->set_spacing(SPACE);
    buttons->append(*_visible);
    buttons->append(*_grid_color);
    _delete->set_child(*Gtk::make_managed<Gtk::Image>(Gio::ThemedIcon::create("edit-delete")));
    _delete->set_tooltip_text(_("Delete this grid"));
    _delete->signal_clicked().connect([this](){
        auto doc = getGrid()->document;
        getGrid()->deleteObject();
        DocumentUndo::done(doc, RC_("Undo", "Remove grid"), INKSCAPE_ICON("document-properties"));
    });
    _delete->set_hexpand();
    _delete->set_halign(Gtk::Align::END);
    buttons->append(*_delete);
    buttons->append(*_options);
    _options->set_popover(*_opt_items);
    _options->set_icon_name("gear");
    auto items = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    items->set_spacing(SPACE);
    items->set_margin(POPUP_MARGIN);
    items->append(*_snap_visible_only);
    items->append(*_dotted);
    _opt_items->set_child(*items);
    _opt_items->set_has_arrow(false);

    _align->set_label(C_("popup-align-grid-origin", "Align"));
    _align->set_tooltip_text(_("Align grid origin relative to active page."));
    _align_popup->set_has_arrow(false);
    _align->set_popover(*_align_popup);

    auto left_col = Gtk::make_managed<Gtk::Grid>();
    main_grid->attach(*left_col, 0, 1);

    auto right_col = Gtk::make_managed<Gtk::Grid>();
    main_grid->attach(*right_col, 1, 1);

    for (auto grid : {left_col, right_col}) {
        grid->set_column_spacing(SPACE);
        grid->set_row_spacing(SPACE);
    }

    auto first_row_height = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::VERTICAL);
    int row = 0;
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    box->set_spacing(SPACE);
    box->append(*_enabled);
    _id->set_ellipsize(Pango::EllipsizeMode::END);
    box->append(*_id);
    _grid_type.set_hexpand();
    _grid_type.set_halign(Gtk::Align::END);
    _grid_type.set_active_by_id(static_cast<int>(_grid->getType()));
    _grid_type.signal_changed().connect([&,this](int index){
        // change grid type
        if (index < 0) return;

        // create grid of required type
        _grid->setType(std::get<1>(grid_types[index]));
    });
    _grid_type.set_tooltip_text(_("Change to a different grid type."));
    box->append(_grid_type);
    left_col->attach(*box, 0, row, 2);
    right_col->attach(*buttons, 0, row, 2);
    ++row;
    first_row_height->add_widget(*box);
    first_row_height->add_widget(*buttons);
    // add "separators"
    {
        auto lbox = Gtk::make_managed<Gtk::Box>();
        lbox->set_size_request(0, SPACE);
        left_col->attach(*lbox, 0, row);
        auto rbox = Gtk::make_managed<Gtk::Box>();
        rbox->set_size_request(0, SPACE);
        right_col->attach(*rbox, 0, row++);
    }

    int first_row = row;
    left_col->attach(*_units, 0, row++, 2);

    auto cur_grid = left_col;
    for (auto rs : std::to_array<Scalar*>({
            // left
            _spacing_x, _spacing_y, _angle_x, _angle_z, _gap_x, _gap_y,
            // right
            _origin_x, _origin_y, _margin_x, _margin_y})) {
        rs->setDigits(6);
        rs->set_hexpand();
        rs->setWidthChars(12);
        int width = 2;
        if (rs == _origin_x) {
            cur_grid = right_col;
            row = first_row;
            // attach "align" popup
            cur_grid->attach(*_align, 0, row++, width);
            _align->set_halign(Gtk::Align::END);
        }
        if (rs == _angle_x) {
            cur_grid->attach(*_angle_popup, 1, row, 1, 3);
        }
        if (rs == _angle_x || rs == _angle_z) {
            rs->setWidthChars(8);
            width = 1;
        }
        cur_grid->attach(*rs, 0, row++, width);
    }

    // Don't allow negative values for spacing or block width/height.
    for (auto rs : std::to_array<Scalar *>({_spacing_x, _spacing_y})) {
        rs->setRange(0, Scalar::COMMON_MAX);
    }

    left_col->attach(*_angle_y_vertical, 0, row++, 2);
    left_col->attach(*_swap_axes, 0, row++, 2);
    right_col->attach(*_no_of_lines, 0, row++, 2);

    _modified_signal = grid->connectModified([this, grid](SPObject const * /*obj*/, unsigned /*flags*/) {
        if (!_wr.isUpdating()) {
            _modified_signal.block();
            update();
            _modified_signal.unblock();
        } else if (_grid->getType() == GridType::MODULAR) {
            // Even if we're updating already from a separate "changed" signal, we want to update
            // grid limits. Each spinbutton will have its own handler that marks the widget
            // registry as "updating" while it changes the grid. But here, we want to update the
            // limits on the OTHER spinbuttons in the dialog even if that's happening.
            updateModularGridLimits();
        }
    });
    update();

    UI::pack_start(*this, *main_grid, false, false);

    std::vector<Gtk::Widget*> widgets;
    for_each_descendant(*main_grid, [&](Gtk::Widget& w){
        if (dynamic_cast<InkSpinButton*>(&w) ||
            dynamic_cast<Gtk::ToggleButton*>(&w) ||
            dynamic_cast<Gtk::MenuButton*>(&w) ||
            dynamic_cast<Gtk::Label*>(&w) ||
            dynamic_cast<LabelledColorPicker*>(&w)
            ) {
            widgets.push_back(&w);
            return ForEachResult::_skip;
        }

        return ForEachResult::_continue;
    });
    _enabled->setSubordinateWidgets(std::move(widgets));

    _wr.setUpdating(false);
}

/**
 * Keep the grid up to date with it's values.
 */
void GridWidget::update()
{
    _wr.setUpdating (true);
    auto scale = _grid->document->getDocumentScale();

    const auto modular = _grid->getType() == GridType::MODULAR;
    const auto axonometric = _grid->getType() == GridType::AXONOMETRIC;
    const auto rectangular = _grid->getType() == GridType::RECTANGULAR;

    _units->setUnit(_grid->getUnit()->abbr);

    // Doc to px so unit is conserved in RegisteredScalerUnit
    auto origin = _grid->getOrigin() * scale;
    _origin_x->setValueKeepUnit(origin[Geom::X], "px");
    _origin_y->setValueKeepUnit(origin[Geom::Y], "px");

    auto spacing = _grid->getSpacing() * scale;
    _spacing_x->setValueKeepUnit(spacing[Geom::X], "px");
    _spacing_y->setValueKeepUnit(spacing[Geom::Y], "px");
    _spacing_x->getLabel()->set_markup_with_mnemonic(modular ? _("Block _width:") : _("Spacing _X:"));
    _spacing_x->set_tooltip_text(modular ? _("Width of grid modules") : _("Distance between vertical grid lines"));
    char const *spacing_y_label = _("Spacing _Y:");
    char const *spacing_y_tooltip = _("Distance between horizontal grid lines");
    switch (_grid->getType()) {
        case GridType::AXONOMETRIC:
            spacing_y_label = _("_Height:");
            spacing_y_tooltip = _("Height of grid cell, vertical distance between grid intersections");
            break;
        case GridType::MODULAR:
            spacing_y_label = _("Block _height:");
            spacing_y_tooltip = _("Height of grid modules");
            break;
        case GridType::RECTANGULAR:
        default:
            break;
    }
    _spacing_y->getLabel()->set_markup_with_mnemonic(spacing_y_label);
    _spacing_y->set_tooltip_text(spacing_y_tooltip);

    auto show = [](Gtk::Widget* w, bool do_show){
        w->set_visible(do_show);
    };

    show(_angle_x, axonometric);
    show(_angle_z, axonometric);
    show(_angle_y_vertical, axonometric);
    show(_angle_popup, axonometric);
    show(_swap_axes, axonometric);
    if (axonometric) {
        _angle_x->setValue(_grid->getAngleX());
        _angle_z->setValue(_grid->getAngleZ());
        _angle_y_vertical->set_active(_grid->isAngleYVertical());
    }

    show(_gap_x, modular);
    show(_gap_y, modular);
    show(_margin_x, modular);
    show(_margin_y, modular);
    if (modular) {
        auto gap = _grid->get_gap() * scale;
        auto margin = _grid->get_margin() * scale;
        _gap_x->setValueKeepUnit(gap.x(), "px");
        _gap_y->setValueKeepUnit(gap.y(), "px");
        _margin_x->setValueKeepUnit(margin.x(), "px");
        _margin_y->setValueKeepUnit(margin.y(), "px");

        updateModularGridLimits();
    }

    _grid_color->setColor(_grid->getMajorColor());

    show(_no_of_lines, !modular);
    _no_of_lines->setValue(_grid->getMajorLineInterval());

    _enabled->set_active(_grid->isEnabled());
    _visible->setActive(_grid->isVisible());

    if (_dotted)
        _dotted->setActive(_grid->isDotted());

    _snap_visible_only->setActive(_grid->getSnapToVisibleOnly());
    // which condition to use to call setActive?
    // _grid_rcb_snap_visible_only->setActive(grid->snapper()->getSnapVisibleOnly());
    _enabled->set_active(_grid->snapper()->getEnabled());

    show(_dotted, rectangular);
    show(_spacing_x, !axonometric);

    _icon->set_from_icon_name(_grid->typeName());
    auto id = _grid->getId() ? _grid->getId() : "-";
    _id->set_label(id);
    _id->set_tooltip_text(id);

    _wr.setUpdating(false);
}

void GridWidget::updateModularGridLimits()
{
    auto unit = _units->getUnit()->abbr;
    _gap_x->setRange(-_spacing_x->getValue(unit) / 2.0, Scalar::COMMON_MAX);
    _gap_y->setRange(-_spacing_y->getValue(unit) / 2.0, Scalar::COMMON_MAX);
    _margin_x->setRange(-_spacing_x->getValue(unit) / 2.0, _gap_x->getValue(unit) / 2.0);
    _margin_y->setRange(-_spacing_y->getValue(unit) / 2.0, _gap_y->getValue(unit) / 2.0);
}

} // namespace Widget

} // namespace Inkscape::UI

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
