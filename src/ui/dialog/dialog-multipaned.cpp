// SPDX-License-Identifier: GPL-2.0-or-later

/** @file
 * @brief A widget with multiple panes. Agnostic to type what kind of widgets panes contain.
 *
 * Authors: see git history
 *   Tavmjong Bah
 *
 * Copyright (c) 2020 Tavmjong Bah, Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "dialog-multipaned.h"

#include <numeric>
#include <glibmm/i18n.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/image.h>

#include "dialog-window.h"
#include "ui/controller.h"
#include "ui/util.h"
#include "ui/widget/canvas-grid.h"

static constexpr int DROPZONE_SIZE      =  5;
static constexpr int DROPZONE_EXPANSION = 15;
static constexpr int HANDLE_SIZE        = 10; // note: match size of handle icon to avoid stretching
static constexpr int HANDLE_CROSS_SIZE  = 25;

namespace Inkscape::UI::Dialog {

/*
 * References:
 *   https://blog.gtk.org/2017/06/
 *   https://developer.gnome.org/gtkmm-tutorial/stable/sec-custom-containers.html.en
 *   https://wiki.gnome.org/HowDoI/Gestures
 *
 * The children widget sizes are "sticky". They change a minimal
 * amount when the parent widget is resized or a child is added or
 * removed.
 *
 * A gesture is used to track handle movement. This must be attached
 * to the parent widget (the offset_x/offset_y values are relative to
 * the widget allocation which changes for the handles as they are
 * moved).
 */

int get_handle_size() {
    return HANDLE_SIZE;
}

/* ============ MyDropZone ============ */

/**
 * Dropzones are eventboxes at the ends of a DialogMultipaned where you can drop dialogs.
 */
class MyDropZone final
    : public Gtk::Box
{
public:
    MyDropZone(Gtk::Orientation orientation);
    ~MyDropZone() final;

    void set_gtypes(std::vector<GType> const &gtypes);
    using DropSlot = sigc::slot<bool (Glib::ValueBase const &, double, double)>;
    sigc::connection connect_drop(DropSlot slot);

    static void add_highlight_instances();
    static void remove_highlight_instances();

private:
    void set_size(int size);
    bool _active = false;
    void add_highlight();
    void remove_highlight();

    Glib::RefPtr<Gtk::DropTarget> const _zone_drop_target;

    static std::vector<MyDropZone *> _instances_list;
    friend class DialogMultipaned;
};

std::vector<MyDropZone *> MyDropZone::_instances_list;

MyDropZone::MyDropZone(Gtk::Orientation orientation)
    : Glib::ObjectBase("MultipanedDropZone")
    , Gtk::Box{orientation}
    , _zone_drop_target{Gtk::DropTarget::create(G_TYPE_INVALID, Gdk::DragAction::MOVE)}
{
    set_name("MultipanedDropZone");
    set_size(DROPZONE_SIZE);

    add_css_class("backgnd-passive");

    _zone_drop_target->signal_motion().connect([this](double x, double y){
        if (!_active) {
            _active = true;
            add_highlight();
            set_size(DROPZONE_SIZE + DROPZONE_EXPANSION);
        }
        return Gdk::DragAction::MOVE;
    }, false); // before

    _zone_drop_target->signal_leave().connect([this]{
        if (_active) {
            _active = false;
            set_size(DROPZONE_SIZE);
        }
    }, false); // before
    add_controller(_zone_drop_target);

    _instances_list.push_back(this);
}

MyDropZone::~MyDropZone()
{
    auto const it = std::find(_instances_list.cbegin(), _instances_list.cend(), this);
    assert(it != _instances_list.cend());
    _instances_list.erase(it);
}

void MyDropZone::set_gtypes(std::vector<GType> const &gtypes)
{
    _zone_drop_target->set_gtypes(gtypes);
}

sigc::connection MyDropZone::connect_drop(DropSlot slot)
{
    return _zone_drop_target->signal_drop().connect(std::move(slot), false); // before
}

void MyDropZone::add_highlight_instances()
{
    for (auto *instance : _instances_list) {
        instance->add_highlight();
    }
}

void MyDropZone::remove_highlight_instances()
{
    for (auto *instance : _instances_list) {
        instance->remove_highlight();
    }
}

void MyDropZone::add_highlight()
{
    remove_css_class("backgnd-passive");
    add_css_class   ("backgnd-active" );
}

void MyDropZone::remove_highlight()
{
    remove_css_class("backgnd-active" );
    add_css_class   ("backgnd-passive");
}

void MyDropZone::set_size(int size)
{
    if (get_orientation() == Gtk::Orientation::HORIZONTAL) {
        set_size_request(size, -1);
    } else {
        set_size_request(-1, size);
    }
}

/* ============  MyHandle  ============ */

/**
 * Handles are event boxes that help with resizing DialogMultipaned' children.
 */
class MyHandle final
    : public Gtk::Orientable
    , public Gtk::Overlay
{
public:
    MyHandle(Gtk::Orientation orientation, int size);
    ~MyHandle() final = default;

    void set_dragging    (bool dragging);
    void set_drag_updated(bool updated );

private:
    void on_motion_enter (double x, double y);
    void on_motion_motion(double x, double y);
    void on_motion_leave ();

    Gtk::EventSequenceState on_click_pressed (Gtk::GestureClick const &gesture);
    Gtk::EventSequenceState on_click_released(Gtk::GestureClick const &gesture);

    void toggle_multipaned();
    void update_click_indicator(double x, double y);
    void show_click_indicator(bool show);
    void draw_func(Cairo::RefPtr<Cairo::Context> const &cr, int width, int height);
    Cairo::Rectangle get_active_click_zone();

    Gtk::DrawingArea * const _drawing_area;
    int _cross_size;
    Gtk::Widget *_child;

    void size_allocate_vfunc(int width, int height, int baseline) final;

    bool is_click_resize_active() const;
    bool _click = false;
    bool _click_indicator = false;

    bool _dragging = false;
    bool _drag_updated = false;
};

MyHandle::MyHandle(Gtk::Orientation orientation, int size = get_handle_size())
    : Glib::ObjectBase("MultipanedHandle")
    , Gtk::Orientable()
    , Gtk::Overlay{}
    , _drawing_area{Gtk::make_managed<Gtk::DrawingArea>()}
    , _cross_size(0)
    , _child(nullptr)
{
    set_name("MultipanedHandle");
    set_orientation(orientation);

    auto const image = Gtk::make_managed<Gtk::Image>();
    if (get_orientation() == Gtk::Orientation::HORIZONTAL) {
        // vertical splitter resizing content horizontally
        image->set_from_icon_name("resizing-handle-vertical-symbolic");
        set_size_request(size, -1);
    } else {
        // horizontal splitter resizing content vertically
        image->set_from_icon_name("resizing-handle-horizontal-symbolic");
        set_size_request(-1, size);
    }
    image->set_pixel_size(size);
    set_child(*image);

    _drawing_area->set_draw_func(sigc::mem_fun(*this, &MyHandle::draw_func));
    add_overlay(*_drawing_area);

    auto const motion = Gtk::EventControllerMotion::create();
    motion->set_propagation_phase(Gtk::PropagationPhase::TARGET);
    motion->signal_enter().connect(sigc::mem_fun(*this, &MyHandle::on_motion_enter));
    motion->signal_motion().connect(sigc::mem_fun(*this, &MyHandle::on_motion_motion));
    motion->signal_leave().connect(sigc::mem_fun(*this, &MyHandle::on_motion_leave));
    _drawing_area->add_controller(motion);

    auto const click = Gtk::GestureClick::create();
    click->set_button(0); // any
    click->set_propagation_phase(Gtk::PropagationPhase::TARGET);
    click->signal_pressed().connect(Controller::use_state([this, &click = *click](auto &&...) { return on_click_pressed(click); }, *click));
    click->signal_released().connect(Controller::use_state([this, &click = *click](auto &&...) { return on_click_released(click); }, *click));
    _drawing_area->add_controller(click);
}

// draw rectangle with rounded corners
void rounded_rectangle(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w, double h, double r) {
    cr->begin_new_sub_path();
    cr->arc(x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cr->arc(x + w - r, y + r, r, 3 * M_PI / 2, 2 * M_PI);
    cr->arc(x + w - r, y + h - r, r, 0, M_PI / 2);
    cr->arc(x + r, y + h - r, r, M_PI / 2, M_PI);
    cr->close_path();
}

// part of the handle where clicking makes it automatically collapse/expand docked dialogs
Cairo::Rectangle MyHandle::get_active_click_zone() {
    const Gtk::Allocation& allocation = get_allocation();
    double width = allocation.get_width();
    double height = allocation.get_height();
    double h = height / 5;
    Cairo::Rectangle rect = { .x = 0, .y = (height - h) / 2, .width = width, .height = h };
    return rect;
}

void MyHandle::draw_func(Cairo::RefPtr<Cairo::Context> const &cr, int /*width*/, int /*height*/)
{
    // show click indicator/highlight?
    if (_click_indicator && is_click_resize_active() && !_dragging) {
        auto rect = get_active_click_zone();
        if (rect.width > 4 && rect.height > 0) {
            auto const fg = get_color();
            rounded_rectangle(cr, rect.x + 2, rect.y, rect.width - 4, rect.height, 3);
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.18);
            cr->fill();
        }
    }
}

void MyHandle::set_dragging(bool dragging) {
    if (_dragging != dragging) {
        _dragging = dragging;
        if (_click_indicator) {
            _drawing_area->queue_draw();
        }
    }
}

void MyHandle::set_drag_updated(bool const updated) {
    _drag_updated = updated;
}

/**
 * Change the mouse pointer into a resize icon to show you can drag.
 */
void MyHandle::on_motion_enter(double x, double y)
{
    if (get_orientation() == Gtk::Orientation::HORIZONTAL) {
        set_cursor("col-resize");
    } else {
        set_cursor("row-resize");
    }

    update_click_indicator(x, y);
}

void MyHandle::on_motion_leave()
{
    set_cursor("");
    show_click_indicator(false);
}

void MyHandle::show_click_indicator(bool show) {
    if (!is_click_resize_active()) return;

    if (show != _click_indicator) {
        _click_indicator = show;
        _drawing_area->queue_draw();
    }
}

void MyHandle::update_click_indicator(double x, double y) {
    if (!is_click_resize_active()) return;

    auto rect = get_active_click_zone();
    bool inside =
        x >= rect.x && x < rect.x + rect.width &&
        y >= rect.y && y < rect.y + rect.height;

    show_click_indicator(inside);
}

bool MyHandle::is_click_resize_active() const {
    return get_orientation() == Gtk::Orientation::HORIZONTAL;
}

Gtk::EventSequenceState MyHandle::on_click_pressed(Gtk::GestureClick const &gesture)
{
    // Detect single-clicks, except after a (moving/updated) drag
    _click = !_drag_updated && gesture.get_current_button() == 1;
    set_drag_updated(false);
    return Gtk::EventSequenceState::NONE;
}

Gtk::EventSequenceState MyHandle::on_click_released(Gtk::GestureClick const &gesture)
{
    // single-click on active zone?
    if (_click && gesture.get_current_button() == 1 && _click_indicator) {
        _click = false;
        _dragging = false;
        // handle clicked
        if (is_click_resize_active()) {
            toggle_multipaned();
            return Gtk::EventSequenceState::CLAIMED;
        }
    }

    _click = false;
    return Gtk::EventSequenceState::NONE;
}

void MyHandle::toggle_multipaned() {
    // visibility toggle of multipaned in a floating dialog window doesn't make sense; skip
    if (dynamic_cast<DialogWindow*>(get_root())) return;

    auto panel = dynamic_cast<DialogMultipaned*>(get_parent());
    if (!panel) return;

    auto const &children = panel->get_multipaned_children();
    Gtk::Widget* multi = nullptr; // multipaned widget to toggle
    bool left_side = true; // panels to the left of canvas
    size_t i = 0;

    // find multipaned widget to resize; it is adjacent (sibling) to 'this' handle in widget hierarchy
    for (auto const &widget : children) {
        if (dynamic_cast<Inkscape::UI::Widget::CanvasGrid*>(widget.get())) {
            // widget past canvas are on the right side (of canvas)
            left_side = false;
        }

        if (widget.get() == this) {
            if (left_side && i > 0) {
                // handle to the left of canvas toggles preceeding panel
                multi = dynamic_cast<DialogMultipaned*>(children[i - 1].get());
            }
            else if (!left_side && i + 1 < children.size()) {
                // handle to the right of canvas toggles next panel
                multi = dynamic_cast<DialogMultipaned*>(children[i + 1].get());
            }

            if (multi) {
                if (multi->is_visible()) {
                    multi->set_visible(false);
                }
                else {
                    multi->set_visible(true);
                }
                // resize parent
                panel->children_toggled();
            }
            break;
        }

        ++i;
    }
}

void MyHandle::on_motion_motion(double x, double y)
{
    // motion invalidates click; it activates resizing
    _click = false;
    update_click_indicator(x, y);
}

/**
 * This allocation handler function is used to add/remove handle icons in order to be able
 * to hide completely a transversal handle into the sides of a DialogMultipaned.
 *
 * The image has a specific size set up in the constructor and will not naturally shrink/hide.
 * In conclusion, we remove it from the handle and save it into an internal reference.
 */
void MyHandle::size_allocate_vfunc(int const width, int const height, int const baseline)
{
    Gtk::Overlay::size_allocate_vfunc(width, height, baseline);

    auto const size = get_orientation() == Gtk::Orientation::HORIZONTAL ? height : width;

    if (_cross_size > size && HANDLE_CROSS_SIZE > size && !_child) {
        _child = get_child();
        unset_child();
    } else if (_cross_size < size && HANDLE_CROSS_SIZE < size && _child) {
        set_child(*_child);
        _child = nullptr;
    }

    _cross_size = size;
}

/* ============ DialogMultipaned ============= */

DialogMultipaned::DialogMultipaned(Gtk::Orientation orientation)
    : Glib::ObjectBase("DialogMultipaned")
    , Gtk::Orientable()
    , Gtk::Widget()
{
    set_name("DialogMultipaned");
    set_orientation(orientation);
    set_hexpand(true); // TODO: GTK4: seems to need this. Is it the correct fix?

    // ============= Add dropzones ==============
    auto const dropzone_s = static_cast<MyDropZone *>(_children.emplace_back(std::make_unique<MyDropZone>(orientation)).get());
    auto const dropzone_e = static_cast<MyDropZone *>(_children.emplace_back(std::make_unique<MyDropZone>(orientation)).get());
    dropzone_s->set_parent(*this);
    dropzone_e->set_parent(*this);

    // ============ Connect signals =============
    // dialog resizing handle:
    auto const drag = Gtk::GestureDrag::create();
    drag->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    drag->signal_drag_begin() .connect(Controller::use_state([this](auto&, auto &&...args) { return on_drag_begin(args...); }, *drag));
    drag->signal_drag_update().connect(Controller::use_state([this](auto&, auto &&...args) { return on_drag_update(args...); }, *drag));
    drag->signal_drag_end()   .connect(Controller::use_state([this](auto&, auto &&...args) { return on_drag_end(args...); }, *drag));
    add_controller(drag);

    _connections.emplace_back(
        _drop_target->signal_drop().connect(
            sigc::mem_fun(*this, &DialogMultipaned::on_drag_data_drop), false)); // before
    _connections.emplace_back(
        dropzone_s->connect_drop(sigc::mem_fun(*this, &DialogMultipaned::on_prepend_drag_data)));
    _connections.emplace_back(
        dropzone_e->connect_drop(sigc::mem_fun(*this, &DialogMultipaned::on_append_drag_data)));

    add_controller(_drop_target);

    // add empty widget to initiate the container
    add_empty_widget();
}

DialogMultipaned::~DialogMultipaned()
{
    // Remove widgets that require special logic to remove.
    // TODO: Understand why this is necessary.
    while (true) {
        auto const it = std::find_if(begin(_children), end(_children), [] (auto const &w) {
            return dynamic_cast<DialogMultipaned *>(w.get()) || dynamic_cast<DialogNotebook *>(w.get());
        });
        if (it != _children.end()) {
            remove(**it);
        } else {
            // no more dialog panels
            break;
        }
    }

    // Remove remaining widgets (DropZones, CanvasGrid).
    for (auto const &child : _children) {
        g_assert(child->get_parent() == this);
        child->unparent();
    }

    _children.clear();
}

void DialogMultipaned::insert(int const pos, std::unique_ptr<Gtk::Widget> child)
{
    auto const parent = child->get_parent();
    g_assert(!parent || parent == this);

    // Zero/positive pos means insert @children[pos]. Negative means @children[children.size()-pos]
    // We update children, so we must get iterator anew each time it is to be used. Check bound too
    g_assert(pos >= 0 &&  pos <= _children.size() || // (prepending) inserting at 1-past-end is A-OK
             pos <  0 && -pos <= _children.size());  // (appending) inserting@ 1-before-begin is NOT
    auto const get_iter = [&]{ return (pos >= 0 ? _children.begin() : _children.end()) + pos; };

    remove_empty_widget(); // Will remove extra widget if existing

    // If there are MyMultipane children that are empty, they will be removed
    while (true) {
        auto const it = std::find_if(begin(_children), end(_children), [] (auto const &w) {
            if (auto const paned = dynamic_cast<DialogMultipaned *>(w.get())) {
                return paned->has_empty_widget();
            }
            return false;
        });
        if (it != _children.end()) {
            remove(**it);
            remove_empty_widget();
        } else {
            break;
        }
    }

    // Add handle
    if (_children.size() > 2) {
        auto my_handle = std::make_unique<MyHandle>(get_orientation());
        my_handle->set_parent(*this);
        _children.insert(get_iter(), std::move(my_handle));
    }

    // Add child
    if (!parent) {
        child->set_parent(*this);
    }
    _children.insert(get_iter(), std::move(child));
}

void DialogMultipaned::prepend(std::unique_ptr<Gtk::Widget> child)
{
    insert(+1, std::move(child)); // After start dropzone
}

void DialogMultipaned::append(std::unique_ptr<Gtk::Widget> child)
{
    insert(-1, std::move(child)); // Before end dropzone
}

void DialogMultipaned::add_empty_widget()
{
    const int EMPTY_WIDGET_SIZE = 60; // magic number

    // The empty widget is a label
    auto label = std::make_unique<Gtk::Label>(_("You can drop dockable dialogs here."));
    label->set_wrap();
    label->set_justify(Gtk::Justification::CENTER);
    label->set_valign(Gtk::Align::CENTER);
    label->set_vexpand();

    auto l = label.get();
    append(std::move(label));
    _empty_widget = l;

    if (get_orientation() == Gtk::Orientation::VERTICAL) {
        int dropzone_size = (get_height() - EMPTY_WIDGET_SIZE) / 2;
        if (dropzone_size > DROPZONE_SIZE) {
            set_dropzone_sizes(dropzone_size, dropzone_size);
        }
    }
}

void DialogMultipaned::remove_empty_widget()
{
    if (_empty_widget) {
        auto it = std::find_if(begin(_children), end(_children), [this] (auto &p) { return p.get() == _empty_widget; });
        if (it != _children.end()) {
            _empty_widget->unparent();
            _children.erase(it);
        }
        _empty_widget = nullptr;
    }

    if (get_orientation() == Gtk::Orientation::VERTICAL) {
        set_dropzone_sizes(DROPZONE_SIZE, DROPZONE_SIZE);
    }
}

Gtk::Widget *DialogMultipaned::get_first_widget()
{
    if (_children.size() > 2) {
        return _children[1].get();
    } else {
        return nullptr;
    }
}

Gtk::Widget *DialogMultipaned::get_last_widget()
{
    if (_children.size() > 2) {
        return _children[_children.size() - 2].get();
    } else {
        return nullptr;
    }
}

/**
 * Set the sizes of the DialogMultipaned dropzones.
 * @param start, the size you want or -1 for the default `DROPZONE_SIZE`
 * @param end, the size you want or -1 for the default `DROPZONE_SIZE`
 */
void DialogMultipaned::set_dropzone_sizes(int start, int end)
{
    bool horizontal = get_orientation() == Gtk::Orientation::HORIZONTAL;

    if (start == -1) {
        start = DROPZONE_SIZE;
    }

    auto dropzone_s = dynamic_cast<MyDropZone *>(_children.front().get());

    if (dropzone_s) {
        if (horizontal) {
            dropzone_s->set_size_request(start, -1);
        } else {
            dropzone_s->set_size_request(-1, start);
        }
    }

    if (end == -1) {
        end = DROPZONE_SIZE;
    }

    auto dropzone_e = dynamic_cast<MyDropZone *>(_children.back().get());

    if (dropzone_e) {
        if (horizontal) {
            dropzone_e->set_size_request(end, -1);
        } else {
            dropzone_e->set_size_request(-1, end);
        }
    }
}

/**
 * Show/hide as requested all children of this container that are of type multipaned
 */
void DialogMultipaned::toggle_multipaned_children(bool show)
{
    _handle = -1;
    _drag_handle = -1;

    for (auto const &child : _children) {
        if (auto panel = dynamic_cast<DialogMultipaned*>(child.get())) {
            panel->set_visible(show);
        }
    }
}

/**
 * Ensure that this dialog container is visible.
 */
void DialogMultipaned::ensure_multipaned_children()
{
    toggle_multipaned_children(true);
    // hide_multipaned = false;
    // queue_allocate();
}

// ****************** OVERRIDES ******************

// The following functions are here to define the behavior of our custom container

Gtk::SizeRequestMode DialogMultipaned::get_request_mode_vfunc() const
{
    if (get_orientation() == Gtk::Orientation::HORIZONTAL) {
        return Gtk::SizeRequestMode::WIDTH_FOR_HEIGHT;
    } else {
        return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
    }
}

void DialogMultipaned::measure_vfunc(Gtk::Orientation const orientation, int const for_size,
                                     int &minimum, int &natural,
                                     int &minimum_baseline, int &natural_baseline) const
{
    minimum = 0;
    natural = 0;
    minimum_baseline = -1;
    natural_baseline = -1;

    for (auto const &child : _children) {
        if (child && child->is_visible()) {
            int child_minimum, child_natural, ignore;
            child->measure(orientation, for_size, child_minimum, child_natural, ignore, ignore);
            if (get_orientation() != orientation) {
                minimum = std::max(minimum, child_minimum);
                natural = std::max(natural, child_natural);
            } else {
                minimum += child_minimum;
                natural += child_natural;
            }
        }
    }

    if (orientation == Gtk::Orientation::HORIZONTAL) {
        natural = std::max(natural, _natural_width);
    }
}

void DialogMultipaned::children_toggled() {
    _handle = -1;
    _drag_handle = -1;
    queue_allocate();
}

[[nodiscard]] static int _get_size(auto const sizes, Gtk::Orientation const orientation)
{
    return orientation == Gtk::Orientation::HORIZONTAL ? sizes.get_width () : sizes.get_height();
}

/**
 * This function allocates the sizes of the children widgets (be them internal or not) from
 * the container's allocated size.
 *
 * Natural width: The width the widget really wants.
 * Minimum width: The minimum width for a widget to be useful.
 * Minimum <= Natural.
 */
void DialogMultipaned::size_allocate_vfunc(int const width, int const height, int const baseline)
{
    Gtk::Widget::size_allocate_vfunc(width, height, baseline);

    auto const allocation = Gdk::Rectangle{0, 0, width, height};
    auto const orientation = get_orientation();

    if (_drag_handle != -1) { // Exchange allocation between the widgets on either side of moved handle
        // Allocation values calculated in on_drag_update();
        _children[_drag_handle - 1]->size_allocate(allocation1, baseline);
        _children[_drag_handle    ]->size_allocate(allocationh, baseline);
        _children[_drag_handle + 1]->size_allocate(allocation2, baseline);
        _drag_handle = -1;
    }
    // initially widgets get created with a 1x1 size; ignore it and wait for the final resize
    else if (allocation.get_width() > 1 && allocation.get_height() > 1) {
        _natural_width = allocation.get_width();
    }

    std::vector<bool> expandables;              // Is child expandable?
    std::vector<int> sizes_minimums;            // Difference between allocated space and minimum space.
    std::vector<int> sizes_naturals;            // Difference between allocated space and natural space.
    std::vector<int> sizes_current;             // The current sizes along main axis
    int left = _get_size(allocation, orientation);

    int index = 0;
    bool force_resize = false;  // initially panels are not sized yet, so we will apply their natural sizes
    int canvas_index = -1;
    for (auto const &child : _children) {
        bool visible = child->get_visible();

        if (dynamic_cast<Inkscape::UI::Widget::CanvasGrid*>(child.get())) {
            canvas_index = index;
        }

        expandables.push_back(child->compute_expand(get_orientation()));

        Gtk::Requisition req_minimum;
        Gtk::Requisition req_natural;
        child->get_preferred_size(req_minimum, req_natural);
        if (auto nb = dynamic_cast<DialogNotebook*>(child.get()); nb && orientation == Gtk::Orientation::VERTICAL) {
            // natural height request from our DialogNotebook is not always reported by get_preferred_size() call;
            // it appears that overridden measure_vfunc is not always invoked, so we read height explicitly:
            int natural = nb->get_requested_height();
            if (natural > req_natural.get_height()) {
                req_natural.set_height(natural);
            }
        }
        if (child.get() == _resizing_widget1 || child.get() == _resizing_widget2) {
            // ignore limits for widget being resized interactively and use their current size
            req_minimum.set_width (0);
            req_minimum.set_height(0);
            auto alloc = child->get_allocation();
            req_natural.set_width (alloc.get_width ());
            req_natural.set_height(alloc.get_height());
        }

        sizes_minimums.push_back(visible ? _get_size(req_minimum, orientation) : 0);
        sizes_naturals.push_back(visible ? _get_size(req_natural, orientation) : 0);

        Gtk::Allocation child_allocation = child->get_allocation();
        int size = 0;
        if (visible) {
            if (dynamic_cast<MyHandle*>(child.get())) {
                // resizing handles should never be smaller than their min size:
                size = _get_size(req_minimum, orientation);
            }
            else if (dynamic_cast<MyDropZone*>(child.get())) {
                // don't upset drop zone sizes
                size = _get_size(req_minimum, orientation);
            }
            else {
                // all other widgets can get smaller than their min size
                size = _get_size(child_allocation, orientation);
                auto const min = _get_size(req_minimum, orientation);
                auto natural = _get_size(req_natural, orientation);
                if (size < min) {
                    if (size == 0 && natural >= min) {
                        // initially, widgets don't have their size established yet, so we should
                        // honor natural size request
                        size = natural;
                    }
                    else {
                        size = min;
                    }
                }
            }
        }
        sizes_current.push_back(size);
        index++;

        if (sizes_current.back() < sizes_minimums.back()) force_resize = true;
    }

    std::vector<int> sizes = sizes_current; // The new allocation sizes

    const int sum_current = std::accumulate(sizes_current.begin(), sizes_current.end(), 0);
    {
        // Precalculate the minimum, natural and current totals
        const int sum_minimums = std::accumulate(sizes_minimums.begin(), sizes_minimums.end(), 0);
        const int sum_naturals = std::accumulate(sizes_naturals.begin(), sizes_naturals.end(), 0);

        // initial resize requested?
        if (force_resize && sum_naturals <= left) {
            sizes = sizes_naturals;
            left -= sum_naturals;
        } else if (sum_minimums <= left && left < sum_current) {
            // requested size exeeds available space; try shrinking it by starting from the last element
            sizes = sizes_current;
            auto excess = sum_current - left;
            for (int i = static_cast<int>(sizes.size() - 1); excess > 0 && i >= 0; --i) {
                auto extra = sizes_current[i] - sizes_minimums[i];
                if (extra > 0) {
                    if (extra >= excess) {
                        // we are done, enough space found
                        sizes[i] -= excess;
                        excess = 0;
                    }
                    else {
                        // shrink as far as possible, then continue to the next panel
                        sizes[i] -= extra;
                        excess -= extra;
                    }
                }
            }

            if (excess > 0) {
                sizes = sizes_minimums;
                left -= sum_minimums;
            }
            else {
                left = 0;
            }
        }
        else {
            left = std::max(0, left - sum_current);
        }
    }

    if (canvas_index >= 0) { // give remaining space to canvas element
        sizes[canvas_index] += left;
    } else { // or, if in a sub-dialogmultipaned, give it to the last panel

        for (int i = static_cast<int>(_children.size()) - 1; i >= 0; --i) {
            if (expandables[i]) {
                sizes[i] += left;
                break;
            }
        }
    }

    // Check if we actually need to change the sizes on the main axis
    left = _get_size(allocation, orientation);
    if (left == sum_current) {
        bool valid = true;
        for (size_t i = 0; i < _children.size(); ++i) {
            valid = sizes_minimums[i] <= sizes_current[i] &&        // is it over the minimums?
                    (expandables[i] || sizes_current[i] <= sizes_naturals[i]); // but does it want to be expanded?
            if (!valid)
                break;
        }
        if (valid) {
            sizes = sizes_current; // The current sizes are good, don't change anything;
        }
    }

    // Set x and y values of allocations (widths should be correct).
    int current_x = allocation.get_x();
    int current_y = allocation.get_y();

    // Allocate
    for (size_t i = 0; i < _children.size(); ++i) {
        Gtk::Allocation child_allocation = _children[i]->get_allocation();
        child_allocation.set_x(current_x);
        child_allocation.set_y(current_y);

        int size = sizes[i];

        if (orientation == Gtk::Orientation::HORIZONTAL) {
            child_allocation.set_width(size);
            current_x += size;
            child_allocation.set_height(allocation.get_height());
        } else {
            child_allocation.set_height(size);
            current_y += size;
            child_allocation.set_width(allocation.get_width());
        }

        _children[i]->size_allocate(child_allocation, baseline);
    }
}

/**
 * Removes a widget from DialogMultipaned.
 * It does not remove handles or dropzones.
 */
void DialogMultipaned::remove(Gtk::Widget &widget)
{
    auto const child = &widget;

    MyDropZone *dropzone = dynamic_cast<MyDropZone *>(child);
    if (dropzone) {
        return;
    }
    MyHandle *my_handle = dynamic_cast<MyHandle *>(child);
    if (my_handle) {
        return;
    }

    const bool visible = child->get_visible();
    if (_children.size() > 2) {
        auto const it = std::find_if(begin(_children), end(_children), [=] (auto &p) { return p.get() == child; });
        if (it != _children.end()) {         // child found
            if (it + 2 != _children.end()) { // not last widget
                my_handle = dynamic_cast<MyHandle *>((it + 1)->get());
                g_assert(my_handle);
                my_handle->unparent();
                child->unparent();
                _children.erase(it, it + 2);
            } else {                        // last widget
                if (_children.size() == 3) { // only widget
                    child->unparent();
                    _children.erase(it);
                } else { // not only widget, delete preceding handle
                    my_handle = dynamic_cast<MyHandle *>((it - 1)->get());
                    g_assert(my_handle);
                    my_handle->unparent();
                    child->unparent();
                    _children.erase(it - 1, it + 1);
                }
            }
        }
    }

    if (visible) {
        queue_resize();
    }

    if (_children.size() == 2) {
        add_empty_widget();
        _empty_widget->set_size_request(300, -1);
        _signal_now_empty.emit();
    }
}

Gtk::EventSequenceState DialogMultipaned::on_drag_begin(double start_x, double start_y)
{
    _hide_widget1 = _hide_widget2 = nullptr;
    _resizing_widget1 = _resizing_widget2 = nullptr;
    // We clicked on handle.
    bool found = false;
    int child_number = 0;
    for (auto const &child : _children) {
        if (auto const my_handle = dynamic_cast<MyHandle *>(child.get())) {
            Gtk::Allocation child_allocation = my_handle->get_allocation();

            // Did drag start in handle?
            int x = child_allocation.get_x();
            int y = child_allocation.get_y();
            if (x < start_x && start_x < x + child_allocation.get_width() &&
                y < start_y && start_y < y + child_allocation.get_height()) {
                found = true;
                my_handle->set_dragging(true);
                break;
            }
        }
        ++child_number;
    }

    if (!found) {
        return Gtk::EventSequenceState::DENIED;
    }

    if (child_number < 1 || child_number > (int)(_children.size() - 2)) {
        std::cerr << "DialogMultipaned::on_drag_begin: Invalid child (" << child_number << "!!" << std::endl;
        return Gtk::EventSequenceState::DENIED;
    }

    // Save for use in on_drag_update().
    _handle = child_number;
    start_allocation1 = _children[_handle - 1]->get_allocation();
    if (!_children[_handle - 1]->is_visible()) {
        start_allocation1.set_width(0);
        start_allocation1.set_height(0);
    }
    start_allocationh = _children[_handle]->get_allocation();
    start_allocation2 = _children[_handle + 1]->get_allocation();
    if (!_children[_handle + 1]->is_visible()) {
        start_allocation2.set_width(0);
        start_allocation2.set_height(0);
    }

    return Gtk::EventSequenceState::CLAIMED;
}

Gtk::EventSequenceState DialogMultipaned::on_drag_end(double offset_x, double offset_y)
{
    if (_handle >= 0 && _handle < _children.size()) {
        if (auto my_handle = dynamic_cast<MyHandle*>(_children[_handle].get())) {
            my_handle->set_dragging(false);
        }
    }

    _handle = -1;
    _drag_handle = -1;
    if (_hide_widget1) {
        _hide_widget1->set_visible(false);
    }
    if (_hide_widget2) {
        _hide_widget2->set_visible(false);
    }
    _hide_widget1 = nullptr;
    _hide_widget2 = nullptr;
    _resizing_widget1 = nullptr;
    _resizing_widget2 = nullptr;

    queue_allocate(); // reimpose limits if any were bent during interactive resizing

    return Gtk::EventSequenceState::DENIED;
}

// docking panels in application window can be collapsed (to left or right side) to make more
// room for canvas; this functionality is only meaningful in app window, not in floating dialogs
bool can_collapse(Gtk::Widget* widget, Gtk::Widget* handle) {
    // can only collapse DialogMultipaned widgets
    if (!widget || dynamic_cast<DialogMultipaned*>(widget) == nullptr) return false;

    // collapsing is not supported in floating dialogs
    if (dynamic_cast<DialogWindow*>(widget->get_root())) return false;

    auto parent = handle->get_parent();
    if (!parent) return false;

    // find where the resizing handle is in relation to canvas area: left or right side;
    // next, find where the panel is in relation to the handle: on its left or right
    bool left_side = true;
    bool left_handle = false;
    size_t panel_index = 0;
    size_t handle_index = 0;
    size_t i = 0;
    for (auto &child : UI::children(*parent)) {
        if (dynamic_cast<Inkscape::UI::Widget::CanvasGrid *>(&child)) {
            left_side = false;
        } else if (&child == handle) {
            left_handle = left_side;
            handle_index = i;
        } else if (&child == widget) {
            panel_index = i;
        }
        ++i;
    }

    if (left_handle && panel_index < handle_index) {
        return true;
    }
    if (!left_handle && panel_index > handle_index) {
        return true;
    }

    return false;
}

// return minimum widget size; this fn works for hidden widgets too
int get_min_width(Gtk::Widget* widget) {
    bool hidden = !widget->is_visible();
    if (hidden) widget->set_visible(true);
    int minimum_size, natural_size, ignore;
    widget->measure(Gtk::Orientation::HORIZONTAL, -1, minimum_size, natural_size, ignore, ignore);
    if (hidden) widget->set_visible(false);
    return minimum_size;
}

// Different docking resizing activities use easing functions to speed up or slow down certain phases of resizing
// Below are two picewise linear functions used for that purpose

// easing function for revealing collapsed panels
double reveal_curve(double val, double size) {
    if (size > 0 && val <= size && val >= 0) {
        // slow start (resistance to opening) and then quick reveal
        auto x = val / size;
        auto pos = x;
        if (x <= 0.2) {
            pos = x * 0.25;
        }
        else {
            pos = x * 9.5 - 1.85;
            if (pos > 1) pos = 1;
        }
        return size * pos;
    }

    return val;
}

// easing function for collapsing panels
// note: factors for x dictate how fast resizing happens when moving mouse (with 1 being at the same speed);
// other constants are to make this fn produce values in 0..1 range and seamlessly connect three segments
double collapse_curve(double val, double size) {
    if (size > 0 && val <= size && val >= 0) {
        // slow start (resistance), short pause and then quick collapse
        auto x = val / size;
        auto pos = x;
        if (x < 0.5) {
            // fast collapsing
            pos = x * 10 - 5 + 0.92;
            if (pos < 0) {
                // panel collapsed
                pos = 0;
            }
        }
        else if (x < 0.6) {
            // pause
            pos = 0.2 * 0.6 + 0.8; // = 0.92;
        }
        else {
            // resistance to collapsing (move slow, x 0.2 decrease)
            pos = x * 0.2 + 0.8;
        }
        return size * pos;
    }

    return val;
}

Gtk::EventSequenceState DialogMultipaned::on_drag_update(double offset_x, double offset_y)
{
    if (_handle < 0) {
        return Gtk::EventSequenceState::NONE;
    }
    // Hack: drag update sends some fractional garbage x, y right after first click, leading to handle movement;
    // ignore them. The only downside is that we won't be able to return to the exact original location, once we move.
    // PROOF: test only the axis the handle moves along; requiring both made a perfectly
    // straight drag (steady hand, tablet or script) do nothing.
    auto const along = get_orientation() == Gtk::Orientation::HORIZONTAL ? offset_x : offset_y;
    if (std::abs(along) < 1) return Gtk::EventSequenceState::NONE;

    auto child1 = _children[_handle - 1].get();
    auto child2 = _children[_handle + 1].get();
    allocation1 = _children[_handle - 1]->get_allocation();
    allocationh = _children[_handle    ]->get_allocation();
    allocation2 = _children[_handle + 1]->get_allocation();

    // HACK: The bias prevents erratic resizing when dragging the handle fast, outside the bounds of the app.
    const int BIAS = 1;

    auto const handle = dynamic_cast<MyHandle *>(_children[_handle].get());
    handle->set_drag_updated(true);

    if (get_orientation() == Gtk::Orientation::HORIZONTAL) {
        // function to resize panel
        auto resize_fn = [](Gtk::Widget* handle, Gtk::Widget* child, int start_width, double& offset_x) {
            int minimum_size = get_min_width(child);
            auto width = start_width + offset_x;
            bool resizing = false;
            Gtk::Widget* hide = nullptr;

            if (!child->is_visible() && can_collapse(child, handle)) {
                child->set_visible(true);
                resizing = true;
            }

            if (width < minimum_size) {
                if (can_collapse(child, handle)) {
                    resizing = true;
                    auto w = start_width == 0 ? reveal_curve(width, minimum_size) : collapse_curve(width, minimum_size);
                    offset_x = w - start_width;
                    // facilitate closing/opening panels: users don't have to drag handle all the
                    // way to collapse/expand a panel, they just need to move it fraction of the way;
                    // note: those thresholds correspond to the easing functions used
                    auto threshold = start_width == 0 ? minimum_size * 0.20 : minimum_size * 0.42;
                    hide = width <= threshold ? child : nullptr;
                }
                else {
                    offset_x = -(start_width - minimum_size) + BIAS;
                }
            }

            return std::make_pair(resizing, hide);
        };

        /*
        TODO NOTE:
        Resizing should ideally take into account all columns, not just adjacent ones (left and right here).
        Without it, expanding second collapsed column does not work, since first one may already have min width,
        and cannot be shrunk anymore. Instead it should be pushed out of the way (canvas should be shrunk).
        */

        // panel on the left
        auto action1 = resize_fn(handle, child1, start_allocation1.get_width(), offset_x);
        _resizing_widget1 = action1.first ? child1 : nullptr;
        _hide_widget1 = action1.second ? child1 : nullptr;

        // panel on the right (needs reversing offset_x, so it can use the same logic)
        offset_x = -offset_x;
        auto action2 = resize_fn(handle, child2, start_allocation2.get_width(), offset_x);
        _resizing_widget2 = action2.first ? child2 : nullptr;
        _hide_widget2 = action2.second ? child2 : nullptr;
        offset_x = -offset_x;

        // set new sizes; they may temporarily violate min size panel requirements
        // GTK is not happy about 0-size allocations
        allocation1.set_width(start_allocation1.get_width() + offset_x);
        allocationh.set_x(start_allocationh.get_x() + offset_x);
        allocation2.set_x(start_allocation2.get_x() + offset_x);
        allocation2.set_width(start_allocation2.get_width() - offset_x);
    } else {
        // nothing fancy about resizing in vertical direction; no panel collapsing happens here
        Gtk::Requisition minimum_req, ignore;

        _children[_handle - 1]->get_preferred_size(minimum_req, ignore);
        int minimum_size = minimum_req.get_height();
        if (start_allocation1.get_height() + offset_y < minimum_size)
            offset_y = -(start_allocation1.get_height() - minimum_size) + BIAS;

        _children[_handle + 1]->get_preferred_size(minimum_req, ignore);
        minimum_size = minimum_req.get_height();
        if (start_allocation2.get_height() - offset_y < minimum_size)
            offset_y = start_allocation2.get_height() - minimum_size - BIAS;

        allocation1.set_height(start_allocation1.get_height() + offset_y);
        allocationh.set_y(start_allocationh.get_y() + offset_y);
        allocation2.set_y(start_allocation2.get_y() + offset_y);
        allocation2.set_height(start_allocation2.get_height() - offset_y);
    }

    _drag_handle = _handle;
    queue_allocate(); // Relayout DialogMultipaned content.

    return Gtk::EventSequenceState::NONE;
}

void DialogMultipaned::set_drop_gtypes(std::vector<GType> const &gtypes)
{
    auto &front = dynamic_cast<MyDropZone &>(*_children.at(0) );
    auto &back  = dynamic_cast<MyDropZone &>(*_children.back());
    _drop_target->set_gtypes(gtypes);
    front.set_gtypes(gtypes);
    back .set_gtypes(gtypes);
}

// extract page and its source notebook from d&d data
static std::optional<std::pair<Gtk::Widget*, DialogNotebook*>> unpack_page(const Glib::ValueBase& value) {
    if (auto source = UI::Widget::TabStrip::unpack_drop_source(value)) {
        auto tabs = source->first;
        auto pos = source->second;
        auto page = find_dialog_page(tabs, pos);
        if (!page) {
            std::cerr << "DialogContainer::unpack_page: page not found!" << std::endl;
            return {};
        }
        auto notebook = find_dialog_notebook(tabs);
        return std::make_pair(page, notebook);
    }
    return {};
}

bool DialogMultipaned::on_drag_data_drop(Glib::ValueBase const &value, double x, double y)
{
    auto page = unpack_page(value);
    if (!page) return false;

    // find notebook under (x, y)
    auto const it = std::find_if(begin(_children), end(_children), [=,this](const auto& w) {
        if (auto notebook = dynamic_cast<DialogNotebook*>(w.get())) {
            double cx, cy;
            if (translate_coordinates(*notebook, x, y, cx, cy)) {
                return notebook->contains(cx, cy);
            }
        }
        return false;
    });
    if (it != _children.end()) {
        // notebook found; move page into it
        auto dest_notebook = static_cast<DialogNotebook*>(it->get());
        return _signal_dock_dialog.emit(*page->first, *page->second, DialogContainer::Middle, dest_notebook);
    }
    else {
        // no notebook under (x, y) - float dialog
        auto src_notebook = page->second;
        return _signal_float_dialog.emit(*page->first, *src_notebook);
    }
}

bool DialogMultipaned::on_prepend_drag_data(Glib::ValueBase const &value, double /*x*/, double /*y*/)
{
    if (auto page = unpack_page(value)) {
        return _signal_dock_dialog.emit(*page->first, *page->second, DialogContainer::Start, nullptr);
    }
    return false;
}

bool DialogMultipaned::on_append_drag_data(Glib::ValueBase const &value, double /*x*/, double /*y*/)
{
    if (auto page = unpack_page(value)) {
        return _signal_dock_dialog.emit(*page->first, *page->second, DialogContainer::End, nullptr);
    }
    return false;
}

sigc::signal<void ()> DialogMultipaned::signal_now_empty()
{
    return _signal_now_empty;
}

void DialogMultipaned::set_restored_width(int width) {
    _natural_width = width;
}

void DialogMultipaned::add_drop_zone_highlight_instances()
{
    MyDropZone::add_highlight_instances();
}

void DialogMultipaned::remove_drop_zone_highlight_instances()
{
    MyDropZone::remove_highlight_instances();
}

} // namespace Inkscape::UI::Dialog

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
