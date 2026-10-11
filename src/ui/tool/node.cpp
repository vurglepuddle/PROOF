// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2009 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "node.h"

#include <2geom/path-sink.h>
#include <glib/gi18n.h>

#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-curve.h"
#include "object/sp-namedview.h"
#include "ui/modifiers.h"
#include "ui/tool/control-point-selection.h"
#include "ui/tool/path-manipulator.h"
#include "ui/tools/node-tool.h"
#include "ui/widget/events/canvas-event.h"
#include "util/join.h"
#include "util/units.h"

namespace {

Inkscape::CanvasItemCtrlType nodeTypeToCtrlType(Inkscape::UI::NodeType type)
{
    Inkscape::CanvasItemCtrlType result = Inkscape::CANVAS_ITEM_CTRL_TYPE_NODE_CUSP;
    switch (type) {
        case Inkscape::UI::NODE_SMOOTH:
            result = Inkscape::CANVAS_ITEM_CTRL_TYPE_NODE_SMOOTH;
            break;
        case Inkscape::UI::NODE_AUTO:
            result = Inkscape::CANVAS_ITEM_CTRL_TYPE_NODE_AUTO;
            break;
        case Inkscape::UI::NODE_SYMMETRIC:
            result = Inkscape::CANVAS_ITEM_CTRL_TYPE_NODE_SYMMETRICAL;
            break;
        case Inkscape::UI::NODE_CUSP:
        default:
            result = Inkscape::CANVAS_ITEM_CTRL_TYPE_NODE_CUSP;
            break;
    }
    return result;
}

/**
 * @brief provides means to estimate float point rounding error due to serialization to svg
 *
 *  Keeps cached value up to date with preferences option `/options/svgoutput/numericprecision`
 *  to avoid costly direct reads
 * */
class SvgOutputPrecisionWatcher : public Inkscape::Preferences::Observer
{
public:
    /// Returns absolute \a value`s rounding serialization error based on current preferences settings
    static double error_of(double value) { return value * instance().rel_error; }

    void notify(const Inkscape::Preferences::Entry &new_val) override
    {
        int digits = new_val.getIntLimited(10, 1, 16);
        set_numeric_precision(digits);
    }

private:
    SvgOutputPrecisionWatcher()
        : Observer("/options/svgoutput/numericprecision")
        , rel_error(1)
    {
        Inkscape::Preferences::get()->addObserver(*this);
        int digits = Inkscape::Preferences::get()->getIntLimited("/options/svgoutput/numericprecision", 10, 1, 16);
        set_numeric_precision(digits);
    }

    ~SvgOutputPrecisionWatcher() override { Inkscape::Preferences::get()->removeObserver(*this); }
    /// Update cached value of relative error with number of significant digits
    void set_numeric_precision(int digits)
    {
        double relative_error = 0.5; // the error is half of last digit
        while (digits > 0) {
            relative_error /= 10;
            digits--;
        }
        rel_error = relative_error;
    }

    static SvgOutputPrecisionWatcher &instance()
    {
        static SvgOutputPrecisionWatcher _instance;
        return _instance;
    }

    std::atomic<double> rel_error; /// Cached relative error
};

/// Returns absolute error of \a point as if serialized to svg with current preferences
double serializing_error_of(const Geom::Point &point)
{
    return SvgOutputPrecisionWatcher::error_of(point.length());
}

/**
 * @brief Returns true if three points are collinear within current serializing precision
 *
 * The algorithm of collinearity check is explicitly used to calculate the check error.
 *
 * This function can be sufficiently reduced or even removed completely if `Geom::are_collinear`
 * would declare it's check algorithm as part of the public API.
 *
 * */
bool are_collinear_within_serializing_error(const Geom::Point &A, const Geom::Point &B, const Geom::Point &C)
{
    const double tolerance_factor = 10; // to account other factors which increase uncertainty
    const double tolerance_A = serializing_error_of(A) * tolerance_factor;
    const double tolerance_B = serializing_error_of(B) * tolerance_factor;
    const double tolerance_C = serializing_error_of(C) * tolerance_factor;
    const double CB_length = (B - C).length();
    const double AB_length = (B - A).length();
    Geom::Point C_reflect_scaled = B + (B - C) / CB_length * AB_length;
    double tolerance_C_reflect_scaled = tolerance_B + (tolerance_B + tolerance_C) *
                                                          (1 + (tolerance_A + tolerance_B) / AB_length) *
                                                          (1 + (tolerance_C + tolerance_B) / CB_length);
    return Geom::are_near(C_reflect_scaled, A, tolerance_C_reflect_scaled + tolerance_A);
}

} // namespace

namespace Inkscape {
namespace UI {

const double BSPLINE_TOL = 0.001;
const double NO_POWER = 0.0;
const double DEFAULT_START_POWER = 1.0 / 3.0;

std::ostream &operator<<(std::ostream &out, NodeType type)
{
    switch (type) {
        case NODE_CUSP:
            out << 'c';
            break;
        case NODE_SMOOTH:
            out << 's';
            break;
        case NODE_AUTO:
            out << 'a';
            break;
        case NODE_SYMMETRIC:
            out << 'z';
            break;
        default:
            out << 'b';
            break;
    }
    return out;
}

/** Computes an unit vector of the direction from first to second control point */
static Geom::Point direction(Geom::Point const &first, Geom::Point const &second)
{
    return Geom::unit_vector(second - first);
}

Geom::Point Handle::_saved_other_pos(0, 0);
Geom::Point Handle::_saved_dir(0, 0);

double Handle::_saved_length = 0.0;

bool Handle::_drag_out = false;

Handle::Handle(NodeSharedData const &data, Geom::Point const &initial_pos, Node *parent)
    : ControlPoint(data.desktop, initial_pos, SP_ANCHOR_CENTER, Inkscape::CANVAS_ITEM_CTRL_TYPE_NODE_HANDLE,
                   data.handle_group)
    , _handle_line(make_canvasitem<CanvasItemCurve>(data.handle_line_group))
    , _parent(parent)
    , _degenerate(true)
{
    setVisible(false);
}

Handle::~Handle() = default;

void Handle::setVisible(bool v)
{
    ControlPoint::setVisible(v);
    _handle_line->set_visible(v);
    set_selected_appearance(_parent->selected());
}

void Handle::_update_bspline_handles()
{
    // move the handle and its opposite the same proportion
    if (_pm()._isBSpline()) {
        setPosition(_pm()._bsplineHandleReposition(this, false));
        double bspline_weight = _pm()._bsplineHandlePosition(this, false);
        other()->setPosition(_pm()._bsplineHandleReposition(other(), bspline_weight));
        _pm().update();
    }
}

void Handle::move(Geom::Point const &new_pos)
{
    Handle *other = this->other();
    Node *node_towards = _parent->nodeToward(this); // node in direction of this handle
    Node *node_away = _parent->nodeAwayFrom(this);  // node in the opposite direction
    Handle *towards = node_towards ? node_towards->handleAwayFrom(_parent) : nullptr;
    Handle *towards_second = node_towards ? node_towards->handleToward(_parent) : nullptr;
    if (Geom::are_near(new_pos, _parent->position())) {
        // The handle becomes degenerate.
        // Adjust node type as necessary.
        if (other->isDegenerate()) {
            // If both handles become degenerate, convert to parent cusp node
            _parent->setType(NODE_CUSP, false);
        } else {
            // Only 1 handle becomes degenerate
            switch (_parent->type()) {
                case NODE_AUTO:
                case NODE_SYMMETRIC:
                    _parent->setType(NODE_SMOOTH, false);
                    break;
                default:
                    // do nothing for other node types
                    break;
            }
        }
        // If the segment between the handle and the node in its direction becomes linear,
        // and there are smooth nodes at its ends, make their handles collinear with the segment.
        if (towards && towards_second->isDegenerate()) {
            if (node_towards->type() == NODE_SMOOTH) {
                towards->setDirection(_parent->position(), node_towards->position());
            }
            if (_parent->type() == NODE_SMOOTH) {
                other->setDirection(node_towards->position(), _parent->position());
            }
        }
        setPosition(new_pos);

        // move the handle and its opposite the same proportion
        _update_bspline_handles();
        return;
    }

    if (_parent->type() == NODE_SMOOTH && Node::_is_line_segment(_parent, node_away)) {
        // restrict movement to the line joining the nodes
        Geom::Point direction = _parent->position() - node_away->position();
        Geom::Point delta = new_pos - _parent->position();
        // project the relative position on the direction line
        Geom::Coord direction_length = Geom::L2sq(direction);
        Geom::Point new_delta;
        if (direction_length == 0) {
            // joining line has zero length - any direction is okay, prevent division by zero
            new_delta = delta;
        } else {
            new_delta = (Geom::dot(delta, direction) / direction_length) * direction;
        }
        setRelativePos(new_delta);

        // move the handle and its opposite the same proportion
        _update_bspline_handles();

        return;
    }

    switch (_parent->type()) {
        case NODE_AUTO:
            _parent->setType(NODE_SMOOTH, false);
            // fall through - auto nodes degrade into smooth nodes
        case NODE_SMOOTH: {
            // for smooth nodes, we need to rotate the opposite handle
            // so that it's collinear with the dragged one, while conserving length.
            other->setDirection(new_pos, _parent->position());
        } break;
        case NODE_SYMMETRIC:
            // for symmetric nodes, place the other handle on the opposite side
            other->setRelativePos(-(new_pos - _parent->position()));
            break;
        default:
            break;
    }
    setPosition(new_pos);

    // move the handle and its opposite the same proportion
    _update_bspline_handles();
    Inkscape::UI::Tools::sp_update_helperpath(_desktop);
}

void Handle::setPosition(Geom::Point const &p)
{
    ControlPoint::setPosition(p);
    _handle_line->set_coords(_parent->position(), position());

    // update degeneration info and visibility
    if (Geom::are_near(position(), _parent->position()))
        _degenerate = true;
    else
        _degenerate = false;

    if (_parent->_handles_shown && _parent->visible() && !_degenerate) {
        setVisible(true);
    } else {
        setVisible(false);
    }
}

void Handle::setLength(double len)
{
    if (isDegenerate())
        return;
    Geom::Point dir = Geom::unit_vector(relativePos());
    setRelativePos(dir * len);
}

void Handle::retract()
{
    move(_parent->position());
}

void Handle::setDirection(Geom::Point const &from, Geom::Point const &to)
{
    setDirection(to - from);
}

void Handle::setDirection(Geom::Point const &dir)
{
    Geom::Point unitdir = Geom::unit_vector(dir);
    setRelativePos(unitdir * length());
}

/**
 * See also: Node::node_type_to_localized_string(NodeType type)
 */
char const *Handle::handle_type_to_localized_string(NodeType type)
{
    switch (type) {
        case NODE_CUSP:
            return _("Corner node handle");
        case NODE_SMOOTH:
            return _("Smooth node handle");
        case NODE_SYMMETRIC:
            return _("Symmetric node handle");
        case NODE_AUTO:
            return _("Auto-smooth node handle");
        default:
            return "";
    }
}

bool Handle::_eventHandler(Tools::ToolBase *event_context, CanvasEvent const &event)
{
    bool ret = false;
    inspect_event(
        event,
        [&](KeyPressEvent const &event) {
            switch (event.keyval) {
                case GDK_KEY_s:
                case GDK_KEY_S:
                    /* if Shift+S is pressed while hovering over a cusp node handle,
                       hold the handle in place; otherwise, process normally.
                       this handle is guaranteed not to be degenerate. */

                    if (mod_shift_only(event) && _parent->_type == NODE_CUSP) {
                        // make opposite handle collinear,
                        // but preserve length, unless degenerate
                        if (other()->isDegenerate())
                            other()->setRelativePos(-relativePos());
                        else
                            other()->setDirection(-relativePos());
                        _parent->setType(NODE_SMOOTH, false);

                        // update display
                        _parent->_pm().update();

                        // update undo history
                        _parent->_pm()._commit(RC_("Undo", "Change node type"));

                        ret = true;
                    }
                    break;

                case GDK_KEY_y:
                case GDK_KEY_Y:

                    /* if Shift+Y is pressed while hovering over a cusp, smooth, or auto node handle,
                       hold the handle in place; otherwise, process normally.
                       this handle is guaranteed not to be degenerate. */

                    if (mod_shift_only(event) &&
                        (_parent->_type == NODE_CUSP || _parent->_type == NODE_SMOOTH || _parent->_type == NODE_AUTO)) {
                        // make opposite handle collinear, and of equal length
                        other()->setRelativePos(-relativePos());
                        _parent->setType(NODE_SYMMETRIC, false);

                        // update display
                        _parent->_pm().update();

                        // update undo history
                        _parent->_pm()._commit(RC_("Undo", "Change node type"));

                        ret = true;
                    }
                    break;
                default:
                    break;
            }
        },

        [&](ButtonPressEvent const &event) {
            if (event.num_press != 2) {
                return;
            }

            // double-click event to set the handles of a node
            // to the position specified by DEFAULT_START_POWER
            handle_2button_press();
        },

        [&](CanvasEvent const &event) {});

    return ControlPoint::_eventHandler(event_context, event);
}

// this function moves the handle and its opposite to the position specified by DEFAULT_START_POWER
void Handle::handle_2button_press()
{
    if (_pm()._isBSpline()) {
        setPosition(_pm()._bsplineHandleReposition(this, DEFAULT_START_POWER));
        this->other()->setPosition(_pm()._bsplineHandleReposition(this->other(), DEFAULT_START_POWER));
        _pm().update();
    }
}

bool Handle::grabbed(MotionEvent const &)
{
    _saved_other_pos = other()->position();
    _saved_length = _drag_out ? 0 : length();
    _saved_dir = Geom::unit_vector(_last_drag_origin() - _parent->position());
    _pm()._handleGrabbed();
    return false;
}

void Handle::dragged(Geom::Point &new_pos, MotionEvent const &event)
{
    auto const confine = Modifiers::Modifier::get(Modifiers::Type::MOVE_CONFINE)->active(event.modifiers);
    auto const no_snap = Modifiers::Modifier::get(Modifiers::Type::MOVE_NO_SNAPPING)->active(event.modifiers);
    auto const link_handles = Modifiers::Modifier::get(Modifiers::Type::NODE_LINK_HANDLES)->active(event.modifiers);
    auto const preserve_length = Modifiers::Modifier::get(Modifiers::Type::NODE_PRESERVE_LENGTH)->active(event.modifiers);

    Geom::Point parent_pos = _parent->position();
    Geom::Point origin = _last_drag_origin();
    SnapManager &sm = _desktop->getNamedView()->snap_manager;
    bool snap = no_snap ? false : sm.someSnapperMightSnap();
    std::optional<Inkscape::Snapper::SnapConstraint> ctrl_constraint;

    if (preserve_length) {
        // with Alt, preserve length of the handle
        new_pos = parent_pos + Geom::unit_vector(new_pos - parent_pos) * _saved_length;
        snap = false;
        _saved_dir = Geom::unit_vector(relativePos());
    } else {
        // with nothing pressed we update the lengths
        _saved_length = _drag_out ? 0 : length();
        _saved_dir = Geom::unit_vector(relativePos());
    }

    if (_parent->type() != NODE_CUSP && link_handles && !preserve_length) {
        // if we hold Shift, and node is not cusp, link the two handles
        other()->setRelativePos(-relativePos());
    }

    // with Ctrl, constrain to M_PI/rotationsnapsperpi increments from vertical
    // and the original position.
    if (confine) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        double snaps = 2 * prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);

        // note: if snapping to the original position is only desired in the original
        // direction of the handle, use Geom::Ray instead of Geom::Line
        Geom::Line original_line(parent_pos, origin);
        Geom::Line perp_line(parent_pos, parent_pos + Geom::rot90(origin - parent_pos));
        Geom::Point snap_pos =
            parent_pos + Geom::constrain_angle(Geom::Point(0, 0), new_pos - parent_pos, snaps, Geom::Point(1, 0));
        Geom::Point orig_pos = original_line.pointAt(original_line.nearestTime(new_pos));
        Geom::Point perp_pos = perp_line.pointAt(perp_line.nearestTime(new_pos));

        Geom::Point result = snap_pos;
        ctrl_constraint = Inkscape::Snapper::SnapConstraint(parent_pos, parent_pos - snap_pos);
        if (Geom::distance(orig_pos, new_pos) < Geom::distance(result, new_pos)) {
            result = orig_pos;
            ctrl_constraint = Inkscape::Snapper::SnapConstraint(parent_pos, parent_pos - orig_pos);
        }
        if (Geom::distance(perp_pos, new_pos) < Geom::distance(result, new_pos)) {
            result = perp_pos;
            ctrl_constraint = Inkscape::Snapper::SnapConstraint(parent_pos, parent_pos - perp_pos);
        }
        new_pos = result;
        // move the handle and its opposite in X fixed positions depending on parameter "steps with control"
        // by default in live BSpline
        if (_pm()._isBSpline()) {
            setPosition(new_pos);
            int steps = _pm()._bsplineGetSteps();
            new_pos =
                _pm()._bsplineHandleReposition(this, ceilf(_pm()._bsplineHandlePosition(this, false) * steps) / steps);
        }
    }

    std::vector<Inkscape::SnapCandidatePoint> unselected;
    // If the snapping is active and we're not working with a B-spline
    if (snap && !_pm()._isBSpline()) {
        // We will only snap this handle to stationary path segments; some path segments may move as we move the
        // handle; those path segments are connected to the parent node of this handle.
        ControlPointSelection::Set &nodes = _parent->_selection.allPoints();
        for (auto node : nodes) {
            Node *n = static_cast<Node *>(node);
            if (_parent != n) { // We're adding all nodes in the path, except the parent node of this handle
                unselected.push_back(n->snapCandidatePoint());
            }
        }
        sm.setupIgnoreSelection(_desktop, true, &unselected);

        Node *node_away = _parent->nodeAwayFrom(this);
        if (_parent->type() == NODE_SMOOTH && Node::_is_line_segment(_parent, node_away)) {
            Inkscape::Snapper::SnapConstraint cl(_parent->position(), _parent->position() - node_away->position());
            Inkscape::SnappedPoint p;
            p = sm.constrainedSnap(Inkscape::SnapCandidatePoint(new_pos, SNAPSOURCE_NODE_HANDLE), cl);
            new_pos = p.getPoint();
        } else if (ctrl_constraint) {
            // NOTE: this is subtly wrong.
            // We should get all possible constraints and snap along them using
            // multipleConstrainedSnaps, instead of first snapping to angle and then to objects
            Inkscape::SnappedPoint p;
            p = sm.constrainedSnap(Inkscape::SnapCandidatePoint(new_pos, SNAPSOURCE_NODE_HANDLE), *ctrl_constraint);
            new_pos = p.getPoint();
        } else {
            sm.freeSnapReturnByRef(new_pos, SNAPSOURCE_NODE_HANDLE);
        }
        sm.unSetup();
    }

    if (_parent->type() == NODE_CUSP && !_drag_out) {
        if (link_handles) { // rotate the other handle as well
            Geom::Point other_relpos = _saved_other_pos - parent_pos;
            other_relpos *= Geom::Rotate(Geom::angle_between(origin - parent_pos, new_pos - parent_pos));
            other()->setRelativePos(other_relpos);
        } else {
            // restore the position
            other()->setPosition(_saved_other_pos);
        }
    }
    // if it is BSpline, but SHIFT or CONTROL are not pressed, fix it in the original position
    if (_pm()._isBSpline() && !no_snap && !confine) {
        new_pos = _last_drag_origin();
    }
    _pm().update();
}

void Handle::ungrabbed(ButtonReleaseEvent const *event)
{
    // hide the handle if it's less than dragtolerance away from the node
    // however, never do this for cancelled drag / broken grab
    // TODO is this actually a good idea?
    if (event) {
        auto prefs = Preferences::get();
        int drag_tolerance = prefs->getIntLimited("/options/dragtolerance/value", 0, 0, 100);

        auto const dist = _desktop->d2w(_parent->position()) - _desktop->d2w(position());
        if (dist.length() <= drag_tolerance) {
            move(_parent->position());
        }
    }

    // HACK: If the handle was dragged out, call parent's ungrabbed handler,
    // so that transform handles reappear
    if (_drag_out) {
        _parent->ungrabbed(event);
    }
    _drag_out = false;
    Tools::sp_update_helperpath(_desktop);
    _pm()._handleUngrabbed();
}

bool Handle::clicked(ButtonReleaseEvent const &event)
{
    if (_pm()._nodeClicked(this->parent(), event, true)) {
        return true;
    }
    _pm()._handleClicked(this, event);
    return true;
}

Handle const *Handle::other() const
{
    return const_cast<Handle *>(this)->other();
}

Handle *Handle::other()
{
    if (this == &_parent->_front) {
        return &_parent->_back;
    } else {
        return &_parent->_front;
    }
}

static double snap_increment_degrees()
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);
    return 180.0 / snaps;
}

Glib::ustring Handle::_getTip(unsigned state) const
{
    /* a trick to mark as BSpline if the node has no strength;
       we are going to use it later to show the appropriate messages.
       we cannot do it in any different way because the function is constant. */
    Handle *h = const_cast<Handle *>(this);
    bool isBSpline = _pm()._isBSpline();
    bool can_shift_rotate = _parent->type() == NODE_CUSP && !other()->isDegenerate();
    Glib::ustring s = C_("Status line hint",
                         "node control handle"); // not expected

    auto const mod_confine = Modifiers::Modifier::get(Modifiers::Type::MOVE_CONFINE);
    auto const mod_bspline_handles = Modifiers::Modifier::get(Modifiers::Type::NODE_BSPLINE_HANDLES);
    auto const mod_link_handles = Modifiers::Modifier::get(Modifiers::Type::NODE_LINK_HANDLES);
    auto const mod_preserve_length = Modifiers::Modifier::get(Modifiers::Type::NODE_PRESERVE_LENGTH);

    if (mod_preserve_length->active(state) && !isBSpline) {
        if (mod_confine->active(state)) {
            if (mod_link_handles->active(state) && can_shift_rotate) {
                auto const combined = Modifiers::generate_label(mod_confine->get_and_mask() |
                                                                mod_link_handles->get_and_mask() |
                                                                mod_preserve_length->get_and_mask());
                s = format_tip(C_("Status line hint", "<b>%s</b>: "
                                                      "preserve length and snap rotation angle to %g° increments, "
                                                      "and rotate both handles"),
                               combined.c_str(), snap_increment_degrees());
            } else {
                auto const combined = Modifiers::generate_label(mod_confine->get_and_mask() |
                                                                mod_preserve_length->get_and_mask());
                s = format_tip(C_("Status line hint", "<b>%s</b>: "
                                                      "preserve length and snap rotation angle to %g° increments"),
                               combined.c_str(), snap_increment_degrees());
            }
        } else {
            if (mod_link_handles->active(state) && can_shift_rotate) {
                auto const combined = Modifiers::generate_label(mod_link_handles->get_and_mask() |
                                                                mod_preserve_length->get_and_mask());
                s = format_tip(C_("Path handle tip", "<b>%s</b>: preserve handle length and rotate both handles"),
                               combined.c_str());
            } else {
                s = format_tip(C_("Path handle tip", "<b>%s</b>: preserve handle length while dragging"),
                               mod_preserve_length->get_label().c_str());
            }
        }
    } else {
        if (mod_confine->active(state)) {
            if (mod_link_handles->active(state) && can_shift_rotate && !isBSpline) {
                auto const combined = Modifiers::generate_label(mod_confine->get_and_mask() |
                                                                mod_link_handles->get_and_mask());
                s = format_tip(C_("Path handle tip", "<b>%s</b>: "
                                                     "snap rotation angle to %g° increments, and rotate both handles"),
                               combined.c_str(), snap_increment_degrees());
            } else if (isBSpline) {
                s = format_tip(C_("Path handle tip",
                    "<b>%s</b>: Snap handle to steps defined in BSpline Live Path Effect"),
                    mod_confine->get_label().c_str());
            } else {
                s = format_tip(C_("Path handle tip", 
                    "<b>%s</b>: snap rotation angle to %g° increments, click to retract"),
                    mod_confine->get_label().c_str(), snap_increment_degrees());
            }
        } else if (mod_link_handles->active(state) && can_shift_rotate && !isBSpline) {
            s = Glib::ustring::compose(C_("Path handle tip",
                "<b>%1</b>: rotate both handles by the same angle"), mod_link_handles->get_label());
        } else if (mod_bspline_handles->active(state) && isBSpline) {
            s = Glib::ustring::compose(C_("Path handle tip", "<b>%1</b>: move handle"),
                mod_bspline_handles->get_label());
        } else {
            char const *handletype = handle_type_to_localized_string(_parent->_type);

            std::set<Glib::ustring> labels;
            labels.insert(mod_confine->get_label());
            if (isBSpline) {
                labels.insert(mod_bspline_handles->get_label());
            } else {
                if (can_shift_rotate) {
                    labels.insert(mod_link_handles->get_label());
                }
                labels.insert(mod_preserve_length->get_label());
            }
            auto const more = Inkscape::Util::join_with_separator(labels);

            if (isBSpline) {
                double power = _pm()._bsplineHandlePosition(h);
                s = format_tip(C_("Status line hint", "<b>BSpline node handle</b> (%.3g power): "
                                                      "Shift-drag to move, "
                                                      "double-click to reset. "
                                                      "(more: %s)"),
                               power, more.c_str());
            } else if (_parent->type() == NODE_CUSP) {
                s = format_tip(C_("Status line hint", "<b>%s</b>: "
                                                      "drag to shape the path"
                                                      ", "
                                                      "hover to lock"
                                                      ", "
                                                      "Shift+S to make smooth"
                                                      ", "
                                                      "Shift+Y to make symmetric"
                                                      ". "
                                                      "(more: %s)"),
                               handletype, more.c_str());
            } else if (_parent->type() == NODE_SMOOTH) {
                s = format_tip(C_("Status line hint", "<b>%s</b>: "
                                                      "drag to shape the path"
                                                      ", "
                                                      "hover to lock"
                                                      ", "
                                                      "Shift+Y to make symmetric"
                                                      ". "
                                                      "(more: %s)"),
                               handletype, more.c_str());
            } else if (_parent->type() == NODE_AUTO) {
                s = format_tip(C_("Status line hint", "<b>%s</b>: "
                                                      "drag to make smooth, "
                                                      "hover to lock"
                                                      ", "
                                                      "Shift+Y to make symmetric"
                                                      ". "
                                                      "(more: %s)"),
                               handletype, more.c_str());
            } else if (_parent->type() == NODE_SYMMETRIC) {
                s = format_tip(C_("Status line hint", "<b>%s</b>: "
                                                      "drag to shape the path"
                                                      ". "
                                                      "(more: %s)"),
                               handletype, more.c_str());
            } else {
                s = C_("Status line hint",
                       "<b>unknown node handle</b>"); // not expected
            }
        }
    }

    return (s);
}

Glib::ustring Handle::_getDragTip(MotionEvent const & /*event*/) const
{
    Geom::Point dist = position() - _last_drag_origin();
    // report angle in mathematical convention
    double angle = Geom::angle_between(Geom::Point(-1, 0), position() - _parent->position());
    angle += M_PI; // angle is (-M_PI...M_PI] - offset by +pi and scale to 0...360
    angle *= 360.0 / (2 * M_PI);

    Inkscape::Util::Quantity x_q = Inkscape::Util::Quantity(dist[Geom::X], "px");
    Inkscape::Util::Quantity y_q = Inkscape::Util::Quantity(dist[Geom::Y], "px");
    Inkscape::Util::Quantity len_q = Inkscape::Util::Quantity(length(), "px");
    Glib::ustring x = x_q.string(_desktop->getNamedView()->display_units);
    Glib::ustring y = y_q.string(_desktop->getNamedView()->display_units);
    Glib::ustring len = len_q.string(_desktop->getNamedView()->display_units);
    Glib::ustring ret = format_tip(C_("Status line hint", "Move handle by %s, %s; angle %.2f°, length %s"), x.c_str(),
                                   y.c_str(), angle, len.c_str());
    return ret;
}

Node::Node(NodeSharedData const &data, Geom::Point const &initial_pos)
    : SelectableControlPoint(data.desktop, initial_pos, SP_ANCHOR_CENTER, Inkscape::CANVAS_ITEM_CTRL_TYPE_NODE_CUSP,
                             *data.selection, data.node_group)
    , _front(data, initial_pos, this)
    , _back(data, initial_pos, this)
    , _type(NODE_CUSP)
    , _handles_shown(false)
{
    _canvas_item_ctrl->set_name("CanvasItemCtrl:Node");
    // NOTE we do not set type here, because the handles are still degenerate
}

Node const *Node::_next() const
{
    return const_cast<Node *>(this)->_next();
}

// NOTE: not using iterators won't make this much quicker because iterators can be 100% inlined.
Node *Node::_next()
{
    NodeList::iterator n = NodeList::get_iterator(this).next();
    if (n) {
        return n.ptr();
    } else {
        return nullptr;
    }
}

Node const *Node::_prev() const
{
    return const_cast<Node *>(this)->_prev();
}

Node *Node::_prev()
{
    NodeList::iterator p = NodeList::get_iterator(this).prev();
    if (p) {
        return p.ptr();
    } else {
        return nullptr;
    }
}

void Node::move(Geom::Point const &new_pos)
{
    // move handles when the node moves.
    Geom::Point delta = new_pos - position();

    // save the previous nodes strength to apply it again once the node is moved
    double nodeWeight = NO_POWER;
    double nextNodeWeight = NO_POWER;
    double prevNodeWeight = NO_POWER;
    Node *n = this;
    Node *nextNode = n->nodeToward(n->front());
    Node *prevNode = n->nodeToward(n->back());
    nodeWeight = fmax(_pm()._bsplineHandlePosition(n->front(), false), _pm()._bsplineHandlePosition(n->back(), false));
    if (prevNode) {
        prevNodeWeight = _pm()._bsplineHandlePosition(prevNode->front());
    }
    if (nextNode) {
        nextNodeWeight = _pm()._bsplineHandlePosition(nextNode->back());
    }

    // Save original position for post-processing
    _unfixed_pos = std::optional<Geom::Point>(position());

    setPosition(new_pos);
    _front.setPosition(_front.position() + delta);
    _back.setPosition(_back.position() + delta);

    // move the affected handles. First the node ones, later the adjoining ones.
    if (_pm()._isBSpline()) {
        _front.setPosition(_pm()._bsplineHandleReposition(this->front(), nodeWeight));
        _back.setPosition(_pm()._bsplineHandleReposition(this->back(), nodeWeight));
        if (prevNode) {
            prevNode->front()->setPosition(_pm()._bsplineHandleReposition(prevNode->front(), prevNodeWeight));
        }
        if (nextNode) {
            nextNode->back()->setPosition(_pm()._bsplineHandleReposition(nextNode->back(), nextNodeWeight));
        }
    }
    Inkscape::UI::Tools::sp_update_helperpath(_desktop);
}

void Node::transform(Geom::Affine const &m)
{
    // save the previous nodes strength to apply it again once the node is moved
    double nodeWeight = NO_POWER;
    double nextNodeWeight = NO_POWER;
    double prevNodeWeight = NO_POWER;
    Node *n = this;
    Node *nextNode = n->nodeToward(n->front());
    Node *prevNode = n->nodeToward(n->back());
    nodeWeight = _pm()._bsplineHandlePosition(n->front());
    if (prevNode) {
        prevNodeWeight = _pm()._bsplineHandlePosition(prevNode->front());
    }
    if (nextNode) {
        nextNodeWeight = _pm()._bsplineHandlePosition(nextNode->back());
    }

    // Save original position for post-processing
    _unfixed_pos = std::optional<Geom::Point>(position());

    setPosition(position() * m);
    _front.setPosition(_front.position() * m);
    _back.setPosition(_back.position() * m);

    // move the involved handles. First the node ones, later the adjoining ones.
    if (_pm()._isBSpline()) {
        _front.setPosition(_pm()._bsplineHandleReposition(this->front(), nodeWeight));
        _back.setPosition(_pm()._bsplineHandleReposition(this->back(), nodeWeight));
        if (prevNode) {
            prevNode->front()->setPosition(_pm()._bsplineHandleReposition(prevNode->front(), prevNodeWeight));
        }
        if (nextNode) {
            nextNode->back()->setPosition(_pm()._bsplineHandleReposition(nextNode->back(), nextNodeWeight));
        }
    }
}

Geom::Rect Node::bounds() const
{
    Geom::Rect b(position(), position());
    b.expandTo(_front.position());
    b.expandTo(_back.position());
    return b;
}

/**
 * Affine transforms keep handle invariants for smooth and symmetric nodes,
 * but smooth nodes at ends of linear segments and auto nodes need special treatment
 *
 * Call this function once you have finished called ::move or ::transform on ALL nodes
 * that are being transformed in that one operation to avoid problematic bugs.
 */
void Node::fixNeighbors()
{
    if (!_unfixed_pos)
        return;

    Geom::Point const new_pos = position();

    // This method restores handle invariants for neighboring nodes,
    // and invariants that are based on positions of those nodes for this one.

    // Fix auto handles
    if (_type == NODE_AUTO)
        _updateAutoHandles();
    if (*_unfixed_pos != new_pos) {
        if (_next() && _next()->_type == NODE_AUTO)
            _next()->_updateAutoHandles();
        if (_prev() && _prev()->_type == NODE_AUTO)
            _prev()->_updateAutoHandles();
    }

    /* Fix smooth handles at the ends of linear segments.
       Rotate the appropriate handle to be collinear with the segment.
       If there is a smooth node at the other end of the segment, rotate it too. */
    Handle *handle, *other_handle;
    Node *other;
    if (_is_line_segment(this, _next())) {
        handle = &_back;
        other = _next();
        other_handle = &_next()->_front;
    } else if (_is_line_segment(_prev(), this)) {
        handle = &_front;
        other = _prev();
        other_handle = &_prev()->_back;
    } else
        return;

    if (_type == NODE_SMOOTH && !handle->isDegenerate()) {
        handle->setDirection(other->position(), new_pos);
    }
    // also update the handle on the other end of the segment
    if (other->_type == NODE_SMOOTH && !other_handle->isDegenerate()) {
        other_handle->setDirection(new_pos, other->position());
    }

    _unfixed_pos.reset();
}

void Node::_updateAutoHandles()
{
    // Recompute the position of automatic handles. For endnodes, retract both handles.
    // (It's only possible to create an end auto node through the XML editor.)
    if (isEndNode()) {
        _front.retract();
        _back.retract();
        return;
    }

    // auto nodes automatically adjust their handles to give
    // an appearance of smoothness, no matter what their surroundings are.
    Geom::Point vec_next = _next()->position() - position();
    Geom::Point vec_prev = _prev()->position() - position();
    double len_next = vec_next.length(), len_prev = vec_prev.length();
    if (len_next > 0 && len_prev > 0) {
        // "dir" is an unit vector perpendicular to the bisector of the angle created
        // by the previous node, this auto node and the next node.
        Geom::Point dir = Geom::unit_vector((len_prev / len_next) * vec_next - vec_prev);
        // Handle lengths are equal to 1/3 of the distance from the adjacent node.
        _back.setRelativePos(-dir * (len_prev / 3));
        _front.setRelativePos(dir * (len_next / 3));
    } else {
        // If any of the adjacent nodes coincides, retract both handles.
        _front.retract();
        _back.retract();
    }
}

void Node::showHandles(bool v)
{
    _handles_shown = v;
    if (!_front.isDegenerate()) {
        _front.setVisible(v);
    }
    if (!_back.isDegenerate()) {
        _back.setVisible(v);
    }
}

void Node::updateHandles()
{
    _handleControlStyling();

    _front._handleControlStyling();
    _back._handleControlStyling();
}

void Node::setType(NodeType type, bool update_handles)
{
    if (type == NODE_PICK_BEST) {
        pickBestType();
        updateState(); // The size of the control might have changed
        return;
    }

    // if update_handles is true, adjust handle positions to match the node type
    // handle degenerate handles appropriately
    if (update_handles) {
        switch (type) {
            case NODE_CUSP:
                // nothing to do
                break;
            case NODE_AUTO:
                // auto handles make no sense for endnodes
                if (isEndNode())
                    return;
                _updateAutoHandles();
                break;
            case NODE_SMOOTH: {
                // ignore attempts to make smooth endnodes.
                if (isEndNode())
                    return;
                // rotate handles to be collinear
                // for degenerate nodes set positions like auto handles
                bool prev_line = _is_line_segment(_prev(), this);
                bool next_line = _is_line_segment(this, _next());
                if (_type == NODE_SMOOTH) {
                    // For a node that is already smooth and has a degenerate handle,
                    // drag out the second handle without changing the direction of the first one.
                    if (_front.isDegenerate()) {
                        double dist = Geom::distance(_next()->position(), position());
                        _front.setRelativePos(Geom::unit_vector(-_back.relativePos()) * dist / 3);
                    }
                    if (_back.isDegenerate()) {
                        double dist = Geom::distance(_prev()->position(), position());
                        _back.setRelativePos(Geom::unit_vector(-_front.relativePos()) * dist / 3);
                    }
                } else if (isDegenerate()) {
                    _updateAutoHandles();
                } else if (_front.isDegenerate()) {
                    // if the front handle is degenerate and next path segment is a line, make back collinear;
                    // otherwise, pull out the other handle to 1/3 of distance to prev.
                    if (next_line) {
                        _back.setDirection(_next()->position(), position());
                    } else if (_prev()) {
                        Geom::Point dir = direction(_back.position(), position());
                        _front.setRelativePos(Geom::distance(_prev()->position(), position()) / 3 * dir);
                    }
                } else if (_back.isDegenerate()) {
                    if (prev_line) {
                        _front.setDirection(_prev()->position(), position());
                    } else if (_next()) {
                        Geom::Point dir = direction(_front.position(), position());
                        _back.setRelativePos(Geom::distance(_next()->position(), position()) / 3 * dir);
                    }
                } else {
                    /* both handles are extended. make collinear while keeping length.
                       first make back collinear with the vector front ---> back,
                       then make front collinear with back ---> node.
                       (not back ---> front, because back's position was changed in the first call) */
                    _back.setDirection(_front.position(), _back.position());
                    _front.setDirection(_back.position(), position());
                }
            } break;
            case NODE_SYMMETRIC:
                if (isEndNode())
                    return; // symmetric handles make no sense for endnodes
                if (isDegenerate()) {
                    // similar to auto handles but set the same length for both
                    Geom::Point vec_next = _next()->position() - position();
                    Geom::Point vec_prev = _prev()->position() - position();

                    if (vec_next.length() == 0 || vec_prev.length() == 0) {
                        // Don't change a degenerate node if it overlaps a neighbor.
                        // (One could, but it seems pointless. In this case the
                        // calculation of 'dir' below needs to be special cased to
                        // avoid divide by zero.)
                        return;
                    }


                    double len_next = vec_next.length(), len_prev = vec_prev.length();
                    double len = (len_next + len_prev) / 6; // take 1/3 of average
                    if (len == 0) {
                        return;
                    }

                    Geom::Point dir = Geom::unit_vector((len_prev / len_next) * vec_next - vec_prev);
                    _back.setRelativePos(-dir * len);
                    _front.setRelativePos(dir * len);
                } else {
                    // At least one handle is extended. Compute average length, use direction from
                    // back handle to front handle. This also works correctly for degenerates
                    double len = (_front.length() + _back.length()) / 2;
                    Geom::Point dir = direction(_back.position(), _front.position());
                    _front.setRelativePos(dir * len);
                    _back.setRelativePos(-dir * len);
                }
                break;
            default:
                break;
        }
        // in node type changes, for BSpline traces, we can either maintain them
        // with NO_POWER power in border mode, or give them the default power in curve mode.
        if (_pm()._isBSpline()) {
            double weight = NO_POWER;
            if (!Geom::are_near(_pm()._bsplineHandlePosition(this->front()), NO_POWER, BSPLINE_TOL)) {
                weight = DEFAULT_START_POWER;
            }
            _front.setPosition(_pm()._bsplineHandleReposition(this->front(), weight));
            _back.setPosition(_pm()._bsplineHandleReposition(this->back(), weight));
        }
    }
    _type = type;
    _setControlType(nodeTypeToCtrlType(_type));
    updateState();
}

void Node::pickBestType()
{
    _type = NODE_CUSP;
    bool front_degen = _front.isDegenerate();
    bool back_degen = _back.isDegenerate();
    bool both_degen = front_degen && back_degen;
    bool neither_degen = !front_degen && !back_degen;
    do {
        // if both handles are degenerate, do nothing
        if (both_degen)
            break;
        // if neither are degenerate, check their respective positions
        if (neither_degen) {
            // for now do not automatically make nodes symmetric, it can be annoying
            /*if (Geom::are_near(front_delta, -back_delta)) {
                _type = NODE_SYMMETRIC;
                break;
            }*/
            if (are_collinear_within_serializing_error(_front.position(), position(), _back.position())) {
                _type = NODE_SMOOTH;
                break;
            }
        }
        // check whether the handle aligns with the previous line segment.
        // we know that if front is degenerate, back isn't, because
        // both_degen was false
        if (front_degen && _next() && _next()->_back.isDegenerate()) {
            if (are_collinear_within_serializing_error(_next()->position(), position(), _back.position())) {
                _type = NODE_SMOOTH;
                break;
            }
        } else if (back_degen && _prev() && _prev()->_front.isDegenerate()) {
            if (are_collinear_within_serializing_error(_prev()->position(), position(), _front.position())) {
                _type = NODE_SMOOTH;
                break;
            }
        }
    } while (false);
    _setControlType(nodeTypeToCtrlType(_type));
    updateState();
}

bool Node::isEndNode() const
{
    return !_prev() || !_next();
}

void Node::sink()
{
    _canvas_item_ctrl->lower_to_bottom();
}

NodeType Node::parse_nodetype(char x)
{
    switch (x) {
        case 'a':
            return NODE_AUTO;
        case 'c':
            return NODE_CUSP;
        case 's':
            return NODE_SMOOTH;
        case 'z':
            return NODE_SYMMETRIC;
        default:
            return NODE_PICK_BEST;
    }
}

bool Node::_eventHandler(Tools::ToolBase *event_context, CanvasEvent const &event)
{
    int dir = 0;
    int state = 0;

    inspect_event(
        event,
        [&](ScrollEvent const &event) {
            state = event.modifiers;
            dir = Geom::sgn(event.delta.y());
        },
        [&](KeyPressEvent const &event) {
            state = event.modifiers;
            switch (event.keyval) {
                case GDK_KEY_Page_Up:
                    dir = 1;
                    break;
                case GDK_KEY_Page_Down:
                    dir = -1;
                    break;
                default:
                    break;
            }
        },
        [&](CanvasEvent const &event) {});

    using namespace Inkscape::Modifiers;
    auto linear_grow = Modifier::get(Modifiers::Type::NODE_GROW_LINEAR)->active(state);
    auto spatial_grow = Modifier::get(Modifiers::Type::NODE_GROW_SPATIAL)->active(state);

    if (dir && (linear_grow || spatial_grow)) {
        if (linear_grow) {
            _linearGrow(dir);
        } else if (spatial_grow) {
            _selection.spatialGrow(this, dir);
        }
        return true;
    }

    return ControlPoint::_eventHandler(event_context, event);
}

void Node::_linearGrow(int dir)
{
    // Interestingly, we do not need any help from PathManipulator when doing linear grow.
    // First handle the trivial case of growing over an unselected node.
    if (!selected() && dir > 0) {
        _selection.insert(this);
        return;
    }

    NodeList::iterator this_iter = NodeList::get_iterator(this);
    NodeList::iterator fwd = this_iter, rev = this_iter;
    double distance_back = 0, distance_front = 0;

    // Linear grow is simple. We find the first unselected nodes in each direction
    // and compare the linear distances to them.
    if (dir > 0) {
        if (!selected()) {
            _selection.insert(this);
            return;
        }

        // find first unselected nodes on both sides
        while (fwd && fwd->selected()) {
            NodeList::iterator n = fwd.next();
            distance_front +=
                Geom::bezier_length(fwd->position(), fwd->_front.position(), n->_back.position(), n->position());
            fwd = n;
            if (fwd == this_iter)
                // there is no unselected node in this cyclic subpath
                return;
        }
        // do the same for the second direction. Do not check for equality with
        // this node, because there is at least one unselected node in the subpath,
        // so we are guaranteed to stop.
        while (rev && rev->selected()) {
            NodeList::iterator p = rev.prev();
            distance_back +=
                Geom::bezier_length(rev->position(), rev->_back.position(), p->_front.position(), p->position());
            rev = p;
        }

        NodeList::iterator t; // node to select
        if (fwd && rev) {
            if (distance_front <= distance_back)
                t = fwd;
            else
                t = rev;
        } else {
            if (fwd)
                t = fwd;
            if (rev)
                t = rev;
        }
        if (t)
            _selection.insert(t.ptr());

        // Linear shrink is more complicated. We need to find the farthest selected node.
        // This means we have to check the entire subpath. We go in the direction in which
        // the distance we traveled is lower. We do this until we run out of nodes (ends of path)
        // or the two iterators meet. On the way, we store the last selected node and its distance
        // in each direction (if any). At the end, we choose the one that is farther and deselect it.
    } else {
        // both iterators that store last selected nodes are initially empty
        NodeList::iterator last_fwd, last_rev;
        double last_distance_back = 0, last_distance_front = 0;

        while (rev || fwd) {
            if (fwd && (!rev || distance_front <= distance_back)) {
                if (fwd->selected()) {
                    last_fwd = fwd;
                    last_distance_front = distance_front;
                }
                NodeList::iterator n = fwd.next();
                if (n)
                    distance_front += Geom::bezier_length(fwd->position(), fwd->_front.position(), n->_back.position(),
                                                          n->position());
                fwd = n;
            } else if (rev && (!fwd || distance_front > distance_back)) {
                if (rev->selected()) {
                    last_rev = rev;
                    last_distance_back = distance_back;
                }
                NodeList::iterator p = rev.prev();
                if (p)
                    distance_back += Geom::bezier_length(rev->position(), rev->_back.position(), p->_front.position(),
                                                         p->position());
                rev = p;
            }
            // Check whether we walked the entire cyclic subpath.
            // This is initially true because both iterators start from this node,
            // so this check cannot go in the while condition.
            // When this happens, we need to check the last node, pointed to by the iterators.
            if (fwd && fwd == rev) {
                if (!fwd->selected())
                    break;
                NodeList::iterator fwdp = fwd.prev(), revn = rev.next();
                double df = distance_front + Geom::bezier_length(fwdp->position(), fwdp->_front.position(),
                                                                 fwd->_back.position(), fwd->position());
                double db = distance_back + Geom::bezier_length(revn->position(), revn->_back.position(),
                                                                rev->_front.position(), rev->position());
                if (df > db) {
                    last_fwd = fwd;
                    last_distance_front = df;
                } else {
                    last_rev = rev;
                    last_distance_back = db;
                }
                break;
            }
        }

        NodeList::iterator t;
        if (last_fwd && last_rev) {
            if (last_distance_front >= last_distance_back)
                t = last_fwd;
            else
                t = last_rev;
        } else {
            if (last_fwd)
                t = last_fwd;
            if (last_rev)
                t = last_rev;
        }
        if (t)
            _selection.erase(t.ptr());
    }
}

void Node::_setState(State state)
{
    // change node size to match type and selection state
    _canvas_item_ctrl->set_size(selected() ? HandleSize::LARGE : HandleSize::NORMAL);
    switch (state) {
        // These were used to set "active" and "prelight" flags but the flags weren't being used.
        case STATE_NORMAL:
        case STATE_MOUSEOVER:
            break;
        case STATE_CLICKED:
            // show the handles when selecting the nodes
            if (_pm()._isBSpline()) {
                this->front()->setPosition(_pm()._bsplineHandleReposition(this->front()));
                this->back()->setPosition(_pm()._bsplineHandleReposition(this->back()));
            }
            break;
    }
    SelectableControlPoint::_setState(state);
}

bool Node::grabbed(MotionEvent const &event)
{
    if (SelectableControlPoint::grabbed(event)) {
        return true;
    }

    if (Modifiers::Modifier::get(Modifiers::Type::NODE_CONFINE_TO_PATH)->active(event.modifiers)) {
        Geom::PathBuilder builder;

        if (auto prev = _prev()) {
            builder.moveTo(prev->position());
            prev->build_segment(builder, this);
        } else {
            builder.moveTo(position());
        }
        if (auto next = _next()) {
            this->build_segment(builder, next);
        }
        builder.flush();
        _short_segment_path = builder.peek();

        return true;
    }

    // Dragging out handles with Shift + drag on a node.
    if (!Modifiers::Modifier::get(Modifiers::Type::NODE_DRAG_HANDLE)->active(event.modifiers)) {
        return false;
    }

    Geom::Point evp = event.pos;
    Geom::Point rel_evp = evp - _last_click_event_point();

    // This should work even if dragtolerance is zero and evp coincides with node position.
    double angle_next = HUGE_VAL;
    double angle_prev = HUGE_VAL;
    bool has_degenerate = false;
    // determine which handle to drag out based on degeneration and the direction of drag
    if (_front.isDegenerate() && _next()) {
        Geom::Point next_relpos = _desktop->d2w(_next()->position()) - _desktop->d2w(position());
        angle_next = fabs(Geom::angle_between(rel_evp, next_relpos));
        has_degenerate = true;
    }
    if (_back.isDegenerate() && _prev()) {
        Geom::Point prev_relpos = _desktop->d2w(_prev()->position()) - _desktop->d2w(position());
        angle_prev = fabs(Geom::angle_between(rel_evp, prev_relpos));
        has_degenerate = true;
    }
    if (!has_degenerate) {
        return false;
    }

    Handle *h = angle_next < angle_prev ? &_front : &_back;

    h->setPosition(_desktop->w2d(evp));
    h->setVisible(true);
    h->transferGrab(this, event);
    Handle::_drag_out = true;
    return true;
}

void Node::dragged(Geom::Point &new_pos, MotionEvent const &event)
{
    auto const confine = Modifiers::Modifier::get(Modifiers::Type::MOVE_CONFINE)->active(event.modifiers);
    auto const confine_handles = Modifiers::Modifier::get(Modifiers::Type::NODE_CONFINE_HANDLES)->active(event.modifiers);
    auto const confine_to_path = Modifiers::Modifier::get(Modifiers::Type::NODE_CONFINE_TO_PATH)->active(event.modifiers);
    auto const no_snap = Modifiers::Modifier::get(Modifiers::Type::MOVE_NO_SNAPPING)->active(event.modifiers);

    // For a note on how snapping is implemented in Inkscape, see snap.h.
    auto &sm = _desktop->getNamedView()->snap_manager;
    // even if we won't really snap, we might still call the one of the
    // constrainedSnap() methods to enforce the constraints, so we need
    // to setup the snapmanager anyway; this is also required for someSnapperMightSnap()
    sm.setup(_desktop);

    bool snap = !no_snap && sm.someSnapperMightSnap();

    Inkscape::SnappedPoint sp;
    std::vector<Inkscape::SnapCandidatePoint> unselected;
    if (snap) {
        /* setup
         * TODO We are doing this every time a snap happens. It should once be done only once
         *      per drag - maybe in the grabbed handler?
         * TODO Unselected nodes vector must be valid during the snap run, because it is not
         *      copied. Fix this in snap.h and snap.cpp, then the above.
         * TODO Snapping to unselected segments of selected paths doesn't work yet. */

        // Build the list of unselected nodes.
        for (auto node : _selection.allPoints()) {
            if (!node->selected()) {
                auto n = static_cast<Node *>(node);
                unselected.emplace_back(n->position(), n->_snapSourceType(), n->_snapTargetType());
            }
        }
        sm.unSetup();
        sm.setupIgnoreSelection(_desktop, true, &unselected);
    }

    // Snap candidate point for free snapping; this will consider snapping tangentially
    // and perpendicularly and therefore the origin or direction vector must be set
    Inkscape::SnapCandidatePoint scp_free(new_pos, _snapSourceType());

    std::optional<Geom::Point> front_direction, back_direction;
    Geom::Point origin = _last_drag_origin();
    Geom::Point dummy_cp;
    if (_front.isDegenerate()) { // If there is no handle for the path segment towards the next node, then this segment
                                 // may be straight
        if (_is_line_segment(this, _next())) {
            front_direction = _next()->position() - origin;
            if (_next()->selected()) {
                dummy_cp = _next()->position() - position();
                scp_free.addVector(dummy_cp);
            } else {
                dummy_cp = _next()->position();
                scp_free.addOrigin(dummy_cp);
            }
        }
    } else { // .. this path segment is curved
        front_direction = _front.relativePos();
        scp_free.addVector(*front_direction);
    }

    if (_back.isDegenerate()) { // If there is no handle for the path segment towards the previous node, then this
                                // segment may be straight
        if (_is_line_segment(_prev(), this)) {
            back_direction = _prev()->position() - origin;
            if (_prev()->selected()) {
                dummy_cp = _prev()->position() - position();
                scp_free.addVector(dummy_cp);
            } else {
                dummy_cp = _prev()->position();
                scp_free.addOrigin(dummy_cp);
            }
        }
    } else { // .. this path segment is curved
        back_direction = _back.relativePos();
        scp_free.addVector(*back_direction);
    }

    if (confine_to_path && _short_segment_path && !confine_handles && !confine) {
        double line_distance;
        // We can't use multipleConstrainedSnaps here because the short_segment_path can be curved

        // 1. Move from the cursor/drag position onto the line
        auto time_on_line = _short_segment_path->nearestTime(new_pos, &line_distance);
        new_pos = _short_segment_path->pointAt(*time_on_line);

        // 2. Free snap to move the line point to a grid or guide
        Inkscape::SnapCandidatePoint scp_mid(new_pos, _snapSourceType());
        Inkscape::SnappedPoint sp = sm.freeSnap(scp_mid);
        new_pos = sp.getPoint();

        // 3. And finally move it back to the segment line to aproximate a constrained snap
        time_on_line = _short_segment_path->nearestTime(new_pos, &line_distance);
        new_pos = _short_segment_path->pointAt(*time_on_line);

        // TODO: We could update the handle positions here to preserve more bezier shapes
    } else if (confine || confine_handles) {
        // We're about to consider a constrained snap, which is already limited to 1D
        // Therefore tangential or perpendicular snapping will not be considered, and therefore
        // all calls above to scp_free.addVector() and scp_free.addOrigin() can be neglected
        std::vector<Inkscape::Snapper::SnapConstraint> constraints;
        if (confine_handles) { // constrain to handle lines
            Inkscape::Preferences *prefs = Inkscape::Preferences::get();
            double snaps = prefs->getDoubleLimited("/options/rotationsnapsperpi/value", 12.0, 0.1, 1800.0);
            double min_angle = M_PI / snaps;

            if (front_direction) { // We only have a front_point if the front handle is extracted, or if it is not
                                   // extracted but the path segment is straight (see above)
                constraints.emplace_back(origin, *front_direction);
            }

            if (back_direction) {
                constraints.emplace_back(origin, *back_direction);
            }

            // For smooth nodes, we will also snap to normals of handle lines. For cusp nodes this would be unintuitive
            // and confusing Only snap to the normals when they are further than snap increment away from the second
            // handle constraint
            if (_type != NODE_CUSP) {
                std::optional<Geom::Point> front_normal = Geom::rot90(*front_direction);
                if (front_normal && (!back_direction ||
                                     (fabs(Geom::angle_between(*front_normal, *back_direction)) > min_angle &&
                                      fabs(Geom::angle_between(*front_normal, *back_direction)) < M_PI - min_angle))) {
                    constraints.emplace_back(origin, *front_normal);
                }

                std::optional<Geom::Point> back_normal = Geom::rot90(*back_direction);
                if (back_normal && (!front_direction ||
                                    (fabs(Geom::angle_between(*back_normal, *front_direction)) > min_angle &&
                                     fabs(Geom::angle_between(*back_normal, *front_direction)) < M_PI - min_angle))) {
                    constraints.emplace_back(origin, *back_normal);
                }
            }
        } else {
            // with Ctrl and no Alt: constrain to axes
            constraints.emplace_back(origin, Geom::Point(1, 0));
            constraints.emplace_back(origin, Geom::Point(0, 1));
        }
        sp = sm.multipleConstrainedSnaps(Inkscape::SnapCandidatePoint(new_pos, _snapSourceType()), constraints,
                                         no_snap);
        new_pos = sp.getPoint();
    } else if (snap) {
        Inkscape::SnappedPoint sp = sm.freeSnap(scp_free);
        new_pos = sp.getPoint();
    }

    sm.unSetup();

    SelectableControlPoint::dragged(new_pos, event);
}

bool Node::clicked(ButtonReleaseEvent const &event)
{
    if (_pm()._nodeClicked(this, event, false)) {
        return true;
    }
    return SelectableControlPoint::clicked(event);
}

Inkscape::SnapSourceType Node::_snapSourceType() const
{
    if (_type == NODE_SMOOTH || _type == NODE_AUTO)
        return SNAPSOURCE_NODE_SMOOTH;
    return SNAPSOURCE_NODE_CUSP;
}

Inkscape::SnapTargetType Node::_snapTargetType() const
{
    if (_type == NODE_SMOOTH || _type == NODE_AUTO)
        return SNAPTARGET_NODE_SMOOTH;
    return SNAPTARGET_NODE_CUSP;
}

Inkscape::SnapCandidatePoint Node::snapCandidatePoint()
{
    return SnapCandidatePoint(position(), _snapSourceType(), _snapTargetType());
}

Handle *Node::handleToward(Node *to)
{
    if (_next() == to) {
        return front();
    }
    if (_prev() == to) {
        return back();
    }
    g_error("Node::handleToward(): second node is not adjacent!");
    return nullptr;
}

Node *Node::nodeToward(Handle *dir)
{
    if (front() == dir) {
        return _next();
    }
    if (back() == dir) {
        return _prev();
    }
    g_error("Node::nodeToward(): handle is not a child of this node!");
    return nullptr;
}

Handle *Node::handleAwayFrom(Node *to)
{
    if (_next() == to) {
        return back();
    }
    if (_prev() == to) {
        return front();
    }
    g_error("Node::handleAwayFrom(): second node is not adjacent!");
    return nullptr;
}

Node *Node::nodeAwayFrom(Handle *h)
{
    if (front() == h) {
        return _prev();
    }
    if (back() == h) {
        return _next();
    }
    g_error("Node::nodeAwayFrom(): handle is not a child of this node!");
    return nullptr;
}

Glib::ustring Node::_getTip(unsigned state) const
{
    bool isBSpline = _pm()._isBSpline();
    Handle *h = const_cast<Handle *>(&_front);
    Glib::ustring s = C_("Path node tip",
                         "node handle"); // not expected

    auto const mod_add_to = Modifiers::Modifier::get(Modifiers::Type::SELECT_ADD_TO);
    auto const mod_cycle_type = Modifiers::Modifier::get(Modifiers::Type::NODE_CYCLE_TYPE);
    auto const mod_delete = Modifiers::Modifier::get(Modifiers::Type::NODE_DELETE);
    auto const mod_drag_handle = Modifiers::Modifier::get(Modifiers::Type::NODE_DRAG_HANDLE);
    auto const mod_grow_linear = Modifiers::Modifier::get(Modifiers::Type::NODE_GROW_LINEAR);
    auto const mod_grow_spatial = Modifiers::Modifier::get(Modifiers::Type::NODE_GROW_SPATIAL);

    if (mod_grow_linear->active(state)) {
        s = Glib::ustring::compose(C_("Path node tip",
            "<b>%1</b>: scroll to select nodes along the path"), mod_grow_linear->get_label());
    }
    else if (mod_grow_spatial->active(state)) {
        s = Glib::ustring::compose(C_("Path node tip",
            "<b>%1</b>: scroll to select nearby nodes"), mod_grow_spatial->get_label());
    }
    else if (mod_drag_handle->active(state)) {
        bool can_drag_out = (_next() && _front.isDegenerate()) || (_prev() && _back.isDegenerate());

        if (can_drag_out) {
            /*if (state_held_control(state)) {
                s = format_tip(C_("Path node tip",
                    "<b>Shift+Ctrl:</b> drag out a handle and snap its angle "
                    "to %f° increments"), snap_increment_degrees());
            }*/
            s = Glib::ustring::compose(C_("Path node tip",
                "<b>%1</b>: drag out a handle, click to toggle selection"),
                mod_drag_handle->get_label());
        } else {
            s = Glib::ustring::compose(C_("Path node tip", "<b>%1</b>: click to toggle selection"),
                mod_add_to->get_label());
        }
    }
    else if (mod_delete->active(state)) {
        s = Glib::ustring::compose(C_("Path node tip",
            "<b>%1</b>: move along handle lines or line segment, click to delete node"),
            mod_delete->get_label());
    }
    else if (mod_cycle_type->active(state)) {
        s = Glib::ustring::compose(C_("Path node tip",
            "<b>%1</b>: move along axes, click to change node type"), mod_cycle_type->get_label());
    }
    else { // No modifiers: assemble tip from node type
        char const *nodetype = node_type_to_localized_string(_type);
        double power = _pm()._bsplineHandlePosition(h);

        std::set<Glib::ustring> labels;
        labels.insert(mod_add_to->get_label());
        labels.insert(mod_cycle_type->get_label());
        labels.insert(mod_delete->get_label());
        labels.insert(mod_drag_handle->get_label());
        labels.insert(mod_grow_linear->get_label());
        labels.insert(mod_grow_spatial->get_label());
        auto const more_labels = Inkscape::Util::join_with_separator(labels);

        if (_selection.transformHandlesEnabled() && selected()) {
            if (_selection.size() == 1) {
                if (!isBSpline) {
                    s = format_tip(C_("Path node tip", "<b>%s</b>: "
                                                       "drag to shape the path"
                                                       ". "
                                                       "(more: %s)"),
                                   nodetype, more_labels.c_str());
                } else {
                    s = format_tip(C_("Path node tip", "<b>BSpline node</b> (%.3g power): "
                                                       "drag to shape the path"
                                                       ". "
                                                       "(more: %s)"),
                                   power, more_labels.c_str());
                }
            } else {
                s = format_tip(C_("Path node tip", "<b>%s</b>: "
                                                   "drag to shape the path"
                                                   ", "
                                                   "click to toggle scale/rotation handles"
                                                   ". "
                                                   "(more: %s)"),
                               nodetype, more_labels.c_str());
            }
        } else if (!isBSpline) {
            s = format_tip(C_("Path node tip", "<b>%s</b>: "
                                               "drag to shape the path"
                                               ", "
                                               "click to select only this node"
                                               ". "
                                               "(more: %s)"),
                           nodetype, more_labels.c_str());
        } else {
            s = format_tip(C_("Path node tip", "<b>BSpline node</b> (%.3g power): "
                                               "drag to shape the path"
                                               ", "
                                               "click to select only this node"
                                               ". "
                                               "(more: %s)"),
                           power, more_labels.c_str());
        }
    }

    return (s);
}

Glib::ustring Node::_getDragTip(MotionEvent const & /*event*/) const
{
    Geom::Point dist = position() - _last_drag_origin();

    Inkscape::Util::Quantity x_q = Inkscape::Util::Quantity(dist[Geom::X], "px");
    Inkscape::Util::Quantity y_q = Inkscape::Util::Quantity(dist[Geom::Y], "px");
    Glib::ustring x = x_q.string(_desktop->getNamedView()->display_units);
    Glib::ustring y = y_q.string(_desktop->getNamedView()->display_units);
    Glib::ustring ret = format_tip(C_("Path node tip", "Move node by %s, %s"), x.c_str(), y.c_str());
    return ret;
}

/**
 * See also: Handle::handle_type_to_localized_string(NodeType type)
 */
char const *Node::node_type_to_localized_string(NodeType type)
{
    switch (type) {
        case NODE_CUSP:
            return _("Corner node");
        case NODE_SMOOTH:
            return _("Smooth node");
        case NODE_SYMMETRIC:
            return _("Symmetric node");
        case NODE_AUTO:
            return _("Auto-smooth node");
        default:
            return "";
    }
}

bool Node::_is_line_segment(Node *first, Node *second)
{
    if (!first || !second)
        return false;
    if (first->_next() == second)
        return first->_front.isDegenerate() && second->_back.isDegenerate();
    if (second->_next() == first)
        return second->_front.isDegenerate() && first->_back.isDegenerate();
    return false;
}

/** Build one segment of the geometric representation.
 * @relates PathManipulator */
void Node::build_segment(Geom::PathBuilder &builder, Node *next_node)
{
    if (next_node->back()->isDegenerate() && this->front()->isDegenerate()) {
        // NOTE: It seems like the renderer cannot correctly handle vline / hline segments,
        // and trying to display a path using them results in funny artifacts.
        builder.lineTo(next_node->position());
    } else {
        // this is a bezier segment
        builder.curveTo(
            this->front()->position(),
            next_node->back()->position(),
            next_node->position());
    }
}


NodeList::NodeList(SubpathList &splist)
    : _list(splist)
{
    this->ln_list = this;
    this->ln_next = this;
    this->ln_prev = this;
}

NodeList::~NodeList()
{
    clear();
}

bool NodeList::empty() const
{
    return ln_next == this;
}

NodeList::size_type NodeList::size() const
{
    size_type sz = 0;
    for (ListNode *ln = ln_next; ln != this; ln = ln->ln_next)
        ++sz;
    return sz;
}

bool NodeList::degenerate() const
{
    return closed() ? empty() : ++begin() == end();
}

NodeList::iterator NodeList::before(double t, double *fracpart)
{
    double intpart;
    *fracpart = std::modf(t, &intpart);
    int index = intpart;

    iterator ret = begin();
    std::advance(ret, index);
    return ret;
}

NodeList::iterator NodeList::before(Geom::PathTime const &pvp)
{
    iterator ret = begin();
    std::advance(ret, pvp.curve_index);
    return ret;
}

NodeList::iterator NodeList::insert(iterator pos, Node *x)
{
    ListNode *ins = pos._node;
    x->ln_next = ins;
    x->ln_prev = ins->ln_prev;
    ins->ln_prev->ln_next = x;
    ins->ln_prev = x;
    x->ln_list = this;
    return iterator(x);
}

void NodeList::splice(iterator pos, NodeList &list)
{
    splice(pos, list, list.begin(), list.end());
}

void NodeList::splice(iterator pos, NodeList &list, iterator i)
{
    NodeList::iterator j = i;
    ++j;
    splice(pos, list, i, j);
}

void NodeList::splice(iterator pos, NodeList & /*list*/, iterator first, iterator last)
{
    ListNode *ins_beg = first._node, *ins_end = last._node, *at = pos._node;
    for (ListNode *ln = ins_beg; ln != ins_end; ln = ln->ln_next) {
        ln->ln_list = this;
    }
    ins_beg->ln_prev->ln_next = ins_end;
    ins_end->ln_prev->ln_next = at;
    at->ln_prev->ln_next = ins_beg;

    ListNode *atprev = at->ln_prev;
    at->ln_prev = ins_end->ln_prev;
    ins_end->ln_prev = ins_beg->ln_prev;
    ins_beg->ln_prev = atprev;
}

void NodeList::shift(int n)
{
    // 1. make the list perfectly cyclic
    ln_next->ln_prev = ln_prev;
    ln_prev->ln_next = ln_next;
    // 2. find new begin
    ListNode *new_begin = ln_next;
    if (n > 0) {
        for (; n > 0; --n)
            new_begin = new_begin->ln_next;
    } else {
        for (; n < 0; ++n)
            new_begin = new_begin->ln_prev;
    }
    // 3. relink begin to list
    ln_next = new_begin;
    ln_prev = new_begin->ln_prev;
    new_begin->ln_prev->ln_next = this;
    new_begin->ln_prev = this;
}

void NodeList::reverse()
{
    for (ListNode *ln = ln_next; ln != this; ln = ln->ln_prev) {
        std::swap(ln->ln_next, ln->ln_prev);
        Node *node = static_cast<Node *>(ln);
        Geom::Point save_pos = node->front()->position();
        node->front()->setPosition(node->back()->position());
        node->back()->setPosition(save_pos);
    }
    std::swap(ln_next, ln_prev);
}

void NodeList::clear()
{
    // ugly but more efficient clearing mechanism
    std::vector<ControlPointSelection *> to_clear;
    std::vector<std::pair<SelectableControlPoint *, long>> nodes;
    long in = -1;
    for (iterator i = begin(); i != end(); ++i) {
        SelectableControlPoint *rm = static_cast<Node *>(i._node);
        if (std::find(to_clear.begin(), to_clear.end(), &rm->_selection) == to_clear.end()) {
            to_clear.push_back(&rm->_selection);
            ++in;
        }
        nodes.emplace_back(rm, in);
    }
    for (auto const &node : nodes) {
        to_clear[node.second]->erase(node.first, false);
    }
    std::vector<std::vector<SelectableControlPoint *>> emission;
    for (long i = 0, e = to_clear.size(); i != e; ++i) {
        emission.emplace_back();
        for (auto const &node : nodes) {
            if (node.second != i)
                break;
            emission[i].push_back(node.first);
        }
    }

    for (size_t i = 0, e = emission.size(); i != e; ++i) {
        to_clear[i]->signal_selection_changed.emit(emission[i], false);
    }

    for (iterator i = begin(); i != end();)
        erase(i++);
}

NodeList::iterator NodeList::erase(iterator i)
{
    // some gymnastics are required to ensure that the node is valid when deleted;
    // otherwise the code that updates handle visibility will break
    Node *rm = static_cast<Node *>(i._node);
    ListNode *rmnext = rm->ln_next, *rmprev = rm->ln_prev;
    ++i;
    delete rm;
    rmprev->ln_next = rmnext;
    rmnext->ln_prev = rmprev;
    return i;
}

// TODO this method is very ugly!
// converting SubpathList to an intrusive list might allow us to get rid of it
void NodeList::kill()
{
    for (SubpathList::iterator i = _list.begin(); i != _list.end(); ++i) {
        if (i->get() == this) {
            _list.erase(i);
            return;
        }
    }
}

NodeList &NodeList::get(Node *n)
{
    return n->nodeList();
}
NodeList &NodeList::get(iterator const &i)
{
    return *(i._node->ln_list);
}

} // namespace UI
} // namespace Inkscape

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
