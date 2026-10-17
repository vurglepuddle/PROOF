// SPDX-License-Identifier: GPL-2.0-or-later
#include "tab-stops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <glib.h>

#include "xml/node.h"

namespace Inkscape::Text {
namespace {
constexpr char const *attributes[] = {"proof:tab-origin", "proof:tab-interval", "proof:tab-stops"};

bool number(char const *text, double &value)
{
    if (!text || std::strlen(text) > 64) return false;
    char *end = nullptr;
    value = g_ascii_strtod(text, &end);
    if (end == text) return false;
    while (g_ascii_isspace(*end)) ++end;
    return !*end && std::isfinite(value);
}

std::string decimal(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    return g_ascii_dtostr(buffer, sizeof(buffer), value);
}
} // namespace

bool LeftTabStops::valid() const
{
    if (!std::isfinite(origin) || std::abs(origin) > 1e9 ||
        !std::isfinite(interval) || interval < 1e-6 || interval > 1e9 || positions.size() > 1024) return false;
    double previous = -1;
    for (auto position : positions) {
        if (!std::isfinite(position) || position < 0 || position > 1e9 || position <= previous) return false;
        previous = position;
    }
    return true;
}

double LeftTabStops::next(double x) const
{
    auto const relative = x - origin;
    auto const explicit_stop = std::upper_bound(positions.begin(), positions.end(), relative + 1e-7);
    if (explicit_stop != positions.end()) return origin + *explicit_stop;
    // Defaults resume on the ruler's original grid, not a grid starting at
    // the last explicit stop (Illustrator synthetic controls, 2026-10-17).
    return origin + interval * (std::floor((relative + 1e-7) / interval) + 1);
}

std::optional<LeftTabStops> LeftTabStops::read(XML::Node const *repr)
{
    if (!repr || !repr->attribute(attributes[1])) return {};
    LeftTabStops out;
    if (!number(repr->attribute(attributes[1]), out.interval)) return {};
    if (auto origin = repr->attribute(attributes[0]); origin && !number(origin, out.origin)) return {};
    if (auto stops = repr->attribute(attributes[2])) {
        if (std::strlen(stops) > 32768) return {};
        while (*stops) {
            while (g_ascii_isspace(*stops)) ++stops;
            if (!*stops) break;
            char *end = nullptr;
            auto const value = g_ascii_strtod(stops, &end);
            if (end == stops || (*end && !g_ascii_isspace(*end)) || out.positions.size() >= 1024) return {};
            out.positions.push_back(value);
            stops = end;
        }
    }
    return out.valid() ? std::optional(out) : std::nullopt;
}

void LeftTabStops::write(XML::Node *repr) const
{
    repr->setAttribute(attributes[0], decimal(origin));
    repr->setAttribute(attributes[1], decimal(interval));
    std::string stops;
    for (auto position : positions) stops += (stops.empty() ? "" : " ") + decimal(position);
    repr->setAttribute(attributes[2], stops);
}

bool LeftTabStops::is_attribute(char const *name)
{
    return std::any_of(std::begin(attributes), std::end(attributes),
                       [name](auto attribute) { return !std::strcmp(name, attribute); });
}

void LeftTabStops::copy_attributes(XML::Node *to, XML::Node const *from)
{
    if (to != from) for (auto attribute : attributes) to->setAttribute(attribute, from->attribute(attribute));
}
} // namespace Inkscape::Text
