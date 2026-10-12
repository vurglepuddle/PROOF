// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOL_LIVE_CORNERS_H
#define INKSCAPE_UI_TOOL_LIVE_CORNERS_H

#include <2geom/pathvector.h>
#include <optional>
#include <string>
#include <vector>

namespace Inkscape::UI::LiveCorners {

inline constexpr char attribute[] = "inkscape:live-corners";
enum class Style { Round, Inverted, Chamfer };
enum class Rounding { Absolute, Relative };

struct Corner {
    std::size_t path = 0, node = 0;
    double radius = 0;
    Style style = Style::Round;
    Rounding rounding = Rounding::Absolute;
    double handle_factor = 1; // Preserve a native rectangle's cubic approximation.
};

struct Geometry {
    Geom::Point anchor, direction;
    double angle = 0, maximum = 0;
};

// The SVG's d contains the expanded, editable outline. The source and corner
// parameters are auxiliary data, never a substitute for the visible geometry.
class Model {
public:
    Geom::PathVector source;
    Geom::Affine frame; // Object edits may bake a transform into d.
    std::vector<Corner> corners;

    static Model fromPath(Geom::PathVector const &path);
    static std::optional<Model> read(char const *text);
    std::string write() const;
    std::optional<Geometry> geometry(Corner const &corner) const;
    Geom::PathVector render() const;
    Geom::PathVector roundedCurves(std::vector<std::size_t> const &selected) const;
    // Preserve intact corners on a normal node edit; bake only damaged ones.
    // A whole-path translation moves the source and retains all parameters.
    bool reconcile(Geom::PathVector const &edited);
    bool hasRounding() const;
    // Apply one common amount without overlapping a neighbouring corner.
    double setRadius(std::vector<std::size_t> const &selected, double requested);
    std::vector<std::vector<Geom::Point>> nodePoints() const;

private:
    bool reconcileTopology(Geom::PathVector const &edited);
    struct Piece {
        std::size_t source_curve = 0;
        std::optional<std::size_t> corner;
        double from = 0, to = 1;
    };
    struct Rendered {
        Geom::PathVector path;
        std::vector<std::vector<Piece>> pieces;
    };
    Rendered build() const;
};

} // namespace Inkscape::UI::LiveCorners
#endif
