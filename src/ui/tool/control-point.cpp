// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "control-point.h"

#include "commit-events.h"
#include "desktop.h"
#include "display/control/snap-indicator.h"
#include "message-context.h"
#include "object/sp-namedview.h"
#include "transform-handle-set.h"
#include "ui/tools/tool-base.h"
#include "ui/widget/canvas.h" // autoscroll
#include "ui/widget/events/canvas-event.h"

namespace Inkscape::UI {

namespace {
ControlPoint *grab_owner = nullptr;
}

ControlPoint *ControlPoint::mouseovered_point = nullptr;

sigc::signal<void (ControlPoint*)> ControlPoint::signal_mouseover_change;

Geom::Point ControlPoint::_drag_event_origin(Geom::infinity(), Geom::infinity());

Geom::Point ControlPoint::_drag_origin(Geom::infinity(), Geom::infinity());

/// Events which should be captured when a handle is being dragged.
static constexpr auto grab_event_mask = EventType::BUTTON_PRESS   |
                                        EventType::BUTTON_RELEASE |
                                        EventType::MOTION         |
                                        EventType::KEY_PRESS      |
                                        EventType::KEY_RELEASE    |
                                        EventType::SCROLL;

bool ControlPoint::_drag_initiated = false;
bool ControlPoint::_event_grab = false;

ControlPoint::ControlPoint(SPDesktop *d, Geom::Point const &initial_pos, SPAnchorType anchor,
                           Inkscape::CanvasItemCtrlType type,
                           Inkscape::CanvasItemGroup *group)
    : _desktop(d)
    , _position(initial_pos)
{
    _canvas_item_ctrl = make_canvasitem<Inkscape::CanvasItemCtrl>(group ? group : _desktop->getCanvasControls(), type);
    _canvas_item_ctrl->set_name("CanvasItemCtrl:ControlPoint");
    _canvas_item_ctrl->set_anchor(anchor);

    _commonInit();
}

ControlPoint::~ControlPoint()
{
    // MUST release the static global lock.
    // If this point is being deleted (e.g. by LPE Update) 
    // while it holds the mouse grab,
    // Otherwise, Inkscape thinks we are still dragging.
    if (grab_owner == this) {
        if (_event_grab) _canvas_item_ctrl->ungrab();
        grab_owner = nullptr;
        _event_grab = false;
        _drag_initiated = false;
    }

    // avoid storing invalid points in mouseovered_point
    if (this == mouseovered_point) {
        _clearMouseover();
    }

    _canvas_item_ctrl->set_visible(false);
}

void ControlPoint::_commonInit()
{
    _canvas_item_ctrl->set_position(_position);
    _event_handler_connection = _canvas_item_ctrl->connect_event([this] (CanvasEvent const &event) {
        // re-routes events into the virtual function   TODO: Refactor this nonsense.
        if (!_desktop) {
            return false;
        }
        return _eventHandler(_desktop->getTool(), event);
    });
}

void ControlPoint::setPosition(Geom::Point const &pos)
{
    _position = pos;
    _canvas_item_ctrl->set_position(_position);
}

void ControlPoint::move(Geom::Point const &pos)
{
    setPosition(pos);
}

void ControlPoint::transform(Geom::Affine const &m) {
    move(position() * m);
}

bool ControlPoint::visible() const
{
    return _canvas_item_ctrl->is_visible();
}

void ControlPoint::setVisible(bool v)
{
    if (v) {
        _canvas_item_ctrl->set_visible(true);
    } else {
        _canvas_item_ctrl->set_visible(false);
    }
}

Glib::ustring ControlPoint::format_tip(char const *format, ...)
{
    va_list args;
    va_start(args, format);
    char *dyntip = g_strdup_vprintf(format, args);
    va_end(args);
    Glib::ustring ret = dyntip;
    g_free(dyntip);
    return ret;
}


// ===== Setters =====

void ControlPoint::_setSize(unsigned int size)
{
    _canvas_item_ctrl->_set_size(size);
}

void ControlPoint::_setControlType(Inkscape::CanvasItemCtrlType type)
{
    _canvas_item_ctrl->set_type(type);
}

// main event callback, which emits all other callbacks.
bool ControlPoint::_eventHandler(Tools::ToolBase *tool, CanvasEvent const &event)
{
    // NOTE the static variables below are shared for all points!
    // TODO handle clicks and drags from other buttons too
    
    if (!tool || !_desktop) {
        return false;
    } else if (tool->getDesktop() !=_desktop) {
        g_warning("ControlPoint: desktop pointers not equal!");
    }

    // offset from the pointer hotspot to the center of the grabbed knot in desktop coords
    static Geom::Point pointer_offset;
    // number of last doubleclicked button
    static unsigned next_release_doubleclick = 0;

    _double_clicked = false;

    auto prefs = Preferences::get();
    int const drag_tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

    bool ret = false;

    auto key_event_handler = [this] (KeyEvent const &event) {
        if (mouseovered_point != this){
            return false;
        }
        if (_drag_initiated) {
            return true; // this prevents the tool from overwriting the drag tip
        } else if (auto change = event.modifiersChange()) {
            // we need to return true if there was a tip available, otherwise the tool's
            // handler will process this event and set the tool's message, overwriting
            // the point's message
            return _updateTip(event.modifiers ^ change);
        }
        return false;
    };

    inspect_event(event,
    [&] (ButtonPressEvent const &event) {
        if (event.num_press == 1) {
            next_release_doubleclick = 0;
            if (event.button == 1 && !tool->is_space_panning()) {
                // 1st mouse button click. internally, start dragging, but do not emit signals
                // or change position until drag tolerance is exceeded.
                _drag_event_origin = event.pos;
                pointer_offset = _position - _desktop->w2d(_drag_event_origin);
                _drag_initiated = false;
                // route all events to this handler
                _canvas_item_ctrl->grab(grab_event_mask); // cursor is null
                _event_grab = true;
                grab_owner = this;
                _setState(STATE_CLICKED);
                ret = true;
            } else {
                ret = _event_grab;
            }
        } else if (event.num_press == 2) {
            // store the button number for next release
            next_release_doubleclick = event.button;
            ret = true;
        }
    },

    [&] (MotionEvent const &event) {
        if (_event_grab && !tool->is_space_panning()) {
            _desktop->getSnapIndicator()->remove_snaptarget(); 
            bool transferred = false;
            if (!_drag_initiated) {
                if (Geom::LInfty(event.pos - _drag_event_origin) <= drag_tolerance) {
                    ret = true;
                    return;
                }

                // if we are here, it means the tolerance was just exceeded.
                _drag_origin = _position;
                transferred = grabbed(event);
                // _drag_initiated might change during the above virtual call
                _drag_initiated = true;
            }

            if (!transferred) {
                // dragging in progress
                auto new_pos = _desktop->w2d(event.pos) + pointer_offset;
                // the new position is passed by reference and can be changed in the handlers.
                dragged(new_pos, event);
                move(new_pos);
                _updateDragTip(event); // update dragging tip after moving to new position

                _desktop->getCanvas()->enable_autoscroll();
                _desktop->set_coordinate_status(_position);
                tool->snap_delay_handler(nullptr, this, event, Tools::DelayedSnapEvent::CONTROL_POINT_HANDLER);
            }
            ret = true;
        }
    },

    [&] (ButtonReleaseEvent const &event) {
        if (_event_grab && event.button == 1) {
            // If we have any pending snap event, then invoke it now!
            // (This is needed because we might not have snapped on the latest GDK_MOTION_NOTIFY event
            // if the mouse speed was too high. This is inherent to the snap-delay mechanism.
            // We must snap at some point in time though, and this is our last chance)
            // PS: For other contexts this is handled already in start_item_handler or start_root_handler
            // if (_desktop && _desktop->getTool() && _desktop->getTool()->_delayed_snap_event) {
            tool->process_delayed_snap_event();

            _canvas_item_ctrl->ungrab();
            _setMouseover(this, event.modifiers);
            _event_grab = false;
            grab_owner = nullptr;

            if (_drag_initiated) {
                // it is the end of a drag
                _drag_initiated = false;
                ungrabbed(&event);
                ret = true;
            } else {
                // it is the end of a click
                if (next_release_doubleclick) {
                    _double_clicked = true;
                    ret = doubleclicked(event);
                } else {
                    ret = clicked(event);
                }
            }
        }
    },

    [&] (EnterEvent const &event) {
        _setMouseover(this, event.modifiers);
        return true;
    },

    [&] (LeaveEvent const &event) {
        _clearMouseover();
        return true;
    },

    // update tips on modifier state change
    // TODO add ESC keybinding as drag cancel
    [&] (KeyPressEvent const &event) {
        switch (Tools::get_latin_keyval(event)) {
        case GDK_KEY_Escape: {
            // ignore Escape if this is not a drag
            if (!_drag_initiated) break;

            // temporarily disable snapping - we might snap to a different place than we were initially
            tool->discard_delayed_snap_event();
            auto &snapprefs = _desktop->getNamedView()->snap_manager.snapprefs;
            bool snap_save = snapprefs.getSnapEnabledGlobally();
            snapprefs.setSnapEnabledGlobally(false);

            auto new_pos = _drag_origin;

            // make a fake event for dragging
            // ASSUMPTION: dragging a point without modifiers will never prevent us from moving it
            //             to its original position
            auto fake = MotionEvent();
            fake.pos = _drag_event_origin;
            fake.modifiers = 0; // unconstrained drag
            fake.time = event.time;
            fake.control_point_synthesized = true;
            dragged(new_pos, fake);

            _canvas_item_ctrl->ungrab();
            _clearMouseover(); // this will also reset state to normal
            _event_grab = false;
            grab_owner = nullptr;
            _drag_initiated = false;

            ungrabbed(nullptr); // ungrabbed handlers can handle a NULL event
            snapprefs.setSnapEnabledGlobally(snap_save);
            ret = true;
            return;
        }
        case GDK_KEY_Tab: {
            // Downcast from ControlPoint to TransformHandle, if possible
            // This is an ugly hack; we should have the transform handle intercept the keystrokes itself
            if (auto th = dynamic_cast<TransformHandle*>(this)) {
                th->getNextClosestPoint(false);
                ret = true;
                return;
            }
            break;
        }
        case GDK_KEY_ISO_Left_Tab: {
            // Downcast from ControlPoint to TransformHandle, if possible
            // This is an ugly hack; we should have the transform handle intercept the keystrokes itself
            if (auto th = dynamic_cast<TransformHandle*>(this)) {
                th->getNextClosestPoint(true);
                ret = true;
                return;
            }
            break;
        }
        default:
            break;
        }

        ret = key_event_handler(event);
    },

    [&] (KeyReleaseEvent const &event) {
        ret = key_event_handler(event);
    },

    [&] (CanvasEvent const &event) {}
    );

    // do not propagate events during grab - it might cause problems
    return ret || _event_grab;
}

void ControlPoint::_setMouseover(ControlPoint *p, unsigned state)
{
    bool visible = p->visible();
    if (visible) { // invisible points shouldn't get mouseovered
        p->_setState(STATE_MOUSEOVER);
    }
    p->_updateTip(state);

    if (visible && mouseovered_point != p) {
        mouseovered_point = p;
        signal_mouseover_change.emit(mouseovered_point);
    }
}

bool ControlPoint::_updateTip(unsigned state)
{
    // TODO: refactor _getTip implementations to be more dynamic, if the user has changed the
    //  modifier settings. For example, dynamically creating the tooltip based off the strings
    //  from the Modifier class and which of several modifiers are pressed (rather than the current
    //  approach which checks each in order, and tries to manually handle the case where some
    //  modifiers are pressed together -- see Handle::_getTip for an example of the trickiness).
    Glib::ustring tip = _getTip(state);
    if (!tip.empty()) {
        _desktop->getTool()->defaultMessageContext()->set(Inkscape::NORMAL_MESSAGE,
            tip.data());
        return true;
    } else {
        _desktop->getTool()->defaultMessageContext()->clear();
        return false;
    }
}

bool ControlPoint::_updateDragTip(MotionEvent const &event)
{
    if (!_hasDragTips()) {
        return false;
    }
    Glib::ustring tip = _getDragTip(event);
    if (!tip.empty()) {
        _desktop->getTool()->defaultMessageContext()->set(Inkscape::NORMAL_MESSAGE,
            tip.data());
        return true;
    } else {
        _desktop->getTool()->defaultMessageContext()->clear();
        return false;
    }
}

void ControlPoint::_clearMouseover()
{
    if (mouseovered_point) {
        mouseovered_point->_desktop->getTool()->defaultMessageContext()->clear();
        mouseovered_point->_setState(STATE_NORMAL);
        mouseovered_point = nullptr;
        signal_mouseover_change.emit(mouseovered_point);
    }
}

void ControlPoint::transferGrab(ControlPoint *prev_point, MotionEvent const &event)
{
    if (!_event_grab) return;

    grab_owner = this;
    grabbed(event);
    prev_point->_canvas_item_ctrl->ungrab();
    _canvas_item_ctrl->grab(grab_event_mask); // cursor is null

    _drag_initiated = true;

    prev_point->_setState(STATE_NORMAL);
    _setMouseover(this, event.modifiers);
}

void ControlPoint::_setState(State state)
{
    _canvas_item_ctrl->set_normal(_selected_appearance);

    switch(state) {
        case STATE_NORMAL:
            break;
        case STATE_MOUSEOVER:
            _canvas_item_ctrl->set_hover();
            break;
        case STATE_CLICKED:
            _canvas_item_ctrl->set_click();
            break;
    };
    _state = state;
}

void ControlPoint::set_selected_appearance(bool selected) {
    if (_selected_appearance == selected) return;

    _selected_appearance = selected;
    _canvas_item_ctrl->set_selected(selected);
}

// TODO: RENAME
void ControlPoint::_handleControlStyling()
{
    _canvas_item_ctrl->set_size_default();
}

bool ControlPoint::_is_drag_cancelled(MotionEvent const &event)
{
    return event.control_point_synthesized;
}

// dummy implementations for handlers

bool ControlPoint::grabbed(MotionEvent const &)
{
    return false;
}

void ControlPoint::dragged(Geom::Point &/*new_pos*/, MotionEvent const &/*event*/)
{
}

void ControlPoint::ungrabbed(ButtonReleaseEvent const */*event*/)
{
}

bool ControlPoint::clicked(ButtonReleaseEvent const &/*event*/)
{
    return false;
}

bool ControlPoint::doubleclicked(ButtonReleaseEvent const &/*event*/)
{
    return false;
}

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
