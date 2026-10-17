// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_LIBNRTYPE_TAB_STOPS_H
#define INKSCAPE_LIBNRTYPE_TAB_STOPS_H

#include <optional>
#include <vector>

namespace Inkscape::XML { class Node; }

namespace Inkscape::Text {

/** Left-aligned paragraph stops, in text layout units. The origin is the
 * paragraph ruler's zero, independent of the first line's indent. */
struct LeftTabStops
{
    double origin = 0;
    double interval = 36;
    std::vector<double> positions;

    bool valid() const;
    double next(double x) const;
    void write(XML::Node *repr) const;
    static std::optional<LeftTabStops> read(XML::Node const *repr);
    static bool is_attribute(char const *name);
    static void copy_attributes(XML::Node *to, XML::Node const *from);
    bool operator==(LeftTabStops const &) const = default;
};

} // namespace Inkscape::Text
#endif
