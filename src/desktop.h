// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Editable view implementation
 */
/*
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Frank Felfe <innerspace@iname.com>
 *   MenTaLguY <mental@rydia.net>
 *   bulia byak <buliabyak@users.sf.net>
 *   Ralf Stephan <ralf@ark.in-berlin.de>
 *   John Bintz <jcoswell@coswellproductions.org>
 *   Johan Engelen <j.b.c.engelen@ewi.utwente.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2007 Jon A. Cruz
 * Copyright (C) 2006-2008 Johan Engelen
 * Copyright (C) 2006 John Bintz
 * Copyright (C) 2004 MenTaLguY
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_DESKTOP_H
#define INKSCAPE_DESKTOP_H

#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtk/gtk.h> // EventController et al.
#include <gdkmm/toplevel.h>
#include <sigc++/signal.h>
#include <2geom/affine.h>
#include <2geom/transforms.h>
#include <2geom/parallelogram.h>

#include "display/rendermode.h"
#include <sigc++/scoped_connection.h>
#include "message-stack.h"
#include "object/sp-gradient.h" // TODO refactor enums out to their own .h file

namespace Gtk {
class Box;
class Toolbar;
class Widget;
class Window;
} // namespace Gtk

/**
 * Iterates until true or returns false.
 * When used as signal accumulator, stops emission if one slot returns true.
 */
struct StopOnTrue {
  using result_type = bool;

  template<typename T_iterator>
  result_type operator()(T_iterator first, T_iterator last) const {
      for (; first != last; ++first)
          if (*first) return true;
      return false;
  }
};

/**
 * Iterates until nonzero or returns 0.
 * When used as signal accumulator, stops emission if one slot returns nonzero.
 */
struct StopOnNonZero {
  using result_type = int;

  template<typename T_iterator>
  result_type operator()(T_iterator first, T_iterator last) const {
      for (; first != last; ++first)
          if (*first) return *first;
      return 0;
  }
};

// ------- Inkscape --------
class SPCSSAttr;
class SPDesktopWidget;
class SPItem;
class SPNamedView;
class SPObject;
class SPStyle;
class SPStop;

class InkscapeWindow;
struct InkscapeApplication;

namespace Inkscape {
struct CanvasEvent;
class LayerManager;
class PageManager;
class MessageContext;
class MessageStack;
class Selection;

class CanvasItem;
class CanvasItemCatchall;
class CanvasItemDrawing;
class CanvasItemGroup;
class CanvasItemRotate;

namespace UI {

class ControlPointSelection;

namespace Dialog {
class DialogContainer;
} // namespace Dialog

namespace Tools {
class ToolBase;
class TextTool;
} // namespace Tools

namespace Widget {
class Canvas;
class Dock;
} // namespace Widget

} // namespace UI

namespace Display {
class TemporaryItemList;
class TemporaryItem;
class TranslucencyGroups;
class SnapIndicator;
class FontHighlight;
} // namespace Display

} // namespace Inkscape

inline constexpr double SP_DESKTOP_ZOOM_MAX = 256.00;
inline constexpr double SP_DESKTOP_ZOOM_MIN =   0.01;

/**
 * To do: update description of desktop. Define separation of desktop-widget, desktop, window, canvas, etc.
 */
class SPDesktop
{
public:
    SPDesktop(SPNamedView *nv);
    ~SPDesktop();

    SPDesktop(SPDesktop const &) = delete;
    SPDesktop &operator=(SPDesktop const &) = delete;
    SPDesktop(SPDesktop &&) = delete;
    SPDesktop &operator=(SPDesktop &&) = delete;

    SPDocument *doc() const { return document; }
    Inkscape::MessageStack *messageStack() const { return _message_stack.get(); }
    Inkscape::MessageContext *tipsMessageContext() const { return _tips_message_context.get(); }

    void setDesktopWidget(SPDesktopWidget *dtw);

    void setHideSelectionBoxes(bool hide);
    bool getHideSelectionBoxes() const { return _hide_selection_boxes; }

private:
    SPDocument *document = nullptr;
    std::unique_ptr<Inkscape::MessageStack> _message_stack;
    std::unique_ptr<Inkscape::MessageContext> _tips_message_context;
    std::unique_ptr<Inkscape::MessageContext> _guides_message_context;

    sigc::scoped_connection _message_changed_connection;
    sigc::scoped_connection _message_idle_connection;
    sigc::connection _document_uri_set_connection;
    sigc::connection _saved_or_modified_conn;
    sigc::connection _document_modified_conn;
    sigc::connection _document_object_bound_conn;

    std::unique_ptr<Inkscape::UI::Tools::ToolBase> _tool;
    std::unique_ptr<Inkscape::Display::TemporaryItemList> _temporary_item_list;
    std::unique_ptr<Inkscape::Display::TranslucencyGroups> _translucency_groups;
    std::unique_ptr<Inkscape::Display::SnapIndicator> _snapindicator;
    /// PROOF: the pink highlight on type set in a font that stands in for another.
    std::unique_ptr<Inkscape::Display::FontHighlight> _font_highlight;

    SPNamedView *namedview = nullptr;
    int _view_number{};

    std::unique_ptr<Inkscape::UI::Widget::Canvas> canvas;

    bool _hide_selection_boxes = false;

    unsigned _translucency_key;

public:
    Inkscape::UI::Tools::ToolBase    *getTool         () const { return _tool.get(); }
    Inkscape::Selection              *getSelection    () const { return _selection.get(); }
    SPDocument                       *getDocument     () const { return document; }
    Inkscape::UI::Widget::Canvas     *getCanvas       () const { return canvas.get(); }
    SPNamedView                      *getNamedView    () const { return namedview; }
    SPDesktopWidget                  *getDesktopWidget() const { return _widget; }
    Inkscape::Display::SnapIndicator *getSnapIndicator() const { return _snapindicator.get(); }
    int viewNumber() const { return _view_number; }

    Inkscape::CanvasItemGroup    *getCanvasControls() const { return _canvas_group_controls; }
    Inkscape::CanvasItemGroup    *getCanvasPagesBg()  const { return _canvas_group_pages_bg; }
    Inkscape::CanvasItemGroup    *getCanvasPagesFg()  const { return _canvas_group_pages_fg; }
    Inkscape::CanvasItemGroup    *getCanvasGrids()    const { return _canvas_group_grids; }
    Inkscape::CanvasItemGroup    *getCanvasGuides()   const { return _canvas_group_guides; }
    Inkscape::CanvasItemGroup    *getCanvasSketch()   const { return _canvas_group_sketch; }
    Inkscape::CanvasItemGroup    *getCanvasTemp()     const { return _canvas_group_temp; }
    Inkscape::CanvasItemCatchall *getCanvasCatchall() const { return _canvas_catchall; }
    Inkscape::CanvasItemDrawing  *getCanvasDrawing()  const { return _canvas_drawing; }

private:
    /// current selection; will never generally be NULL
    std::unique_ptr<Inkscape::Selection> _selection;

    // Groups
    Inkscape::CanvasItemGroup    *_canvas_group_controls = nullptr; ///< Handles, knots, nodes, etc.
    Inkscape::CanvasItemGroup    *_canvas_group_drawing  = nullptr; ///< SVG Drawing
    Inkscape::CanvasItemGroup    *_canvas_group_grids    = nullptr; ///< Grids.
    Inkscape::CanvasItemGroup    *_canvas_group_guides   = nullptr; ///< Guide lines.
    Inkscape::CanvasItemGroup    *_canvas_group_sketch   = nullptr; ///< Temporary items before becoming permanent.
    Inkscape::CanvasItemGroup    *_canvas_group_temp     = nullptr; ///< Temporary items that self-destruct.
    Inkscape::CanvasItemGroup    *_canvas_group_pages_bg = nullptr; ///< Page background
    Inkscape::CanvasItemGroup    *_canvas_group_pages_fg = nullptr; ///< Page border + shadow.
    // Individual items
    Inkscape::CanvasItemCatchall *_canvas_catchall       = nullptr; ///< The bottom item for unclaimed events.
    Inkscape::CanvasItemDrawing  *_canvas_drawing        = nullptr; ///< The actual SVG drawing (a.k.a. arena).

    void _setupCanvasItems();

public:
    SPCSSAttr *current = nullptr;  ///< Current style
    bool _focusMode = false; ///< Whether we're focused working or general working

    unsigned dkey = 0;
    unsigned interaction_disabled_counter = 0;
    bool waiting_cursor = false;
    bool showing_dialogs = false;
    bool rotation_locked = false;
    /// \todo Fixme: This has to be implemented in different way
    bool guides_active = false;

    // Storage for selected dragger used by GrDrag as it's created and deleted by tools
    SPItem *gr_item = nullptr;
    GrPointType gr_point_type = POINT_LG_BEGIN;
    unsigned gr_point_i = 0;
    Inkscape::PaintTarget gr_fill_or_stroke = Inkscape::FOR_FILL;

    // An id attribute is not allowed to be the empty string.
    Glib::ustring _reconstruction_old_layer_id;

    sigc::signal<bool (SPCSSAttr const *, bool)>::accumulated<StopOnTrue> _set_style_signal;
    sigc::signal<int (SPStyle *, int)>::accumulated<StopOnNonZero> _query_style_signal;

    /// Emitted when the zoom factor changes (not emitted when scrolling).
    /// The parameter is the new zoom factor
    sigc::signal<void (double)> signal_zoom_changed;

    template <typename F> sigc::connection connectDestroy(F &&slot) {
        return _destroy_signal.connect(std::forward<F>(slot));
    }

    template <typename F> sigc::connection connectDocumentReplaced(F &&slot) {
        return _document_replaced_signal.connect(std::forward<F>(slot));
    }

    template <typename F> sigc::connection connectEventContextChanged(F &&slot) {
        return _event_context_changed_signal.connect(std::forward<F>(slot));
    }

    template <typename F> sigc::connection connectSetStyle(F &&slot) {
        return _set_style_signal.connect(std::forward<F>(slot));
    }

    template <typename F> sigc::connection connectQueryStyle(F &&slot) {
        return _query_style_signal.connect(std::forward<F>(slot));
    }

    template <typename F> sigc::connection connectHideSelectionBoxes(F &&slot) {
        return _signal_hide_selection_boxes_changed.connect(std::forward<F>(slot));
    }

    // there's an object selected and it has a gradient fill and/or stroke; one of the gradient stops has been selected
    // callback receives sender pointer and selected stop pointer
    sigc::connection connect_gradient_stop_selected(sigc::slot<void (SPStop *)> const &slot);
    // a path is being edited and one of its control points has been (de)selected using node tool
    // callback receives sender pointer and control spoints selection pointer
    sigc::connection connect_control_point_selected(sigc::slot<void (Inkscape::UI::ControlPointSelection *)> const &slot);
    // there's an active text frame and user moves or clicks text cursor within it using text tool
    // callback receives sender pointer and text tool pointer
    sigc::connection connect_text_cursor_moved(sigc::slot<void (Inkscape::UI::Tools::TextTool *)> const &slot);

    void emit_gradient_stop_selected(SPStop *stop);
    void emit_control_point_selected(Inkscape::UI::ControlPointSelection *selection);
    void emit_text_cursor_moved(Inkscape::UI::Tools::TextTool *tool);

    Inkscape::LayerManager &layerManager() { return *_layer_manager; }
    Inkscape::LayerManager const &layerManager() const { return *_layer_manager; }

    Inkscape::MessageContext *guidesMessageContext() const {
        return _guides_message_context.get();
    }

    Inkscape::Display::TemporaryItem *add_temporary_canvasitem(Inkscape::CanvasItem *item, int lifetime_msecs, bool move_to_bottom = true);
    void remove_temporary_canvasitem(Inkscape::Display::TemporaryItem *tempitem);

    Inkscape::Display::TranslucencyGroups &getTranslucencyGroups() const { return *_translucency_groups; }

    Inkscape::UI::Dialog::DialogContainer *getContainer();

    bool isWithinViewport(SPItem const *item) const;
    bool itemIsHidden(SPItem const *item) const;

    void activate_guides(bool activate);
    void change_document(SPDocument *document);

    void setTool(std::string const &toolName);

    void set_coordinate_status(Geom::Point const &p);
    SPItem *getItemFromListAtPointBottom(std::vector<SPItem *> const &list, Geom::Point const &p) const;
    SPItem *getItemAtPoint(Geom::Point const &p, bool into_groups, SPItem *upto = nullptr) const;
    std::vector<SPItem*> getItemsAtPoints(std::vector<Geom::Point> ps, bool all_layers = true, bool topmost_only = true, size_t limit = 0, bool active_only = true, double distance = 0.25) const;
    SPItem *getGroupAtPoint(Geom::Point const &p) const;
    Geom::Point point() const;

    void prev_transform();
    void next_transform();
    void clear_transform_history();

    void set_display_area(bool log = true);
    void set_display_area(Geom::Point const &c, Geom::Point const &w, bool log = true);
    void set_display_area(Geom::Rect const &a, Geom::Coord border, bool log = true);
    Geom::Parallelogram get_display_area() const;
    void set_display_width(Geom::Rect const &a, Geom::Coord border);
    void set_display_center(Geom::Rect const &a);

    void zoom_absolute(Geom::Point const &c, double zoom, bool keep_point = true);
    void zoom_relative(Geom::Point const &c, double zoom, bool keep_point = true);
    void zoom_realworld(Geom::Point const &c, double ratio);

    void zoom_drawing();
    void zoom_selection();
    void schedule_zoom_from_document();

    double current_zoom() const { return _current_affine.getZoom(); }
    Geom::Point current_center() const;

    void zoom_quick(bool enable = true);
    /// Returns whether the desktop is in quick zoom mode or not.
    bool quick_zoomed() const { return _quick_zoom_enabled; }

    void quick_preview(bool activate);

    void set_rotation_lock(bool lock) { rotation_locked = lock; }
    bool get_rotation_lock() const { return rotation_locked; }

    void zoom_grab_focus();
    void rotate_grab_focus();

    void rotate_absolute_keep_point  (Geom::Point const &c, double rotate);
    void rotate_relative_keep_point  (Geom::Point const &c, double rotate);
    void rotate_absolute_center_point(Geom::Point const &c, double rotate);
    void rotate_relative_center_point(Geom::Point const &c, double rotate);

    enum CanvasFlip {
        FLIP_NONE       = 0,
        FLIP_HORIZONTAL = 1,
        FLIP_VERTICAL   = 2
    };
    void flip_absolute_keep_point  (Geom::Point const &c, CanvasFlip flip);
    void flip_relative_keep_point  (Geom::Point const &c, CanvasFlip flip);
    void flip_absolute_center_point(Geom::Point const &c, CanvasFlip flip);
    void flip_relative_center_point(Geom::Point const &c, CanvasFlip flip);
    bool is_flipped(CanvasFlip flip);

    Geom::Rotate const &current_rotation() const { return _current_affine.getRotation(); }

    void scroll_absolute(Geom::Point const &point);
    void scroll_relative(Geom::Point const &delta);
    void scroll_relative_in_svg_coords(double dx, double dy);
    bool scroll_to_point(Geom::Point const &s_dt, double autoscrollspeed = 0);

    Geom::IntPoint getWindowSize() const;
    void setWindowSize(Geom::IntPoint const &size);
    void setWindowTransient(Gtk::Window &window, int transient_policy = 1);
    InkscapeWindow const *getInkscapeWindow() const;
    InkscapeWindow       *getInkscapeWindow();
    void presentWindow();

    void showInfoDialog(Glib::ustring const &message);
    bool warnDialog (Glib::ustring const &text);

    void setRenderMode(Inkscape::RenderMode mode);
    void setColorMode(Inkscape::ColorMode  mode);

    void toggleCommandPalette();
    void toggleRulers();
    void toggleScrollbars();

    void setTempHideOverlays(bool hide);
    void layoutWidget();
    void setToolboxFocusTo(char const *label);
    Gtk::Widget *get_toolbar_by_name(Glib::ustring const &name);

    void setToolboxAdjustmentValue(char const *id, double val);
    bool isToolboxButtonActive(char const *id) const;
    void updateDialogs();
    void showNotice(Glib::ustring const &msg, int timeout = 0);

    void setWaitingCursor();
    void clearWaitingCursor();
    bool isWaitingCursor() const { return waiting_cursor; };

    void toggleLockGuides();
    void toggleToolbar(char const *toolbar_name);

    bool isMinimised() const;
    bool is_darktheme () const;
    bool is_maximized () const;
    bool is_fullscreen() const;
    bool is_focusMode () const;

    void focusMode(bool mode = true);

    /// Transformation from window to desktop coordinates (zoom/rotate).
    Geom::Affine const &w2d() const { return _current_affine.w2d(); }
    Geom::Point w2d(Geom::Point const &p) const { return p * _current_affine.w2d(); }
    /// Transformation from desktop to window coordinates
    Geom::Affine const &d2w() const { return _current_affine.d2w(); }
    Geom::Point d2w(Geom::Point const &p) const { return p * _current_affine.d2w(); }
    Geom::Affine const &doc2dt() const;
    Geom::Affine const &dt2doc() const;
    Geom::Point doc2dt(Geom::Point const &p) const { return p * doc2dt(); }
    Geom::Point dt2doc(Geom::Point const &p) const { return p * dt2doc(); }

    double yaxisdir() const { return doc2dt()[3]; }
    bool yaxisdown() const { return yaxisdir() > 0; }

    void setDocument(SPDocument *doc);

    void onWindowStateChanged(Gdk::Toplevel::State changed, Gdk::Toplevel::State new_toplevel_state);

    void applyCurrentOrToolStyle(SPObject *obj, Glib::ustring const &tool_path, bool with_text, const Glib::ustring &use_current = "") const;
    void applyCurrentOrToolStyle(Inkscape::XML::Node *repr, Glib::ustring const &tool_path, bool with_text, const Glib::ustring &use_current = "") const;
    SPCSSAttr *getCurrentOrToolStyle(Glib::ustring const &tool_path, bool with_text, const Glib::ustring &use_current = "") const;
    Glib::ustring getCurrentOrToolStylePath(Glib::ustring const &tool_path);

private:
    SPDesktopWidget *_widget = nullptr;

    void _attachDocument();
    void _detachDocument();

    // This simple class ensures that _w2d is always in sync with _rotation and _scale
    // We keep rotation and scale separate to avoid having to extract them from the affine.
    // With offset, this describes fully how to map the drawing to the window.
    // Future: merge offset as a translation in w2d.
    class DesktopAffine
    {
    public:
        Geom::Affine const &w2d() const { return _w2d; };
        Geom::Affine const &d2w() const { return _d2w; };

        void setScale(Geom::Scale scale) {
            _scale = scale;
            _update();
        }
        void addScale(Geom::Scale scale) {
            _scale *= scale;
            _update();
        }

        void setRotate(Geom::Rotate rotate) {
            _rotate = rotate;
            _update();
        }
        void setRotate(double rotate) {
            setRotate(Geom::Rotate{rotate});
        }
        void addRotate(Geom::Rotate rotate) {
            _rotate *= rotate;
            _update();
        }
        void addRotate(double rotate) {
            addRotate(Geom::Rotate{rotate});
        }

        void setFlip(CanvasFlip flip) {
            _flip = Geom::Scale();
            addFlip( flip );
        }

        bool isFlipped(CanvasFlip flip) {
            if ((flip & FLIP_HORIZONTAL) && Geom::are_near(_flip[0], -1)) {
                return true;
            }
            if ((flip & FLIP_VERTICAL) && Geom::are_near(_flip[1], -1)) {
                return true;
            }
            return false;
        }

        void addFlip(CanvasFlip flip) {
            if (flip & FLIP_HORIZONTAL) {
                _flip *= Geom::Scale(-1.0, 1.0);
            }
            if (flip & FLIP_VERTICAL) {
                _flip *= Geom::Scale(1.0, -1.0);
            }
            _update();
        }

        double getZoom() const {
            return _d2w.descrim();
        }

        Geom::Rotate const &getRotation() const {
            return _rotate;
        }

        void setOffset(Geom::Point offset) {
            _offset = offset;
        }
        void addOffset(Geom::Point offset) {
            _offset += offset;
        }
        Geom::Point const &getOffset() {
            return _offset;
        }

    private:
        void _update() {
            _d2w = _scale * _rotate * _flip;
            _w2d = _d2w.inverse();
        }
        Geom::Affine _w2d;      // Window to desktop
        Geom::Affine _d2w;      // Desktop to window
        Geom::Rotate _rotate;   // Rotate part of _w2d
        Geom::Scale  _scale;    // Scale part of _w2d, holds y-axis direction
        Geom::Scale  _flip;     // Flip part of _w2d
        Geom::Point  _offset;   // Point on canvas to align to (0,0) of window
    };

    DesktopAffine _current_affine;
    std::list<DesktopAffine> transforms_past;
    std::list<DesktopAffine> transforms_future;
    bool _quick_zoom_enabled = false; ///< Signifies that currently we're in quick zoom mode
    DesktopAffine _quick_zoom_affine; ///< The transform of the screen before quick zoom

    bool _overlays_visible = true; ///< Whether the overlays are temporarily hidden
    bool _saved_guides_visible = false; ///< Remembers guides' visibility when hiding overlays

    std::unique_ptr<Inkscape::LayerManager> _layer_manager;

    sigc::signal<void (SPDesktop *)> _destroy_signal;
    sigc::signal<void (SPDesktop *, SPDocument *)>     _document_replaced_signal;
    sigc::signal<void (SPDesktop *, Inkscape::UI::Tools::ToolBase *)> _event_context_changed_signal;
    sigc::signal<void (SPStop *)> _gradient_stop_selected;
    sigc::signal<void (Inkscape::UI::ControlPointSelection *)> _control_point_selected;
    sigc::signal<void (Inkscape::UI::Tools::TextTool *)> _text_cursor_moved;

    sigc::signal<void (bool)> _signal_hide_selection_boxes_changed;

    sigc::scoped_connection _reconstruction_start_connection;
    sigc::scoped_connection _reconstruction_finish_connection;
    sigc::scoped_connection _schedule_zoom_from_document_connection;
    sigc::scoped_connection _y_axis_flipped;
    sigc::scoped_connection _layer_changed_connection;

    bool drawing_handler(Inkscape::CanvasEvent const &event, Inkscape::DrawingItem *item);
    void reconstruction_start();
    void reconstruction_finish();
    void handle_y_axis_flip(double yshift);

    // pinch zoom
    std::optional<double> _begin_zoom;
    void on_zoom_begin(Gdk::EventSequence *sequence);
    void on_zoom_scale(double scale);
    void on_zoom_end(Gdk::EventSequence *sequence);

    void onStatusMessage(Inkscape::MessageType type, char const *message);

    std::deque<SPItem*> const &get_flat_item_list(bool into_groups, bool active_only) const;
    SPItem *_getItemFromListAtPointBottom(SPGroup *group, std::vector<SPItem*> const &list, Geom::Point const &p) const;
    std::vector<SPItem*> find_items_at_point(std::deque<SPItem*> const &nodes, Geom::Point const &p, int items_count = 0, SPItem *upto = nullptr, std::optional<double> distance = {}) const;
    SPItem *find_item_at_point(std::deque<SPItem*> const &nodes, Geom::Point const &p, SPItem *upto = nullptr) const;
    SPItem *find_group_at_point(SPGroup *group, Geom::Point const &p) const;

    // Find items by geometry --------------------
    mutable std::map<unsigned long, std::deque<SPItem*>> _node_cache; // Used to speed up search.

    void updateTranslucencyGroups();
public:
    void clearNodeCache() { _node_cache.clear(); }
};

#endif // INKSCAPE_DESKTOP_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
