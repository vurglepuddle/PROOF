// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_WIDGET_FILL_STROKE_CONTROLS_H
#define INKSCAPE_UI_WIDGET_FILL_STROKE_CONTROLS_H

#include <array>
#include <optional>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/fixed.h>
#include <gtkmm/menubutton.h>
#include "paint-popover-manager.h"

class SPDesktop;
class SPStyle;

namespace Inkscape::UI::Widget {

// The Illustrator-style fill/stroke targets in the tools sidebar.
class FillStrokeControls final : public Gtk::Box {
public:
    FillStrokeControls();
    ~FillStrokeControls() override;
    void setDesktop(SPDesktop *desktop);
    enum class Command { ToggleTarget, Swap, Defaults, None };
    void execute(Command command);

private:
    struct Preview {
        enum class Kind { None, Solid, Mixed, Gradient, Other };
        Kind kind = Kind::None;
        uint32_t rgba = 0xffffffff;
        std::vector<std::pair<double, uint32_t>> stops;
    };
    void refresh();
    void bindSelection();
    void closePopovers();
    void setActive(bool fill);
    void setupPaint(bool fill);
    std::vector<sigc::connection> connectPaint(bool fill);
    bool queryStyle(SPStyle &style, bool fill) const;
    void draw(Cairo::RefPtr<Cairo::Context> const &cr, int width, int height, bool fill);
    // The small commands round the tiles, drawn like them: default paints and swap over them,
    // Illustrator's Color, Gradient and None under them.
    enum class Glyph { Defaults, Swap, Color, Gradient, None };
    void drawGlyph(Cairo::RefPtr<Cairo::Context> const &cr, Glyph glyph);
    void setNone();
    void setDefaults();
    void swapPaint();
    void applyStyle(char const *fill, char const *stroke);

    SPDesktop *_desktop = nullptr;
    bool _fill_active = true;
    unsigned _context = 0;
    Gtk::Fixed _targets;
    Gtk::MenuButton _fill, _stroke;
    Gtk::DrawingArea _fill_preview, _stroke_preview;
    struct Small {
        Gtk::Button button;
        Gtk::DrawingArea area;
    };
    std::array<Small, 5> _small; // in Glyph order
    std::array<Preview, 2> _preview;
    std::optional<PaintPopoverManager::Registration> _fill_registration, _stroke_registration;
    sigc::scoped_connection _selection_changed, _selection_modified;
    sigc::scoped_connection _document_changed, _desktop_destroyed, _style_changed;
};
} // namespace Inkscape::UI::Widget
#endif
