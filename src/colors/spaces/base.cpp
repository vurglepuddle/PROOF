// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Manage color spaces
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2023 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <iostream>

#include "base.h"

#include <sstream>
#include <utility>

#include "colors/color.h"
#include "colors/manager.h"
#include "components.h"
#include "gamut.h"

namespace Inkscape::Colors::Space {

/**
 * Construct a Color Space with all required data for display and use.
 *
 * @arg type       - The type of space, this will be the same as the end class
 * @arg components - The number of component channels in this color space
 * @arg name       - The common name for this color space
 * @arg shortName  - A shorter name used in drop down selections and other tight spaces
 * @arg icon       - An icon name which is used in many UI locations as a short hand for this space.
 * @arg spaceIsUnbounded - If true, this color space can have values larger and smaller than 0.0 to 1.0
 *
 * Further information about the components themselves are contained within components.cpp
 */
AnySpace::AnySpace(Type type, int components, std::string name, std::string shortName, std::string icon, bool spaceIsUnbounded)
    : _type(type)
    , _components(components)
    , _name(std::move(name))
    , _shortName(std::move(shortName))
    , _icon(std::move(icon))
    , _spaceIsUnbounded(spaceIsUnbounded)
{}

/**
 * Return true if the given data would be valid for this color space.
 */
bool AnySpace::isValidData(std::vector<double> const &values) const
{
    auto const n_values = values.size();
    auto const n_space = getComponentCount();
    return n_values == n_space || n_values == n_space + 1;
}

/**
 * Get the best rendering intent for this color space conversion.
 */
RenderingIntent AnySpace::getBestIntent(std::shared_ptr<AnySpace> const &to_space) const
{
    // Choose best rendering intent based on the intent priority
    auto intent = RenderingIntent::UNKNOWN;
    if (_intent_priority <= to_space->_intent_priority || getIntent() == RenderingIntent::UNKNOWN) {
        intent = to_space->getIntent();
    } else {
        intent = getIntent();
    }
    if (intent == RenderingIntent::UNKNOWN) {
        intent = RenderingIntent::PERCEPTUAL;
    }
    return intent;
}

/**
 * Convert the color into an RGBA32 for use within Gdk rendering.
 */
uint32_t AnySpace::toRGBA(std::vector<double> const &values, double opacity) const
{
    auto to_int32 = [opacity](std::vector<double> const &values) {
        switch (values.size()) {
            case 3:
                return SP_RGBA32_F_COMPOSE(values[0], values[1], values[2], opacity);
            case 4:
                return SP_RGBA32_F_COMPOSE(values[0], values[1], values[2], opacity * values[3]);
            default:
                throw ColorError("Color values should be size 3 for RGB or 4 for RGBA.");
        }
    };

    // We will always output sRGB for RGBA integers
    static auto srgb = Manager::get().find(Type::RGB);
    if (getType() != Type::RGB) {
        std::vector<double> copy = values;
        if (convert(copy, srgb)) {
            return to_int32(copy);
        }
        throw ColorError("Couldn't convert color space to sRGB.");
    }
    return to_int32(values);
}



/**
 * Return true if the color would be out of gamut in the target color space.
 *
 * NOTE: This can NOT work if the base color spaces are exactly the same. i.e. device-cmyk(sRGB)
 * will always return false despite not being reversible with RGB (which is also sRGB).
 *
 * If you want gamut checking via lcms2, you must use different icc profiles.
 *
 * @arg input - Channel values in this space.
 * @arg to_space - The target space to compare against.
 */
bool AnySpace::outOfGamut(std::vector<double> const &input, std::shared_ptr<AnySpace> to_space) const
{
    auto from_profile = getProfile();
    auto to_profile = to_space->getProfile();
    if (*to_profile == *from_profile)
        return false;
    // 1. Look in the checker cache for the color profile pair (PROOF: this space's own profile
    // can change, see DeviceCMYK)
    auto pair_id = from_profile->getChecksum() + to_profile->getChecksum();
    std::shared_ptr<Colors::CMS::GamutChecker> checker;
    {
        std::lock_guard lock(_gamut_checkers_mutex.mutex);
        auto &cached = _gamut_checkers[pair_id];
        if (!cached) {
            // 2. Create a new transform for this one way profile-pair
            cached = std::make_shared<Colors::CMS::GamutChecker>(from_profile, to_profile);
        }
        checker = cached;
    }
    return checker->check_gamut(input);
}

bool AnySpace::isOutOfGamut(const Colors::Color& color, double eps) {
    return out_of_gamut(color, shared_from_this(), eps);
}

Color AnySpace::toGamut(const Colors::Color& color) {
    // By default apply CSS Level 4 gamut mapping: https://www.w3.org/TR/css-color-4/#gamut-mapping
    // This approach will match behavior of browsers.
    // If we had an ICC profile selected, we could use that instead.
    return to_gamut_css(color, shared_from_this());
}

/**
 * Return a list of Component objects, in order of the channels in this color space
 *
 * @arg alpha - If true, returns the alpha component as well
 */
Components const &AnySpace::getComponents(bool alpha) const
{
    return Space::Components::get(getComponentType(), alpha);
}

} // namespace Inkscape::Colors::Space
