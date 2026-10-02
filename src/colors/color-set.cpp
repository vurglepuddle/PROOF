// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * A set of colors which can be modified together used for color pickers
 *//*
 * Copyright (C) 2024 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "color-set.h"

#include <algorithm>
#include <ranges>
#include <unordered_map>

#include "colors/color.h"
#include "colors/spaces/base.h"
#include "colors/spaces/components.h"

namespace Inkscape::Colors {

/**
 * Construct a new ColorSet object to contain a group of colors which will
 * be modified collectively.
 *
 * @arg space - optionally constrain each color added to be in this color space
 * @arg alpha - optionally constrain each color to have or not have an alpha channel
 */
ColorSet::ColorSet(std::shared_ptr<Space::AnySpace> space, std::optional<bool> alpha)
    : _space_constraint(std::move(space))
    , _alpha_constraint(alpha)
{}

/**
 * Get a list of components for the color space set to this color set.
 */
Space::Components const &ColorSet::getComponents() const
{
    if (!_space_constraint)
        throw ColorError("Components are only available on a color space constrained ColorSet.");
    return _space_constraint->getComponents(_alpha_constraint && *_alpha_constraint);
}

/**
 * Reset the color set and remove all colors from it.
 */
void ColorSet::clear()
{
    if (!_colors.empty()) {
        _colors.clear();
        _colors_index.clear();
        colors_cleared();
    }
}

/**
 * Set this color to being grabbed for a continuous set of changes.
 */
void ColorSet::grab()
{
    if (!_blocked && !_grabbed) {
        block();
        signal_grabbed.emit();
        unblock();
        _grabbed = true;
    }
}

/**
 * Set the color as being released from continuous changes.
 */
void ColorSet::release()
{
    if (!_blocked && _grabbed) {
        _grabbed = false;
        block();
        signal_released.emit();
        unblock();
    }
}

/**
 * Called when the colors change in some way.
 */
void ColorSet::colors_changed()
{
    // Send signals about this change
    if (!_blocked) {
        block();
        signal_changed.emit();
        unblock();
    }
}

/**
 * Called when the list of colors changes (add or clear)
 */
void ColorSet::colors_cleared()
{
    if (!_blocked) {
        block();
        signal_cleared.emit();
        unblock();
    }
}

/**
 * Returns true if all of the colors are the same color.
 */
bool ColorSet::isSame() const
{
    if (_colors.empty())
        return true;
    for (auto const &[id, color] : _colors) {
        if (color != _colors[0].second)
            return false;
    }
    return true;
}

/**
 * Overwrite all colors so they equal the the new color
 *
 * @arg other - The other color to replace all colors in this set with.
 *
 * @returns the number of colors changed.
 */
unsigned ColorSet::setAll(Color const &other)
{
    unsigned changed = 0;
    for (auto &[id, color] : _colors) {
        auto was = color;
        color.set(other, true);
        // Comparison after possible color space conversion
        changed += (was != color);
    }
    if (changed) {
        colors_changed();
    }
    return changed;
}

/**
 * Set each of the colors from the other color set by id. Creating new entries where the id is not found.
 *
 * @arg other - The other color set to find colors from.
 *
 * @returns the number of colors changed or added
 */
unsigned ColorSet::setAll(ColorSet const &other)
{
    unsigned changed = 0;
    for (auto &[id, color] : other) {
        changed += _set(id, color);
    }
    if (changed > 0) {
        colors_changed();
    }
    return changed;
}

/**
 * Set a single color in the color set by its id.
 *
 * @arg id - The id to use, if this id isn't set a new item is created.
 * @arg other - The other color to replace all colors in this set with.
 *
 * @returns true if this color was changed.
 */
bool ColorSet::set(std::string id, Color const &other)
{
    if (_set(std::move(id), other)) {
        colors_changed();
        return true;
    }
    return false;
}

/**
 * Remove any other colors and set to just this one color.
 *
 * @arg color - The color to set this color-set to.
 *
 * @returns true if the color was new or changed.
 */
bool ColorSet::set(Color const &other)
{
    // Always clear the colors if it's being used differently
    if (_colors.size() != 1 || _colors[0].first != "single") {
        _colors.clear();
        _colors_index.clear();
    }
    return set("single", other);
}

/**
 * Get the color if there is only one color set with set(Color)
 *
 * @returns the available color, normalized
 */
std::optional<Color> ColorSet::get() const
{
    return get("single");
}

/*
 * Internal function for setting a color by id without calling the changed signal.
 */
bool ColorSet::_set(std::string const &id, Color const &other)
{
    // Loading a selection must retain its color space, not the previous
    // selection's space. Only explicit constraints may convert incoming data.
    Color copy = other;

    // Enforce constraints on space and alpha if any
    if (_space_constraint)
        copy.convert(_space_constraint);

    if (_alpha_constraint) {
        copy.enableOpacity(*_alpha_constraint);
    }

    auto it = _colors_index.find(id);
    if (it != _colors_index.end()) {
        return _colors[it->second].second.set(copy, false);
    }

    // Add a new entry for this id
    size_t pos = _colors.size();
    _colors.emplace_back(id, copy);
    _colors_index.emplace(_colors.back().first, pos);

    return true;
}

/**
 * Return a single color by it's index. The color will be normalized
 * before returning a copy as some functions can modify colors out
 * of bounds.
 *
 * @arg index - The id of the color used in ColorSet::set()
 *
 * @returns a normalized color object from the given index or none if the id is not found.
 */
std::optional<Color> ColorSet::get(std::string const &id) const
{
    auto it = _colors_index.find(id);
    if (it != _colors_index.end()) {
        auto &color = _colors[it->second].second;
        return color.normalized();
    }
    return {};
}

/**
 * Set this one component to this specific value for all colors.
 *
 * @arg index - The index to replace, will cause an error if out of bounds.
 * @arg other - The other color to replace all colors in this set with.
 *
 * @returns the number of colors changed.
 */
unsigned ColorSet::setAll(Space::Component const &c, double value)
{
    if (!isValid(c)) {
        throw ColorError("Incompatible color component used in ColorSet::set.");
    }
    unsigned changed = 0;
    for (auto &[id, color] : _colors) {
        auto was = color;
        color.set(c.index, value);
        // Comparison after possible color space conversion
        changed += (was != color);
    }
    if (changed) {
        colors_changed();
    }
    return changed;
}

/**
 * Set the average value in this component by taking the average
 * finding the delta and moving all colors by the given delta.
 *
 * This will not run normalization so out of bound changes can remember
 * their values until the mutation period is finished and normalization
 * is run on the returned colors. see ColorSet::get().
 *
 * @arg c - The component to change the average for
 * @arg value - The new average value that the set will calculate to
 */
void ColorSet::setAverage(Space::Component const &c, double value)
{
    if (!_space_constraint || _space_constraint->getComponentType() != c.type)
        throw ColorError("Incompatible color component used in ColorSet::moveAverageTo.");
    auto delta = value - getAverage(c);
    for (auto &[id, color] : _colors) {
        color.set(c.index, color[c.index] + delta);
    }
    colors_changed();
}

/**
 * Get the average value for this component across all colors.
 *
 * @arg c - The component to get the average for
 *
 * @returns the normalized average value for all colors in this set.
 */
double ColorSet::getAverage(Space::Component const &c) const {
    if (!isValid(c)) {
        throw ColorError("Incompatible color component used in ColorSet::get.");
    }
    double value = 0.0;
    for (auto const &[id, color] : _colors) {
        value += color[c.index];
    }
    return c.normalize(value / _colors.size());
}

/**
 * Get a list of all normalized values for this one component.
 *
 * @arg c - The component to collect all the values for.
 *
 * @returns a list of normalized values in this component.
 */
std::vector<double> ColorSet::getAll(Space::Component const &c) const
{
    if (!isValid(c)) {
        throw ColorError("Incompatible color component used in ColorSet::getAll.");
    }
    std::vector<double> ret(_colors.size());
    std::transform(_colors.begin(), _colors.end(), ret.begin(),
                   [c](auto &iter) { return c.normalize(iter.second[c.index]); });
    return ret;
}

/**
 * Return the best color space from this collection of colors. If the color
 * space is constrained then the result will be that space. Otherwise picks
 * the space with the most colors.
 */
std::shared_ptr<Space::AnySpace> ColorSet::getBestSpace() const
{
    if (_space_constraint)
        return _space_constraint;

    unsigned biggest = 0;
    std::shared_ptr<Space::AnySpace> ret;
    std::unordered_map<Space::AnySpace*, unsigned> counts;

    for (auto const &color : _colors | std::views::values) {
        auto space = color.getSpace().get();
        auto& count = counts[space];

        if (++count > biggest) {
            biggest = count;
            ret = color.getSpace();
        }
    }

    return ret;
}

bool ColorSet::isValid(const Space::Component& component) const {
    return _space_constraint && _space_constraint->getComponentType() == component.type;
}

/**
 * Return the average color between this set of colors.
 *
 * If space is not constrained, it will return the average in the best color space.
 * If alpha is not constrained, the average will always include an alpha channel
 */
Color ColorSet::getAverage() const
{
    if (isEmpty())
        throw ColorError("Can't get the average color of no colors.");

    auto avg_space = getBestSpace();
    auto avg_alpha = _alpha_constraint.value_or(true);

    std::vector<double> values(avg_space->getComponentCount() + avg_alpha);

    for (auto const &[id, color] : _colors) {
        // Colors::Color will return the alpha channel as 1.0 if it doesn't exist.
        if (color.getSpace() == avg_space) {
            for (unsigned int i = 0; i < values.size(); i++) {
                values[i] += color[i];
            }
        } else if (auto copy = color.converted(avg_space)) {
            for (unsigned int i = 0; i < values.size(); i++) {
                values[i] += (*copy)[i];
            }
        }
    }
    for (double &value : values) {
        value /= _colors.size();
    }
    return Color(avg_space, values);
}

/**
 * Return the number of items in the color set.
 */
unsigned ColorSet::size() const
{
    return _colors.size();
}

} // namespace Inkscape::Colors

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
