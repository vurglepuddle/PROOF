// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors: see git history
 *
 * Copyright (C) 2017 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_COLORS_CMS_TRANSFORM_H
#define SEEN_COLORS_CMS_TRANSFORM_H

#include <cassert>
#include <lcms2.h>
#include <memory>

#include "colors/spaces/enum.h"

namespace Inkscape::Colors::CMS {

enum class Alpha {
    NONE,
    PRESENT,
    PREMULTIPLIED,
};

class Profile;
class Transform
{
public:
    explicit Transform(cmsHTRANSFORM handle, bool global = false)
        : _handle(handle)
        , _context(!global ? cmsGetTransformContextID(handle) : nullptr)
        , _format_in(cmsGetTransformInputFormat(handle))
        , _format_out(cmsGetTransformOutputFormat(handle))
        , _channels_in(T_CHANNELS(_format_in))
        , _channels_out(T_CHANNELS(_format_out))
    {
        //assert(_handle);
    }
    ~Transform()
    {
        if (_handle) {
            cmsDeleteTransform(_handle);
        }
    }
    Transform(Transform const &) = delete;
    Transform &operator=(Transform const &) = delete;

    explicit operator bool() const { return isValid(); }

    bool isValid() const { return _handle; }
    cmsHTRANSFORM getHandle() const { return _handle; }

protected:
    cmsHTRANSFORM _handle;
    cmsContext _context;

    static Alpha alpha_mode(bool premultiplied, bool present)
    {
        return !present ? Alpha::NONE : (premultiplied ? Alpha::PREMULTIPLIED : Alpha::PRESENT);
    }

    static int lcms_color_format(std::shared_ptr<Profile> const &profile, int size = 0, bool decimal = true,
                                 Alpha alpha = Alpha::NONE);

public:
    // PROOF: also for pictures converted row by row (display/tiff-pixels.cpp).
    static int lcms_intent(RenderingIntent intent);
    static int lcms_bpc(RenderingIntent intent);
    cmsUInt32Number _format_in;
    cmsUInt32Number _format_out;
    int _channels_in;
    int _channels_out;
};

} // namespace Inkscape::Colors::CMS

#endif // SEEN_COLORS_CMS_TRANSFORM_H

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
