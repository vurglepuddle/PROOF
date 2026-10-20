// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   Martin Owens <doctormo@geek-2.com>
 *
 * Copyright (C) 2023-2025 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_COLORS_SPACES_BASE_H
#define SEEN_COLORS_SPACES_BASE_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <iostream>

#include "colors/parser.h"
#include "enum.h"
#include "colors/cms/profile.h"
#include "colors/cms/transform-color.h"

constexpr double SCALE_UP(double v, double a, double b)
{
    return (v * (b - a)) + a;
}
constexpr double SCALE_DOWN(double v, double a, double b)
{
    return (v - a) / (b - a);
}

namespace Inkscape::Colors {
namespace CMS {
class TransformColor;
class GamutChecker;
} // namespace CMS
class Color;
class Manager;

namespace Space {

class Components;

/// PROOF: a lock for a space's caches; a copy of the space gets a lock of its own.
struct CacheMutex
{
    CacheMutex() = default;
    CacheMutex(CacheMutex const &) {}
    CacheMutex &operator=(CacheMutex const &) { return *this; }
    std::mutex mutex;
};

class AnySpace : public std::enable_shared_from_this<AnySpace>
{
public:
    virtual ~AnySpace() = default;

    bool operator==(AnySpace const &other) const { return other.getName() == getName(); }
    bool operator!=(AnySpace const &other) const { return !(*this == other); };

    // Each space has a unique type enum for easier static use, use getComponentType
    // for seeing the internal type of the space, for example in the CMS space.
    bool operator==(Type type) const { return type == _type; }
    bool operator!=(Type type) const { return type != _type; }

    Type getType() const { return _type; }
    std::string const& getName() const { return _name; }
    std::string const& getShortName() const { return _shortName; }
    std::string getSvgName() const { return _svgNames.empty() ? "" : _svgNames[0]; }
    std::vector<std::string> const& getSvgNames() const { return _svgNames; }
    std::string const& getIcon() const { return _icon; }
    unsigned int getComponentCount() const { return _components; }
    virtual Type getComponentType() const { return getType(); }
    virtual std::shared_ptr<Colors::CMS::Profile> const getProfile() const = 0;
    RenderingIntent getIntent() const { return _intent; }
    RenderingIntent getBestIntent(std::shared_ptr<AnySpace> const &to_space) const;
    // Specifies if this color space can be used for color interpolation in the render engine.
    virtual bool canInterpolateColors() const { return true; }
    // Some color spaces (like XYZ or LAB) do not put restrictions on valid ranges of values;
    // others (like sRGB) do, which means that channels outside those bounds represent colors out of gamut.
    bool isUnbounded() const { return _spaceIsUnbounded; }
    // Check if 'color' is out of gamut in '*this' color space;
    // use epsilon value to ignore some small deviations from valid domain (they can arise during conversions).
    bool isOutOfGamut(const Colors::Color& color, double eps = 0.0001);
    // Bring 'color' into gamut of '*this' color space
    Color toGamut(const Colors::Color& color);

    Components const &getComponents(bool alpha = false) const;
    std::string const getPrefsPath() const { return "/colorselector/" + getName() + "/"; }

    /**
     * Returns true almost always. Only CMS could have a different return value.
     */
    virtual bool hasValidCmsProfile() const { return true; }

    virtual bool spaceToProfile(std::vector<double> &io) const = 0;
    virtual bool profileToProfile(std::vector<double> &io, std::shared_ptr<Colors::CMS::Profile> const &profile, RenderingIntent intent) const = 0;
    virtual bool profileToSpace(std::vector<double> &io) const = 0;

    /**
     * Convert between any two color spaces and their profiles.
     */
    bool convert(std::vector<double> &io, std::shared_ptr<AnySpace> to_space) const
    {
        return convert(io, to_space, getBestIntent(to_space));
    }
    bool convert(std::vector<double> &io, std::shared_ptr<AnySpace> to_space, RenderingIntent intent) const
    {
        return spaceToProfile(io)
            && profileToProfile(io, to_space->getProfile(), intent)
            && to_space->profileToSpace(io);
    }

protected:
    friend class Colors::Color;

    AnySpace(Type type, int components, std::string name, std::string shortName, std::string icon, bool spaceIsUnbounded = false);

    bool isValidData(std::vector<double> const &values) const;
    virtual std::string toString(std::vector<double> const &values, bool opacity = true) const = 0;
    uint32_t toRGBA(std::vector<double> const &values, double opacity = 1.0) const;

    virtual bool overInk(std::vector<double> const &input) const { return false; }
    bool outOfGamut(std::vector<double> const &input, std::shared_ptr<AnySpace> to_space) const;

    RenderingIntent _intent = RenderingIntent::UNKNOWN;
    int _intent_priority = 0;
    std::vector<std::string> _svgNames;
private:
    mutable CacheMutex _gamut_checkers_mutex;
    mutable std::map<std::string, std::shared_ptr<Colors::CMS::GamutChecker>> _gamut_checkers;
    Type _type;
    int _components;
    std::string _name;
    std::string _shortName;
    std::string _icon;
    bool _spaceIsUnbounded;
};

template <bool isDirect = true>
class ProfileSpace : public AnySpace
{
public:
    ProfileSpace(Type type, int channels, std::string name, std::string shortName, std::string icon, bool spaceIsUnbounded = false)
        : AnySpace(type, channels, std::move(name), std::move(shortName), std::move(icon), spaceIsUnbounded)
    {}

    bool spaceToProfile(std::vector<double> &io) const override { return true; }
    bool profileToSpace(std::vector<double> &io) const override { return true; }
    bool profileToProfile(std::vector<double> &io, std::shared_ptr<Colors::CMS::Profile> const &to_profile, RenderingIntent intent) const override
    {
        auto from_profile = getProfile();
        if (*from_profile == *to_profile)
            return true;
        auto to_size = to_profile->getSize();
        auto from_size = from_profile->getSize();
        bool has_alpha = io.size() == from_size + 1;
        if (auto tr = makeTransform(from_profile, to_profile, intent)) {
            while (io.size() - has_alpha < to_size) io.insert(io.begin() + from_size, 0.0);
            auto ret = tr->do_transform(io, io);
            io.erase(io.begin() + to_size, io.end() - has_alpha);
            return ret;
        }
        return false;
    }
private:
    /**
     * Construct an lcms2 transform and cache the results for later use.
     */
    std::shared_ptr<Colors::CMS::TransformColor> makeTransform(std::shared_ptr<Colors::CMS::Profile> const &from_profile,
                                                               std::shared_ptr<Colors::CMS::Profile> const &to_profile,
                                                               RenderingIntent intent) const
    {
        // PROOF: keyed by both profiles, as a space's own profile can change (DeviceCMYK shown
        // through a working profile), and locked, as canvas tiles are drawn on several threads.
        std::lock_guard lock(_transforms_mutex.mutex);
        auto &transform = _transforms[{from_profile->getChecksum() + to_profile->getChecksum(), intent}];
        if (!transform) {
            // Create a new transform for this one way profile-pair
            transform = std::make_shared<Colors::CMS::TransformColor>(from_profile, to_profile, intent);
        }
        return transform;
    }

    mutable CacheMutex _transforms_mutex;
    mutable std::map<std::pair<std::string, RenderingIntent>, std::shared_ptr<Colors::CMS::TransformColor>> _transforms;
};

template <typename T>
class ConvertableSpace : public ProfileSpace<false>
{
public:
    using TargetSpace = T;

    ConvertableSpace(Type type, std::string name, std::string shortName, std::string icon, bool spaceIsUnbounded = false)
        : ProfileSpace(type, TargetSpace::OutputChannels, std::move(name), std::move(shortName), std::move(icon), spaceIsUnbounded)
    {}

    bool spaceToProfile(std::vector<double> &io) const override
    {
        bool has_alpha = io.size() == TargetSpace::OutputChannels + 1;
        if constexpr (TargetSpace::OutputChannels < TargetSpace::ProfileChannels) {
            while (io.size() - has_alpha < TargetSpace::ProfileChannels) {
                io.insert(io.end() - has_alpha, 0.0);
            }
        }
        T::spaceToProfile(io.data(), io.data());
        if constexpr (TargetSpace::OutputChannels > TargetSpace::ProfileChannels) {
            io.erase(io.begin() + TargetSpace::ProfileChannels, io.end() - has_alpha);
        }
        return true;
    }
    bool profileToSpace(std::vector<double> &io) const override
    {
        bool has_alpha = io.size() == TargetSpace::ProfileChannels + 1;
        if constexpr (TargetSpace::ProfileChannels < TargetSpace::OutputChannels) {
            while (io.size() - has_alpha < TargetSpace::OutputChannels) {
                io.insert(io.end() - has_alpha, 0.0);
            }
        }
        T::profileToSpace(io.data(), io.data());
        if constexpr (TargetSpace::ProfileChannels > TargetSpace::OutputChannels) {
            io.erase(io.begin() + TargetSpace::OutputChannels, io.end() - has_alpha);
        }
        return true;
    }
};

} // namespace Space
} // namespace Inkscape::Colors

#endif // SEEN_COLORS_SPACES_BASE_H
