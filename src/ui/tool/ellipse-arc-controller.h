// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOL_ELLIPSE_ARC_CONTROLLER_H
#define INKSCAPE_UI_TOOL_ELLIPSE_ARC_CONTROLLER_H
#include <memory>
#include <sigc++/scoped_connection.h>
namespace Inkscape::UI {
class PathManipulator;
class EllipseArcPoint;
// Native arc controls alongside deferred path anchors; conversion/release ends
// their lifetime with the owning manipulator.
class EllipseArcController {
public:
    explicit EllipseArcController(PathManipulator &pm);
    ~EllipseArcController();
    void queueRefresh();
    void pathReleased();
private:
    void refresh();
    PathManipulator &_pm;
    sigc::scoped_connection _refresh_idle;
    std::unique_ptr<EllipseArcPoint> _start, _end;
    EllipseArcPoint *_active = nullptr;
    bool _dragging = false;
    friend class EllipseArcPoint;
};
} // namespace Inkscape::UI
#endif
