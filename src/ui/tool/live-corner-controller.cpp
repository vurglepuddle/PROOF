// SPDX-License-Identifier: GPL-2.0-or-later
#include "live-corner-controller.h"

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/eventcontrollerkey.h>
#include "control-point.h"
#include "control-point-selection.h"
#include "desktop.h"
#include "document-undo.h"
#include "display/control/canvas-item-bpath.h"
#include "object/sp-item.h"
#include "object/sp-rect.h"
#include "object/sp-namedview.h"
#include "path-manipulator.h"
#include "preferences.h"
#include "ui/icon-names.h"
#include "ui/tools/tool-base.h"
#include "ui/widget/canvas.h"
#include "ui/widget/events/canvas-event.h"
#include "xml/repr.h"
#include "util/units.h"

namespace Inkscape::UI {
using namespace LiveCorners;

class CornerDisplayPreferences {
public:
    Pref<bool> visible{show_widgets_preference, true};
    Pref<double> maximum_angle{maximum_angle_preference, 180, 0, 180};
    explicit CornerDisplayPreferences(std::function<void()> changed) {
        visible.action = changed;
        maximum_angle.action = std::move(changed);
    }
};

class LiveCornerWidget final : public ControlPoint {
public:
    LiveCornerWidget(LiveCornerController &owner, std::size_t corner)
        : ControlPoint(owner._pm._desktop, Geom::Point(), SP_ANCHOR_CENTER,
                       CANVAS_ITEM_CTRL_TYPE_BOX_RADIUS), _owner(owner), _corner(corner)
    {
        _canvas_item_ctrl->set_name("CanvasItemCtrl:LiveCorner");
    }
    void display(Geom::Point const &point, bool selected, bool limit = false)
    {
        setPosition(point);
        set_selected_appearance(selected);
        _canvas_item_ctrl->set_stroke(limit ? 0xe53935ff : 0x277fffff);
    }
protected:
    bool grabbed(MotionEvent const &) override { _owner.begin(_corner); return false; }
    void dragged(Geom::Point &at, MotionEvent const &event) override
    {
        at = _owner.drag(_corner, at - _last_drag_origin(), _is_drag_cancelled(event));
    }
    void ungrabbed(ButtonReleaseEvent const *event) override { _owner.finish(event == nullptr); }
    bool clicked(ButtonReleaseEvent const &event) override
    {
        if (event.button != 1) return false;
        if (event.modifiers & GDK_ALT_MASK) _owner.cycle(_corner);
        else _owner.choose(_corner, event.modifiers & GDK_SHIFT_MASK);
        return true;
    }
    bool doubleclicked(ButtonReleaseEvent const &event) override
    {
        if (event.button != 1) return false;
        _owner.open(_corner, position());
        return true;
    }
    Glib::ustring _getTip(unsigned) const override
    {
        return _("Live Corner: drag to round; Shift-click selects corners; Alt-click changes type; double-click edits radius");
    }
    Glib::ustring _getDragTip(MotionEvent const &) const override
    {
        auto unit = _owner._pm._desktop->getNamedView()->getDisplayUnit();
        return format_tip(_("Corner radius: %.2f %s"),
            _owner._model.corners[_corner].radius * _owner.displayScale(), unit ? unit->abbr.c_str() : "px");
    }
    bool _hasDragTips() const override { return true; }
private:
    LiveCornerController &_owner;
    std::size_t _corner;
};

bool isLiveCorner(ControlPoint const *point) { return dynamic_cast<LiveCornerWidget const *>(point); }

LiveCornerController::LiveCornerController(PathManipulator &pm) : _pm(pm)
{
    _title.set_text(_("Corners"));
    _radius_label.set_text(_("Radius (px)"));
    _type_label.set_text(_("Corner type"));
    _rounding_label.set_text(_("Rounding"));
    _type.set_model(Gtk::StringList::create({_ ("Round"), _ ("Inverted Round"), _ ("Chamfer")}));
    _rounding.set_model(Gtk::StringList::create({_ ("Absolute"), _ ("Relative")}));
    _radius.set_range(0, 1e9);
    _radius.set_increments(1, 10);
    _radius.set_digits(2);
    _fields.set_margin(10);
    for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&_title, &_type_label, &_type,
         &_radius_label, &_radius, &_rounding_label, &_rounding}) _fields.append(*widget);
    _popup.set_child(_fields);
    _popup.set_autohide(true);
    _radius.signal_value_changed().connect([this] { applyFields(); });
    _type.property_selected().signal_changed().connect([this] { applyFields(); });
    _rounding.property_selected().signal_changed().connect([this] { applyFields(); });
    _popup.signal_closed().connect([this] {
        if (!_updating && !_editing.empty()) finish();
    });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint key, guint, Gdk::ModifierType) {
        if (key != GDK_KEY_Escape && key != GDK_KEY_Return && key != GDK_KEY_KP_Enter) return false;
        // Nested dropdown lists consume Enter/Escape themselves. Only accept or
        // cancel Corners after the list has closed and focus returned to a field.
        auto root = gtk_widget_get_root(GTK_WIDGET(_popup.gobj()));
        auto focus = root ? gtk_root_get_focus(root) : nullptr;
        auto dropdown_open = [this](GtkWidget *widget, auto const &self) -> bool {
            for (auto child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
                if (GTK_IS_POPOVER(child) && child != GTK_WIDGET(_popup.gobj()) && gtk_widget_get_mapped(child)) return true;
                if (self(child, self)) return true;
            }
            return false;
        };
        bool nested = dropdown_open(GTK_WIDGET(_pm._desktop->getCanvas()->gobj()), dropdown_open);
        if (nested) return false;
        if (key != GDK_KEY_Escape && (!focus ||
            (focus != GTK_WIDGET(_radius.gobj()) && !gtk_widget_is_ancestor(focus, GTK_WIDGET(_radius.gobj()))))) return false;
        if (key != GDK_KEY_Escape) _radius.update();
        finish(key == GDK_KEY_Escape);
        _popup.popdown();
        _pm._desktop->getCanvas()->grab_focus();
        return true;
    }, false);
    _popup.add_controller(keys);
    _display = std::make_unique<CornerDisplayPreferences>([this] { queueRefresh(); });
    queueRefresh();
}

LiveCornerController::~LiveCornerController()
{
    _display.reset();
    _refresh_idle.disconnect();
    _updating = true;
    if (_dragging && _pm._path) _pm.previewCorners(_pm._spcurve);
    _popup.popdown();
    if (_popup.get_parent()) _popup.unparent();
    _widgets.clear();
}

void LiveCornerController::queueRefresh(bool reset_selection)
{
    if (_dragging) return;
    if (reset_selection) _selected.clear();
    if (_refresh_idle.connected()) return;
    _refresh_idle = Glib::signal_idle().connect([this] { refresh(); return false; });
}

void LiveCornerController::refresh()
{
    if (_dragging) return;
    if (!_pm._path) { _widgets.clear(); return; }
    auto shape = cast<SPShape>(_pm._path);
    if (!shape || shape->hasPathEffectRecursive() || shape->hasMarkers()) { _widgets.clear(); return; }
    auto text = _pm._path->getRepr()->attribute(attribute);
    auto parsed = Model::read(text);
    if (parsed && parsed->reconcile(_pm._spcurve)) _model = std::move(*parsed);
    else _model = Model::fromPath(_pm._spcurve);
    // Recover native rectangle radii without replacing their elliptical outline.
    // Unequal rx/ry use the same affine frame as a nonuniformly scaled round.
    if (!parsed) {
        if (auto rect = cast<SPRect>(_pm._path)) {
            auto rx = std::min(rect->width.computed / 2, rect->rx._set ? rect->rx.computed : rect->ry.computed);
            auto ry = std::min(rect->height.computed / 2, rect->ry._set ? rect->ry.computed : rect->rx.computed);
            if (rx > 0 && ry > 0) {
                double ratio = ry / rx;
                double x = rect->x.computed, y = rect->y.computed / ratio;
                double w = rect->width.computed, h = rect->height.computed / ratio;
                Geom::Path outline(Geom::Point(x, y));
                outline.appendNew<Geom::LineSegment>(Geom::Point(x + w, y));
                outline.appendNew<Geom::LineSegment>(Geom::Point(x + w, y + h));
                outline.appendNew<Geom::LineSegment>(Geom::Point(x, y + h));
                outline.close();
                _model = Model::fromPath({outline});
                _model.frame = Geom::Scale(1, ratio);
                _model.setRadius({0, 1, 2, 3}, rx);
                // SPRect uses C1 = .554 rather than the circular kappa. Keep
                // that outline exact, so a later node edit only bakes its damage.
                for (auto &corner : _model.corners) corner.handle_factor = .554 / (4. / 3. * std::tan(M_PI / 8));
            }
        }
    }
    _points = _model.nodePoints();
    if (!_display->visible) {
        _shown.clear();
        _widgets.clear();
        return;
    }
    bool any = !_pm._selection.empty();
    std::vector<std::size_t> shown;
    for (std::size_t c = 0; c < _model.corners.size(); ++c) {
        if (_display->maximum_angle < 180) {
            auto angle = _model.displayAngle(_model.corners[c], _model.frame * _pm._getTransform());
            if (!angle || *angle > _display->maximum_angle * M_PI / 180 + 1e-8) continue;
        }
        bool show = !any;
        for (auto point : _points[c]) {
            point *= _pm._getTransform();
            for (auto selected : _pm._selection) {
                if (Geom::are_near(selected->position(), point, 1e-5)) { show = true; break; }
            }
        }
        if (show) shown.push_back(c);
    }
    // Keep thousands of on-canvas widgets out of large drawings, as Illustrator
    // does. Geometry and saved live parameters are unaffected by this display cap.
    if (shown.size() > 100) shown.clear();
    _shown = std::move(shown);
    _widgets.clear();
    for (auto c : _shown) {
        auto widget = std::make_unique<LiveCornerWidget>(*this, c);
        widget->display(widgetPosition(c), _selected.contains(c));
        _widgets.push_back(std::move(widget));
    }
}

Geom::Point LiveCornerController::widgetPosition(std::size_t c) const
{
    auto info = _model.geometry(_model.corners[c]);
    if (!info) return {};
    Geom::Point at = info->anchor * _model.frame;
    if (_points[c].size() == 2) at = (_points[c][0] + _points[c][1]) / 2;
    auto transform = _pm._getTransform();
    auto anchor = info->anchor * _model.frame * transform;
    auto direction = Geom::unit_vector((info->anchor + info->direction) * _model.frame * transform - anchor);
    return at * transform + direction * (12 / _pm._desktop->current_zoom());
}

double LiveCornerController::displayScale() const
{
    auto item = cast<SPItem>(_pm._path);
    auto unit = _pm._desktop->getNamedView()->getDisplayUnit();
    return item && unit ? std::max(1e-9, std::abs((_model.frame * item->i2doc_affine()).descrim())) / unit->factor : 1;
}

void LiveCornerController::choose(std::size_t c, bool add)
{
    if (add) {
        if (!_selected.erase(c)) _selected.insert(c);
    } else {
        _selected.clear();
        _selected.insert(c);
    }
    for (std::size_t i = 0; i < _widgets.size(); ++i) _widgets[i]->display(widgetPosition(_shown[i]), _selected.contains(_shown[i]));
}

std::vector<std::size_t> LiveCornerController::targets(std::size_t c) const
{
    if (_selected.contains(c)) return {_selected.begin(), _selected.end()};
    if (_selected.empty() && std::find(_shown.begin(), _shown.end(), c) != _shown.end()) return _shown;
    return {c};
}

void LiveCornerController::begin(std::size_t c)
{
    _refresh_idle.disconnect();
    _before = _model;
    _editing = targets(c);
    _start_radius = _model.corners[c].radius;
    _dragging = true;
    _cancelled = false;
}

Geom::Point LiveCornerController::drag(std::size_t c, Geom::Point const &delta, bool cancelled)
{
    if (cancelled) {
        _cancelled = true;
        _model = _before;
        _points = _model.nodePoints();
        _limit_preview.reset();
    }
    else {
        auto info = _before.geometry(_before.corners[c]);
        auto inverse = (_model.frame * _pm._getTransform()).inverse();
        auto local_delta = delta * inverse - Geom::Point(0, 0) * inverse;
        double amount = _start_radius + Geom::dot(local_delta, info->direction) *
            (_model.corners[c].rounding == Rounding::Relative ? 1 : std::tan(info->angle / 2)) /
            std::cos(info->angle / 2);
        _model = _before;
        double applied = _model.setRadius(_editing, amount);
        _points = _model.nodePoints();
        bool limit = amount > applied + 1e-5;
        for (std::size_t i = 0; i < _widgets.size(); ++i) _widgets[i]->display(widgetPosition(_shown[i]), _selected.contains(_shown[i]), limit);
        if (limit) {
            if (!_limit_preview) _limit_preview = make_canvasitem<Inkscape::CanvasItemBpath>(_pm._desktop->getCanvasTemp());
            _limit_preview->set_stroke(0xe53935ff);
            _limit_preview->set_bpath(_model.roundedCurves(_editing) * _pm._getTransform());
        } else _limit_preview.reset();
    }
    _pm.previewCorners(_model.render());
    return widgetPosition(c);
}

void LiveCornerController::finish(bool cancelled)
{
    if (!_dragging) return;
    _limit_preview.reset();
    if (cancelled || _cancelled) {
        _model = _before;
        _pm.previewCorners(_pm._spcurve);
    } else if (_model.write() != _before.write()) commit();
    _dragging = false;
    _editing.clear();
    queueRefresh();
}

void LiveCornerController::commit()
{
    std::vector<Geom::Point> selected;
    for (auto &subpath : _pm._subpaths) {
        for (auto node = subpath->begin(); node != subpath->end(); ++node) {
            if (node->selected()) selected.push_back(node->position());
        }
    }
    auto restored = selected;
    auto before = _before.nodePoints(), after = _model.nodePoints();
    auto transform = _pm._getTransform();
    for (std::size_t c = 0; c < before.size(); ++c) {
        for (std::size_t i = 0; i < before[c].size(); ++i) {
            if (!std::any_of(selected.begin(), selected.end(), [&](auto const &point) {
                return Geom::are_near(before[c][i] * transform, point, 1e-5);
            })) continue;
            if (before[c].size() == after[c].size()) restored.push_back(after[c][i] * transform);
            else for (auto point : after[c]) restored.push_back(point * transform);
        }
    }
    _pm.commitCorners(_model.render(), _model.write(), restored);
    DocumentUndo::done(_pm._desktop->getDocument(), RC_("Undo", "Edit live corners"), INKSCAPE_ICON("tool-node-editor"));
}

void LiveCornerController::cycle(std::size_t c)
{
    begin(c);
    for (auto i : _editing) _model.corners[i].style = Style((int(_model.corners[i].style) + 1) % 3);
    finish();
}

void LiveCornerController::open(std::size_t c, Geom::Point const &at)
{
    begin(c);
    _updating = true;
    auto const &corner = _model.corners[c];
    auto unit = _pm._desktop->getNamedView()->getDisplayUnit();
    _radius_label.set_text(Glib::ustring(_("Radius")) + " (" + (unit ? unit->abbr : "px") + ")");
    _radius.set_value(corner.radius * displayScale());
    _type.set_selected(unsigned(corner.style));
    _rounding.set_selected(unsigned(corner.rounding));
    _updating = false;
    if (!_popup.get_parent()) _popup.set_parent(*_pm._desktop->getCanvas());
    auto window = _pm._desktop->d2w(at);
    _popup.set_pointing_to(Gdk::Rectangle(int(window[0]), int(window[1]), 1, 1));
    _popup.popup();
    _radius.grab_focus();
}

void LiveCornerController::applyFields()
{
    if (_updating || !_dragging || _editing.empty()) return;
    // A dropdown change can arrive before the spin entry commits its text.
    // Parse it under the update guard before applying the other fields.
    _updating = true;
    _radius.update();
    _updating = false;
    for (auto c : _editing) {
        _model.corners[c].style = Style(_type.get_selected());
        _model.corners[c].rounding = Rounding(_rounding.get_selected());
    }
    auto amount = _model.setRadius(_editing, _radius.get_value() / displayScale());
    _points = _model.nodePoints();
    _updating = true;
    _radius.set_value(amount * displayScale());
    _updating = false;
    _pm.previewCorners(_model.render());
}

void LiveCornerController::updateMetadata()
{
    if (_dragging || !_pm._path) return;
    auto repr = _pm._path->getRepr();
    auto text = repr->attribute(attribute);
    if (!text) return;
    auto model = Model::read(text);
    std::string value;
    if (model && model->reconcile(_pm._spcurve)) value = model->write();
    repr->setAttribute(attribute, value.empty() ? nullptr : value.c_str());
    queueRefresh();
}

void LiveCornerController::captureNativeCorners()
{
    if (_dragging || !is<SPRect>(_pm._path)) return;
    // Native rx/ry already represent live rounds before auxiliary data exists.
    // Capture them before an ordinary anchor edit replaces the rectangle.
    refresh();
    auto model = _model;
    if (!model.hasRounding() || !model.reconcile(_pm._spcurve)) return;
    auto value = model.write();
    _pm._path->getRepr()->setAttribute(attribute, value.empty() ? nullptr : value.c_str());
}

void LiveCornerController::pathReleased()
{
    _limit_preview.reset();
    _dragging = false;
    _editing.clear();
    _updating = true;
    _popup.popdown();
    _updating = false;
    for (auto &widget : _widgets) widget->setVisible(false);
    queueRefresh();
}

} // namespace Inkscape::UI
