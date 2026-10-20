// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2023 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_COLORS_SPACES_DEVICECMYK_H
#define SEEN_COLORS_SPACES_DEVICECMYK_H

#include "rgb.h"

namespace Inkscape::Colors::Space {

/**
 * This sRGB based DeviceCMYK space is uncalibrated and fixed to the sRGB icc profile.
 */
class DeviceCMYK : public RGBBase<DeviceCMYK>
{
public:
    constexpr static int OutputChannels = 4;
    constexpr static int ProfileChannels = 3;

    DeviceCMYK(): RGBBase(Type::CMYK, "DeviceCMYK", "CMYK", "color-selector-cmyk") {
        _svgNames.emplace_back("device-cmyk");
    }
    ~DeviceCMYK() override = default;

    /**
     * Convert the DeviceCMYK color into sRGB components used in the sRGB icc profile.
     *
     * See CSS Color Module Level 5, device-cmyk Uncalibrated conversion.
     *
     * @arg io - A vector of the input values, where the new values will be stored.
     */
    template<typename T, bool PremultipliedAlpha = false>
    inline static void spaceToProfile(T const *i, T *o)
    {
        double black;
        double white;

        if constexpr (PremultipliedAlpha) {
            black = i[3] / i[4];
            white = (1.0 - i[3]) / i[4];
        } else {
            black = i[3];
            white = 1.0 - black;
        }
        o[0] = 1.0 - std::min(1.0, i[0] * white + black);
        o[1] = 1.0 - std::min(1.0, i[1] * white + black);
        o[2] = 1.0 - std::min(1.0, i[2] * white + black);
    }

    /**
     * Convert from sRGB icc values to DeviceCMYK values
     *
     * See CSS Color Module Level 5, device-cmyk Uncalibrated conversion.
     *
     * @arg io - A vector of the input values, where the new values will be stored.
     */
    template<typename T, bool PremultiplyAlpha = false>
    inline static void profileToSpace(T const *i, T *o)
    {
        // Insert black channel at position 3
        o[3] = 1.0 - std::max(std::max(i[0], i[1]), i[2]);
        double const white = 1.0 - o[3];

        // Each channel is its color chart opposite (cyan->red) with a bit of white removed.
        o[0] = white ? (1.0 - i[0] - o[3]) / white : 0.0;
        o[1] = white ? (1.0 - i[1] - o[3]) / white : 0.0;
        o[2] = white ? (1.0 - i[2] - o[3]) / white : 0.0;

        if constexpr (PremultiplyAlpha) {
            o[0] = o[0] * i[4];
            o[1] = o[1] * i[4];
            o[2] = o[2] * i[4];
            o[3] = o[3] * i[4];
            o[4] = i[4];
        }
    }

    std::string toString(std::vector<double> const &values, bool opacity = true) const override;

    bool overInk(std::vector<double> const &input) const override;

    /**
     * PROOF: show DeviceCMYK through a CMYK profile instead of the uncalibrated formula, as CSS
     * Color 5's `@color-profile device-cmyk` does, and as Illustrator shows CMYK paint in a
     * document without a CMYK profile of its own: through its working CMYK space. The numbers
     * stay as they are; their colour on screen and in conversions follows the profile. A null
     * profile goes back to the formula, the default. Set from the main thread
     * (DocumentColors::showDeviceCMYK).
     */
    void setProfile(std::shared_ptr<Colors::CMS::Profile> profile, RenderingIntent intent);
    std::shared_ptr<Colors::CMS::Profile> const getProfile() const override;
    bool spaceToProfile(std::vector<double> &io) const override;
    bool profileToSpace(std::vector<double> &io) const override;

private:
    mutable CacheMutex _profile_mutex;
    std::shared_ptr<Colors::CMS::Profile> _profile;
};

} // namespace Inkscape::Colors::Space

#endif // SEEN_COLORS_SPACES_DEVICECMYK_H
