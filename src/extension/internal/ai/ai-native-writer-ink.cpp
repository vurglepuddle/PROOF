// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * A spot ink's definition as Illustrator's records carry it. See ai-native-writer-ink.h.
 */
#include "ai-native-writer-ink.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

#include "colors/color.h"
#include "colors/document-colors.h"
#include "colors/manager.h"
#include "colors/spaces/base.h"
#include "colors/spaces/cms.h"
#include "colors/spaces/lab.h"
#include "spot-ink.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;

std::string num(double v)
{
    auto s = std::format("{:.4f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

bool unit(double v)
{
    return std::isfinite(v) && v >= -1e-9 && v <= 1.0 + 1e-9;
}

/// Three channels from 0 to 1, clipped; none when one isn't a number.
std::optional<std::array<double, 3>> clipped(std::optional<Colors::Color> const &c)
{
    if (!c || c->size() < 3) return {};
    std::array<double, 3> out{};
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite((*c)[i])) return {};
        out[i] = std::clamp((*c)[i], 0.0, 1.0);
    }
    return out;
}

/// The process colour that stands in for an ink defined in RGB or Lab: through the document's
/// CMYK profile when it has one, else through DeviceCMYK, which follows the working CMYK space
/// when one is chosen (DocumentColors::showDeviceCMYK). With the working rendering intent, as
/// Illustrator converts with its colour settings' (relative colorimetric with black point
/// compensation unless changed): not the profile element's, which a file doesn't carry (the
/// import writes relative colorimetric, a document made here may have none, which is perceptual).
std::optional<std::array<double, 4>> process(Colors::Color const &definition, SPDocument *document)
{
    std::shared_ptr<Colors::Space::AnySpace> space = Colors::DocumentColors::assignedSpace(document);
    if (!space || space->getComponentType() != Type::CMYK) space = Colors::Manager::get().find(Type::CMYK);
    auto values = definition.getValues();
    if (!space || !definition.getSpace() ||
        !definition.getSpace()->convert(values, space, Colors::DocumentColors::workingIntent()) || values.size() < 4) {
        return {};
    }
    std::array<double, 4> out{};
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(values[i])) return {};
        out[i] = std::clamp(values[i], 0.0, 1.0);
    }
    return out;
}

} // namespace

std::variant<InkDefinition, std::string> ink_definition(Colors::Color const &alternate, SPDocument *document)
{
    auto const space = alternate.getSpace();
    if (!space) return "spot inks defined in other colour spaces";
    auto const type = space->getType();
    auto const component = space->getComponentType();
    InkDefinition ink;
    if (component == Type::CMYK) {
        for (unsigned i = 0; i < 4; ++i) {
            if (!unit(alternate[i])) return "out-of-range ink definitions";
            ink.cmyk[i] = std::clamp(alternate[i], 0.0, 1.0);
        }
        return ink;
    }
    if (type == Type::LAB) {
        // Inkscape keeps L/100 and a, b scaled from [-128, 127] (ai-native-import.cpp, definition).
        using Lab = Colors::Space::Lab;
        auto const span = Lab::MAX_SCALE - Lab::MIN_SCALE;
        for (unsigned i = 0; i < 3; ++i) {
            if (!unit(alternate[i])) return "out-of-range ink definitions";
        }
        ink.model = InkDefinition::Model::Lab;
        ink.values = {std::clamp(alternate[0], 0.0, 1.0) * Lab::LUMA_SCALE,
                      std::clamp(alternate[1], 0.0, 1.0) * span + Lab::MIN_SCALE,
                      std::clamp(alternate[2], 0.0, 1.0) * span + Lab::MIN_SCALE};
        auto const rgb = clipped(alternate.converted(Type::RGB));
        if (!rgb) return "spot inks without an RGB equivalent";
        ink.rgb = *rgb;
    } else if (component == Type::RGB || type == Type::CSSNAME || type == Type::HSL || type == Type::HSV ||
               type == Type::HWB) {
        // RGB in a profile keeps its numbers, as CMYK in one does; the rest are sRGB spelt
        // differently.
        bool const numbers = type == Type::RGB || type == Type::CMS;
        if (numbers) {
            for (unsigned i = 0; i < 3; ++i) {
                if (!unit(alternate[i])) return "out-of-range ink definitions";
            }
        }
        auto const rgb = clipped(numbers ? std::optional(alternate) : alternate.converted(Type::RGB));
        if (!rgb) return "spot inks without an RGB equivalent";
        ink.model = InkDefinition::Model::RGB;
        ink.values = *rgb;
        ink.rgb = *rgb;
    } else {
        return "spot inks defined in other colour spaces";
    }
    auto const cmyk = process(alternate, document);
    if (!cmyk) return "spot inks without a CMYK equivalent";
    ink.cmyk = *cmyk;
    return ink;
}

std::string ink_operands(InkDefinition const &ink, Colors::Color const &alternate, double tint, SPDocument *document)
{
    auto cmyk = std::optional(ink.cmyk);
    if (ink.model != InkDefinition::Model::CMYK && tint <= 1e-9) {
        // No ink at all is paper, as Illustrator writes it, whatever a profile makes of white.
        cmyk = std::array<double, 4>{};
    } else if (ink.model != InkDefinition::Model::CMYK && tint < 1.0 - 1e-9) {
        cmyk = process(SpotInk::display_color(alternate, tint), document);
    }
    if (!cmyk) return {};
    auto out = std::format("{} {} {} {}", num((*cmyk)[0]), num((*cmyk)[1]), num((*cmyk)[2]), num((*cmyk)[3]));
    if (ink.model != InkDefinition::Model::CMYK) {
        out += std::format(" {} {} {}", num(ink.values[0]), num(ink.values[1]), num(ink.values[2]));
    }
    return out;
}

} // namespace Inkscape::Extension::Internal::AiNative
