// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_WIDGET_CANVAS_PREFS_H
#define INKSCAPE_UI_WIDGET_CANVAS_PREFS_H

#include "preferences.h"

namespace Inkscape::UI::Widget {

class Prefs
{
public:
    Prefs()
    {
        devmode.action = [this] { set_devmode(devmode); };
        devmode.action();
    }

    // Main preferences
    Pref<int>    xray_radius              = { "/options/rendering/xray-radius", 100, 1, 1500 };
    Pref<int>    outline_overlay_opacity  = { "/options/rendering/outline-overlay-opacity", 50, 0, 100 };
    // PROOF: Responsive by default (Inkscape: Multiscale, which tears moving objects).
    Pref<int>    update_strategy          = { "/options/rendering/update_strategy", 1, 1, 3 };
    Pref<bool>   request_opengl           = { "/options/rendering/request_opengl" };
    Pref<int>    grabsize                 = { "/options/grabsize/value", 3, 1, 15 };
    Pref<int>    numthreads               = { "/options/threading/numthreads", 0, 1, 256 };

    // Colour management
    Pref<bool>   use_user_profile         = { "/options/displayprofile/use_user_profile" };
    Pref<void>   displayprofile           = { "/options/displayprofile" };
    Pref<void>   softproof                = { "/options/softproof" };

    // Auto-scrolling
    Pref<int>    autoscrolldistance       = { "/options/autoscrolldistance/value", 0, -1000, 10000 };
    Pref<double> autoscrollspeed          = { "/options/autoscrollspeed/value", 1.0, 0.0, 10.0 };

    // Devmode preferences
    Pref<int>    tile_size                = { "/options/rendering/tile_size", 300, 1, 10000 };
    Pref<int>    render_time_limit        = { "/options/rendering/render_time_limit", 80, 1, 5000 };
    Pref<int>    pixelstreamer_method     = { "/options/rendering/pixelstreamer_method", 1, 1, 4 };
    Pref<int>    padding                  = { "/options/rendering/padding", 350, 0, 1000 };
    Pref<int>    prerender                = { "/options/rendering/prerender", 100, 0, 1000 };
    Pref<int>    preempt                  = { "/options/rendering/preempt", 250, 0, 1000 };
    Pref<int>    coarsener_min_size       = { "/options/rendering/coarsener_min_size", 200, 0, 1000 };
    Pref<int>    coarsener_glue_size      = { "/options/rendering/coarsener_glue_size", 80, 0, 1000 };
    Pref<double> coarsener_min_fullness   = { "/options/rendering/coarsener_min_fullness", 0.3, 0.0, 1.0 };

    // Debug switches
    Pref<bool>   debug_framecheck         = { "/options/rendering/debug_framecheck" };
    Pref<bool>   debug_logging            = { "/options/rendering/debug_logging" };
    Pref<bool>   debug_delay_redraw       = { "/options/rendering/debug_delay_redraw" };
    Pref<int>    debug_delay_redraw_time  = { "/options/rendering/debug_delay_redraw_time", 50, 0, 1000000 };
    Pref<bool>   debug_show_redraw        = { "/options/rendering/debug_show_redraw" };
    Pref<bool>   debug_show_unclean       = { "/options/rendering/debug_show_unclean" }; // no longer implemented
    Pref<bool>   debug_show_snapshot      = { "/options/rendering/debug_show_snapshot" };
    Pref<bool>   debug_show_clean         = { "/options/rendering/debug_show_clean" }; // no longer implemented
    Pref<bool>   debug_disable_redraw     = { "/options/rendering/debug_disable_redraw" };
    Pref<bool>   debug_sticky_decoupled   = { "/options/rendering/debug_sticky_decoupled" };
    Pref<bool>   debug_animate            = { "/options/rendering/debug_animate" };

private:
    // Developer mode
    Pref<bool> devmode = { "/options/rendering/devmode" };

    void set_devmode(bool on)
    {
        tile_size.set_enabled(on);
        render_time_limit.set_enabled(on);
        pixelstreamer_method.set_enabled(on);
        padding.set_enabled(on);
        prerender.set_enabled(on);
        preempt.set_enabled(on);
        coarsener_min_size.set_enabled(on);
        coarsener_glue_size.set_enabled(on);
        coarsener_min_fullness.set_enabled(on);
        debug_framecheck.set_enabled(on);
        debug_logging.set_enabled(on);
        debug_delay_redraw.set_enabled(on);
        debug_delay_redraw_time.set_enabled(on);
        debug_show_redraw.set_enabled(on);
        debug_show_unclean.set_enabled(on);
        debug_show_snapshot.set_enabled(on);
        debug_show_clean.set_enabled(on);
        debug_disable_redraw.set_enabled(on);
        debug_sticky_decoupled.set_enabled(on);
        debug_animate.set_enabled(on);
    }
};

} // namespace Inkscape::UI::Widget

#endif // INKSCAPE_UI_WIDGET_CANVAS_PREFS_H

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
