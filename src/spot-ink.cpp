// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Spot (named) inks, stored in the document as PROOF swatches. See spot-ink.h.
 */
#include "spot-ink.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <vector>
#include <glib.h>

#include "colors/spaces/base.h"
#include "colors/spaces/enum.h"
#include "colors/spaces/lab.h"
#include "document.h"
#include "object/sp-defs.h"
#include "svg/css-ostringstream.h"
#include "xml/document.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::SpotInk {
namespace {

constexpr char const *ATTR_INK = "proof:ink";
constexpr char const *ATTR_ALTERNATE = "proof:ink-alternate";
constexpr char const *ATTR_TINT = "proof:ink-tint";
constexpr double TINT_EPSILON = 1e-4;

double clamp01(double value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 1.0;
}

std::string tint_text(double tint)
{
    CSSOStringStream os;
    os << clamp01(tint);
    return os.str();
}

/// Pick an unused id, preferring the readable stem.
std::string unique_id(SPDocument *doc, std::string const &stem)
{
    if (!doc || !doc->getObjectById(stem)) {
        return stem;
    }
    for (int n = 2;; ++n) {
        auto candidate = stem + "-" + std::to_string(n);
        if (!doc->getObjectById(candidate)) {
            return candidate;
        }
    }
}

} // namespace

std::string id_stem(std::string const &name)
{
    std::string stem = "ink-";
    bool dash = false;
    for (unsigned char ch : name) {
        if (std::isalnum(ch)) {
            stem += static_cast<char>(std::tolower(ch));
            dash = false;
        } else if (!dash && stem.size() > 4) {
            stem += '-';
            dash = true;
        }
    }
    while (stem.size() > 4 && stem.back() == '-') {
        stem.pop_back();
    }
    return stem.size() > 4 ? stem : "ink-unnamed";
}

Colors::Color display_color(Colors::Color const &alternate, double tint)
{
    using Colors::Space::Type;
    tint = clamp01(tint);
    auto values = alternate.getValues();
    auto const space = alternate.getSpace();
    auto const channels = std::min<std::size_t>(space->getComponentCount(), values.size());

    switch (space->getType()) {
        case Type::CMYK:
        case Type::CMY:
            // Inks scale with the tint; paper is zero ink.
            for (std::size_t i = 0; i < channels; ++i) {
                values[i] *= tint;
            }
            break;
        case Type::LAB: {
            // Blend towards paper white (L 100, a 0, b 0), as PDF Separation tints do.
            using Lab = Colors::Space::Lab;
            auto const span = Lab::MAX_SCALE - Lab::MIN_SCALE;
            values[0] = 1.0 - tint * (1.0 - values[0]);
            for (std::size_t i = 1; i < std::min<std::size_t>(channels, 3); ++i) {
                auto const actual = Lab::MIN_SCALE + values[i] * span;
                values[i] = (tint * actual - Lab::MIN_SCALE) / span;
            }
            break;
        }
        case Type::RGB:
        case Type::linearRGB:
        case Type::Gray:
            for (std::size_t i = 0; i < channels; ++i) {
                values[i] = 1.0 - tint * (1.0 - values[i]);
            }
            break;
        default:
            // Other spaces: blend in sRGB.
            if (auto rgb = alternate.converted(Type::RGB)) {
                return display_color(*rgb, tint);
            }
            return alternate;
    }
    return Colors::Color(space, std::move(values));
}

std::optional<Ink> read(XML::Node const *swatch)
{
    if (!swatch) {
        return {};
    }
    auto const name = swatch->attribute(ATTR_INK);
    auto const alternate_text = swatch->attribute(ATTR_ALTERNATE);
    if (!name || !*name || !alternate_text) {
        return {};
    }
    auto alternate = Colors::Color::parse(alternate_text);
    if (!alternate) {
        return {};
    }
    double tint = 1.0;
    if (auto text = swatch->attribute(ATTR_TINT)) {
        tint = clamp01(g_ascii_strtod(text, nullptr));
    }
    return Ink{name, *alternate, tint};
}

std::string ensure(XML::Document *xml_doc, XML::Node *defs, SPDocument *doc, Ink const &ink)
{
    auto const tint = clamp01(ink.tint);

    // Reuse an existing swatch for the same ink, definition and tint.
    for (auto child = defs->firstChild(); child; child = child->next()) {
        if (auto existing = read(child)) {
            if (existing->name == ink.name && std::fabs(existing->tint - tint) < TINT_EPSILON &&
                existing->alternate.isClose(ink.alternate, 1e-3)) {
                if (auto id = child->attribute("id")) {
                    return id;
                }
            }
        }
    }

    auto stem = id_stem(ink.name);
    auto const label = label_for(ink.name, tint);
    if (tint < 1.0 - TINT_EPSILON) {
        stem += "-t" + std::to_string(static_cast<int>(std::lround(tint * 100)));
    }
    auto const id = unique_id(doc, stem);

    auto grad = xml_doc->createElement("svg:linearGradient");
    grad->setAttribute("id", id);
    grad->setAttribute("inkscape:swatch", "solid");
    grad->setAttribute("inkscape:label", label);
    grad->setAttribute(ATTR_INK, ink.name);
    grad->setAttribute(ATTR_ALTERNATE, ink.alternate.toString(false));
    if (tint < 1.0 - TINT_EPSILON) {
        grad->setAttribute(ATTR_TINT, tint_text(tint));
    }

    auto stop = xml_doc->createElement("svg:stop");
    stop->setAttribute("offset", "0");
    auto const shown = display_color(ink.alternate, tint).toString(false);
    stop->setAttribute("style", "stop-color:" + shown + ";stop-opacity:1");
    grad->appendChild(stop);
    GC::release(stop);

    defs->appendChild(grad);
    GC::release(grad);
    return id;
}

std::string ensure(SPDocument *doc, Ink const &ink)
{
    return ensure(doc->getReprDoc(), doc->getDefs()->getRepr(), doc, ink);
}

std::string label_for(std::string const &name, double tint)
{
    tint = clamp01(tint);
    // PDF's registration ink, which prints on every plate, is Illustrator's [Registration].
    auto const shown = name == "All" ? std::string("[Registration]") : name;
    if (tint < 1.0 - TINT_EPSILON) {
        return shown + " " + std::to_string(static_cast<int>(std::lround(tint * 100))) + "%";
    }
    return shown;
}

Colors::Color alternate_for(Colors::Color const &shown, double tint)
{
    using Colors::Space::Type;
    tint = clamp01(tint);
    if (tint < 1e-3 || tint > 1.0 - TINT_EPSILON) {
        return shown; // full strength, or no ink to scale back up from
    }
    auto values = shown.getValues();
    auto const space = shown.getSpace();
    auto const channels = std::min<std::size_t>(space->getComponentCount(), values.size());
    switch (space->getType()) {
        case Type::CMYK:
        case Type::CMY:
            for (std::size_t i = 0; i < channels; ++i) {
                values[i] = clamp01(values[i] / tint);
            }
            break;
        case Type::LAB: {
            using Lab = Colors::Space::Lab;
            auto const span = Lab::MAX_SCALE - Lab::MIN_SCALE;
            values[0] = clamp01(1.0 - (1.0 - values[0]) / tint);
            for (std::size_t i = 1; i < std::min<std::size_t>(channels, 3); ++i) {
                auto const actual = std::clamp((Lab::MIN_SCALE + values[i] * span) / tint, Lab::MIN_SCALE,
                                               Lab::MAX_SCALE);
                values[i] = (actual - Lab::MIN_SCALE) / span;
            }
            break;
        }
        case Type::RGB:
        case Type::linearRGB:
        case Type::Gray:
            for (std::size_t i = 0; i < channels; ++i) {
                values[i] = clamp01(1.0 - (1.0 - values[i]) / tint);
            }
            break;
        default:
            if (auto rgb = shown.converted(Type::RGB)) {
                return alternate_for(*rgb, tint);
            }
            return shown;
    }
    return Colors::Color(space, std::move(values));
}

namespace {

/// Every swatch in defs belonging to this ink (same name and definition).
std::vector<XML::Node *> family(XML::Node *defs, Ink const &ink)
{
    std::vector<XML::Node *> members;
    for (auto child = defs->firstChild(); child; child = child->next()) {
        if (auto member = read(child)) {
            if (member->name == ink.name && member->alternate.isClose(ink.alternate, 1e-3)) {
                members.push_back(child);
            }
        }
    }
    return members;
}

void set_stop_color(XML::Node *swatch, Colors::Color const &color)
{
    for (auto child = swatch->firstChild(); child; child = child->next()) {
        if (child->name() && std::string_view(child->name()) == "svg:stop") {
            auto css = sp_repr_css_attr(child, "style");
            sp_repr_css_set_property_string(css, "stop-color", color.toString(false));
            sp_repr_css_change(child, css, "style");
            sp_repr_css_attr_unref(css);
            return;
        }
    }
}

} // namespace

int redefine(SPDocument *doc, Ink const &ink, Colors::Color const &new_alternate)
{
    auto const text = new_alternate.toString(false);
    int changed = 0;
    for (auto member : family(doc->getDefs()->getRepr(), ink)) {
        auto const tint = read(member)->tint;
        member->setAttribute(ATTR_ALTERNATE, text);
        set_stop_color(member, display_color(new_alternate, tint));
        ++changed;
    }
    return changed;
}

int rename(SPDocument *doc, Ink const &ink, std::string const &new_name)
{
    if (new_name.empty()) {
        return 0;
    }
    int changed = 0;
    for (auto member : family(doc->getDefs()->getRepr(), ink)) {
        member->setAttribute(ATTR_INK, new_name);
        member->setAttribute("inkscape:label", label_for(new_name, read(member)->tint));
        ++changed;
    }
    return changed;
}

bool make_spot(SPDocument *doc, XML::Node *swatch, std::string const &name)
{
    if (!swatch || name.empty()) {
        return false;
    }
    for (auto child = swatch->firstChild(); child; child = child->next()) {
        if (child->name() && std::string_view(child->name()) == "svg:stop") {
            auto css = sp_repr_css_attr(child, "style");
            auto const shown = sp_repr_css_property(css, "stop-color", "");
            auto color = Colors::Color::parse(shown);
            sp_repr_css_attr_unref(css);
            if (!color) {
                return false;
            }
            swatch->setAttribute(ATTR_INK, name);
            swatch->setAttribute(ATTR_ALTERNATE, color->toString(false));
            swatch->removeAttribute(ATTR_TINT);
            swatch->setAttribute("inkscape:label", name);
            return true;
        }
    }
    return false;
}

void make_process(XML::Node *swatch)
{
    if (!swatch) {
        return;
    }
    swatch->removeAttribute(ATTR_INK);
    swatch->removeAttribute(ATTR_ALTERNATE);
    swatch->removeAttribute(ATTR_TINT);
}

} // namespace Inkscape::SpotInk
