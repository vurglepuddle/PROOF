// SPDX-License-Identifier: GPL-2.0-or-later
/** Opaque process linear and radial gradients shared by modern and Illustrator 7 records.
 *
 * Adobe's 1998 format, section 3.5, provides editable Bg geometry independently of
 * the optional imaging matrices. Collapse the SVG affine scalar field to an absolute
 * art-space vector and use identity Bg. A modern stroke also carries its original
 * placement in Xm while Bg compensates Illustrator's stroked bounds. See
 * doc/PROOF-ai-interchange.md. Actual
 * Illustrator and native/PDF parity require qualification beyond synthetic readback.
 *
 * A radial instance is written as Illustrator writes every one: Bb, the highlight Bh,
 * Bg, and in the modern form the whole placement Bm (the unit circle onto the art).
 * Illustrator itself reads Bg, an ellipse there by its aspect ratio and, turned, by Bg's
 * matrix on its canvas (see radial_instance). Illustrator 7 records get circles only.
 *
 * A stop that is a tint of a spot ink (proof:ink on the <stop>, as the .ai import writes it) is
 * written as one, colour style 3, and the ink handed back for the file to declare. An ink defined
 * in RGB or Lab is written twice, as Illustrator writes it: that stand-in, then the exact stop.
 *
 * A stroke's gradient is the same instance opened with `1 Bb` about the stroke operator, the
 * gradient spread over the stroke's area as SVG spreads it: Illustrator's default, "within
 * stroke". Its "along" and "across" have no SVG counterpart and aren't written.
 */
#include "ai-native-writer-gradient.h"
#include "ai-native-writer-string.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <variant>
#include <vector>

#include "colors/color.h"
#include "colors/cms/profile.h"
#include "colors/document-colors.h"
#include "colors/spaces/base.h"
#include "object/sp-gradient.h"
#include "object/gradient-perceptual.h"
#include "object/sp-gradient-reference.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-shape.h"
#include "object/sp-stop.h"
#include "spot-ink.h"
#include "style.h"
#include "xml/node.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;
constexpr std::size_t MAX_STOPS = 256; // The native reader's default limit, including endpoints.
constexpr std::size_t MAX_REFS = 32;

/// The classic IEC encoding: its quantized D50 matrix, D65 media white, zero
/// black point and the exact sRGB transfer function in a 1024-entry 16-bit table.
/// Generated from the colour definition; no installed profile or artwork is needed.
std::shared_ptr<Colors::CMS::Profile> classic_srgb_profile()
{
    auto handle = cmsCreate_sRGBProfile();
    if (!handle) return {};
    cmsSetProfileVersion(handle, 2.1);
    cmsCIEXYZ white{.9504547119140625, 1, 1.08905029296875}, black{0, 0, 0};
    std::array<cmsCIEXYZ, 3> matrix{{
        {.436065673828125, .2224884033203125, .013916015625},
        {.3851470947265625, .7168731689453125, .097076416015625},
        {.14306640625, .06060791015625, .7140960693359375}
    }};
    std::array<cmsUInt16Number, 1024> samples;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        double const x = double(i) / (samples.size() - 1);
        double const linear = x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
        samples[i] = static_cast<cmsUInt16Number>(std::round(linear * 65535));
    }
    auto curve = cmsBuildTabulatedToneCurve16(nullptr, samples.size(), samples.data());
    bool const written = curve && cmsWriteTag(handle, cmsSigChromaticAdaptationTag, nullptr) &&
        cmsWriteTag(handle, cmsSigChromaticityTag, nullptr) && cmsWriteTag(handle, cmsSigMediaWhitePointTag, &white) &&
        cmsWriteTag(handle, cmsSigMediaBlackPointTag, &black) &&
        cmsWriteTag(handle, cmsSigRedColorantTag, &matrix[0]) &&
        cmsWriteTag(handle, cmsSigGreenColorantTag, &matrix[1]) &&
        cmsWriteTag(handle, cmsSigBlueColorantTag, &matrix[2]) &&
        cmsWriteTag(handle, cmsSigRedTRCTag, curve) && cmsWriteTag(handle, cmsSigGreenTRCTag, curve) &&
        cmsWriteTag(handle, cmsSigBlueTRCTag, curve);
    if (curve) cmsFreeToneCurve(curve);
    if (!written) {
        cmsCloseProfile(handle);
        return {};
    }
    return Colors::CMS::Profile::create(handle);
}

} // namespace

/// Compare the two supported sRGB encodings by colour data, not name/profile ID.
/// Creation dates and descriptions vary; extra colour/LUT tags fail closed.
bool standard_rgb_profile(Colors::CMS::Profile const &profile)
{
    static std::array const references{Colors::CMS::Profile::create_srgb(), classic_srgb_profile()};
    auto const descriptive = [](cmsTagSignature tag, bool classic) {
        return tag == cmsSigProfileDescriptionTag || tag == cmsSigCopyrightTag ||
               tag == cmsSigDeviceMfgDescTag || tag == cmsSigDeviceModelDescTag ||
               (classic && (tag == cmsSigLuminanceTag || tag == cmsSigMeasurementTag || tag == cmsSigTechnologyTag ||
                            tag == cmsSigViewingConditionsTag || tag == cmsSigViewingCondDescTag));
    };
    // Read the preserved profile bytes. lcms may re-encode a tag after a
    // transform has parsed it; that representation is not the file's definition.
    using Tags = std::map<cmsTagSignature, std::vector<unsigned char>>;
    auto const colour_tags = [&](std::vector<unsigned char> const &data, bool classic) -> std::optional<Tags> {
        if (data.size() < 132) return {};
        auto const u32 = [&](std::size_t at) {
            return (std::uint32_t(data[at]) << 24) | (std::uint32_t(data[at + 1]) << 16) |
                   (std::uint32_t(data[at + 2]) << 8) | data[at + 3];
        };
        auto const size = u32(0), count = u32(128);
        if (size < 132 || size > data.size() || count > (size - 132) / 12) return {};
        Tags out;
        std::set<cmsTagSignature> seen;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto const entry = 132 + 12 * i;
            auto const tag = static_cast<cmsTagSignature>(u32(entry));
            auto const offset = u32(entry + 4), length = u32(entry + 8);
            if (!seen.insert(tag).second || offset < 132 + 12 * count || offset > size || length < 8 || length > size - offset) return {};
            if (!descriptive(tag, classic)) out.emplace(tag, std::vector<unsigned char>(data.begin() + offset, data.begin() + offset + length));
        }
        return out;
    };
    static auto const reference_tags = [&] {
        return std::array{references[0] ? colour_tags(references[0]->dumpData(), false) : std::nullopt,
                          references[1] ? colour_tags(references[1]->dumpData(), true) : std::nullopt};
    }();
    auto const handle = profile.getHandle();
    if (profile.getColorSpace() != cmsSigRgbData) return false;
    auto const bytes = profile.dumpData(); // Includes the PCS illuminant in header bytes 68..79.
    if (bytes.size() < 128) return false;
    for (std::size_t n = 0; n < references.size(); ++n) {
        auto const &reference = references[n];
        if (!reference || !reference_tags[n] || profile.getProfileClass() != reference->getProfileClass() ||
            cmsGetPCS(handle) != cmsGetPCS(reference->getHandle()) ||
            cmsGetProfileVersion(handle) != cmsGetProfileVersion(reference->getHandle())) continue;
        auto const reference_bytes = reference->dumpData();
        if (!std::equal(bytes.begin() + 68, bytes.begin() + 80, reference_bytes.begin() + 68)) continue;
        if (colour_tags(bytes, n == 1) == reference_tags[n]) return true;
    }
    return false;
}

namespace {

std::string num(double v)
{
    auto s = std::format("{:.4f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

std::string ps_string(std::string const &s)
{
    std::string out = "(";
    for (unsigned char c : s) {
        if (c == '(' || c == ')' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c >= 127) {
            out += std::format("\\{:03o}", c);
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + ")";
}

WrittenGradient refused(std::string why)
{
    WrittenGradient out;
    out.refused.push_back(std::move(why));
    return out;
}

bool finite(Geom::Affine const &m)
{
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

bool regular(Geom::Affine const &m)
{
    if (!finite(m)) return false;
    double const scale = std::max({std::abs(m[0]), std::abs(m[1]), std::abs(m[2]), std::abs(m[3])});
    return scale > 0 && std::isfinite(m.det()) && std::abs(m.det()) > 1e-12 * scale * scale;
}

bool ink_metadata(SPObject const *object)
{
    return object->getRepr()->attribute("proof:ink") || object->getRepr()->attribute("proof:ink-alternate") ||
           object->getRepr()->attribute("proof:ink-tint");
}

// Check the entire chain before composed-vector access: malformed hrefs must not become
// synthetic transparent stops or a silently defaulted placement.
std::optional<std::string> check_references(SPGradient const *gradient)
{
    std::set<SPGradient const *> visited;
    constexpr std::array<char const *, 5> linear{"x1", "y1", "x2", "y2", "gradientTransform"};
    constexpr std::array<char const *, 7> radial{"cx", "cy", "r", "fx", "fy", "fr", "gradientTransform"};
    std::vector<char const *> placement(linear.begin(), linear.end());
    if (is<SPRadialGradient>(gradient)) placement.assign(radial.begin(), radial.end());
    for (auto const *g = gradient; g;) {
        if (!visited.insert(g).second || visited.size() > MAX_REFS) return "cyclic or excessive gradient references";
        // A spot swatch is a one-stop gradient; its stops are read from <stop>s (stop_inks).
        if (ink_metadata(g)) return "spot ink swatches used as gradients";
        if (g != gradient) {
            for (auto const *key : placement) {
                if (!gradient->getRepr()->attribute(key) && g->getRepr()->attribute(key)) {
                    return "inherited gradient geometry or transforms";
                }
            }
        }
        auto const *next = g->ref ? g->ref->getObject() : nullptr;
        if (!next && (g->getRepr()->attribute("xlink:href") || g->getRepr()->attribute("href"))) {
            return "unresolved gradient references";
        }
        g = next;
    }
    if (!gradient->getVector()->hasStops()) return "gradients without stops";
    return {};
}

/** The spot ink of each effective stop, or none, beside SPGradient::rebuildVector's stops: the
 * <stop>s of the first gradient along the references that has any, with a copy of the first
 * at 0 and of the last at 1 when they don't reach there. Empty when they don't line up, and
 * with `undefined` set when a stop names an ink without a definition that can be read. */
std::vector<std::optional<SpotInk::Ink>> stop_inks(SPGradient const *gradient, std::size_t effective, bool &undefined)
{
    auto const *owner = gradient;
    for (std::size_t i = 0; owner && !owner->hasStops() && i <= MAX_REFS; ++i) {
        owner = owner->ref ? owner->ref->getObject() : nullptr;
    }
    std::vector<std::optional<SpotInk::Ink>> inks;
    std::vector<double> offsets;
    if (!owner) return inks;
    for (auto const &child : owner->children) {
        if (auto const *stop = cast<SPStop>(&child)) {
            inks.push_back(SpotInk::read(stop->getRepr()));
            if (!inks.back() && ink_metadata(stop)) {
                undefined = true;
                return {};
            }
            double const offset = offsets.empty() ? stop->offset : std::max<double>(stop->offset, offsets.back());
            offsets.push_back(std::clamp(offset, 0.0, 1.0));
        }
    }
    if (inks.empty()) return inks;
    if (offsets.front() > 0.0) inks.insert(inks.begin(), inks.front());
    if (offsets.back() < 1.0) inks.push_back(inks.back());
    if (inks.size() != effective) inks.clear();
    return inks;
}

/// A linear gradient's instance: Bb, then Bg with the absolute art-space vector of the scalar
/// field `matrix` carries the gradient's own onto.
std::variant<std::string, WrittenGradient> linear_instance(SPLinearGradient const *gradient, Geom::Affine const &matrix,
                                                           std::string const &name, std::string const &open,
                                                           SPShape const *stroke_shape, Geom::Affine const &item_to_art)
{
    Geom::Point const p0(gradient->x1.computed, gradient->y1.computed);
    Geom::Point const d(gradient->x2.computed - p0.x(), gradient->y2.computed - p0.y());
    double const dd = d.x() * d.x() + d.y() * d.y();
    if (!std::isfinite(dd) || dd <= 0) return refused("zero-length or nonfinite linear gradients");
    auto const inverse = matrix.inverse();
    Geom::Point const n((inverse[0] * d.x() + inverse[1] * d.y()) / dd,
                        (inverse[2] * d.x() + inverse[3] * d.y()) / dd);
    double const nn = n.x() * n.x() + n.y() * n.y();
    if (!std::isfinite(nn) || nn <= 0) return refused("zero-length or nonfinite linear gradients");
    auto const vector = n / nn;
    auto origin = p0 * matrix;
    double length = std::hypot(vector.x(), vector.y());
    double const angle = std::atan2(vector.y(), vector.x()) * 180.0 / std::acos(-1.0);
    if (!std::isfinite(origin.x()) || !std::isfinite(origin.y()) || !std::isfinite(length) || num(length) == "0") {
        return refused("zero-length or nonfinite linear gradients");
    }
    std::string imaging;
    if (stroke_shape) {
        // Keep the actual scalar field for native/PDF readers. Illustrator takes Bg instead,
        // expanding a simple path's gradient to its stroked bounds in the gradient's frame.
        // Compound paths do not get that expansion (Illustrator 29, 2026-10-21).
        imaging = std::format("{} {} {} {} {} {} Xm\n", num(vector.x()), num(vector.y()), num(vector.y()),
                              num(-vector.x()), num(origin.x()), num(origin.y()));
        if (stroke_shape->curve()->size() == 1) {
            double const cosine = vector.x() / length, sine = vector.y() / length;
            Geom::Affine const frame(cosine, -sine, sine, cosine, 0, 0);
            auto const path = stroke_shape->geometricBounds(item_to_art * frame);
            auto const stroked = stroke_shape->visualBounds(item_to_art * frame, false, false, false);
            if (!path || !stroked || !std::isfinite(path->width()) || !std::isfinite(stroked->width()) ||
                path->width() <= 1e-9 || stroked->width() <= 1e-9) {
                return refused("linear gradient strokes without bounds along their ramp");
            }
            auto at = origin * frame;
            at.x() = path->left() + (at.x() - stroked->left()) * path->width() / stroked->width();
            origin = at * frame.inverse();
            length *= path->width() / stroked->width();
            if (!std::isfinite(origin.x()) || !std::isfinite(origin.y()) || !std::isfinite(length) || num(length) == "0") {
                return refused("zero-length or nonfinite linear gradient strokes");
            }
        }
    }
    return std::format("{}1 {} {} {} {} {} 1 0 0 1 0 0 Bg\n{}", open, name, num(origin.x()), num(origin.y()), num(angle),
                       num(length), imaging);
}

/** A radial gradient's instance, as Illustrator writes every radial one: Bb, the highlight
 * `x y angle share Bh`, Bg, and in the modern form Bm, the whole placement.
 *
 * Illustrator reads Bg and not Bm, and reads Bg two ways (Illustrator 29, 2026-10-20; see
 * doc/PROOF-ai-interchange.md, "Radial gradients"). In a file whose creator is Illustrator,
 * its origin and length are relative to the object's bounds; in any other, as here, they are
 * art space, and its angle turns nothing. So the ellipse is written lying along x: Bg's
 * length is its x radius and the aspect ratio (modern form) y's over x's, either side of 1.
 * One that is turned also gets Bg's matrix, which Illustrator applies on its y-down canvas,
 * where the art origin is `canvas`: there it turns the ellipse about its centre.
 *
 * Bm, which PostScript and PROOF's reader take, carries the unit circle onto the art, x along
 * the first axis and y along the other, mirrored as most of Illustrator's own are. Bh's angle
 * and share are taken in it (`share·(cos a, −sin a)` through Bm is the offset `x y`);
 * Illustrator takes them in the ellipse before its turn, which is the same.
 */
std::variant<std::string, WrittenGradient> radial_instance(SPRadialGradient const *gradient, Geom::Affine const &matrix,
                                                           std::string const &name, bool legacy, Geom::Point const &canvas,
                                                           std::string const &open, SPShape const *stroke_shape,
                                                           Geom::Affine const &item_to_art, bool after_gradient_fill)
{
    double const pi = std::acos(-1.0);
    double const r = gradient->r.computed;
    if (!std::isfinite(r) || r <= 0) return refused("zero-radius or nonfinite radial gradients");
    // An absent fr reads as 0; SVG 2's focal circle has no counterpart in Illustrator.
    if (!std::isfinite(gradient->fr.computed) || std::abs(gradient->fr.computed) > 1e-9 * r) {
        return refused("radial gradients with a focal radius");
    }
    Geom::Point const centre(gradient->cx.computed, gradient->cy.computed);
    auto const unit = Geom::Affine(r, 0, 0, r, centre.x(), centre.y()) * matrix;
    if (!regular(unit)) return refused("singular or nonfinite gradient transforms");

    // The circle becomes an ellipse whose axes are the eigenvectors of A·Aᵀ, where A's
    // columns are the images of the unit axes.
    double const p = unit[0] * unit[0] + unit[2] * unit[2];
    double const q = unit[0] * unit[1] + unit[2] * unit[3];
    double const s = unit[1] * unit[1] + unit[3] * unit[3];
    double const mean = (p + s) / 2, half = std::hypot((p - s) / 2, q);
    double rx = std::sqrt(mean + half), ry = std::sqrt(std::max(mean - half, 0.0));
    double turn = 0.5 * std::atan2(2 * q, p - s); // Of the first axis, in (−90°, 90°].
    if (rx - ry <= 1e-6 * rx) {
        ry = rx;
        turn = 0;
    } else {
        if (legacy) return refused("elliptical radial gradients (Illustrator 7)");
        // Lying along y needs no turn: y's radius is then the larger.
        if (std::abs(std::abs(turn) - pi / 2) < 1e-6) {
            std::swap(rx, ry);
            turn = 0;
        } else if (std::abs(turn) < 1e-6) {
            turn = 0;
        }
    }
    double const cosine = std::cos(turn), sine = std::sin(turn);
    Geom::Affine const bm(rx * cosine, rx * sine, ry * sine, -ry * cosine, unit[4], unit[5]);
    if (!regular(bm) || !std::isfinite(rx) || num(rx) == "0" || num(ry / rx) == "0") {
        return refused("singular or nonfinite gradient transforms");
    }

    // The focal point in the unit circle. The canvas draws one outside the circle on it.
    Geom::Point focus((gradient->fx.computed - centre.x()) / r, (gradient->fy.computed - centre.y()) / r);
    if (!std::isfinite(focus.x()) || !std::isfinite(focus.y())) return refused("nonfinite radial gradient focal points");
    // Illustrator 29 drops a stroke's noncentral Bh focal point on open (verified by its
    // rendering and re-saved records). Fills retain it; refuse strokes rather than change them.
    if (stroke_shape && Geom::L2(focus) > 1e-9) return refused("radial gradient strokes with an offset focal point");
    if (Geom::L2(focus) > 1) focus /= Geom::L2(focus);
    auto const offset = focus * unit.withoutTranslation();
    auto const in_bm = offset * bm.withoutTranslation().inverse();
    double const share = std::min(Geom::L2(in_bm), 1.0);
    std::string out = open;
    if (num(share) == "0") {
        // Illustrator shares Bh between consecutive fill/stroke instances: resetting the
        // centred stroke would also erase the fill's focal point. Its stroke remains centred.
        if (!after_gradient_fill) out += "0 0 0 0 Bh\n";
    } else {
        out += std::format("{} {} {} {} Bh\n", num(offset.x()), num(offset.y()),
                           num(std::atan2(-in_bm.y(), in_bm.x()) * 180 / pi), num(share));
    }

    // The turn about the centre, on the canvas: y runs down there, so it turns the other way.
    Geom::Affine turned;
    if (turn != 0) {
        Geom::Point const there(unit[4] + canvas.x(), canvas.y() - unit[5]);
        Geom::Affine const back(cosine, -sine, sine, cosine, 0, 0);
        turned = back * Geom::Translate(there - there * back);
        if (!regular(turned)) return refused("singular or nonfinite gradient transforms");
    }
    // To ten places: the turn is about a point some 4000 points out on the canvas, where four
    // would put the ellipse a fifth of a point off. Illustrator writes these to fifteen.
    auto const fine = [](double v) {
        auto t = std::format("{:.10f}", v);
        t.erase(t.find_last_not_of('0') + 1);
        if (t.back() == '.') t.pop_back();
        return t == "-0" ? std::string("0") : t;
    };
    Geom::Point origin(unit[4], unit[5]);
    double length = rx;
    if (stroke_shape) {
        // Illustrator normalizes a stroke's Bg against the path bounds, then draws it against
        // the stroked bounds. Undo that expansion in the gradient's unturned frame. Bm stays
        // the actual imaging geometry for the PDF/native reader. Verified in Illustrator 29
        // with a circle, a turned ellipse and an off-centre placement (2026-10-20).
        // Compound paths do not get the expansion (2026-10-21).
        auto const unturn = Geom::Translate(-origin) * Geom::Affine(cosine, -sine, sine, cosine, 0, 0) *
                            Geom::Translate(origin);
        auto const frame = item_to_art * unturn;
        auto const path = stroke_shape->geometricBounds(frame);
        auto const stroked = stroke_shape->visualBounds(frame, false, false, false);
        if (!path || !stroked) return refused("radial gradient strokes without bounds");
        double const path_diagonal = std::hypot(path->width(), path->height());
        double const stroke_diagonal = std::hypot(stroked->width(), stroked->height());
        if (!std::isfinite(path_diagonal) || !std::isfinite(stroke_diagonal) ||
            path_diagonal <= 0 || stroke_diagonal <= 0) return refused("radial gradient strokes without bounds");
        // Off-centre turned ellipses need another placement convention: Illustrator still
        // moves their centre after this correction. Keep the qualified centred case only.
        if (turn != 0 && Geom::L2(origin - path->midpoint()) > 1e-6 * std::max(path_diagonal, 1.0)) {
            return refused("off-centre rotated radial gradient strokes");
        }
        if (stroke_shape->curve()->size() == 1) {
            length *= path_diagonal / stroke_diagonal;
            for (unsigned i = 0; i < 2; ++i) {
                if (path->dimensions()[i] <= 1e-9 || stroked->dimensions()[i] <= 1e-9) {
                    if (std::abs(origin[i] - path->midpoint()[i]) > 1e-6) {
                        return refused("radial gradient strokes offset from degenerate path bounds");
                    }
                } else {
                    origin[i] = path->min()[i] + (origin[i] - stroked->min()[i]) *
                                path->dimensions()[i] / stroked->dimensions()[i];
                }
            }
        }
        if (!std::isfinite(length) || num(length) == "0") return refused("zero-radius or nonfinite radial gradients");
    }
    out += std::format("1 {} {} {} 0 {} {} {} {} {} {} {}", name, num(origin.x()), num(origin.y()), num(length), fine(turned[0]),
                       fine(turned[1]), fine(turned[2]), fine(turned[3]), fine(turned[4]), fine(turned[5]));
    if (legacy) return out + " Bg\n";
    return out + std::format(" {} Bg\n{} {} {} {} {} {} Bm\n", num(ry / rx), num(bm[0]), num(bm[1]), num(bm[2]),
                             num(bm[3]), num(bm[4]), num(bm[5]));
}

} // namespace

std::optional<double> coverage_grey(Colors::Color const &color)
{
    auto const space = color.getSpace();
    if (!space) return {};
    // Standard sRGB in a profile (as the import gives a mask's greys) is its own numbers: converted,
    // white came out a few millionths over 1.
    bool raw = space->getType() == Type::RGB || space->getType() == Type::CSSNAME;
    if (!raw && space->getType() == Type::CMS && space->getComponentType() == Type::RGB && space->hasValidCmsProfile()) {
        if (auto const profile = space->getProfile()) {
            static std::map<std::string, bool> standard;
            auto const &key = profile->getChecksum();
            if (key.empty()) {
                raw = standard_rgb_profile(*profile);
            } else {
                auto [known, added] = standard.try_emplace(key, false);
                if (added) known->second = standard_rgb_profile(*profile);
                raw = known->second;
            }
        }
    }
    std::array<double, 3> rgb{};
    if (raw) {
        rgb = {color[0], color[1], color[2]};
    } else {
        auto const shown = color.converted(Type::RGB);
        if (!shown) return {};
        rgb = {(*shown)[0], (*shown)[1], (*shown)[2]};
    }
    for (auto &v : rgb) {
        if (!std::isfinite(v) || v < -1e-3 || v > 1 + 1e-3) return {};
        v = std::clamp(v, 0.0, 1.0);
    }
    auto const [lo, hi] = std::minmax({rgb[0], rgb[1], rgb[2]});
    if (hi - lo > 2.0 / 255) return {};
    // Coverage as PROOF's canvas takes it from a mask (drawing-item.cpp, MaskLuminanceToAlpha): a
    // grey's own value, and of a grey a CMYK profile tints a little, what PROOF draws.
    return std::clamp(0.2125 * rgb[0] + 0.7154 * rgb[1] + 0.0721 * rgb[2], 0.0, 1.0);
}

std::string gradient_end(char op)
{
    switch (op) {
        case 'S': case 'B': return "1 BB\n";
        case 's': case 'b': return "2 BB\n";
        default: return "0 BB\n";
    }
}

WrittenGradient write_gradient(SPShape const *shape, Geom::Affine const &item_to_art,
                               std::string const &unique_name, bool legacy, Geom::Point const &canvas, bool stroke,
                               bool after_gradient_fill, bool coverage)
{
    if (!shape || !shape->style) return refused("gradients without a shape");
    auto const *style = shape->style;
    // Illustrator 7's records have no gradient strokes: they came with Illustrator CS6.
    if (stroke && legacy) return refused("gradients on strokes (Illustrator 7)");
    if (stroke && (!shape->curve() || shape->curve()->empty())) return refused("gradient strokes without a path");
    SPIPaint const &paint = stroke ? static_cast<SPIPaint const &>(style->stroke) : style->fill;
    // The modern form writes the object's opacity as its transparency, and the paint's own with
    // it when the paint is the object's only one (ai-native-writer.cpp); its stops keep theirs.
    auto const paint_opacity = stroke ? style->stroke_opacity.as_double() : style->fill_opacity.as_double();
    if (legacy && (style->opacity.as_double() < 1.0 - 1e-6 || paint_opacity < 1.0 - 1e-6)) {
        return refused("gradient transparency");
    }
    auto const *server = paint.isPaintserver() && paint.href ? paint.href->getObject() : nullptr;
    // A mesh is an object of its own in Illustrator (ai-native-writer-mesh.h), which has no stroke.
    if (is<SPMeshGradient>(server)) return refused(stroke ? "mesh gradients on strokes" : "mesh gradients");
    auto const *linear = cast<SPLinearGradient>(server);
    auto const *radial = cast<SPRadialGradient>(server);
    SPGradient const *gradient = linear;
    if (radial) gradient = radial;
    if (!gradient || gradient->isSolid()) {
        return refused(stroke ? "strokes other than linear or radial gradients" : "fills other than linear or radial gradients");
    }
    if (auto why = check_references(gradient)) return refused(*why);
    bool const perceptual = PerceptualGradient::enabled(gradient);
    auto const *method = gradient->getVector()->getRepr()->attribute("proof:gradient-method");
    if (method && *method && std::string_view(method) != "oklab" && std::string_view(method) != "classic") {
        return refused("unrecognised gradient interpolation settings");
    }
    if (perceptual && legacy) return refused("Perceptual gradients (Illustrator 7)");
    if (perceptual && coverage) return refused("Perceptual gradients in opacity masks");
    if (gradient->fetchSpread() != SP_GRADIENT_SPREAD_PAD) return refused("repeating or reflected gradients");
    auto const interpolation = gradient->style->color_interpolation.getInterpolationSpace();
    if (interpolation && interpolation->getType() != Type::RGB && interpolation->getType() != Type::CMYK) {
        return refused("gradient interpolation other than RGB or CMYK");
    }
    if (unique_name.empty()) return refused("gradients without a generated name");
    auto const name_bytes = legacy ? legacy_name_bytes(unique_name) : std::optional(unique_name);
    if (!name_bytes) return refused("gradient names outside Windows-1252 (Illustrator 7)");

    bool const bbox_units = gradient->getUnits() == SP_GRADIENT_UNITS_OBJECTBOUNDINGBOX;
    std::vector<SVGLength const *> lengths;
    if (linear) {
        lengths = {&linear->x1, &linear->y1, &linear->x2, &linear->y2};
    } else {
        lengths = {&radial->cx, &radial->cy, &radial->r, &radial->fx, &radial->fy};
    }
    for (auto const *length : lengths) {
        if (bbox_units && length->unit != SVGLength::NONE && length->unit != SVGLength::PERCENT) {
            return refused("non-normalized object-bounding-box gradient lengths");
        }
        if (!bbox_units && (length->unit == SVGLength::PERCENT || length->unit == SVGLength::EM ||
                            length->unit == SVGLength::EX)) {
            return refused("viewport-relative gradient lengths");
        }
        if (!std::isfinite(length->computed)) return refused("nonfinite gradient geometry");
    }
    auto matrix = gradient->gradientTransform;
    if (bbox_units) {
        auto const box = shape->geometricBounds();
        if (!box || box->width() <= 0 || box->height() <= 0) return refused("empty gradient object bounds");
        matrix *= Geom::Affine(box->width(), 0, 0, box->height(), box->left(), box->top());
    }
    matrix *= item_to_art;
    if (!regular(matrix)) return refused("singular or nonfinite gradient transforms");
    auto const name = ps_string(*name_bytes);
    // A stroke's instance opens with `1 Bb`: 804 about an S or s in Illustrator's files between
    // the owner's set and the collaborator's (and 66 notes that paint nothing); a fill's with `Bb`.
    std::string const open = stroke ? "1 Bb\n" : "Bb\n";
    auto instance = linear ? linear_instance(linear, matrix, name, open, stroke ? shape : nullptr, item_to_art)
                           : radial_instance(radial, matrix, name, legacy, canvas, open, stroke ? shape : nullptr,
                                             item_to_art, stroke && after_gradient_fill);
    if (auto *why = std::get_if<WrittenGradient>(&instance)) return std::move(*why);

    auto const *effective = gradient->getGradientVector();
    if (effective->stops.size() < 2 || effective->stops.size() > MAX_STOPS) return refused("excessive or missing gradient stops");
    bool undefined = false;
    auto const inks = stop_inks(gradient, effective->stops.size(), undefined);
    if (undefined) return refused("spot ink stops without a definition");
    if (inks.empty()) return refused("invalid gradient stops");
    if (perceptual && std::any_of(inks.begin(), inks.end(), [](auto const &ink) { return bool(ink); })) {
        return refused("Perceptual gradients with spot inks");
    }
    auto const midpoints = PerceptualGradient::midpoints(gradient);
    // Past its end a gradient extends with its last stop in SVG, but with the first of those at its
    // end in Illustrator (29.2.1, linear and radial, 2026-10-22): two different ones there can't be
    // drawn the same. At its start, and inside, both draw as SVG does.
    for (std::size_t index = 1; index < effective->stops.size(); ++index) {
        auto const &a = effective->stops[index - 1], &b = effective->stops[index];
        if (a.offset < 1.0 - 1e-6 || !a.color || !b.color) continue;
        bool const same_ink = inks[index - 1] && inks[index]
            ? inks[index - 1]->name == inks[index]->name && inks[index - 1]->tint == inks[index]->tint
            : !inks[index - 1] && !inks[index];
        if (!same_ink || *a.color != *b.color) return refused("gradient stops of different colours at its end");
    }
    WrittenGradient out;
    std::optional<Type> model;
    std::vector<std::string> stops;
    double previous = -1;
    // Stops with an opacity of their own are written as Illustrator writes every stop since
    // CS4: the opacity and the marker 6 before the midpoint. So are the stops of a gradient
    // with an ink defined in RGB or Lab, as all 26 of Illustrator's are. A gradient whose stops
    // are all opaque and process or CMYK inks keeps the older form; Illustrator 7 has no other.
    bool clear = false, newer = false;
    std::map<Colors::CMS::Profile const *, bool> rgb_profiles;
    for (std::size_t index = 0; index < effective->stops.size(); ++index) {
        auto const &stop = effective->stops[index];
        if (!stop.color || !std::isfinite(stop.color->getOpacity())) return refused("invalid gradient stops");
        clear = clear || stop.color->getOpacity() < 1.0 - 1e-6;
        auto const &ink = inks[index];
        auto const ink_space = ink ? ink->alternate.getSpace() : nullptr;
        newer = newer || clear || (ink_space && ink_space->getComponentType() != Type::CMYK);
    }
    if (clear && legacy) return refused("gradient transparency");
    for (std::size_t index = 0; index < effective->stops.size(); ++index) {
        auto const &stop = effective->stops[index];
        auto const &ink = inks[index];
        if (!stop.color || !std::isfinite(stop.offset)) return refused("invalid gradient stops");
        auto const &color = *stop.color;
        if (coverage) {
            // A mask's grey, whatever space it was given in: Gray, `grey 0`, as Illustrator
            // writes the stops of its own masks' gradients.
            if (ink) return refused("spot inks in opacity masks");
            auto const grey = coverage_grey(color);
            if (!grey) return refused("colours other than grey in opacity masks");
            if (interpolation && interpolation->getType() != Type::RGB) {
                return refused("gradient interpolation other than RGB in opacity masks");
            }
            auto const offset = std::round(stop.offset * 1000000.0) / 1000000.0;
            if (offset < previous) return refused("gradient stops out of order");
            previous = offset;
            auto const after = (newer ? " " + num(std::clamp(color.getOpacity(), 0.0, 1.0)) + " 6" : std::string()) +
                               " 50 " + num(offset * 100) + " %_Bs\n";
            if (Colors::DocumentColors::mode(shape->document) == Type::CMYK) {
                // Illustrator makes a Gray stop a tint of black there, one less the grey: the same.
                stops.push_back(num(*grey) + " 0" + after);
                continue;
            }
            // In an RGB document Illustrator draws a Gray stop through its Gray working space, where
            // black is 26 of 255 (Illustrator 29.2.1, 2026-10-22); a neutral RGB stop is as given.
            auto const alternate = Colors::Color(Type::RGB, {*grey, *grey, *grey}).converted(Type::CMYK);
            if (!alternate) return refused("RGB gradient colours without a CMYK alternate");
            stops.push_back(std::format("{} {} {} {} {} {} {} 2", num((*alternate)[0]), num((*alternate)[1]),
                                        num((*alternate)[2]), num((*alternate)[3]), num(*grey), num(*grey), num(*grey)) +
                            after);
            continue;
        }
        auto const space = color.getSpace();
        // CMYK in a profile (the document's, as an import assigns it to every process paint of a
        // CMYK file) is written as its numbers, as solid paint is; the profile goes with the PDF
        // half, where PROOF's export keeps the numbers too.
        bool const icc_cmyk = space && space->getType() == Type::CMS && space->getComponentType() == Type::CMYK;
        bool icc_rgb = false;
        if (!ink && space && space->getType() == Type::CMS && space->getComponentType() == Type::RGB &&
            space->hasValidCmsProfile()) {
            auto const profile = space->getProfile();
            if (profile) {
                auto [known, added] = rgb_profiles.try_emplace(profile.get(), false);
                if (added) known->second = standard_rgb_profile(*profile);
                icc_rgb = known->second;
            }
        }
        if (!ink && !icc_cmyk && !icc_rgb && (!space || (space->getType() != Type::RGB && space->getType() != Type::CSSNAME &&
                                             space->getType() != Type::CMYK))) {
            return refused("gradient colours other than standard RGB or CMYK");
        }
        // A CSS colour name is standard sRGB with different spelling, not a named ink. Each stop
        // has a colour style of its own, so RGB stops go beside CMYK or spot ones as Illustrator
        // writes them (the owner's one such gradient: a spot ink, style 3, then RGB, style 2).
        auto const stop_model = ink || icc_cmyk ? Type::CMYK
                                : icc_rgb || space->getType() == Type::CSSNAME ? Type::RGB : space->getType();
        if (legacy && model && *model != stop_model) return refused("mixed gradient colour spaces (Illustrator 7)");
        model = stop_model;
        if (interpolation && interpolation->getType() != stop_model) {
            return refused("gradient interpolation different from its stop colour space");
        }
        auto const offset = std::round(stop.offset * 1000000.0) / 1000000.0;
        // Two stops at one place, a hard edge or two at an end, are written as they come: Illustrator
        // keeps both in the order listed (the owner's files: six gradients with two at 100, one with
        // two at 0). Illustrator 7's reading of them is unqualified.
        if (offset < previous) return refused("gradient stops out of order");
        if (offset == previous && legacy) return refused("gradient hard stops (Illustrator 7)");
        previous = offset;
        // After the colour and its style: [opacity 6] midpoint ramp.
        auto const after = (newer ? " " + num(std::clamp(color.getOpacity(), 0.0, 1.0)) + " 6" : std::string()) +
                           " " + num(perceptual ? midpoints[index] * 100 : 50) + " " + num(offset * 100);
        auto const tail = after + " %_Bs\n";
        if (ink) {
            // A tint of a spot ink: its full-strength CMYK, its name, the tint taken from 1 as
            // x writes it, colour style 3 (Adobe's 1998 specification, Bs; Illustrator writes
            // the same as the stand-in of its own named stops, `%_BS`).
            auto const &a = ink->alternate;
            if (ink->name == "All" || ink->name == "[Registration]") return refused("Registration in gradients");
            auto const definition = ink_definition(a, shape->document);
            if (auto const *why = std::get_if<std::string>(&definition)) {
                return refused(*why == "out-of-range ink definitions" ? "out-of-range gradient colours" : *why);
            }
            auto const &defined = std::get<InkDefinition>(definition);
            bool const process = defined.model == InkDefinition::Model::CMYK;
            if (legacy && !process) return refused("spot inks defined in RGB or Lab (Illustrator 7)");
            auto const ink_bytes = legacy ? legacy_name_bytes(ink->name) : std::optional(ink->name);
            if (!ink_bytes) return refused("ink names outside Windows-1252 (Illustrator 7)");
            if (!std::isfinite(ink->tint)) return refused("out-of-range gradient colours");
            auto const &cmyk = defined.cmyk;
            auto const stand_in = std::format("{} {} {} {} {} {} 3", num(cmyk[0]), num(cmyk[1]), num(cmyk[2]),
                                              num(cmyk[3]), ps_string(*ink_bytes), num(1.0 - ink->tint));
            if (process) {
                stops.push_back(stand_in + tail);
            } else {
                // Illustrator's exact stop for an ink defined in RGB or Lab, after the stand-in:
                // the CMYK at this tint and the definition, the tint, its type (1 RGB, 2 Lab),
                // then colour style 4. Illustrator 29 writes 4 for both (its copy of the control,
                // 2026-10-21), as all 22 Lab stops in the owner's files; 4 RGB ones there, from
                // an older version, have 5, which Illustrator 29 reads too.
                auto const operands = ink_operands(defined, a, ink->tint, shape->document);
                if (operands.empty()) return refused("spot inks without a CMYK equivalent");
                stops.push_back(stand_in + after + " %_BS\n%_" + operands + " " + ps_string(*ink_bytes) + " " +
                                num(1.0 - ink->tint) + " " + std::to_string(static_cast<int>(defined.model)) + " 4" +
                                after + " Bs\n");
            }
            out.inks.emplace_back(ink->name, defined);
            continue;
        }
        auto const count = stop_model == Type::CMYK ? 4u : 3u;
        for (unsigned i = 0; i < count; ++i) {
            if (!std::isfinite(color[i]) || color[i] < 0 || color[i] > 1) return refused("out-of-range gradient colours");
        }
        std::ostringstream line;
        if (stop_model == Type::RGB) {
            auto const alternate = color.converted(Type::CMYK);
            if (!alternate) return refused("RGB gradient colours without a CMYK alternate");
            for (unsigned i = 0; i < 4; ++i) line << num((*alternate)[i]) << ' ';
        }
        for (unsigned i = 0; i < count; ++i) line << num(color[i]) << ' ';
        line << (stop_model == Type::CMYK ? 1 : 2) << tail;
        stops.push_back(line.str());
    }

    // Illustrator lists a linear gradient's stops last first and a radial one's first first,
    // and takes them in that order whatever their ramp points say: a radial's written last
    // first opened inside out in Illustrator 29 (2026-10-20). Its own files, without
    // exception: 6,906 linear definitions descending, 4,482 radial ones ascending.
    auto definition = [&](bool newer_method) {
        std::ostringstream text;
        text << "%AI5_BeginGradient: " << name << '\n' << name << (radial ? " 1 " : " 0 ") << stops.size()
             << (newer_method ? " 1 0 Bd\n[\n" : " Bd\n[\n");
        if (radial) {
            for (auto const &stop : stops) text << stop;
        } else {
            for (auto it = stops.rbegin(); it != stops.rend(); ++it) text << *it;
        }
        text << "BD\n%AI5_EndGradient\n";
        return text.str();
    };
    out.definition = perceptual ? "%AI17_Begin_Content_if_version_gt:24 16\n" + definition(true) +
        "%AI17_Alternate_Content\n" + definition(false) + "%AI17_End_Versioned_Content\n" : definition(false);
    out.begin = std::get<std::string>(instance);
    return out;
}

} // namespace Inkscape::Extension::Internal::AiNative
