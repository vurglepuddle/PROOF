// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_DISPLAY_STROKE_TRANSFORM_H
#define INKSCAPE_DISPLAY_STROKE_TRANSFORM_H

#include <2geom/affine.h>

class SPStyle;

namespace Inkscape {

struct StrokeTransformStyle
{
    bool width_set = false;
    bool width_inherit = false;
    bool dash_set = false;
    bool offset_set = false;
    bool has_dashes = false;
    bool own_decoration = false;
    bool hairline = false;
};

StrokeTransformStyle stroke_style(SPStyle const &style);

struct StrokeTransform
{
    Geom::Affine transform;
    double expansion = 1.0;
    double width_expansion = 1.0;
    double dash_expansion = 1.0;
    double offset_expansion = 1.0;
    double decoration_expansion = 1.0;
    bool inherited = false;

    void cascade(StrokeTransformStyle const &style)
    {
        if (inherited || style.hairline) {
            if (style.width_set) width_expansion = 1.0;
            if (style.dash_set) dash_expansion = 1.0;
            if (style.offset_set) offset_expansion = 1.0;
        } else {
            if (!style.width_inherit) width_expansion = expansion;
            if (style.dash_set) dash_expansion = expansion;
            if (style.offset_set) offset_expansion = style.has_dashes ? expansion : 1.0;
        }
        if (style.own_decoration) decoration_expansion = width_expansion;
    }
};

} // namespace Inkscape

#endif
