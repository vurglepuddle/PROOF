// SPDX-License-Identifier: GPL-2.0-or-later
#include "gradient-rendering.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>

#include "colors/color.h"
#include "colors/document-colors.h"
#include "colors/manager.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cms.h"
#include "gradient-perceptual.h"
#include "sp-gradient.h"
#include "sp-stop.h"
#include "spot-ink.h"

namespace Inkscape::GradientRendering {
namespace {

using Type = Colors::Space::Type;

/// Samples between two stops whose inks differ by a whole channel; fewer for a smaller step. Cairo
/// joins them in RGB; with these, the ramps of Illustrator's controls agree with its own within 1
/// to 4 levels over most of their length (PROOF-document-colors.md).
constexpr double MOST_SAMPLES = 64;

/// A long vector (an imported gradient with its midpoints sampled) gets fewer samples a stop.
constexpr double SAMPLE_BUDGET = 8192;

/// Cuts along each side of a mesh patch whose corners differ by a whole channel; fewer for less.
constexpr double MOST_CUTS = 8;

using Ink = std::array<double, 4>;

/// The document's CMYK: its profile's, or the working CMYK profile's when it has none.
std::shared_ptr<Colors::Space::AnySpace> document_cmyk(SPDocument *document)
{
    if (Colors::DocumentColors::mode(document) != Type::CMYK) {
        return {};
    }
    std::shared_ptr<Colors::Space::AnySpace> target = Colors::DocumentColors::assignedSpace(document);
    return target ? target : Colors::Manager::get().find(Type::CMYK);
}

/// A colour's inks in the document's CMYK: a CMYK colour's own numbers, as the document reads
/// them (interpret); other paint converted, as Illustrator converts it into a CMYK document.
std::optional<Ink> inks(SPDocument *document, std::shared_ptr<Colors::Space::AnySpace> const &target,
                        Colors::Color const &paint)
{
    auto const color = Colors::DocumentColors::interpret(document, paint);
    auto const cmyk = color.getSpace()->getComponentType() == Type::CMYK ? std::optional(color)
                                                                          : color.converted(target);
    if (!cmyk || cmyk->getValues().size() < 4) {
        return {};
    }
    auto const &v = cmyk->getValues();
    return Ink{v[0], v[1], v[2], v[3]};
}

Colors::Color ink_color(std::shared_ptr<Colors::Space::AnySpace> const &target, Ink const &ink, double opacity)
{
    auto color = Colors::Color(target, {ink[0], ink[1], ink[2], ink[3]});
    color.setOpacity(opacity);
    return color;
}

/// The ink on each stop of the vector, read from its stop element. The vector holds the stop
/// elements of the gradient that has them, in order, with a copy of the first at 0 and of the last
/// at 1 when they don't reach the ends (SPGradient::rebuildVector).
std::vector<std::optional<SpotInk::Ink>> stop_inks(SPGradient const *gradient, SPGradientVector const &vector)
{
    std::vector<std::optional<SpotInk::Ink>> result(vector.stops.size());
    auto const *source = gradient->getVector();
    std::vector<SPStop const *> stops;
    for (auto const &child : source->children) {
        if (auto const stop = cast<SPStop>(&child)) {
            stops.push_back(stop);
        }
    }
    if (stops.empty()) {
        return result;
    }
    std::size_t const lead = std::clamp<double>(stops.front()->offset, 0, 1) > 0 ? 1 : 0;
    if (vector.stops.size() < stops.size() + lead || vector.stops.size() > stops.size() + lead + 1) {
        return result; // Not the vector of these stops.
    }
    for (std::size_t i = 0; i < vector.stops.size(); ++i) {
        auto const k = std::min<std::size_t>(i < lead ? 0 : i - lead, stops.size() - 1);
        result[i] = SpotInk::read(stops[k]->getRepr());
    }
    return result;
}

/// Whether an ink stands beside process colour (or beside another ink) in the vector: what
/// Illustrator draws overprinting.
bool ink_beside_process(std::vector<std::optional<SpotInk::Ink>> const &inks)
{
    std::optional<std::string> name;
    bool process = false, other_ink = false;
    for (auto const &ink : inks) {
        if (!ink) {
            process = true;
        } else if (!name) {
            name = ink->name;
        } else if (*name != ink->name) {
            other_ink = true;
        }
    }
    return name && (process || other_ink);
}

/// Colour as amounts of colorant: the inks themselves in a CMYK document, 1 - each channel in an
/// RGB one (what a channel takes away from white paper).
using Amounts = std::array<double, 4>;

/// What a stop holds as Illustrator paints it: the tints of its inks and its process colour.
struct Channels
{
    std::map<std::string, double> tints;
    Amounts process{};
};

} // namespace

/**
 * PROOF: an ink beside process colour, as Illustrator draws it. A CMYK document's page paints such
 * a gradient in DeviceN, its tint transform giving each CMYK channel 1 - (1 - tint x the ink's
 * CMYK)(1 - process), the tints and process channels blended linearly: the ink overprinting the
 * process ramp. An RGB document's page paints the ink's Separation shading and the RGB shading
 * over it with overprint on (`/MultiChannelPaint`), which Illustrator shows as the ink's tint
 * ramp multiplied over the process ramp, each against white: the same rule in what each channel
 * takes away from white. The Separation's tints are the ink's own: its inks scaled for a CMYK
 * definition, blended towards white in Lab or RGB (SpotInk::display_color). A blend of the
 * stops' colours is up to 26 levels from Illustrator's renders, darker mid-ramp
 * (PROOF-document-colors.md, "An ink beside process colour").
 */
static SPGradientVector ink_blend(SPDocument *document, SPGradientVector const &vector,
                                  std::vector<std::optional<SpotInk::Ink>> const &stop_ink)
{
    bool const cmyk = Colors::DocumentColors::mode(document) == Type::CMYK;
    auto const target = cmyk ? document_cmyk(document) : Colors::Manager::get().find(Type::RGB);
    if (!target) {
        return vector;
    }
    unsigned const count = cmyk ? 4 : 3;
    auto const amounts = [&](Colors::Color const &color) -> std::optional<Amounts> {
        if (cmyk) {
            return inks(document, target, color);
        }
        auto const rgb = color.converted(Type::RGB);
        if (!rgb || rgb->getValues().size() < 3) {
            return {};
        }
        auto const &v = rgb->getValues();
        return Amounts{1 - v[0], 1 - v[1], 1 - v[2], 0};
    };

    // Each ink's definition, and what it takes away at a tint: in a CMYK document the tint times
    // its inks at full strength, as the DeviceN tint transform has it; in an RGB document the
    // Separation's own tint (SpotInk::display_color).
    std::map<std::string, Colors::Color> definition;
    std::map<std::string, Amounts> full;
    auto const at_tint = [&](std::string const &name, double tint) -> Amounts {
        if (cmyk) {
            auto const &f = full.at(name);
            return {f[0] * tint, f[1] * tint, f[2] * tint, f[3] * tint};
        }
        return amounts(SpotInk::display_color(definition.at(name), tint)).value_or(Amounts{});
    };
    std::vector<Channels> channels;
    for (std::size_t i = 0; i < vector.stops.size(); ++i) {
        Channels c;
        if (auto const &ink = stop_ink[i]) {
            if (!full.contains(ink->name)) {
                auto const strength = amounts(ink->alternate);
                if (!strength) {
                    return vector;
                }
                full[ink->name] = *strength;
                definition.emplace(ink->name, ink->alternate);
            }
            c.tints[ink->name] = ink->tint;
        } else {
            auto const process = vector.stops[i].color ? amounts(*vector.stops[i].color) : std::nullopt;
            if (!process) {
                return vector;
            }
            c.process = *process;
        }
        channels.push_back(std::move(c));
    }

    auto const shown = [&](Channels const &c, double opacity) {
        Amounts paper;
        for (unsigned j = 0; j < 4; ++j) {
            paper[j] = 1 - c.process[j];
        }
        for (auto const &[name, tint] : c.tints) {
            auto const taken = at_tint(name, tint);
            for (unsigned j = 0; j < 4; ++j) {
                paper[j] *= 1 - taken[j];
            }
        }
        std::vector<double> values(count);
        for (unsigned j = 0; j < count; ++j) {
            values[j] = cmyk ? 1 - paper[j] : paper[j];
        }
        auto color = Colors::Color(target, std::move(values));
        color.setOpacity(opacity);
        return color;
    };
    auto const between = [](Channels const &a, Channels const &b, double t) {
        Channels mix;
        for (unsigned j = 0; j < 4; ++j) {
            mix.process[j] = a.process[j] + (b.process[j] - a.process[j]) * t;
        }
        for (auto const &[name, tint] : a.tints) {
            mix.tints[name] += tint * (1 - t);
        }
        for (auto const &[name, tint] : b.tints) {
            mix.tints[name] += tint * t;
        }
        return mix;
    };

    double const most = std::clamp(SAMPLE_BUDGET / vector.stops.size(), 1.0, MOST_SAMPLES);
    auto result = vector;
    result.stops.clear();
    for (std::size_t i = 0; i < vector.stops.size(); ++i) {
        auto const &a = vector.stops[i];
        double const opacity_a = a.color ? a.color->getOpacity() : 1;
        result.stops.push_back({shown(channels[i], opacity_a), a.offset});
        if (i + 1 == vector.stops.size()) {
            break;
        }
        auto const &b = vector.stops[i + 1];
        if (b.offset <= a.offset) {
            continue; // A hard edge.
        }
        double const opacity_b = b.color ? b.color->getOpacity() : 1;
        auto const &ca = channels[i], &cb = channels[i + 1];
        bool const same = ca.process == cb.process && ca.tints == cb.tints;
        auto const divisions = same ? 1u : static_cast<unsigned>(most);
        for (unsigned d = 1; d < divisions; ++d) {
            double const t = double(d) / divisions;
            result.stops.push_back({shown(between(channels[i], channels[i + 1], t), opacity_a + (opacity_b - opacity_a) * t),
                                    a.offset + (b.offset - a.offset) * t});
        }
    }
    return result;
}

namespace {

using Cubic = std::array<Geom::Point, 4>;
using Grid = std::array<Cubic, 4>; ///< A tensor patch's control points, [row][column].

/// The part of a cubic from `a` to `b`, by de Casteljau.
Cubic part(Cubic const &c, double a, double b)
{
    auto split = [](Cubic const &q, double t, bool first) {
        auto const l = [t](Geom::Point const &p, Geom::Point const &r) { return p + (r - p) * t; };
        auto const p01 = l(q[0], q[1]), p12 = l(q[1], q[2]), p23 = l(q[2], q[3]);
        auto const p012 = l(p01, p12), p123 = l(p12, p23), m = l(p012, p123);
        return first ? Cubic{q[0], p01, p012, m} : Cubic{m, p123, p23, q[3]};
    };
    auto const head = b < 1 ? split(c, b, true) : c;
    return a > 0 ? split(head, a / b, false) : head;
}

/// The tensor grid of a patch as Cairo takes it: corner Ck starts side k, which runs to C(k+1);
/// the inner point k is the one next to Ck; one not set is the Coons patch's (ISO 32000, as
/// Cairo computes it). Rows run from side 0 (C0 to C1) to side 2, columns from side 3 to side 1.
Grid tensor_grid(SPGradientPatch const &patch)
{
    std::array<Cubic, 4> sides;
    for (unsigned k = 0; k < 4; ++k) {
        auto const start = k == 0 ? patch.points[0][0] : sides[k - 1][3];
        auto const end = patch.points[k][3];
        if (patch.pathtype[k] && std::strchr("lLzZ", patch.pathtype[k])) {
            sides[k] = {start, start + (end - start) / 3, start + (end - start) * 2 / 3, end};
        } else {
            sides[k] = {start, patch.points[k][1], patch.points[k][2], end};
        }
    }
    Grid g;
    for (unsigned i = 0; i < 4; ++i) {
        g[0][i] = sides[0][i];
        g[i][3] = sides[1][i];
        g[3][3 - i] = sides[2][i];
        g[3 - i][0] = sides[3][i];
    }
    auto const coons = [&g](unsigned r, unsigned c) {
        // The corner this inner point is next to, and the far ones, by the symmetry of the formula.
        unsigned const r0 = r == 1 ? 0 : 3, r3 = 3 - r0, c0 = c == 1 ? 0 : 3, c3 = 3 - c0;
        unsigned const r1 = r0 == 0 ? 1 : 2, c1 = c0 == 0 ? 1 : 2;
        return (g[r0][c0] * -4 + (g[r0][c1] + g[r1][c0]) * 6 - (g[r0][c3] + g[r3][c0]) * 2 +
                (g[r3][c1] + g[r1][c3]) * 3 - g[r3][c3]) / 9;
    };
    std::array<std::pair<unsigned, unsigned>, 4> const inner = {{{1, 1}, {1, 2}, {2, 2}, {2, 1}}};
    std::array<Geom::Point, 4> points;
    for (unsigned k = 0; k < 4; ++k) {
        auto const [r, c] = inner[k];
        points[k] = patch.tensorIsSet[k] ? patch.tensorpoints[k] : coons(r, c);
    }
    for (unsigned k = 0; k < 4; ++k) {
        g[inner[k].first][inner[k].second] = points[k];
    }
    return g;
}

/// The patch of the grid between rows s0..s1 and columns t0..t1 of its parameters.
SPGradientPatch sub_patch(Grid const &g, double s0, double s1, double t0, double t1)
{
    Grid rows, h;
    for (unsigned r = 0; r < 4; ++r) {
        rows[r] = part(g[r], t0, t1);
    }
    for (unsigned c = 0; c < 4; ++c) {
        auto const column = part({rows[0][c], rows[1][c], rows[2][c], rows[3][c]}, s0, s1);
        for (unsigned r = 0; r < 4; ++r) {
            h[r][c] = column[r];
        }
    }
    SPGradientPatch patch{};
    for (unsigned i = 0; i < 4; ++i) {
        patch.points[0][i] = h[0][i];
        patch.points[1][i] = h[i][3];
        patch.points[2][i] = h[3][3 - i];
        patch.points[3][i] = h[3 - i][0];
    }
    std::array<std::pair<unsigned, unsigned>, 4> const inner = {{{1, 1}, {1, 2}, {2, 2}, {2, 1}}};
    for (unsigned k = 0; k < 4; ++k) {
        patch.pathtype[k] = 'C';
        patch.tensorIsSet[k] = true;
        patch.tensorpoints[k] = h[inner[k].first][inner[k].second];
    }
    return patch;
}

} // namespace

SPGradientVector canvas_vector(SPGradient const *gradient)
{
    if (PerceptualGradient::enabled(gradient)) {
        return PerceptualGradient::rendering_vector(gradient);
    }
    auto const &vector = *gradient->getGradientVector();
    auto const inks = stop_inks(gradient, vector);
    if (ink_beside_process(inks)) {
        return ink_blend(gradient->document, vector, inks);
    }
    return cmyk_blend(gradient->document, vector);
}

SPGradientVector pdf_vector(SPGradient const *gradient)
{
    if (PerceptualGradient::enabled(gradient)) {
        return PerceptualGradient::rendering_vector(gradient);
    }
    auto const &vector = *gradient->getGradientVector();
    auto const inks = stop_inks(gradient, vector);
    if (ink_beside_process(inks)) {
        return ink_blend(gradient->document, vector, inks);
    }
    return vector;
}

SPGradientVector cmyk_blend(SPDocument *document, SPGradientVector const &vector)
{
    auto const target = vector.stops.size() < 2 ? nullptr : document_cmyk(document);
    if (!target) {
        return vector;
    }
    std::vector<Ink> stop_inks;
    for (auto const &stop : vector.stops) {
        auto const ink = stop.color ? inks(document, target, *stop.color) : std::nullopt;
        if (!ink) {
            return vector;
        }
        stop_inks.push_back(*ink);
    }
    double const most = std::clamp(SAMPLE_BUDGET / vector.stops.size(), 1.0, MOST_SAMPLES);

    auto result = vector;
    result.stops.clear();
    for (std::size_t i = 0; i < vector.stops.size(); ++i) {
        auto const &a = vector.stops[i];
        double const opacity_a = a.color->getOpacity();
        result.stops.push_back({ink_color(target, stop_inks[i], opacity_a), a.offset});
        if (i + 1 == vector.stops.size()) {
            break;
        }
        auto const &b = vector.stops[i + 1];
        if (b.offset <= a.offset) {
            continue; // A hard edge.
        }
        double step = 0;
        for (unsigned k = 0; k < 4; ++k) {
            step = std::max(step, std::abs(stop_inks[i + 1][k] - stop_inks[i][k]));
        }
        auto const divisions = static_cast<unsigned>(std::clamp(std::ceil(step * most), 1.0, most));
        double const opacity_b = b.color->getOpacity();
        for (unsigned d = 1; d < divisions; ++d) {
            double const t = double(d) / divisions;
            Ink mix;
            for (unsigned k = 0; k < 4; ++k) {
                mix[k] = stop_inks[i][k] + (stop_inks[i + 1][k] - stop_inks[i][k]) * t;
            }
            result.stops.push_back({ink_color(target, mix, opacity_a + (opacity_b - opacity_a) * t),
                                    a.offset + (b.offset - a.offset) * t});
        }
    }
    return result;
}

SPGradientMesh canvas_mesh(SPDocument *document, SPGradientMesh const &mesh)
{
    auto const target = document_cmyk(document);
    if (!target) {
        return mesh;
    }
    auto result = mesh;
    result.patches.clear();
    for (auto const &row : mesh.patches) {
        auto &cut = result.patches.emplace_back();
        for (auto const &patch : row) {
            // The corners' inks and opacities, C0 to C3.
            std::array<Ink, 4> corner;
            std::array<double, 4> opacity;
            bool converted = true;
            for (unsigned k = 0; k < 4 && converted; ++k) {
                auto const ink = patch.color[k] ? inks(document, target, *patch.color[k]) : std::nullopt;
                converted = ink.has_value();
                if (ink) {
                    corner[k] = *ink;
                    opacity[k] = patch.color[k]->getOpacity();
                }
            }
            if (!converted) {
                cut.push_back(patch);
                continue;
            }
            double spread = 0;
            for (unsigned c = 0; c < 4; ++c) {
                auto const [low, high] = std::minmax({corner[0][c], corner[1][c], corner[2][c], corner[3][c]});
                spread = std::max(spread, high - low);
            }
            auto const cuts = static_cast<unsigned>(std::clamp(std::ceil(spread * MOST_CUTS), 1.0, MOST_CUTS));

            // Bilinear in the patch's parameters, as Cairo blends: C0 at s 0 t 0, C1 at s 0 t 1,
            // C2 at s 1 t 1, C3 at s 1 t 0.
            auto const color_at = [&](double s, double t) {
                double const w[4] = {(1 - s) * (1 - t), (1 - s) * t, s * t, s * (1 - t)};
                Ink mix{};
                double alpha = 0;
                for (unsigned k = 0; k < 4; ++k) {
                    for (unsigned c = 0; c < 4; ++c) {
                        mix[c] += w[k] * corner[k][c];
                    }
                    alpha += w[k] * opacity[k];
                }
                return ink_color(target, mix, alpha);
            };
            auto const grid = tensor_grid(patch);
            for (unsigned i = 0; i < cuts; ++i) {
                double const s0 = double(i) / cuts, s1 = double(i + 1) / cuts;
                for (unsigned j = 0; j < cuts; ++j) {
                    double const t0 = double(j) / cuts, t1 = double(j + 1) / cuts;
                    auto piece = sub_patch(grid, s0, s1, t0, t1);
                    piece.color[0] = color_at(s0, t0);
                    piece.color[1] = color_at(s0, t1);
                    piece.color[2] = color_at(s1, t1);
                    piece.color[3] = color_at(s1, t0);
                    cut.push_back(std::move(piece));
                }
            }
        }
    }
    return result;
}

} // namespace Inkscape::GradientRendering
