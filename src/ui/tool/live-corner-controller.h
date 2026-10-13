// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOL_LIVE_CORNER_CONTROLLER_H
#define INKSCAPE_UI_TOOL_LIVE_CORNER_CONTROLLER_H

#include "live-corners.h"
#include "display/control/canvas-item-ptr.h"
#include <gtkmm/box.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/spinbutton.h>
#include <sigc++/scoped_connection.h>
#include <memory>
#include <set>

namespace Inkscape { class CanvasItemBpath; }
namespace Inkscape::UI {
class PathManipulator;
class LiveCornerWidget;
class ControlPoint;
class CornerDisplayPreferences;

// The widgets have a lifetime independent of the editable node list, so that
// expanding a corner does not destroy the widget which owns the current grab.
class LiveCornerController {
public:
    explicit LiveCornerController(PathManipulator &pm);
    ~LiveCornerController();
    void queueRefresh(bool reset_selection = false);
    void geometryChanged();
    void updateMetadata();
    void captureNativeCorners();
    void pathReleased();

private:
    void refresh();
    void choose(std::size_t corner, bool add);
    std::vector<std::size_t> targets(std::size_t corner) const;
    void begin(std::size_t corner);
    Geom::Point drag(std::size_t corner, Geom::Point const &delta, bool cancelled);
    void finish(bool cancelled = false);
    void cycle(std::size_t corner);
    void open(std::size_t corner, Geom::Point const &at);
    void applyFields();
    void commit();
    std::vector<LiveCornerController *> groupControllers();
    double updateGroupRadius(double displayed_amount);
    Geom::Point widgetPosition(std::size_t corner) const;
    double displayScale() const;

    PathManipulator &_pm;
    LiveCorners::Model _model, _before;
    std::vector<std::vector<Geom::Point>> _points;
    std::vector<std::unique_ptr<LiveCornerWidget>> _widgets;
    std::vector<std::size_t> _shown, _editing;
    std::set<std::size_t> _selected;
    bool _dragging = false, _cancelled = false, _updating = false;
    std::unique_ptr<CornerDisplayPreferences> _display;
    double _start_radius = 0;
    LiveCornerController *_leader = nullptr;
    // Only peers are retained; owning our own PM would create a cycle.
    std::vector<std::shared_ptr<PathManipulator>> _peers;
    sigc::scoped_connection _refresh_idle;
    CanvasItemPtr<Inkscape::CanvasItemBpath> _limit_preview;

    Gtk::Popover _popup;
    Gtk::Box _fields{Gtk::Orientation::VERTICAL, 5};
    Gtk::Label _title, _radius_label, _type_label, _rounding_label;
    Gtk::SpinButton _radius;
    Gtk::DropDown _type, _rounding;

    friend class LiveCornerWidget;
};
bool isLiveCorner(ControlPoint const *point);
} // namespace Inkscape::UI
#endif
