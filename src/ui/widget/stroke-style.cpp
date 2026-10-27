// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Bryce Harrington <brycehar@bryceharrington.org>
 *   bulia byak <buliabyak@users.sf.net>
 *   Maximilian Albert <maximilian.albert@gmail.com>
 *   Josh Andler <scislac@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2001-2005 authors
 * Copyright (C) 2001 Ximian, Inc.
 * Copyright (C) 2004 John Cliff
 * Copyright (C) 2008 Maximilian Albert (gtkmm-ification)
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "stroke-style.h"

#include <glibmm/i18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>

#include "actions/actions-tools.h"
#include "dash-selector.h"
#include "desktop.h"
#include "desktop-style.h"
#include "document-undo.h"
#include "document.h"
#include "fill-or-stroke.h"
#include "inkscape.h"
#include "object/sp-namedview.h"
#include "object/sp-text.h"
#include "selection.h"
#include "style.h"
#include "svg/css-ostringstream.h"
#include "ui/dialog-events.h"
#include "ui/dialog/dialog-base.h"
#include "ui/icon-loader.h"
#include "ui/icon-names.h"
#include "ui/pack.h"
#include "ui/tools/marker-tool.h"
#include "ui/util.h"
#include "ui/widget/style/marker-combo-box.h"
#include "ui/widget/unit-menu.h"
#include "widgets/style-utils.h"

using Inkscape::DocumentUndo;

/**
 * Extract the actual name of the link
 * e.g. get mTriangle from url(#mTriangle).
 * \return Buffer containing the actual name, allocated from GLib;
 * the caller should free the buffer when they no longer need it.
 */
SPObject* getMarkerObj(gchar const *n, SPDocument *doc)
{
    gchar const *p = n;
    while (*p != '\0' && *p != '#') {
        p++;
    }

    if (*p == '\0' || p[1] == '\0') {
        return nullptr;
    }

    p++;
    int c = 0;
    while (p[c] != '\0' && p[c] != ')') {
        c++;
    }

    if (p[c] == '\0') {
        return nullptr;
    }

    gchar* b = g_strdup(p);
    b[c] = '\0';

    // FIXME: get the document from the object and let the caller pass it in
    SPObject *marker = doc->getObjectById(b);

    g_free(b);
    return marker;
}

/**
 * Get a dash array and offset from the style.
 *
 * Both values are de-scaled by the style's width if needed.
 */
std::pair<std::vector<double>, double> getDashFromStyle(SPStyle *style) {
    auto prefs = Inkscape::Preferences::get();

    std::vector<double> ret;
    size_t len = style->stroke_dasharray.values.size();

    double scaledash = 1.0;
    if (prefs->getBool("/options/dash/scale", true) && style->stroke_width.computed) {
        scaledash = style->stroke_width.computed;
    }

    double offset = style->stroke_dashoffset.value / scaledash;
    for (unsigned i = 0; i < len; i++) {
        ret.push_back(style->stroke_dasharray.values[i].value / scaledash);
    }

    return {std::move(ret), offset};
}

namespace Inkscape::UI::Widget {

/**
 * Creates a label widget with the given text, at the given col, row
 * position in the table.
 */
static Gtk::Label *spw_label(Gtk::Grid *table, const gchar *label_text, int col, int row,
                             Gtk::Widget* target)
{
  auto const label_widget = Gtk::make_managed<Gtk::Label>();
  g_assert(label_widget != nullptr);
  if (target != nullptr) {
    label_widget->set_text_with_mnemonic(label_text);
    label_widget->set_mnemonic_widget(*target);
  } else {
    label_widget->set_text(label_text);
  }
  label_widget->set_visible(true);

  label_widget->set_halign(Gtk::Align::START);
  label_widget->set_valign(Gtk::Align::CENTER);
  label_widget->set_margin_start(4);
  label_widget->set_margin_end(4);

  table->attach(*label_widget, col, row, 1, 1);

  return label_widget;
}

/**
 * Creates a horizontal layout manager with 4-pixel spacing between children
 * and space for 'width' columns.
 */
static Gtk::Box *spw_hbox(Gtk::Grid *table, int width, int col, int row)
{
    auto const hb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    g_assert(hb != nullptr);
    hb->set_visible(true);
    hb->set_hexpand();
    hb->set_halign(Gtk::Align::FILL);
    hb->set_valign(Gtk::Align::CENTER);
    hb->add_css_class("linked");
    table->attach(*hb, col, row, width, 1);
    return hb;
}

/**
 * Construct a stroke-style radio button with a given icon
 *
 * \param[in] grp          The Gtk::CheckButton with which to group
 * \param[in] icon         The icon to use for the button
 * \param[in] button_type  The type of stroke-style radio button (join/cap)
 * \param[in] stroke_style The style attribute to associate with the button
 */
StrokeStyle::StrokeStyleButton::StrokeStyleButton(Gtk::ToggleButton    *&grp,
                                                  char const            *icon,
                                                  StrokeStyleButtonType  button_type,
                                                  gchar const           *stroke_style)
    : 
        button_type(button_type),
        stroke_style(stroke_style)
{
    if (!grp) {
        grp = this;
    } else {
        set_group(*grp);
    }
    set_visible(true);

    auto px = Gtk::manage(sp_get_icon_image(icon, Gtk::IconSize::NORMAL));
    g_assert(px != nullptr);
    px->set_visible(true);
    set_child(*px);
}

std::vector<double> parse_dash_pattern(const Glib::ustring& input) {
    std::vector<double> output;
    if (input.empty()) return output;

    std::istringstream stream(input.c_str());
    while (stream) {
        double val;
        stream >> val;
        if (stream) {
            output.push_back(val);
        }
    }

    return output;
}

StrokeStyle::StrokeStyle() :
    Gtk::Box(),
    miterLimitSpin(),
    widthSpin(),
    unitSelector(),
    joinMiter(),
    joinRound(),
    joinBevel(),
    capButt(),
    capRound(),
    capSquare(),
    dashSelector(),
    update(false),
    desktop(nullptr),
    startMarkerConn(),
    midMarkerConn(),
    endMarkerConn(),
    _old_unit(nullptr)
{
    set_name("StrokeSelector");

    table = Gtk::make_managed<Gtk::Grid>();
    table->set_margin(4);
    table->set_row_spacing(4);
    table->set_hexpand(true);
    table->set_halign(Gtk::Align::FILL);
    table->set_visible(true);
    append(*table);

    Gtk::Box *hb;
    gint i = 0;

    //spw_label(t, C_("Stroke width", "_Width:"), 0, i);

    hb = spw_hbox(table, 3, 1, i);

// TODO: when this is gtkmmified, use a ScalarUnit instead of the separate
// spinbutton and unit selector for stroke width. In sp_stroke_style_line_update, use
// setHundredPercent to remember the averaged width corresponding to 100%. Then the
// stroke_width_set_unit will be removed (because ScalarUnit takes care of conversions itself)
    widthAdj = Gtk::Adjustment::create(1.0, 0.0, 1000.0, 0.1, 10.0, 0.0);
    widthSpin = Gtk::make_managed<SpinButton>(widthAdj, 0.1, 3);
    widthSpin->set_tooltip_text(_("Stroke width"));
    widthSpin->set_visible(true);
    spw_label(table, C_("Stroke width", "_Width"), 0, i, widthSpin);

    sp_dialog_defocus_on_enter(*widthSpin);

    UI::pack_start(*hb, *widthSpin, true, true);
    unitSelector = Gtk::make_managed<UnitMenu>();
    unitSelector->setUnitType(Inkscape::Util::UNIT_TYPE_LINEAR);
    SPDesktop *desktop = SP_ACTIVE_DESKTOP;

    unitSelector->addUnit(*Util::UnitTable::get().getUnit("%"));
    _hairline_item = unitSelector->append(_("Hairline"));
    _old_unit = unitSelector->getUnit();
    if (desktop) {
        unitSelector->setUnit(desktop->getNamedView()->display_units->abbr);
        _old_unit = desktop->getNamedView()->display_units;
    }
    widthSpin->setUnitMenu(unitSelector);
    unitSelector->signal_changed().connect(sigc::mem_fun(*this, &StrokeStyle::unitChangedCB));
    unitSelector->set_visible(true);

    UI::pack_start(*hb, *unitSelector, FALSE, FALSE);
    widthAdj->signal_value_changed().connect(sigc::mem_fun(*this, &StrokeStyle::setStrokeWidth));

    i++;

    /* Dash */
    spw_label(table, _("Dashes"), 0, i, nullptr); //no mnemonic for now
                                            //decide what to do:
                                            //   implement a set_mnemonic_source function in the
                                            //   DashSelector class, so that we do not have to
                                            //   expose any of the underlying widgets?
    dashSelector = Gtk::make_managed<DashSelector>();
    dashSelector->changed_signal.connect([this](auto){
        if (update || _editing_dash_pattern) {
            return;
        }
        _editing_dash_pattern = true;
        auto& dash_pattern = dashSelector->get_dash_pattern();
        update_dash_entry(dash_pattern);
        setStrokeDash();
        _editing_dash_pattern = false;
    });
    table->attach(*dashSelector, 1, i, 3, 1);

    i++;

    _pattern_entry = Gtk::make_managed<Gtk::Entry>();
    _pattern_entry->signal_changed().connect([this](){
        if (update || _editing_dash_pattern) {
            return;
        }
        _editing_dash_pattern = true;
        update = true;
        auto pattern = parse_dash_pattern(_pattern_entry->get_text());
        dashSelector->set_dash_pattern(pattern, dashSelector->get_offset());
        update = false;
        setStrokeDash();
        _editing_dash_pattern = false;
    });
    table->attach(*_pattern_entry, 1, i, 4, 1);

    _pattern_label = spw_label(table, _("_Pattern"), 0, i, _pattern_entry);
    _pattern_label->set_tooltip_text(_("Repeating \"dash gap ...\" pattern"));

    i++;

    /* Drop down marker selectors*/
    // TRANSLATORS: Path markers are an SVG feature that allows you to attach arbitrary shapes
    // (arrowheads, bullets, faces, whatever) to the start, end, or middle nodes of a path.

    spw_label(table, _("Markers"), 0, i, nullptr);

    hb = spw_hbox(table, 3, 1, i);
    i++;

    startMarkerCombo = Gtk::make_managed<MarkerComboBox>("marker-start", SP_MARKER_LOC_START);
    startMarkerCombo->set_flat(true);
    startMarkerCombo->set_tooltip_text(_("Start Markers are drawn on the first node of a path or shape"));
    startMarkerConn = startMarkerCombo->connect_changed([this]{ markerSelectCB(startMarkerCombo, SP_MARKER_LOC_START); });
    startMarkerCombo->connect_edit([this]{ enterEditMarkerMode(SP_MARKER_LOC_START); });
    startMarkerCombo->set_visible(true);

    UI::pack_start(*hb, *startMarkerCombo, true, true);

    midMarkerCombo = Gtk::make_managed<MarkerComboBox>("marker-mid", SP_MARKER_LOC_MID);
    midMarkerCombo->set_flat(true);
    midMarkerCombo->set_tooltip_text(_("Mid Markers are drawn on every node of a path or shape except the first and last nodes"));
    midMarkerConn = midMarkerCombo->connect_changed([this]{ markerSelectCB(midMarkerCombo, SP_MARKER_LOC_MID); });
    midMarkerCombo->connect_edit([this]{ enterEditMarkerMode(SP_MARKER_LOC_MID); });
    midMarkerCombo->set_visible(true);

    UI::pack_start(*hb, *midMarkerCombo, true, true);

    endMarkerCombo = Gtk::make_managed<MarkerComboBox>("marker-end", SP_MARKER_LOC_END);
    endMarkerCombo->set_flat(true);
    endMarkerCombo->set_tooltip_text(_("End Markers are drawn on the last node of a path or shape"));
    endMarkerConn = endMarkerCombo->connect_changed([this]{ markerSelectCB(endMarkerCombo, SP_MARKER_LOC_END); });
    endMarkerCombo->connect_edit([this]{ enterEditMarkerMode(SP_MARKER_LOC_END); });
    endMarkerCombo->set_visible(true);

    UI::pack_start(*hb, *endMarkerCombo, true, true);
    i++;

    /* Cap type */
    // TRANSLATORS: cap type specifies the shape for the ends of lines
    //spw_label(t, _("_Cap:"), 0, i);
    spw_label(table, _("Cap"), 0, i, nullptr);

    hb = spw_hbox(table, 3, 1, i);

    Gtk::ToggleButton *capGrp = nullptr;

    capButt = makeRadioButton(capGrp, INKSCAPE_ICON("stroke-cap-butt"),
                                hb, STROKE_STYLE_BUTTON_CAP, "butt");

    // TRANSLATORS: Butt cap: the line shape does not extend beyond the end point
    //  of the line; the ends of the line are square
    capButt->set_tooltip_text(_("Butt cap"));

    capRound = makeRadioButton(capGrp, INKSCAPE_ICON("stroke-cap-round"),
                                hb, STROKE_STYLE_BUTTON_CAP, "round");

    // TRANSLATORS: Round cap: the line shape extends beyond the end point of the
    //  line; the ends of the line are rounded
    capRound->set_tooltip_text(_("Round cap"));

    capSquare = makeRadioButton(capGrp, INKSCAPE_ICON("stroke-cap-square"),
                                hb, STROKE_STYLE_BUTTON_CAP, "square");

    // TRANSLATORS: Square cap: the line shape extends beyond the end point of the
    //  line; the ends of the line are square
    capSquare->set_tooltip_text(_("Square cap"));

    i++;

    /* Join type */
    // TRANSLATORS: The line join style specifies the shape to be used at the
    //  corners of paths. It can be "miter", "round" or "bevel".
    spw_label(table, _("Join"), 0, i, nullptr);

    hb = spw_hbox(table, 3, 1, i);

    Gtk::ToggleButton *joinGrp = nullptr;

    joinBevel = makeRadioButton(joinGrp, INKSCAPE_ICON("stroke-join-bevel"),
                                hb, STROKE_STYLE_BUTTON_JOIN, "bevel");

    // TRANSLATORS: Bevel join: joining lines with a blunted (flattened) corner.
    //  For an example, draw a triangle with a large stroke width and modify the
    //  "Join" option (in the Fill and Stroke dialog).
    joinBevel->set_tooltip_text(_("Bevel join"));

    joinRound = makeRadioButton(joinGrp, INKSCAPE_ICON("stroke-join-round"),
                                hb, STROKE_STYLE_BUTTON_JOIN, "round");

    // TRANSLATORS: Round join: joining lines with a rounded corner.
    //  For an example, draw a triangle with a large stroke width and modify the
    //  "Join" option (in the Fill and Stroke dialog).
    joinRound->set_tooltip_text(_("Round join"));

    joinMiter = makeRadioButton(joinGrp, INKSCAPE_ICON("stroke-join-miter"),
                                hb, STROKE_STYLE_BUTTON_JOIN, "miter");

    // TRANSLATORS: Miter join: joining lines with a sharp (pointed) corner.
    //  For an example, draw a triangle with a large stroke width and modify the
    //  "Join" option (in the Fill and Stroke dialog).
    joinMiter->set_tooltip_text(_("Miter join"));

    i++;

    _miter_hb = spw_hbox(table, 3, 1, i);

    /* Miterlimit  */
    // TRANSLATORS: Miter limit: only for "miter join", this limits the length
    //  of the sharp "spike" when the lines connect at too sharp an angle.
    // When two line segments meet at a sharp angle, a miter join results in a
    //  spike that extends well beyond the connection point. The purpose of the
    //  miter limit is to cut off such spikes (i.e. convert them into bevels)
    //  when they become too long.
    //spw_label(t, _("Miter _limit:"), 0, i);
    miterLimitAdj = Gtk::Adjustment::create(4.0, 0.0, 100000.0, 0.1, 10.0, 0.0);
    miterLimitSpin = Gtk::make_managed<SpinButton>(miterLimitAdj, 0.1, 2);
    miterLimitSpin->set_tooltip_text(_("Maximum length of the miter (in units of stroke width)"));
    miterLimitSpin->set_width_chars(6);
    miterLimitSpin->set_visible(true);
    sp_dialog_defocus_on_enter(*miterLimitSpin);

    UI::pack_start(*_miter_hb, *miterLimitSpin, true, true);
    miterLimitAdj->signal_value_changed().connect(sigc::mem_fun(*this, &StrokeStyle::setStrokeMiter));

    i++;
    // this is a hack to align label text with the first row in paint order selector
    auto& vbox = *Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox.set_homogeneous();
    /* Paint order */
    // TRANSLATORS: Paint order determines the order the 'fill', 'stroke', and 'markers are painted.
    vbox.append(*Gtk::make_managed<Gtk::Label>(_("Order")));
    vbox.append(*Gtk::make_managed<Gtk::Box>());
    _align_label = Gtk::make_managed<Gtk::Box>();
    vbox.append(*_align_label);
    table->attach(vbox, 0, i);

    _paint_order = Gtk::make_managed<PaintOrderWidget>();
    paintOrderConn = _paint_order->signal_values_changed().connect([this]() {
        if (update || !this->desktop) return;
        auto po = _paint_order->getValue();
        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "paint-order", po.get_value().c_str());
        sp_desktop_set_style(this->desktop, css);
        sp_repr_css_attr_unref(css);
    });
    table->attach(*_paint_order, 1, i, 3, 1);

    i++;
}

StrokeStyle::~StrokeStyle()
{
}

void StrokeStyle::setDesktop(SPDesktop *desktop)
{
    if (this->desktop != desktop) {

        if (this->desktop) {
            _document_replaced_connection.disconnect();
        }
        this->desktop = desktop;

        if (!desktop) {
            _handleDocumentReplaced(nullptr, nullptr);
            return;
        }

        _document_replaced_connection =
            desktop->connectDocumentReplaced(sigc::mem_fun(*this, &StrokeStyle::_handleDocumentReplaced));

        _handleDocumentReplaced(nullptr, desktop->getDocument());
        for (MarkerComboBox *combo : {startMarkerCombo, midMarkerCombo, endMarkerCombo}) {
            combo->setDesktop(desktop);
        }

        updateLine();
    }
}

void StrokeStyle::_handleDocumentReplaced(SPDesktop *, SPDocument *document)
{
    for (MarkerComboBox *combo : { startMarkerCombo, midMarkerCombo, endMarkerCombo }) {
        combo->setDocument(document);
    }
}


/**
 * Helper function for creating stroke-style radio buttons.
 *
 * \param[in] grp           The Gtk::CheckButton with which to group
 * \param[in] icon          The icon for the button
 * \param[in] hb            The Gtk::Box container in which to add the button
 * \param[in] button_type   The type (join/cap) for the button
 * \param[in] stroke_style  The style attribute to associate with the button
 *
 * \details After instantiating the button, it is added to a container box and
 *          a handler for the toggle event is connected.
 */
StrokeStyle::StrokeStyleButton *
StrokeStyle::makeRadioButton(Gtk::ToggleButton    *&grp,
                             char const            *icon,
                             Gtk::Box              *hb,
                             StrokeStyleButtonType  button_type,
                             gchar const           *stroke_style)
{
    g_assert(icon != nullptr);
    g_assert(hb  != nullptr);

    auto const tb = Gtk::make_managed<StrokeStyleButton>(grp, icon, button_type, stroke_style);
    UI::pack_start(*hb, *tb, false, false);
    tb->signal_toggled().connect([this, tb]() { buttonToggledCB(tb); });
    tb->set_hexpand();
    tb->set_halign(Gtk::Align::FILL);
    return tb;
}

void StrokeStyle::enterEditMarkerMode(SPMarkerLoc _editMarkerMode)
{
    SPDesktop *desktop = this->desktop;

    if (desktop) {
        set_active_tool(desktop, "Marker");
        Inkscape::UI::Tools::MarkerTool *mt = dynamic_cast<Inkscape::UI::Tools::MarkerTool*>(desktop->getTool());

        if(mt) {
            mt->editMarkerMode = _editMarkerMode;
            mt->selection_changed(desktop->getSelection());
        }
    }
}


bool StrokeStyle::areMarkersBeingUpdated()
{
    return startMarkerCombo->in_update() || midMarkerCombo->in_update() || endMarkerCombo->in_update();
}

/**
 * Handles when user selects one of the markers from the marker combobox.
 * Gets the marker uri string and applies it to all selected
 * items in the current desktop.
 */
void StrokeStyle::markerSelectCB(MarkerComboBox *marker_combo, SPMarkerLoc const which)
{
    if (update || areMarkersBeingUpdated()) {
        return;
    }

    SPDocument *document = desktop->getDocument();
    if (!document) {
        return;
    }

    // Get marker ID; could be empty (to remove marker)
    std::string marker = marker_combo->get_active_marker_uri();

    update = true;

    SPCSSAttr *css = sp_repr_css_attr_new();
    gchar const *combo_id = marker_combo->get_id();
    sp_repr_css_set_property(css, combo_id, marker.c_str());

    for (auto item : desktop->getSelection()->items()) {
        if (!is<SPShape>(item)) {
            continue;
        }
        if (Inkscape::XML::Node* selrepr = item->getRepr()) {
            sp_repr_css_change_recursive(selrepr, css, "style");
        }

        item->requestModified(SP_OBJECT_MODIFIED_FLAG);
        item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
        // perform update to make sure any previously referenced markers are released,
        // so they can be collected by DocumentUndo::done collect orphans
        document->ensureUpToDate();

        DocumentUndo::done(document, RC_("Undo", "Set markers"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }

    // edit marker mode - update
    if (auto mt = dynamic_cast<Inkscape::UI::Tools::MarkerTool*>(desktop->getTool())) {
        mt->editMarkerMode = which;
        mt->selection_changed(desktop->getSelection());
    }

    sp_repr_css_attr_unref(css);
    css = nullptr;

    update = false;
};

/**
 * Callback for when UnitMenu widget is modified.
 * Triggers update action.
 */
void StrokeStyle::unitChangedCB()
{
    Inkscape::Util::Unit const *new_unit = unitSelector->getUnit();

    if (_old_unit == new_unit)
        return;

    // If the unit selector is set to hairline, don't do the normal conversion.
    if (isHairlineSelected()) {
        // Force update in setStrokeWidth
        _old_unit = new_unit;
        _last_width = -1;
        setStrokeWidth();
        return;
    }

    if (new_unit->type == Inkscape::Util::UNIT_TYPE_DIMENSIONLESS) {
        // Prevent update in setStrokeWidth
        _last_width = 100.0;
        widthSpin->set_value(100);
    } else {
        // Remove the non-scaling-stroke effect and the hairline extensions
        if (!update) {
            SPCSSAttr *css = sp_repr_css_attr_new();
            sp_repr_css_unset_property(css, "vector-effect");
            sp_repr_css_unset_property(css, "-inkscape-stroke");
            sp_desktop_set_style(desktop, css);
            sp_repr_css_attr_unref(css);
            css = nullptr;
            DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Remove hairline stroke"),
                INKSCAPE_ICON("dialog-fill-and-stroke"));
        }
        if (_old_unit->type == Inkscape::Util::UNIT_TYPE_DIMENSIONLESS) {
            // Prevent update of unit (inf-loop) in updateLine
            _old_unit = new_unit;
            // Going from % to any other unit means our widthSpin is completely invalid.
            updateLine();
        } else {
            // Scale the value and record the old_unit
            widthSpin->set_value(Inkscape::Util::Quantity::convert(widthSpin->get_value(), _old_unit, new_unit));
        }
    }
    _old_unit = new_unit;
}

/**
 * Callback for when stroke style widget is modified.
 * Triggers update action.
 */
void
StrokeStyle::selectionModifiedCB(guint flags)
{
    // We care deeply about only updating when the style is updated
    // if we update on other flags, we slow inkscape down when dragging
    if (flags & (SP_OBJECT_STYLE_MODIFIED_FLAG)) {
        updateLine();
    }
}

/**
 * Callback for when stroke style widget is changed.
 * Triggers update action.
 */
void
StrokeStyle::selectionChangedCB()
{
    updateLine();
}

/**
 * Sets selector widgets' dash style from an SPStyle object.
 */
void
StrokeStyle::setDashSelectorFromStyle(DashSelector *dsel, SPStyle *style)
{
    auto [dash_pattern, offset] = getDashFromStyle(style);
    dsel->set_dash_pattern(dash_pattern, offset);
    update_dash_entry(dash_pattern);
}

void StrokeStyle::update_dash_entry(const std::vector<double> &dash_pattern)
{
    if (_editing_dash_pattern || contains_focus(*_pattern_entry)) { /* The GtkText object has focus. The focus test is required
                                                                       as the StokeStyle widget is updated after all changed
                                                                       are made (unnecessarily via selectionModifiedCB()).
                                                                       Without this test, the cursor is placed at the beginning
                                                                       of the GtkEntry after each character is typed. */
        return;
    }

    std::ostringstream ost;
    for (auto d : dash_pattern) {
        ost << d << ' ';
    }
    _pattern_entry->set_text(ost.str().c_str());

    if (!dash_pattern.empty()) {
        _pattern_label->set_visible(true);
        _pattern_entry->set_visible(true);
    }
    else {
        _pattern_label->set_visible(false);
        _pattern_entry->set_visible(false);
    }
}

/**
 * Sets the join type for a line, and updates the stroke style widget's buttons
 */
void
StrokeStyle::setJoinType (unsigned const jointype)
{
    Gtk::ToggleButton *tb = nullptr;
    switch (jointype) {
        case SP_STROKE_LINEJOIN_MITER:
            tb = joinMiter;
            break;
        case SP_STROKE_LINEJOIN_ROUND:
            tb = joinRound;
            break;
        case SP_STROKE_LINEJOIN_BEVEL:
            tb = joinBevel;
            break;
        default:
            // Should not happen
            std::cerr << "StrokeStyle::setJoinType(): Invalid value: " << jointype << std::endl;
            tb = joinMiter;
            break;
    }
    setJoinButtons(tb);
}

/**
 * Sets the cap type for a line, and updates the stroke style widget's buttons
 */
void
StrokeStyle::setCapType (unsigned const captype)
{
    Gtk::ToggleButton *tb = nullptr;
    switch (captype) {
        case SP_STROKE_LINECAP_BUTT:
            tb = capButt;
            break;
        case SP_STROKE_LINECAP_ROUND:
            tb = capRound;
            break;
        case SP_STROKE_LINECAP_SQUARE:
            tb = capSquare;
            break;
        default:
            // Should not happen
            std::cerr << "StrokeStyle::setCapType(): Invalid value: " << captype << std::endl;
            tb = capButt;
            break;
    }
    setCapButtons(tb);
}

/**
 * Sets the cap type for a line, and updates the stroke style widget's buttons
 */
void
StrokeStyle::setPaintOrder (gchar const *paint_order, bool enable_markers)
{
    std::vector<std::string> compiled;
    SPIPaintOrder temp;
    temp.read( paint_order );
    _paint_order->setValue(temp, enable_markers);
}

/**
 * Callback for when stroke style widget is updated, including markers, cap type,
 * join type, etc.
 */
void
StrokeStyle::updateLine()
{
    if (update) {
        return;
    }

    auto *widg = get_parent()->get_parent()->get_parent()->get_parent(); 
    auto dialogbase = dynamic_cast<Inkscape::UI::Dialog::DialogBase*>(widg);
    if (dialogbase && !dialogbase->getShowing()) {
        return;
    }

    update = true;

    Inkscape::Selection *sel = desktop ? desktop->getSelection() : nullptr;

    if (!sel || sel->isEmpty()) {
        // Nothing selected, grey-out all controls in the stroke-style dialog
        table->set_sensitive(false);

        update = false;

        return;
    }

    FillOrStroke kind = STROKE;

    // create temporary style
    SPStyle query(SP_ACTIVE_DOCUMENT);
    // query into it
    int result_sw = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_STROKEWIDTH);
    int result_ml = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_STROKEMITERLIMIT);
    int result_cap = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_STROKECAP);
    int result_join = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_STROKEJOIN);
    int result_order = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_PAINTORDER);

    SPIPaint &targPaint = *query.getFillOrStroke(kind == FILL);

    {
        table->set_sensitive(true);
        widthSpin->set_sensitive(true);

        if (result_sw == QUERY_STYLE_MULTIPLE_AVERAGED) {
            unitSelector->setUnit("%");
        } else if (query.stroke_extensions.hairline) {
            unitSelector->set_selected(_hairline_item);
        } else {
            // same width, or only one object; no sense to keep percent, switch to absolute
            Inkscape::Util::Unit const *tempunit = unitSelector->getUnit();
            if (tempunit->type != Inkscape::Util::UNIT_TYPE_LINEAR) {
                unitSelector->setUnit(desktop->getNamedView()->display_units->abbr);
            }
        }

        Inkscape::Util::Unit const *unit = unitSelector->getUnit();

        if (query.stroke_extensions.hairline) {
            widthSpin->set_sensitive(false);
            widthAdj->set_value(1);
        } else if (unit->type == Inkscape::Util::UNIT_TYPE_LINEAR) {
            double avgwidth = Inkscape::Util::Quantity::convert(query.stroke_width.computed, "px", unit);
            widthAdj->set_value(avgwidth);
        } else {
            widthAdj->set_value(100);
        }

        // if none of the selected objects has a stroke, than quite some controls should be disabled
        // These options should also be disabled for hairlines, since they don't make sense for
        // 0-width lines.
        // The markers might still be shown though, so marker and stroke-width widgets stay enabled
        bool is_enabled = (result_sw != QUERY_STYLE_NOTHING) && !targPaint.isNoneSet()
                           && !query.stroke_extensions.hairline;
        joinMiter->set_sensitive(is_enabled);
        joinRound->set_sensitive(is_enabled);
        joinBevel->set_sensitive(is_enabled);

        miterLimitSpin->set_sensitive(is_enabled);

        capButt->set_sensitive(is_enabled);
        capRound->set_sensitive(is_enabled);
        capSquare->set_sensitive(is_enabled);

        dashSelector->set_sensitive(is_enabled);
        _pattern_entry->set_sensitive(is_enabled);
        _paint_order->set_sensitive(is_enabled);
    }

    if (result_ml != QUERY_STYLE_NOTHING)
        miterLimitAdj->set_value(query.stroke_miterlimit.value); // TODO: reflect averagedness?

    using Inkscape::is_query_style_updateable;
    if (! is_query_style_updateable(result_join)) {
        setJoinType(query.stroke_linejoin.value);
    } else {
        setJoinButtons(nullptr);
    }

    if (! is_query_style_updateable(result_cap)) {
        setCapType (query.stroke_linecap.value);
    } else {
        setCapButtons(nullptr);
    }

    bool has_markers = false;
    auto const objects = sel->items_vector();
    if (!objects.empty()) {
        auto const style = sp_desktop_paint_sources({objects.front()}).front()->style; // a text: its first character's span
        /* Markers */
        has_markers = updateAllMarkers(objects); // FIXME: make this desktop query too

        /* Dash */
        setDashSelectorFromStyle(dashSelector, style); // FIXME: make this desktop query too
    }
    table->set_sensitive(true);

    if (! is_query_style_updateable(result_order)) {
        setPaintOrder (query.paint_order.value, has_markers);
        _align_label->set_visible(has_markers);
    } else {
        setPaintOrder (nullptr, true);
    }

    update = false;
}

/**
 * Sets a line's dash properties in a CSS style object.
 */
void set_scaled_dash(SPCSSAttr* css, int ndash, const double *dash, double offset, double scale) {
    if (ndash > 0) {
        Inkscape::CSSOStringStream osarray;
        for (int i = 0; i < ndash; i++) {
            osarray << dash[i] * scale;
            if (i < (ndash - 1)) {
                osarray << ",";
            }
        }
        sp_repr_css_set_property(css, "stroke-dasharray", osarray.str().c_str());

        Inkscape::CSSOStringStream osoffset;
        osoffset << offset * scale;
        sp_repr_css_set_property(css, "stroke-dashoffset", osoffset.str().c_str());
    } else {
        sp_repr_css_set_property(css, "stroke-dasharray", "none");
        sp_repr_css_set_property(css, "stroke-dashoffset", nullptr);
    }
}

double calc_scale_line_width(double width_typed, const SPItem* item, const Unit* unit) {
    if (unit->abbr == "%") {
        auto scale = item->i2doc_affine().descrim();;
        double old_w = item->style->stroke_width.computed;
        return (old_w * width_typed / 100) * scale;
    } else if (unit->type == Inkscape::Util::UNIT_TYPE_LINEAR) {
        return Inkscape::Util::Quantity::convert(width_typed, unit, "px");
    }
    return width_typed;
}

/**
 * Set the stroke width and adjust the dash pattern if needed.
 */
void StrokeStyle::setStrokeWidth()
{
    double width_typed = widthAdj->get_value();

    // Don't change the selection if an update is happening,
    // but also store the value for later comparison.
    if (update || fabs(_last_width - width_typed) < 1E-6) {
        _last_width = width_typed;
        return;
    }
    update = true;

    auto prefs = Inkscape::Preferences::get();
    auto unit = unitSelector->getUnit();

    SPCSSAttr *css = sp_repr_css_attr_new();
    if (isHairlineSelected()) {
        /* For renderers that don't understand -inkscape-stroke:hairline, fall back to 1px non-scaling */
        width_typed = 1;
        sp_repr_css_set_property(css, "vector-effect", "non-scaling-stroke");
        sp_repr_css_set_property(css, "-inkscape-stroke", "hairline");
    } else {
        sp_repr_css_unset_property(css, "vector-effect");
        sp_repr_css_unset_property(css, "-inkscape-stroke");
    }

    for (auto item : desktop->getSelection()->items()) {
        const double width = calc_scale_line_width(width_typed, item, unit);
        sp_repr_css_set_property_double(css, "stroke-width", width);

        if (prefs->getBool("/options/dash/scale", true)) {
            // This will read the old stroke-width to un-scale the pattern.
            auto [dash, offset] = getDashFromStyle(item->style);
            set_scaled_dash(css, dash.size(), dash.data(), offset, width);
        }
        sp_desktop_apply_css_recursive (item, css, true);
    }
    sp_desktop_set_style (desktop, css, false);

    sp_repr_css_attr_unref(css);
    DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Set stroke width"),
                       INKSCAPE_ICON("dialog-fill-and-stroke"));

    if (unit->abbr == "%") {
        // reset to 100 percent
        _last_width = 100.0;
        widthAdj->set_value(100.0);
    } else {
        _last_width = width_typed;
    }
    update = false;
}

/**
 * Apply the stroke dash pattern to objects, scale to the existing width if needed.
 */
void StrokeStyle::setStrokeDash()
{
    if (update) {
        return;
    }
    update = true;

    auto document = desktop->getDocument();
    auto prefs = Inkscape::Preferences::get();

    const auto& dash = dashSelector->get_dash_pattern();
    double offset    = dashSelector->get_offset();

    SPCSSAttr *css = sp_repr_css_attr_new();
    for (auto item : desktop->getSelection()->items()) {
        double scale = item->i2doc_affine().descrim();
        if(prefs->getBool("/options/dash/scale", true)) {
            scale = item->style->stroke_width.computed * scale;
        }

        set_scaled_dash(css, dash.size(), dash.data(), offset, scale);
        sp_desktop_apply_css_recursive (item, css, true);
    }
    sp_desktop_set_style (desktop, css, false);

    sp_repr_css_attr_unref(css);
    DocumentUndo::done(document, RC_("Undo", "Set stroke dash"),
                       INKSCAPE_ICON("dialog-fill-and-stroke"));
    update = false;
}

/**
 * Set the Miter Limit value only.
 */
void StrokeStyle::setStrokeMiter()
{
    if (update) return;
    update = true;

    SPCSSAttr *css = sp_repr_css_attr_new();
    auto const value = miterLimitAdj->get_value();
    sp_repr_css_set_property_double(css, "stroke-miterlimit", value);

    for (auto item : desktop->getSelection()->items()) {
        sp_desktop_apply_css_recursive(item, css, true);
    }
    sp_desktop_set_style (desktop, css, false);
    sp_repr_css_attr_unref(css);
    DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Set stroke miter"),
                       INKSCAPE_ICON("dialog-fill-and-stroke"));
    update = false;
}

/**
 * Returns whether the currently selected stroke width is "hairline"
 *
 */
bool
StrokeStyle::isHairlineSelected() const
{
    return unitSelector->get_selected() == _hairline_item;
}


/**
 * This routine handles toggle events for buttons in the stroke style dialog.
 *
 * When activated, this routine gets the data for the various widgets, and then
 * calls the respective routines to update css properties, etc.
 *
 */
void StrokeStyle::buttonToggledCB(StrokeStyleButton *tb)
{
    if (update) {
        return;
    }

    if (tb->get_active()) {
        /* TODO: Create some standardized method */
        SPCSSAttr *css = sp_repr_css_attr_new();

        switch (tb->get_button_type()) {
            case STROKE_STYLE_BUTTON_JOIN: 
                sp_repr_css_set_property(css, "stroke-linejoin", tb->get_stroke_style());
                sp_desktop_set_style (desktop, css);
                setJoinButtons(tb);
                break;
            case STROKE_STYLE_BUTTON_CAP:
                sp_repr_css_set_property(css, "stroke-linecap", tb->get_stroke_style());
                sp_desktop_set_style (desktop, css);
                setCapButtons(tb);
                break;
        }

        sp_repr_css_attr_unref(css);
        css = nullptr;

        DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Set stroke style"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }
}

/**
 * Updates the join style toggle buttons
 */
void
StrokeStyle::setJoinButtons(Gtk::ToggleButton *active)
{
    joinMiter->set_active(active == joinMiter);
    miterLimitSpin->set_sensitive(active == joinMiter && !isHairlineSelected());
    joinRound->set_active(active == joinRound);
    joinBevel->set_active(active == joinBevel);
    // Hide entire miter row when not Miter join
    _miter_hb->set_visible(active == joinMiter);
}

/**
 * Updates the cap style toggle buttons
 */
void
StrokeStyle::setCapButtons(Gtk::ToggleButton *active)
{
    capButt->set_active(active == capButt);
    capRound->set_active(active == capRound);
    capSquare->set_active(active == capSquare);
}


/**
 * Recursively builds a simple list from an arbitrarily complex selection
 * of items and grouped items
 */
static void buildGroupedItemList(SPObject *element, std::vector<SPObject*> &simple_list)
{
    if (is<SPGroup>(element)) {
        for (SPObject *i = element->firstChild(); i; i = i->getNext()) {
            buildGroupedItemList(i, simple_list);
        }
    } else {
        simple_list.push_back(element);
    }
}


/**
 * Updates the marker combobox to highlight the appropriate marker and scroll to
 * that marker.
 *
 * @returns true if any of the objects, children of groups, have markers applied
 */
bool
StrokeStyle::updateAllMarkers(std::vector<SPItem*> const &objects)
{
    struct { MarkerComboBox *key; int loc; } const keyloc[] = {
            { startMarkerCombo, SP_MARKER_LOC_START },
            { midMarkerCombo, SP_MARKER_LOC_MID },
            { endMarkerCombo, SP_MARKER_LOC_END }
    };

    bool all_texts = true;

    auto simplified_list = std::vector<SPObject *>();
    for (SPItem *item : objects) {
        buildGroupedItemList(item, simplified_list);
    }

    for (SPObject *object : simplified_list) {
        if (!is<SPText>(object)) {
            all_texts = false;
            break;
        }
    }

    bool has_markers = false;

    // Use the first in the list that has the marker of each type, if any
    for (auto const &markertype : keyloc) {
        // For all three marker types,

        // find the corresponding combobox item
        MarkerComboBox *combo = markertype.key;

        // Quit if we're in update state
        if (combo->in_update()) {
            return true; // Assume markers
        }

        // Per SVG spec, text objects cannot have markers; disable combobox if only texts are selected
        // They should also be disabled for hairlines, since scaling against a 0-width line doesn't
        // make sense.
        combo->set_sensitive(!all_texts && !isHairlineSelected());

        SPObject *marker = nullptr;

        if (!all_texts && !isHairlineSelected()) {
            for (SPObject *object : simplified_list) {
                char const *value = object->style->marker_ptrs[markertype.loc]->value();

                // If the object has this type of markers,
                if (value == nullptr)
                    continue;

                // Extract the name of the marker that the object uses
                marker = getMarkerObj(value, object->document);
                has_markers = true;
            }
        }

        // Scroll the combobox to that marker
        combo->set_current(marker);
    }

    return has_markers;
}

} // namespace Inkscape::UI::Widget

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
