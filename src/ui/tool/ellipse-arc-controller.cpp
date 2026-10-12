// SPDX-License-Identifier: GPL-2.0-or-later
#include "ellipse-arc-controller.h"
#include <cmath>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include "control-point.h"
#include "desktop.h"
#include "document-undo.h"
#include "object/sp-ellipse.h"
#include "path-manipulator.h"
#include "preferences.h"
#include "ui/icon-names.h"
#include "ui/widget/events/canvas-event.h"

namespace Inkscape::UI {
class EllipseArcPoint final : public ControlPoint {
public:
    EllipseArcPoint(EllipseArcController &owner, bool end)
        : ControlPoint(owner._pm._desktop, {}, SP_ANCHOR_CENTER, CANVAS_ITEM_CTRL_TYPE_ROTATE)
        , _owner(owner), _end(end)
    {
        _canvas_item_ctrl->set_name(end ? "CanvasItemCtrl:EllipseArcEnd" : "CanvasItemCtrl:EllipseArcStart");
    }
    Geom::Point displayPosition() const
    {
        auto ellipse = cast<SPGenericEllipse>(_owner._pm._path);
        if (!ellipse) return {};
        auto transform = _owner._pm._getTransform();
        auto at = ellipse->getPointAtAngle(_end ? ellipse->end : ellipse->start) * transform;
        auto radial = Geom::unit_vector(at - Geom::Point(ellipse->cx.computed, ellipse->cy.computed) * transform);
        auto zoom = _desktop->current_zoom();
        // Leave path anchors accessible, including the coincident start/end of
        // a full ellipse. Offsets have constant screen size at every zoom.
        auto offset = radial * (14 / zoom);
        if (Geom::distance(ellipse->getPointAtAngle(ellipse->start) * transform,
                           ellipse->getPointAtAngle(ellipse->end) * transform) * zoom < 16) {
            offset += Geom::Point(-radial[Geom::Y], radial[Geom::X]) * ((_end ? 8 : -8) / zoom);
        }
        return at + offset;
    }
protected:
    bool grabbed(MotionEvent const &) override
    {
        auto ellipse = cast<SPGenericEllipse>(_owner._pm._path);
        if (!ellipse) return false;
        _start = ellipse->start; _finish = ellipse->end; _type = ellipse->arcType();
        _offset = position() - ellipse->getPointAtAngle(_end ? _finish : _start) * _owner._pm._getTransform();
        _owner._dragging = true;
        _owner._active = this;
        _owner._pm.hideDragPoint();
        return false;
    }
    void dragged(Geom::Point &at, MotionEvent const &event) override
    {
        auto ellipse = cast<SPGenericEllipse>(_owner._pm._path);
        if (!ellipse || !_owner._dragging) return;
        if (_is_drag_cancelled(event)) { restore(); at = _last_drag_origin(); return; }
        auto point = (at - _offset) * _owner._pm._getTransform().inverse();
        auto delta = (point - Geom::Point(ellipse->cx.computed, ellipse->cy.computed)) *
                     Geom::Scale(ellipse->rx.computed, ellipse->ry.computed).inverse();
        if (Geom::L2(delta) < 1e-9) return;
        double angle = std::atan2(delta[Geom::Y], delta[Geom::X]);
        if (event.modifiers & GDK_CONTROL_MASK) {
            double snaps = Preferences::get()->getDoubleLimited("/options/rotationsnapsperpi/value", 12, .1, 1800);
            angle = std::round(angle * snaps / M_PI) * M_PI / snaps;
        }
        ellipse->start = _start; ellipse->end = _finish;
        if (_end) ellipse->end = angle; else ellipse->start = angle;
        if (event.modifiers & GDK_SHIFT_MASK) {
            double difference = angle - (_end ? _finish : _start);
            if (_end) ellipse->start += difference; else ellipse->end += difference;
        }
        // Native inside/outside convention, with its dead band.
        double side = Geom::dot(delta, delta);
        if (side < .75) ellipse->setArcType(SP_GENERIC_ELLIPSE_ARC_TYPE_ARC);
        else if (side > 1.25) ellipse->setArcType(SP_GENERIC_ELLIPSE_ARC_TYPE_SLICE);
        ellipse->normalize();
        preview();
        at = ellipse->getPointAtAngle(_end ? ellipse->end : ellipse->start) * _owner._pm._getTransform() + _offset;
    }
    void ungrabbed(ButtonReleaseEvent const *event) override
    {
        auto ellipse = cast<SPGenericEllipse>(_owner._pm._path);
        if (!ellipse || !_owner._dragging) return;
        if (!event) restore();
        bool changed = ellipse->start != _start || ellipse->end != _finish || ellipse->arcType() != _type;
        _owner._dragging = false;
        _owner._active = nullptr;
        _owner._pm.refreshEllipseGeometry(event && changed);
        if (event && changed) DocumentUndo::done(ellipse->document, RC_("Undo", "Change ellipse arc"), INKSCAPE_ICON("draw-ellipse"));
        _owner.queueRefresh();
    }
    bool clicked(ButtonReleaseEvent const &event) override
    {
        if (event.button != 1) return false;
        auto ellipse = cast<SPGenericEllipse>(_owner._pm._path);
        if (ellipse && (event.modifiers & GDK_SHIFT_MASK) && (ellipse->start != 0 || ellipse->end != 0)) {
            ellipse->start = ellipse->end = 0;
            preview();
            _owner._pm.refreshEllipseGeometry(true);
            DocumentUndo::done(ellipse->document, RC_("Undo", "Make ellipse whole"), INKSCAPE_ICON("draw-ellipse"));
            _owner.queueRefresh();
        }
        return true;
    }
    Glib::ustring _getTip(unsigned) const override
    {
        return _end ? _("Ellipse arc end: drag to change angle; Ctrl snaps; Shift moves both ends; Shift-click makes whole")
                    : _("Ellipse arc start: drag to change angle; Ctrl snaps; Shift moves both ends; Shift-click makes whole");
    }
private:
    void preview()
    {
        auto ellipse = cast<SPGenericEllipse>(_owner._pm._path);
        if (!ellipse) return;
        ellipse->set_shape();
        ellipse->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
        if (auto curve = ellipse->curveForEdit()) _owner._pm.previewCorners(*curve);
    }
    void restore()
    {
        if (auto ellipse = cast<SPGenericEllipse>(_owner._pm._path)) {
            ellipse->start = _start; ellipse->end = _finish; ellipse->setArcType(_type); preview();
        }
    }
    EllipseArcController &_owner;
    bool _end;
    double _start = 0, _finish = 0;
    GenericEllipseArcType _type = SP_GENERIC_ELLIPSE_ARC_TYPE_SLICE;
    Geom::Point _offset;
    friend class EllipseArcController;
};
EllipseArcController::EllipseArcController(PathManipulator &pm) : _pm(pm) { queueRefresh(); }
EllipseArcController::~EllipseArcController()
{
    _refresh_idle.disconnect();
    if (_active && _pm._path) _active->restore();
}
void EllipseArcController::pathReleased()
{
    _refresh_idle.disconnect();
    _active = nullptr;
    _start.reset(); _end.reset(); _dragging = false;
}
void EllipseArcController::queueRefresh()
{
    if (_dragging || _refresh_idle.connected()) return;
    _refresh_idle = Glib::signal_idle().connect([this] { refresh(); return false; });
}
void EllipseArcController::refresh()
{
    if (_dragging) return;
    auto ellipse = cast<SPGenericEllipse>(_pm._path);
    if (!ellipse || !PathManipulator::canEditShape(ellipse) || ellipse->rx.computed <= 0 || ellipse->ry.computed <= 0) {
        _start.reset(); _end.reset(); return;
    }
    if (!_start) _start = std::make_unique<EllipseArcPoint>(*this, false);
    if (!_end) _end = std::make_unique<EllipseArcPoint>(*this, true);
    _start->setPosition(_start->displayPosition());
    _end->setPosition(_end->displayPosition());
}
} // namespace Inkscape::UI
