// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_FRAMECHECK_H
#define INKSCAPE_FRAMECHECK_H

#include <glib.h>

namespace Inkscape::FrameCheck {

/// RAII object that logs a timing event for the duration of its lifetime.
struct Event
{
    gint64 start;
    char const *name;
    int subtype;

    Event() : start(-1) {}

    Event(char const *name, int subtype = 0) : start(g_get_monotonic_time()), name(name), subtype(subtype) {}

    Event(Event &&p) { movefrom(p); }

    ~Event() { finish(); }

    Event &operator=(Event &&p)
    {
        finish();
        movefrom(p);
        return *this;
    }

private:
    void movefrom(Event &p)
    {
        start = p.start;
        name = p.name;
        subtype = p.subtype;
        p.start = -1;
    }

    void finish() { if (start != -1) write(); }

    void write();
};

/// PROOF: an event outside the canvas, logged only while Framecheck is enabled in preferences.
bool enabled();
inline Event maybe(char const *name, int subtype = 0) { return enabled() ? Event(name, subtype) : Event(); }

} // namespace Inkscape::FrameCheck

#endif // INKSCAPE_FRAMECHECK_H

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
