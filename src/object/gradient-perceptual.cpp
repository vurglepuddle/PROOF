// SPDX-License-Identifier: GPL-2.0-or-later
#include "gradient-perceptual.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <lcms2.h>

#include "colors/spaces/base.h"
#include "colors/spaces/oklab.h"
#include "sp-gradient.h"
#include "sp-stop.h"
#include <glib.h>
#include "xml/node.h"

namespace Inkscape::PerceptualGradient {
namespace {
using Triple = std::array<double, 3>;
using Type = Colors::Space::Type;
constexpr cmsCIEXYZ D65{.95047, 1.0, 1.08883};

Triple xyz_to_linear(Triple const &v)
{
    return {3.2404542*v[0] - 1.5371385*v[1] - .4985314*v[2],
           -.9692660*v[0] + 1.8760108*v[1] + .0415560*v[2],
            .0556434*v[0] - .2040259*v[1] + 1.0572252*v[2]};
}

Triple linear_to_xyz(Triple const &v)
{
    return {.4124564*v[0] + .3575761*v[1] + .1804375*v[2],
            .2126729*v[0] + .7151522*v[1] + .0721750*v[2],
            .0193339*v[0] + .1191920*v[1] + .9503041*v[2]};
}

std::optional<Triple> to_oklab(Colors::Color const &color)
{
    auto source = color;
    if (color.getSpace()->getComponentType() == Type::CMYK) {
        // Illustrator's Perceptual CMYK gradients go through RGB first. The
        // document profile re-separates the result, including the endpoints.
        auto rgb = color.converted(Type::RGB);
        if (!rgb) return {};
        for (unsigned c = 0; c < 3; ++c) rgb->set(c, std::clamp((*rgb)[c], 0.0, 1.0));
        source = std::move(*rgb);
    }
    auto xyz = source.converted(Type::XYZ50);
    if (!xyz) return {};
    cmsCIEXYZ in{(*xyz)[0], (*xyz)[1], (*xyz)[2]}, out;
    if (!cmsAdaptToIlluminant(&out, cmsD50_XYZ(), &D65, &in)) return {};
    auto rgb = xyz_to_linear({out.X, out.Y, out.Z});
    Triple lab;
    Colors::Space::OkLab::fromLinearRGB(rgb.data(), lab.data());
    return lab;
}

std::optional<Colors::Color> from_oklab(Triple const &lab, Colors::Color const &target)
{
    Triple rgb;
    Colors::Space::OkLab::toLinearRGB(lab.data(), rgb.data());
    auto xyz = linear_to_xyz(rgb);
    cmsCIEXYZ in{xyz[0], xyz[1], xyz[2]}, out;
    if (!cmsAdaptToIlluminant(&out, &D65, cmsD50_XYZ(), &in)) return {};
    auto source = Colors::Color(Type::XYZ50, {out.X, out.Y, out.Z});
    if (target.getSpace()->getComponentType() == Type::CMYK) {
        auto rgb = source.converted(Type::RGB);
        if (!rgb) return {};
        for (unsigned c = 0; c < 3; ++c) rgb->set(c, std::clamp((*rgb)[c], 0.0, 1.0));
        source = std::move(*rgb);
    }
    return source.converted(target.getSpace());
}
}

bool enabled(SPGradient const *gradient)
{
    auto const *vector = gradient ? gradient->getVector() : nullptr;
    auto const *method = vector ? vector->getRepr()->attribute("proof:gradient-method") : nullptr;
    return method && !std::strcmp(method, "oklab");
}

std::vector<double> midpoints(SPGradient const *gradient)
{
    auto const *vector = gradient->getGradientVector();
    std::vector<double> result(vector->stops.size(), .5);
    auto const *source = gradient->getVector();
    auto index = std::size_t{0};
    bool first = true;
    for (auto const &child : source->children) {
        auto const *stop = cast<SPStop>(&child);
        if (!stop) continue;
        if (first && stop->offset > 0) ++index; // The implicit start cap.
        first = false;
        if (index >= result.size()) break;
        if (auto const *value = stop->getRepr()->attribute("proof:gradient-midpoint")) {
            double const midpoint = g_ascii_strtod(value, nullptr);
            if (std::isfinite(midpoint) && midpoint >= .13 && midpoint <= .87) result[index] = midpoint;
        }
        ++index;
    }
    return result;
}

SPGradientVector rendering_vector(SPGradient const *gradient)
{
    auto result = *gradient->getGradientVector();
    if (!enabled(gradient) || result.stops.size() < 2) return result;
    auto const points = midpoints(gradient);
    auto const original = std::move(result.stops);
    unsigned const divisions = std::clamp<std::size_t>(32768 / original.size(), 1, 128);
    for (std::size_t i = 0; i < original.size(); ++i) {
        auto const &a = original[i];
        auto endpoint = a;
        auto ca = a.color ? to_oklab(*a.color) : std::nullopt;
        if (ca && a.color->getSpace()->getComponentType() == Type::CMYK) {
            if (auto color = from_oklab(*ca, *a.color)) {
                color->setOpacity(a.color->getOpacity());
                endpoint.color = std::move(color);
            }
        }
        result.stops.push_back(std::move(endpoint));
        if (i + 1 == original.size()) break;
        auto const &b = original[i + 1];
        if (!a.color || !b.color || b.offset <= a.offset) continue;
        auto cb = to_oklab(*b.color);
        if (!ca || !cb) continue;
        double const midpoint = points[i];
        bool const mirrored = midpoint < .5;
        double const power = std::log(.5) / std::log(mirrored ? 1 - midpoint : midpoint);
        // Bounded vector approximation. Sampling stays out of the document and
        // native definitions, and is recomputed after edits to any real stop.
        for (unsigned k = 1; k < divisions; ++k) {
            double const u = double(k) / divisions;
            // Illustrator 30.6 mirrors the power curve for midpoints below 50%.
            // This preserves the curve when reversing stops and their midpoint.
            double const t = mirrored ? 1 - std::pow(1 - u, power) : std::pow(u, power);
            Triple mix;
            for (unsigned c = 0; c < 3; ++c) mix[c] = (*ca)[c] + ((*cb)[c] - (*ca)[c]) * t;
            auto color = from_oklab(mix, *a.color);
            if (!color) continue;
            color->setOpacity(a.color->getOpacity() + (b.color->getOpacity() - a.color->getOpacity()) * t);
            result.stops.push_back({std::move(color), a.offset + (b.offset - a.offset) * u});
        }
    }
    return result;
}
}
