// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Illustrator's own records for a PROOF document. See ai-native-writer.h.
 *
 * The record forms are the ones Illustrator 29.2.1 opened as editable art in PROOF's spikes
 * (tools/native-ai-spike.py, 2026-10-02; the writer spike, 2026-10-17), in the coordinates
 * of the writer recipe (doc/PROOF-ai-interchange.md): the first artboard's top-left corner
 * is 0,0, y negative downward, in points.
 * The Illustrator 7 variant uses Adobe's 1998 file-format specification, sections 5 and 9,
 * and the standalone writer spike: bottom-left origin, ten-operand layers and x/X spot tints.
 */

#include "ai-native-writer.h"
#include "ai-native-writer-gradient.h"
#include "ai-native-writer-ink.h"
#include "ai-native-writer-picture.h"
#include "ai-native-writer-string.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <variant>

#include <glib.h>
#include <2geom/bezier-curve.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>

#include "colors/color.h"
#include "colors/document-colors.h"
#include "colors/spaces/base.h"
#include "document.h"
#include "helper/geom.h"
#include "object/sp-clippath.h"
#include "object/sp-gradient.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-mask.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-page.h"
#include "object/sp-pattern.h"
#include "object/sp-rect.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-use.h"
#include "page-manager.h"
#include "spot-ink.h"
#include "style.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;

constexpr double PX_TO_PT = 0.75;

/// The node tool's best fit when no authored type is given: opposite handles, or a
/// handle tangent to the neighbouring line. Two straight sides stay a corner.
bool smooth_join(Geom::Path const &path, std::size_t node)
{
    auto const count = path.size();
    if (!count || (!path.closed() && (node == 0 || node == count))) return false;
    node %= count;
    auto prev = (node + count - 1) % count, next = node;
    // Ellipse construction can leave a microscopic closing line. It rounds to
    // zero in the records; infer both coincident anchors from the actual sides.
    auto tiny_line = [&](std::size_t at) {
        auto const &curve = path[at];
        return curve.isLineSegment() && Geom::distance(curve.initialPoint(), curve.finalPoint()) < 1e-9;
    };
    if (path.closed()) {
        for (std::size_t i = 0; i < count && tiny_line(prev); ++i) prev = (prev + count - 1) % count;
        for (std::size_t i = 0; i < count && tiny_line(next); ++i) next = (next + 1) % count;
    }
    auto const &before = path[prev];
    auto const &after = path[next];
    auto const anchor = after.initialPoint();
    auto const incoming = dynamic_cast<Geom::CubicBezier const *>(&before);
    auto const outgoing = dynamic_cast<Geom::CubicBezier const *>(&after);
    auto back = incoming ? (*incoming)[2] - anchor : Geom::Point();
    auto front = outgoing ? (*outgoing)[1] - anchor : Geom::Point();
    bool const back_handle = back.length() > 1e-9, front_handle = front.length() > 1e-9;
    if (!back_handle && !front_handle) return false;
    if (!back_handle) {
        if (!before.isLineSegment()) return false;
        back = before.initialPoint() - anchor;
    }
    if (!front_handle) {
        if (!after.isLineSegment()) return false;
        front = after.finalPoint() - anchor;
    }
    double const scale = back.length() * front.length();
    return scale > 0 && Geom::dot(back, front) < 0 && std::abs(Geom::cross(back, front)) <= 1e-6 * scale;
}

/// A number as Illustrator writes it: up to four decimals, no trailing zeros.
std::string num(double v)
{
    auto s = std::format("{:.4f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

/// A number to eight decimals, for what a repeat multiplies: a pattern's tile and matrix. At four,
/// a tile some thousand repeats out from its origin would lie a point off.
std::string precise(double v)
{
    auto s = std::format("{:.8f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

/// A PostScript string: parentheses and backslashes escaped, other bytes outside ASCII in octal.
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

double scale24(unsigned value)
{
    return value / double(SP_SCALE24_MAX);
}

/// Illustrator's transparency for an object that has none of its own: `mode opacity isolate
/// knockout shape Xy`, Normal at full opacity.
constexpr std::string_view OPAQUE = "0 1 0 0 0 Xy";

/// Illustrator's number for each of PROOF's blend modes, in SPBlendMode's order. Illustrator's
/// own runs Normal, Multiply, Screen, Overlay, Soft Light, Hard Light, Color Dodge, Color Burn,
/// Darken, Lighten, Difference, Exclusion, Hue, Saturation, Color, Luminosity.
constexpr std::array<int, SP_CSS_BLEND_ENDMODE> BLEND_INDEX{0, 1, 2, 8, 9, 3, 6, 7, 5, 4, 10, 11, 12, 13, 14, 15};

class Writer
{
public:
    Writer(SPDocument *doc, RecordsVersion version)
        : _doc(doc), _legacy(version == RecordsVersion::Illustrator7)
    {}

    WrittenRecords write();

private:
    void layer(SPGroup const *group);
    void item(SPItem const *item);
    void contents(SPObject const *parent);
    void shape(SPShape const *shape, bool clipped);
    void check_effects(SPItem const *item, std::string_view where = {});
    std::string transparency(SPItem const *item, double alpha = 1.0);
    void end_group(std::string_view own);
    bool open_clip(SPItem const *item);
    std::string opacity_mask(SPItem const *owner);
    void clear_state();
    std::vector<std::string> path_types(SPShape const *shape, Geom::PathVector const &paths, Geom::Affine const &affine);
    void path_data(Geom::Path const &path, std::string_view types);
    void state(std::string &cache, std::string const &line);
    std::optional<std::string> paint(SPIPaint const &p, SPStyle const *style, bool fill, Geom::Affine const &affine);
    std::optional<std::string> pattern_paint(SPPattern const *pattern, Geom::Affine const &affine, bool fill);
    std::optional<std::string> define_pattern(SPPattern const *root, double width, double height);
    std::optional<std::string> colour(Colors::Color const &c, bool fill);
    bool declare_ink(std::string const &name, InkDefinition const &ink);
    std::string encoded_name(std::string const &text, std::string_view what);
    void ink_comments(std::ostream &head);
    void refuse(std::string what) { _refused.insert(std::move(what)); }

    SPDocument *_doc;
    bool const _legacy;
    Geom::Affine _to_art;
    /// The art origin on the y-down canvas where Illustrator applies a gradient's Bg matrix.
    Geom::Point _canvas;
    std::ostringstream _body;
    std::set<std::string> _refused;
    std::map<std::string, InkDefinition> _spots;
    /// Registration used as defined in CMYK (Xs), and as an RGB document defines it, in RGB (Xz).
    bool _registration = false;
    bool _registration_rgb = false;
    std::vector<std::string> _gradients;
    /// Each pattern definition written: the tile it is for, its name, and its records.
    struct PatternDefinition
    {
        SPPattern const *root;
        double width, height;
        std::string name, records;
    };
    std::vector<PatternDefinition> _patterns;
    /// Whether a pattern's tile is being written.
    bool _in_pattern = false;
    /// The opacity masks whose art is being written, outermost first: in one, a colour is how
    /// much of the object shows.
    std::vector<SPMask const *> _masks;
    /// The object whose mask goes with the next item written: the import's wrapper about it.
    SPItem const *_mask_owner = nullptr;
    std::size_t _layers = 0;
    // What has been set already, so each operator is written only when it changes.
    std::string _fill_over, _fill, _stroke_over, _stroke, _stroke_style, _rule;
    /// The transparency that holds, as for the paints; a file begins opaque.
    std::string _transparency{OPAQUE};
    /// Whether `1 Xw` (hidden) and `1 A` (locked) hold where the body is.
    bool _hiding = false, _locking = false;
    /// The alpha of the colour the last paint() set, which the modern form takes into the
    /// object's opacity.
    double _alpha = 1.0;
};

std::string Writer::encoded_name(std::string const &text, std::string_view what)
{
    if (!_legacy) return ps_string(text);
    auto const bytes = legacy_name_bytes(text);
    if (!bytes) {
        refuse(std::string(what) + " outside Windows-1252 (Illustrator 7)");
        return {};
    }
    return ps_string(*bytes);
}

void Writer::ink_comments(std::ostream &head)
{
    if (_registration) head << "%%CMYKProcessColor: 1 1 1 1 ([Registration])\n";
    if (_registration_rgb) head << "%%RGBProcessColor: 0 0 0 ([Registration])\n";
    bool first = true;
    for (auto const &[name, c] : _spots) {
        head << (first ? "%%DocumentCustomColors: " : "%%+ ") << encoded_name(name, "ink names") << '\n';
        first = false;
    }
    // CMYK inks by their CMYK, then RGB and Lab inks by their RGB, as Illustrator lists them
    // (ai-native-writer-ink.h).
    first = true;
    for (auto const &[name, ink] : _spots) {
        if (ink.model != InkDefinition::Model::CMYK) continue;
        auto const &c = ink.cmyk;
        head << (first ? "%%CMYKCustomColor: " : "%%+ ")
             << std::format("{} {} {} {} {}\n", num(c[0]), num(c[1]), num(c[2]), num(c[3]), encoded_name(name, "ink names"));
        first = false;
    }
    first = true;
    for (auto const &[name, ink] : _spots) {
        if (ink.model == InkDefinition::Model::CMYK) continue;
        auto const &c = ink.rgb;
        head << (first ? "%%RGBCustomColor: " : "%%+ ")
             << std::format("{} {} {} {}\n", num(c[0]), num(c[1]), num(c[2]), encoded_name(name, "ink names"));
        first = false;
    }
}

std::optional<std::string> Writer::colour(Colors::Color const &c, bool fill)
{
    if (c.getOpacity() < 1.0 - 1e-6 && _legacy) {
        refuse("transparency");
        return {};
    }
    _alpha = c.getOpacity();
    if (!_masks.empty()) {
        // In a mask a colour is how much shows: the grey PROOF draws, as Gray, which means the
        // same in Illustrator whatever the document's colour mode.
        auto const grey = coverage_grey(c);
        if (!grey) {
            refuse("colours other than grey in opacity masks");
            return {};
        }
        return num(*grey) + (fill ? " g" : " G");
    }
    if (c.getSpace() && c.getSpace()->getComponentType() == Type::CMYK) {
        return std::format("{} {} {} {} {}", num(c[0]), num(c[1]), num(c[2]), num(c[3]), fill ? "k" : "K");
    }
    if (auto rgb = c.converted(Type::RGB)) {
        return std::format("{} {} {} {}", num((*rgb)[0]), num((*rgb)[1]), num((*rgb)[2]), fill ? "Xa" : "XA");
    }
    refuse("colours that are neither CMYK nor RGB");
    return {};
}

/// The operator that sets this paint, or nothing for none (and for what is refused). `affine`
/// places the painted object in art space, as a pattern needs.
std::optional<std::string> Writer::paint(SPIPaint const &p, SPStyle const *style, bool fill, Geom::Affine const &affine)
{
    _alpha = 1.0;
    if (p.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL || p.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) {
        refuse("context paint");
        return {};
    }
    if (p.paintOrigin == SP_CSS_PAINT_ORIGIN_CURRENT_COLOR) return colour(style->color.getColor(), fill);
    if (p.isNone()) return {};
    if (p.isColor()) return colour(p.getColor(), fill);
    auto server = p.isPaintserver() && p.href ? p.href->getObject() : nullptr;
    auto swatch = cast<SPGradient>(server);
    if (swatch && swatch->isSolid()) {
        auto reserved = [](auto const *node) {
            auto const name = node->attribute("proof:ink");
            return name && (std::string_view(name) == "All" || std::string_view(name) == "[Registration]");
        };
        auto *ink_node = swatch->getRepr();
        auto ink = SpotInk::read(ink_node);
        if (!ink && swatch->getVector()) {
            if (reserved(ink_node)) {
                refuse("malformed Registration ink definitions");
                return {};
            }
            ink_node = swatch->getVector()->getRepr();
            ink = SpotInk::read(ink_node);
        }
        if (!ink) {
            if (reserved(ink_node)) {
                refuse("malformed Registration ink definitions");
                return {};
            }
            return colour(const_cast<SPGradient *>(swatch)->getPreviewAverageColor(), fill);
        }
        if (!_masks.empty()) {
            refuse("spot inks in opacity masks");
            return {};
        }
        // All is the document's reserved PDF Registration ink. A literal
        // [Registration] spot has a different PDF meaning, so do not silently
        // promote it to Registration in the private artwork.
        if (ink->name == "[Registration]") {
            refuse("ambiguous Registration ink names");
            return {};
        }
        auto const &a = ink->alternate;
        if (ink->name == "All") {
            if (_legacy) {
                refuse("Registration ink (Illustrator 7)");
                return {};
            }
            bool valid_tint = true;
            if (auto const text = ink_node->attribute("proof:ink-tint")) {
                char *end = nullptr;
                auto const tint = g_ascii_strtod(text, &end);
                valid_tint = end != text && std::isfinite(tint) && tint >= 0 && tint <= 1;
                while (end && g_ascii_isspace(*end)) ++end;
                valid_tint = valid_tint && end && !*end;
            }
            auto const type = a.getSpace() ? a.getSpace()->getComponentType() : Type::NONE;
            bool const cmyk = type == Type::CMYK && a[0] == 1.0 && a[1] == 1.0 && a[2] == 1.0 && a[3] == 1.0;
            // An RGB document's Registration is RGB black: the owner's Illustrator CS5 and CS6
            // RGB documents define it so, and their pages draw it /Separation /All /DeviceRGB.
            // Illustrator 29 keeps it so in a CMYK document too, records and page.
            bool const rgb = type == Type::RGB && a[0] == 0.0 && a[1] == 0.0 && a[2] == 0.0;
            if (!valid_tint || a.getOpacity() != 1.0 || (!cmyk && !rgb)) {
                refuse("noncanonical Registration ink definitions");
                return {};
            }
            if (cmyk ? _registration_rgb : _registration) {
                refuse("Registration defined in both CMYK and RGB");
                return {};
            }
            if (cmyk) {
                _registration = true;
                // Illustrator writes a process Registration declaration and Xs/XS,
                // never an ordinary custom ink named All. Its operand is 1 - tint.
                return "1 1 1 1 ([Registration]) " + num(1.0 - ink->tint) + (fill ? " Xs" : " XS");
            }
            // In RGB Illustrator writes it as an RGB ink, the CMYK of the tinted colour first, with
            // Xz/XZ: `c m y k 0 0 0 ([Registration]) 0 1 Xz` at full strength.
            auto const definition = ink_definition(a, _doc);
            auto const operands = std::holds_alternative<InkDefinition>(definition)
                ? ink_operands(std::get<InkDefinition>(definition), a, ink->tint, _doc) : std::string();
            if (operands.empty()) {
                refuse("noncanonical Registration ink definitions");
                return {};
            }
            _registration_rgb = true;
            return operands + " ([Registration]) " + num(1.0 - ink->tint) + " 1" + (fill ? " Xz" : " XZ");
        }
        auto const definition = ink_definition(a, _doc);
        if (auto const *why = std::get_if<std::string>(&definition)) {
            refuse(*why);
            return {};
        }
        auto const &defined = std::get<InkDefinition>(definition);
        bool const process = defined.model == InkDefinition::Model::CMYK;
        if (_legacy && !process) {
            refuse("spot inks defined in RGB or Lab (Illustrator 7)");
            return {};
        }
        auto const operands = ink_operands(defined, a, ink->tint, _doc);
        if (operands.empty()) {
            refuse("spot inks without a CMYK equivalent");
            return {};
        }
        if (!declare_ink(ink->name, defined)) return {};
        auto const head = operands + " " + encoded_name(ink->name, "ink names");
        // The operand is the tint taken from 1: 0 is the ink at full strength.
        // Illustrator 7 uses x/X for CMYK spots at every tint. Its Xx/XX form is RGB only.
        if (_legacy) return head + " " + num(1.0 - ink->tint) + " " + (fill ? "x" : "X");
        // x knows only CMYK: an RGB or Lab ink is Xx with its type at full strength too.
        if (process && ink->tint >= 1.0 - 1e-6) return head + " 0 " + (fill ? "x" : "X");
        return head + " " + num(1.0 - ink->tint) + " " + std::to_string(static_cast<int>(defined.model)) +
               (fill ? " Xx" : " XX");
    }
    if (auto pattern = cast<SPPattern>(server)) return pattern_paint(pattern, affine, fill);
    refuse(is<SPMeshGradient>(server) ? "mesh gradients" : "gradients");
    return {};
}

/**
 * `(name) 0 0 1 1 0 0 0 0 0 [a b c d tx ty] p` (`P` for a stroke): a pattern as Illustrator 9 and
 * later place one, all of it in the matrix from the pattern's own space to art space; the numbers
 * before it are at rest (ai-native-reader.cpp, Reader::pattern_paint).
 *
 * The subset is the one the import makes and PROOF's own pattern library has: tiles and content in
 * the user space of what they paint, with any x, y, width, height and pattern transform.
 */
std::optional<std::string> Writer::pattern_paint(SPPattern const *pattern, Geom::Affine const &affine, bool fill)
{
    if (_legacy) {
        refuse("patterns (Illustrator 7)");
        return {};
    }
    if (_in_pattern) {
        refuse("patterns inside patterns");
        return {};
    }
    if (!_masks.empty()) {
        refuse("patterns in opacity masks");
        return {};
    }
    if (pattern->patternUnits() != SPPattern::UNITS_USERSPACEONUSE ||
        pattern->patternContentUnits() != SPPattern::UNITS_USERSPACEONUSE) {
        refuse("patterns sized to their object");
        return {};
    }
    if (pattern->viewbox()) {
        refuse("patterns with a view box");
        return {};
    }
    auto const *root = pattern->rootPattern();
    double const w = pattern->width(), h = pattern->height();
    if (!root->hasItemChildren() || !std::isfinite(w) || !std::isfinite(h) || w <= 1e-9 || h <= 1e-9) {
        refuse("empty patterns");
        return {};
    }
    // Illustrator's pattern space is the tile's with y up, its box from 0,0. SVG puts the
    // content's origin at the tile's top-left corner, which is 0,h there; x, y and the pattern
    // transform then place the tile in the painted object's space.
    auto const m = Geom::Affine(1, 0, 0, -1, 0, h) * Geom::Translate(pattern->x(), pattern->y()) *
                   pattern->getTransform() * affine;
    bool numbers = true;
    for (int i = 0; i < 6; ++i) numbers = numbers && std::isfinite(m[i]);
    if (!numbers || std::abs(m.det()) < 1e-12) {
        refuse("patterns that can't be placed");
        return {};
    }
    auto const name = define_pattern(root, w, h);
    if (!name) return {};
    _alpha = 1.0;
    return std::format("{} 0 0 1 1 0 0 0 0 0 [{} {} {} {} {} {}] {}", ps_string(*name), precise(m[0]), precise(m[1]),
                       precise(m[2]), precise(m[3]), precise(m[4]), precise(m[5]), fill ? 'p' : 'P');
}

/**
 * The pattern's definition, once for each tile: `(name) 0 0 width height`, its art on hidden lines
 * as the document's is written, `E`. Its name is the pattern's label (the import keeps
 * Illustrator's swatch name there), else its id, numbered when two tiles would share one.
 */
std::optional<std::string> Writer::define_pattern(SPPattern const *root, double w, double h)
{
    for (auto const &d : _patterns) {
        if (d.root == root && d.width == w && d.height == h) return d.name;
    }
    std::string const base = root->label() ? root->label() : root->getId() ? root->getId() : "Pattern";
    auto taken = [&](std::string const &name) {
        return std::any_of(_patterns.begin(), _patterns.end(), [&](auto const &d) { return d.name == name; });
    };
    auto name = base;
    for (int i = 2; taken(name); ++i) name = base + " " + std::to_string(i);

    // The tile's art in a state of its own: Illustrator reads it so (Reader::pattern_section),
    // and what holds around it must hold again after it.
    std::ostringstream art;
    std::swap(_body, art);
    auto saved = std::tuple(_to_art, _fill_over, _fill, _stroke_over, _stroke, _stroke_style, _rule, _transparency,
                            _hiding, _locking);
    _to_art = Geom::Affine(1, 0, 0, -1, 0, h);
    clear_state();
    _in_pattern = true;
    _body << "0 A\n0 Xw\n";
    _hiding = _locking = false;
    for (auto const &child : root->children) {
        if (auto it = cast<SPItem>(&child)) item(it);
    }
    _in_pattern = false;
    std::swap(_body, art);
    std::tie(_to_art, _fill_over, _fill, _stroke_over, _stroke, _stroke_style, _rule, _transparency, _hiding, _locking) =
        std::move(saved);
    if (!_refused.empty()) return {};

    auto const quoted = ps_string(name);
    std::string records = "%AI3_BeginPattern: " + quoted + "\n" + quoted + " 0 0 " + precise(w) + " " + precise(h) + "\n";
    std::istringstream lines(art.str());
    for (std::string line; std::getline(lines, line);) records += "%_" + line + "\n";
    records += "E\n%AI3_EndPattern\n";
    _patterns.push_back({root, w, h, name, std::move(records)});
    return name;
}

/// Adds a spot ink to those the file declares; false, refused, when the name has another definition.
bool Writer::declare_ink(std::string const &name, InkDefinition const &ink)
{
    if (_legacy && !legacy_name_bytes(name)) {
        refuse("ink names outside Windows-1252 (Illustrator 7)");
        return false;
    }
    auto [known, added] = _spots.emplace(name, ink);
    if (!added && known->second != ink) {
        refuse("one ink name with two definitions");
        return false;
    }
    return true;
}

void Writer::state(std::string &cache, std::string const &line)
{
    if (cache == line) return;
    cache = line;
    _body << line << '\n';
}

std::vector<std::string> Writer::path_types(SPShape const *shape, Geom::PathVector const &paths, Geom::Affine const &affine)
{
    std::vector<std::string> types(paths.size());
    auto const attr = shape->getAttribute("sodipodi:nodetypes");
    if (!attr) return types;
    // The node tool indexes types after converting the local geometry to lines and
    // cubics. Keep the writer's transformed geometry and its approximation order:
    // when an arc gains subdivisions, carry the original segment's endpoint type
    // to its new final piece, and infer the newly inserted joins separately.
    auto const local = pathv_to_linear_and_cubic_beziers(*shape->curve());
    std::string_view remaining(attr);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        // The node tool ignores naked movetos. Closed paths duplicate the seam
        // type, and use the final duplicate when it is present.
        auto const count = local[i].empty() ? 0 : local[i].size() + 1;
        auto const take = std::min(count, remaining.size());
        std::string const authored(remaining.substr(0, take));
        remaining.remove_prefix(take);
        if (local[i].size() == paths[i].size()) {
            types[i] = authored;
            continue;
        }
        types[i].assign(paths[i].empty() ? 0 : paths[i].size() + 1, 'b');
        if (!authored.empty() && !types[i].empty()) types[i][0] = authored[0];
        std::size_t from = 1, to = 1;
        for (auto const &curve : (*shape->curve())[i]) {
            Geom::Path piece(curve.initialPoint());
            piece.append(curve);
            Geom::PathVector const one{piece};
            auto const old_count = pathv_to_linear_and_cubic_beziers(one)[0].size();
            auto const new_count = pathv_to_linear_and_cubic_beziers(one * affine)[0].size();
            if (old_count == new_count) {
                for (std::size_t j = 0; j < new_count && from + j < authored.size() && to + j < types[i].size(); ++j) {
                    types[i][to + j] = authored[from + j];
                }
            } else if (old_count && new_count && from + old_count <= authored.size() && to + new_count <= types[i].size()) {
                types[i][to + new_count - 1] = authored[from + old_count - 1];
            }
            from += old_count;
            to += new_count;
        }
        if (paths[i].closed() && !types[i].empty() && authored.size() < count) types[i].back() = types[i].front();
    }
    return types;
}

void Writer::path_data(Geom::Path const &path, std::string_view types)
{
    // A coordinate that isn't a number would be written "nan" or "inf", which no reader takes.
    auto const finite = [](Geom::Point const &p) { return std::isfinite(p.x()) && std::isfinite(p.y()); };
    bool numbers = finite(path.initialPoint());
    for (auto const &c : path) {
        if (auto cubic = dynamic_cast<Geom::CubicBezier const *>(&c)) {
            numbers = numbers && finite((*cubic)[1]) && finite((*cubic)[2]);
        }
        numbers = numbers && finite(c.finalPoint());
    }
    if (!numbers) {
        refuse("nonfinite path coordinates");
        return;
    }
    auto const start = path.initialPoint();
    _body << num(start.x()) << ' ' << num(start.y()) << " m\n";
    auto segment = [&](Geom::Curve const &c, std::size_t node) {
        char type = node < types.size() ? types[node] : '\0';
        if (path.closed() && node == path.size() && node >= types.size() && !types.empty()) type = types.front();
        bool const smooth = type == 's' || type == 'z' || type == 'a' || (type != 'c' && smooth_join(path, node));
        if (auto cubic = dynamic_cast<Geom::CubicBezier const *>(&c)) {
            _body << std::format("{} {} {} {} {} {} {}\n", num((*cubic)[1].x()), num((*cubic)[1].y()),
                                 num((*cubic)[2].x()), num((*cubic)[2].y()), num((*cubic)[3].x()),
                                 num((*cubic)[3].y()), smooth ? 'c' : 'C');
        } else {
            _body << num(c.finalPoint().x()) << ' ' << num(c.finalPoint().y()) << (smooth ? " l\n" : " L\n");
        }
    };
    std::size_t node = 0;
    // Includes a non-degenerate closing line. Its destination type, or the final
    // cubic's when it ends at the start, carries the closed seam's semantics.
    for (auto const &c : path) segment(c, ++node);
}

/// Opens a clipping group for the item's clipping path: `q`, the path, `W n`. Whether it did.
bool Writer::open_clip(SPItem const *item)
{
    auto clip = item->getClipObject();
    if (!clip) return false;
    std::vector<SPShape const *> shapes;
    for (auto const &child : clip->children) {
        if (auto s = cast<SPShape>(&child)) {
            shapes.push_back(s);
        } else if (is<SPItem>(&child)) {
            shapes.clear();
            break;
        }
    }
    if (clip->clippath_units() || shapes.size() != 1 || !shapes[0]->curve()) {
        refuse("clipping paths other than one path");
        return false;
    }
    auto const affine = shapes[0]->transform * item->i2doc_affine() * _to_art;
    auto pv = pathv_to_linear_and_cubic_beziers(*shapes[0]->curve() * affine);
    if (pv.empty()) {
        refuse("empty clipping paths");
        return false;
    }
    if (shapes[0]->getClipObject()) {
        refuse("clipping on a clipping path's geometry");
        return false;
    }
    if (shapes[0]->getMaskObject()) {
        refuse("opacity masks on clipping paths");
        return false;
    }
    auto const types = path_types(shapes[0], pv, affine);
    _body << "q\n";
    // SVG clips use clip-rule, independently of the artwork's fill-rule. XR is shared
    // state: update its cache so the next painted object restores its own fill rule.
    state(_rule, shapes[0]->style->clip_rule.computed == SP_WIND_RULE_EVENODD ? "1 XR" : "0 XR");
    // The clipping path has none of the transparency that holds: released, it is an ordinary path.
    if (!_legacy) state(_transparency, std::string(OPAQUE));
    // Several contours of ONE path are a compound clip, including its holes. Separate
    // SVG clip children are a union, so they cannot be concatenated this way.
    if (pv.size() > 1) _body << "*u\n";
    for (std::size_t i = 0; i < pv.size(); ++i) {
        // Clipping implicitly closes an open contour. Illustrator also needs the actual
        // closing side, or it folds the last anchor into the first (as with crop clips).
        pv[i].close(true);
        path_data(pv[i], types[i]);
        _body << "h\nW\nn\n";
    }
    if (pv.size() > 1) _body << "*U\n";
    return true;
}

/// `clipped`: the shape is the only content of its own clipping group, which carries the
/// shape's transparency (Writer::item); the shape itself has only its paint's.
void Writer::shape(SPShape const *s, bool clipped)
{
    auto const *style = s->style;
    if (!s->curve() || s->curve()->empty()) return;
    if (s->hasMarkers()) refuse("markers");
    auto const affine = s->i2doc_affine() * _to_art;
    auto const is_gradient = [](SPIPaint const &p) {
        auto const g = cast<SPGradient>(p.isPaintserver() && p.href ? p.href->getObject() : nullptr);
        return g && !g->isSolid();
    };
    bool const fill_gradient = is_gradient(style->fill);
    bool const stroke_gradient = style->stroke_width.computed > 0 && is_gradient(style->stroke);
    // Where Illustrator places a gradient in a pattern's space isn't qualified.
    if (_in_pattern && (fill_gradient || stroke_gradient)) {
        refuse("gradients in patterns");
        return;
    }
    // The fill and stroke have independent instances. Illustrator's filled-stroke control
    // closes the fill instance before opening the stroke's, with one final b/B operator on
    // the same path (doc/PROOF-ai-interchange.md, "Filled and dual-gradient strokes").
    std::optional<WrittenGradient> fill_instance, stroke_instance;
    std::optional<std::string> fill, stroke;
    auto prepare_gradient = [&](bool on_stroke) {
        auto gradient = write_gradient(s, affine, "PROOF_G" + std::to_string(_gradients.size() + 1), _legacy, _canvas,
                                       on_stroke, on_stroke && fill_gradient, !_masks.empty());
        for (auto const &why : gradient.refused) refuse(why);
        for (auto const &[name, ink] : gradient.inks) declare_ink(name, ink);
        if (!gradient.definition.empty()) _gradients.push_back(gradient.definition);
        return gradient;
    };
    if (fill_gradient) fill_instance = prepare_gradient(false);
    if (stroke_gradient) stroke_instance = prepare_gradient(true);
    // A compound path's early contours need a plain paint before its final gradient.
    double fill_alpha = 1.0, stroke_alpha = 1.0;
    fill = fill_gradient ? std::optional<std::string>("0 g") : paint(style->fill, style, true, affine);
    if (!fill_gradient) fill_alpha = _alpha;
    if (stroke_gradient) {
        stroke = "0 G";
    } else if (style->stroke_width.computed > 0) {
        stroke = paint(style->stroke, style, false, affine);
        stroke_alpha = _alpha;
    }
    fill_alpha *= scale24(style->fill_opacity.value);
    stroke_alpha *= scale24(style->stroke_opacity.value);
    bool const clear_fill = fill && fill_alpha < 1.0 - 1e-6, clear_stroke = stroke && stroke_alpha < 1.0 - 1e-6;
    // A paint's own opacity, when it is the object's only paint, is the object's: the same
    // picture. A fill and a stroke each with an opacity of its own take an appearance.
    double alpha = 1.0;
    if (_legacy) {
        if (clear_fill || clear_stroke) refuse("transparency");
    } else if (fill && stroke) {
        if (clear_fill || clear_stroke) refuse("separate fill and stroke transparency");
    } else {
        alpha = fill ? fill_alpha : stroke ? stroke_alpha : 1.0;
    }
    auto const own = transparency(clipped ? nullptr : s, alpha);

    if (stroke) {
        if (style->vector_effect.stroke) refuse("non-scaling strokes");
        if (!affine.withoutTranslation().preservesAngles(1e-6)) refuse("strokes on stretched or skewed objects");
        for (auto const layer : style->paint_order.layer) {
            if (layer == SP_CSS_PAINT_ORDER_FILL) break;
            if (layer == SP_CSS_PAINT_ORDER_STROKE && fill) {
                refuse("strokes painted under their fill");
                break;
            }
        }
    }
    if (!_refused.empty()) return;

    auto const overprint = s->getAttribute("proof:overprint") ? std::string(s->getAttribute("proof:overprint")) : "";
    state(_fill_over, overprint.find("fill") != std::string::npos ? "1 O" : "0 O");
    if (fill) state(_fill, *fill);
    state(_stroke_over, overprint.find("stroke") != std::string::npos ? "1 R" : "0 R");
    if (stroke) {
        state(_stroke, *stroke);
        double const scale = std::sqrt(std::abs(affine.det()));
        // As for coordinates: a width or dash that isn't a number would be written "inf"/"nan".
        bool numbers = std::isfinite(style->stroke_width.computed * scale) &&
                       std::isfinite(style->stroke_dashoffset.computed * scale);
        for (auto const &d : style->stroke_dasharray.values) numbers = numbers && std::isfinite(d.computed * scale);
        if (!numbers) {
            refuse("nonfinite stroke widths or dashes");
            return;
        }
        std::string dash = "[";
        for (auto const &d : style->stroke_dasharray.values) {
            dash += (dash.size() > 1 ? " " : "") + num(d.computed * scale);
        }
        dash += "]" + num(style->stroke_dashoffset.computed * scale) + " d";
        int const cap = style->stroke_linecap.computed == SP_STROKE_LINECAP_ROUND    ? 1
                        : style->stroke_linecap.computed == SP_STROKE_LINECAP_SQUARE ? 2 : 0;
        int const join = style->stroke_linejoin.computed == SP_STROKE_LINEJOIN_ROUND   ? 1
                         : style->stroke_linejoin.computed == SP_STROKE_LINEJOIN_BEVEL ? 2 : 0;
        state(_stroke_style, std::format("{} w {} J {} j {} M {}", num(style->stroke_width.computed * scale), cap,
                                         join, num(style->stroke_miterlimit.value), dash));
    }
    state(_rule, style->fill_rule.computed == SP_WIND_RULE_EVENODD ? "1 XR" : "0 XR");
    // After the paints and before the path, as Illustrator writes it. A compound path's paths
    // take it, and the compound path has its paths'.
    if (!_legacy) state(_transparency, own);

    auto const pv = pathv_to_linear_and_cubic_beziers(*s->curve() * affine);
    auto const types = path_types(s, pv, affine);
    auto op = [&](Geom::Path const &p) {
        char c = fill && stroke ? 'b' : fill ? 'f' : stroke ? 's' : 'n';
        return p.closed() ? c : static_cast<char>(g_ascii_toupper(c));
    };
    // The instance brackets the final contour's paint operator; its end says whether that
    // operator strokes the path, and closes it first.
    auto contour = [&](std::size_t i) {
        path_data(pv[i], types[i]);
        bool const last = i + 1 == pv.size();
        auto const c = op(pv[i]);
        if (fill_instance && last) {
            _body << fill_instance->begin;
            if (stroke_instance) _body << gradient_end(c);
        }
        if (stroke_instance && last) _body << stroke_instance->begin;
        _body << c << '\n';
        if ((fill_instance || stroke_instance) && last) _body << gradient_end(c);
    };
    if (pv.size() == 1) {
        contour(0);
        return;
    }
    _body << "*u\n";
    for (std::size_t i = 0; i < pv.size(); ++i) contour(i);
    _body << "*U\n";
}

/// `where` names what isn't an object (a layer, the document): the modern form writes an
/// object's and a group's transparency, and refuses it there as Illustrator 7 does everywhere.
void Writer::check_effects(SPItem const *it, std::string_view where)
{
    auto const *style = it->style;
    if (_legacy || !where.empty()) {
        if (scale24(style->opacity.value) < 1.0 - 1e-6) refuse("transparency" + std::string(where));
        if (style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL) refuse("blend modes" + std::string(where));
    }
    if (it->getMaskObject()) {
        // Illustrator 7 has no masks; Illustrator keeps a layer's transparency apart from its
        // objects'. A clone's own x and y would move what it shows but not its mask.
        if (_legacy) refuse("opacity masks (Illustrator 7)");
        else if (!where.empty()) refuse("opacity masks" + std::string(where));
        else if (is<SPUse>(it)) refuse("opacity masks on clones");
        else if (_in_pattern) refuse("opacity masks in patterns");
    }
    if (!_masks.empty()) {
        // What PROOF's reader takes in a mask's art (Reader::art_dictionary): Normal or Multiply,
        // no knockout.
        auto const blend = style->mix_blend_mode.value;
        if (blend != SP_CSS_BLEND_NORMAL && blend != SP_CSS_BLEND_MULTIPLY) {
            refuse("blend modes other than Multiply in opacity masks");
        }
        if (auto const knockout = it->getAttribute("proof:knockout"); knockout && std::string_view(knockout) == "on") {
            refuse("knockout groups in opacity masks");
        }
        if (is<SPImage>(it)) refuse("pictures in opacity masks");
    }
    if (const_cast<SPStyle *>(style)->getFilter()) refuse("filters");
}

/// The object's own transparency as Illustrator's operator: its blend mode, opacity (times
/// `alpha`), isolated blending and knockout, as the import keeps them. With no object, `alpha`.
std::string Writer::transparency(SPItem const *it, double alpha)
{
    int mode = 0, isolate = 0, knockout = 0, shape = 0;
    double opacity = alpha;
    if (it) {
        auto const *style = it->style;
        opacity *= scale24(style->opacity.value);
        auto const blend = static_cast<std::size_t>(style->mix_blend_mode.value);
        if (blend < BLEND_INDEX.size()) mode = BLEND_INDEX[blend];
        isolate = style->isolation.value == SP_CSS_ISOLATION_ISOLATE;
        auto const is = [&](char const *name, std::string_view value) {
            auto const v = it->getAttribute(name);
            return v && value == v;
        };
        knockout = is("proof:knockout", "on") ? 1 : is("proof:knockout", "neutral") ? 2 : 0;
        shape = is("proof:knockout-shape", "true");
    }
    return std::format("{} {} {} {} {} Xy", mode, num(opacity), isolate, knockout, shape);
}

/// What Illustrator writes after a group or clipping group ends: the group's own transparency,
/// then `0 0 Xd` and `6 () XW`; for one with none of its own, `9 () XW`. Without either, the
/// next object's `Xy` would be the group's.
void Writer::end_group(std::string_view own)
{
    if (_legacy) return;
    if (own == OPAQUE) {
        _body << "9 () XW\n";
        return;
    }
    // Written even when it holds already: a group's own needs no inference from its contents.
    _transparency = own;
    _body << own << "\n0 0 Xd\n6 () XW\n";
}

/// Forgets what holds, so that the next object sets all of its own state.
void Writer::clear_state()
{
    _fill_over.clear(), _fill.clear(), _stroke_over.clear(), _stroke.clear(), _stroke_style.clear(), _rule.clear();
    _transparency.clear();
}

/// The object in the import's wrapper about a masked one (Builder::masked): a group with the mask
/// and nothing else of its own, about one object. Illustrator's mask is the object's own.
SPItem const *mask_wrapped(SPItem const *it)
{
    auto const *group = cast<SPGroup>(it);
    if (!group || group->isLayer() || !group->getMaskObject() || group->getClipObject() || group->isHidden() ||
        group->getAttribute("sodipodi:insensitive") || group->getAttribute("proof:knockout") ||
        group->getAttribute("proof:knockout-shape")) {
        return nullptr;
    }
    auto const *style = group->style;
    if (scale24(style->opacity.value) < 1.0 - 1e-6 || style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL ||
        style->isolation.value == SP_CSS_ISOLATION_ISOLATE || const_cast<SPStyle *>(style)->getFilter()) {
        return nullptr;
    }
    SPItem const *only = nullptr;
    for (auto const &child : group->children) {
        if (auto const *item = cast<SPItem>(&child)) {
            if (only) return nullptr;
            only = item;
        }
    }
    auto const *inner = cast<SPGroup>(only);
    if (!only || is<SPUse>(only) || only->getMaskObject() || (inner && inner->isLayer())) return nullptr;
    return only;
}

/// Whether `first`, the first of a mask's art, is the white the import put under the art of a
/// mask with Clip off (Builder::masked): a plain rectangle, covering all that `owner` paints.
bool white_backdrop(SPMask const *mask, SPItem const *first, SPItem const *owner)
{
    auto const is_value = [](SPObject const *object, char const *name, std::string_view value) {
        auto const v = object->getAttribute(name);
        return v && value == v;
    };
    auto const *rect = cast<SPRect>(first);
    if (!rect || !is_value(mask, "proof:ai-mask-clipping", "false") || !is_value(first, "proof:mask-backdrop", "white") ||
        first->getClipObject() || first->getMaskObject() || first->isHidden()) {
        return false;
    }
    auto const *style = first->style;
    auto const &t = first->transform;
    if (!style->fill.isColor() || !style->stroke.isNone() || scale24(style->fill_opacity.value) < 1.0 - 1e-6 ||
        scale24(style->opacity.value) < 1.0 - 1e-6 || style->mix_blend_mode.value != SP_CSS_BLEND_NORMAL ||
        const_cast<SPStyle *>(style)->getFilter() || std::abs(t[1]) > 1e-12 || std::abs(t[2]) > 1e-12 ||
        rect->rx.computed > 0 || rect->ry.computed > 0) {
        return false;
    }
    auto const &white = style->fill.getColor();
    auto const grey = coverage_grey(white);
    if (!grey || *grey < 1.0 - 1e-6 || white.getOpacity() < 1.0 - 1e-6) return false;
    auto const box = first->geometricBounds(t);
    // Where the object paints nothing, its mask changes nothing.
    auto const painted = owner->visualBounds(Geom::identity(), true, true, false);
    if (!box) return false;
    if (!painted) return true;
    auto covering = *box;
    covering.expandBy(1e-6 * std::max({1.0, box->width(), box->height()}));
    return covering.contains(*painted);
}

/**
 * The opacity mask of `owner`, as Illustrator writes one straight after the object it masks: an
 * AI9_Mask in the object's ArtDictionary on hidden lines, linked and not inverted, its art in a
 * group of its own between X= and X+ (the owner's files, 2026-10-22). The art is written in a
 * state of its own, all of it set; whether what held before holds again after it is the reader's
 * to say (Reader::end_obj puts it back), so the next object sets all of its own too.
 *
 * An SVG mask shows nothing where its art isn't, as Illustrator's does with Clip on. Clip off
 * leaves the object as it is there, as an endless white backdrop would: what the import made of
 * such a mask is a white rectangle under its art (Builder::masked), and the mask is written with
 * Clip off without it where it covers all that the object paints. Otherwise the rectangle is art
 * like any other, and Clip is on: what PROOF draws either way.
 */
std::string Writer::opacity_mask(SPItem const *owner)
{
    auto const *mask = owner->getMaskObject();
    if (!mask) return {};
    if (mask->mask_content_units() == SP_CONTENT_UNITS_OBJECTBOUNDINGBOX) {
        refuse("opacity masks sized to their object");
        return {};
    }
    if (std::find(_masks.begin(), _masks.end(), mask) != _masks.end()) {
        refuse("opacity masks inside themselves");
        return {};
    }
    std::vector<SPItem const *> art;
    for (auto const &child : mask->children) {
        if (auto const *it = cast<SPItem>(&child)) art.push_back(it);
    }
    bool const clip = art.empty() || !white_backdrop(mask, art.front(), owner);
    if (!clip) art.erase(art.begin());
    // Illustrator keeps a mask's art in a group, which comes back as one: art that is one group
    // with nothing of its own is that group, and isn't put in another each time it is written.
    if (art.size() == 1) {
        auto const *group = cast<SPGroup>(art.front());
        auto const *style = group ? group->style : nullptr;
        if (group && !group->isLayer() && !group->getClipObject() && !group->getMaskObject() && !group->isHidden() &&
            !group->getAttribute("sodipodi:insensitive") && !group->getAttribute("proof:knockout") &&
            !group->getAttribute("proof:knockout-shape") && scale24(style->opacity.value) >= 1.0 - 1e-6 &&
            style->mix_blend_mode.value == SP_CSS_BLEND_NORMAL && style->isolation.value != SP_CSS_ISOLATION_ISOLATE &&
            !const_cast<SPStyle *>(style)->getFilter()) {
            art.clear();
            for (auto const &child : group->children) {
                if (auto const *it = cast<SPItem>(&child)) art.push_back(it);
            }
        }
    }

    std::ostringstream records;
    std::swap(_body, records);
    auto saved = std::tuple(_to_art, _fill_over, _fill, _stroke_over, _stroke, _stroke_style, _rule, _transparency,
                            _hiding, _locking);
    // The art lies in the user space of the object, its own transform included.
    _to_art = owner->i2doc_affine() * _to_art;
    clear_state();
    // The state carries on into the dictionary: art in a hidden or locked object's mask isn't.
    if (_hiding) _body << "0 Xw\n";
    if (_locking) _body << "0 A\n";
    _hiding = _locking = false;
    _masks.push_back(mask);
    _body << "u\n";
    for (auto const *it : art) item(it);
    _body << "U\n";
    end_group(OPAQUE);
    _masks.pop_back();
    std::swap(_body, records);
    std::tie(_to_art, _fill_over, _fill, _stroke_over, _stroke, _stroke_style, _rule, _transparency, _hiding, _locking) =
        std::move(saved);

    auto out = std::format("%_/ArtDictionary :\n%_/Mask :\n%_1 /Bool (Linked) ,\n%_0 /Bool (Inverted) ,\n"
                           "%_{} /Bool (Clipping) ,\n%_0 /Bool (Disabled) ,\n%_X=\n", clip ? 1 : 0);
    // A mask in the art is on hidden lines already, and Illustrator hides a line only once.
    std::istringstream lines(records.str());
    for (std::string line; std::getline(lines, line);) out += (line.starts_with("%_") ? "" : "%_") + line + "\n";
    out += "%_X+\n%_ (Art) ,\n%_; (AI9_Mask) ,\n%_;\n%_\n";
    clear_state();
    return out;
}

void Writer::item(SPItem const *it)
{
    // Whose mask goes after this object: its own, or that of the import's wrapper about it,
    // which is written as the object alone, as Illustrator had it.
    auto mask_owner = std::exchange(_mask_owner, nullptr);
    if (!mask_owner && it->getMaskObject()) mask_owner = it;
    if (mask_owner == it && !_legacy && !_in_pattern) {
        if (auto const *only = mask_wrapped(it)) {
            _mask_owner = it;
            item(only);
            return;
        }
    }
    bool const masked = mask_owner && !_legacy && !_in_pattern && !is<SPUse>(it);
    // The mask goes straight after the object, before a group's own state.
    auto const attach = [&] {
        if (masked) _body << opacity_mask(mask_owner);
    };
    check_effects(it);
    // Only the object's own: a locked layer's objects aren't each locked.
    bool const hidden = it->isHidden();
    bool const locked = it->getAttribute("sodipodi:insensitive") != nullptr;
    if (hidden) {
        if (_legacy) {
            refuse("individually hidden objects (Illustrator 7)");
        } else {
            _body << "1 Xw\n";
            _hiding = true;
        }
    }
    if (locked) {
        _body << "1 A\n";
        _locking = true;
    }
    bool const clipped = open_clip(it);
    // A clipped object's transparency is its clipping group's, written after it ends.
    auto const own = _legacy ? std::string(OPAQUE) : transparency(it);

    if (auto use = cast<SPUse>(it)) {
        // A clone is written as a copy of what it shows, in a group for its own transparency.
        bool const wrap = !clipped && own != OPAQUE;
        if (wrap) _body << "u\n";
        if (use->child) item(use->child);
        if (wrap) {
            _body << "U\n";
            end_group(own);
        }
    } else if (auto group = cast<SPGroup>(it)) {
        if (!clipped) _body << "u\n";
        contents(group);
        if (!clipped) {
            _body << "U\n";
            attach();
            end_group(own);
        }
    } else if (auto s = cast<SPShape>(it)) {
        auto const before = _body.tellp();
        shape(s, clipped);
        // A shape without a path writes nothing, and has nothing to mask.
        if (!clipped && _body.tellp() != before) attach();
    } else if (auto image = cast<SPImage>(it); image && _in_pattern) {
        refuse("pictures in patterns");
    } else if (image) {
        auto const written = write_picture(image, image->i2doc_affine() * _to_art, _legacy);
        for (auto const &why : written.refused) refuse(why);
        if (!_legacy && !written.records.empty()) state(_transparency, clipped ? std::string(OPAQUE) : own);
        // A picture cropped to its viewport is in a clipping group, whose mask comes before its
        // end as a group's does.
        constexpr std::string_view crop_end = "9 () XW\n";
        auto const &records = written.records;
        if (!clipped && !records.empty() && records.ends_with(std::string("Q\n") + std::string(crop_end))) {
            _body << std::string_view(records).substr(0, records.size() - crop_end.size());
            attach();
            _body << crop_end;
        } else {
            _body << records;
            if (!clipped && !records.empty()) attach();
        }
    } else {
        refuse("type");
    }

    if (clipped) {
        _body << "Q\n";
        attach();
        end_group(own);
    }
    if (locked) {
        _body << "0 A\n";
        _locking = false;
    }
    if (hidden && !_legacy) {
        _body << "0 Xw\n";
        _hiding = false;
    }
}

void Writer::contents(SPObject const *parent)
{
    for (auto const &child : parent->children) {
        auto it = cast<SPItem>(&child);
        if (!it) continue;
        auto group = cast<SPGroup>(it);
        // A layer in a mask's art or in a tile is a group there.
        if (group && group->isLayer() && _masks.empty() && !_in_pattern) {
            layer(group);
        } else {
            item(it);
        }
    }
}

void Writer::layer(SPGroup const *group)
{
    ++_layers;
    auto attr = [&](char const *name) {
        auto v = group->getAttribute(name);
        return v ? std::string(v) : std::string();
    };
    int r = 79, g = 128, b = 255;
    auto const color = attr("inkscape:highlight-color");
    if (color.size() >= 7 && color[0] == '#') {
        auto hex = [&](std::size_t at) { return static_cast<int>(std::strtol(color.substr(at, 2).c_str(), nullptr, 16)); };
        r = hex(1), g = hex(3), b = hex(5);
    }
    // Illustrator shows a layer in its preset colour by index, whatever RGB follows it. The
    // presets as the owner's files pair them (census, 2026-10-17; 4 to 6 vary between files and
    // are left out): the nearest one.
    static constexpr std::array<std::array<int, 4>, 18> presets{{
        {0, 79, 128, 255}, {1, 255, 79, 79},   {2, 79, 255, 79},   {3, 79, 79, 255},  {7, 128, 128, 128},
        {8, 0, 0, 0},      {9, 255, 102, 0},   {10, 0, 85, 0},     {11, 0, 153, 153}, {12, 204, 153, 102},
        {13, 153, 51, 0},  {14, 153, 51, 255}, {15, 255, 153, 0},  {16, 0, 0, 136},   {17, 255, 153, 204},
        {19, 153, 0, 0},   {21, 255, 153, 153}, {0, 79, 128, 255}}};
    auto const nearest = *std::min_element(presets.begin(), presets.end(), [&](auto const &p, auto const &q) {
        auto d = [&](auto const &c) { return (c[1] - r) * (c[1] - r) + (c[2] - g) * (c[2] - g) + (c[3] - b) * (c[3] - b); };
        return d(p) < d(q);
    });
    auto const dim = attr("proof:layer-dim");
    auto const label = group->label() ? group->label() : group->defaultLabel();
    if (_legacy) {
        if (!dim.empty() && dim != "50") refuse("layer image dimming other than 50% (Illustrator 7)");
        for (auto const *parent = group->parent; parent; parent = parent->parent) {
            if (auto ancestor = cast<SPGroup>(parent); ancestor && ancestor->isLayer()) {
                refuse("nested layers (Illustrator 7)");
                break;
            }
        }
    }
    // visible, preview, unlocked, printing, dimmed, 0, 1, colour index, its RGB, 0, dim %, 0: as
    // Illustrator 29 writes it (`1 1 1 1 0 0 1 0 79 128 255 0 50 0 Lb`).
    _body << "%AI5_BeginLayer\n"
          << std::format("{} {} {} {} {} 0 ", group->isHidden() ? 0 : 1,
                         attr("proof:layer-preview") == "false" ? 0 : 1, attr("sodipodi:insensitive").empty() ? 1 : 0,
                         attr("proof:layer-print") == "false" ? 0 : 1, dim.empty() ? 0 : 1);
    if (!_legacy) _body << "1 ";
    _body << std::format("{} {} {} {}", nearest[0], nearest[1], nearest[2], nearest[3]);
    if (!_legacy) _body << " 0 " << (dim.empty() ? "50" : dim) << " 0";
    _body << " Lb\n" << encoded_name(label, "layer names") << " Ln\n0 A\n";
    if (!_legacy) _body << "0 Xw\n";
    _hiding = _locking = false;
    // Illustrator keeps a layer's transparency apart from its objects'; PROOF doesn't write it yet.
    check_effects(group, " on layers");
    // Whether a layer begins with what held before it is the reader's to say: after anything
    // transparent, the layer's first object has its transparency written in full.
    if (_transparency != OPAQUE) _transparency.clear();
    bool const clipped = open_clip(group);
    contents(group);
    if (clipped) {
        _body << "Q\n";
        end_group(OPAQUE);
    }
    _body << "LB\n%AI5_EndLayer--\n";
}

WrittenRecords Writer::write()
{
    // Shapes such as rectangles have their path only once the document is up to date.
    _doc->ensureUpToDate();
    auto const title = encoded_name(_doc->getDocumentName() ? _doc->getDocumentName() : "Untitled", "document titles");
    // Artboards: the document's pages, or its own size when it has none.
    std::vector<std::pair<std::string, Geom::Rect>> boards;
    for (auto const *page : _doc->getPageManager().getPages()) {
        boards.emplace_back(page->getLabel(), page->getDocumentRect());
    }
    if (boards.empty()) boards.emplace_back("Artboard 1", Geom::Rect(Geom::Point(0, 0), _doc->getDimensions()));
    if (_legacy && boards.size() != 1) refuse("more than one artboard (Illustrator 7)");
    auto const origin = boards.front().second.corner(0);
    _to_art = Geom::Translate(-origin) * Geom::Scale(PX_TO_PT, -PX_TO_PT);
    // The standalone format's origin is the single artboard's bottom-left, y upward.
    if (_legacy) _to_art *= Geom::Translate(0, boards.front().second.height() * PX_TO_PT);
    // The document's extent from the first artboard's top-left, and Illustrator's canvas around
    // it: the template box at its centre. Illustrator reads a file another application wrote on
    // the 8640-point canvas of its versions 3 to 8, its centre at 4320 4320 and y down, and
    // applies a gradient's Bg matrix there (doc/PROOF-ai-interchange.md, "Radial gradients").
    Geom::Rect extent = boards.front().second;
    for (auto const &[name, rect] : boards) extent.unionWith(rect);
    double const extent_w = (extent.right() - origin.x()) * PX_TO_PT, extent_h = (extent.bottom() - origin.y()) * PX_TO_PT;
    double const template_x = extent_w / 2 + 0.5, template_y = -extent_h / 2 - 0.5;
    _canvas = {4320 - template_x, 4320 + template_y};

    // Items outside any layer go to a layer of their own, as Illustrator keeps every object in one.
    auto const *root = _doc->getRoot();
    check_effects(root, " on the whole document");
    if (root->getClipObject()) refuse("clipping on the SVG root");
    bool loose = false;
    for (auto const &child : root->children) {
        auto group = cast<SPGroup>(&child);
        if (is<SPItem>(&child) && !(group && group->isLayer())) loose = true;
    }
    for (auto const &child : root->children) {
        auto group = cast<SPGroup>(&child);
        if (group && group->isLayer()) layer(group);
    }
    if (loose) {
        ++_layers;
        if (_transparency != OPAQUE) _transparency.clear();
        _body << (_legacy ? "%AI5_BeginLayer\n1 1 1 1 0 0 0 79 128 255 Lb\n(Layer) Ln\n0 A\n"
                         : "%AI5_BeginLayer\n1 1 1 1 0 0 1 0 79 128 255 0 50 0 Lb\n(Layer) Ln\n0 A\n0 Xw\n");
        _hiding = _locking = false;
        for (auto const &child : root->children) {
            auto group = cast<SPGroup>(&child);
            if (auto it = cast<SPItem>(&child); it && !(group && group->isLayer())) item(it);
        }
        _body << "LB\n%AI5_EndLayer--\n";
    }

    WrittenRecords out;
    if (!_refused.empty()) {
        out.refused.assign(_refused.begin(), _refused.end());
        return out;
    }

    bool const cmyk = Colors::DocumentColors::mode(_doc) == Type::CMYK;
    if (_legacy) {
        double const w = boards.front().second.width() * PX_TO_PT;
        double const h = boards.front().second.height() * PX_TO_PT;
        std::ostringstream head;
        head << "%!PS-Adobe-3.0\n%%Creator: PROOF\n%%Title: " << title << '\n'
             << std::format("%%BoundingBox: 0 0 {} {}\n%%HiResBoundingBox: 0 0 {} {}\n",
                            num(std::ceil(w)), num(std::ceil(h)), num(w), num(h))
             << "%%DocumentProcessColors: Cyan Magenta Yellow Black\n";
        ink_comments(head);
        // Newer Illustrator converts legacy rasters into the document's colour model on
        // open. Keep the source mode as an advisory comment; version 7 ignores this later
        // comment (1998 specification, section 1.5), while the artwork remains version 7.
        head << "%AI5_FileFormat 3\n%AI3_ColorUsage: Color\n%AI9_ColorModel: " << (cmyk ? 2 : 1) << '\n'
             << std::format("%AI3_Cropmarks: 0 0 {} {}\n%AI3_TileBox: 0 0 {} {}\n", num(w), num(h), num(w), num(h))
             // Legacy conversion places the artboard ceil(w/2) left and ceil(h/2) up
             // from this origin. Match the crop rectangle, including fractional sizes,
             // or Illustrator creates a second, displaced artboard.
             << std::format("%AI3_TemplateBox: {} {} {} {}\n", num(std::ceil(w / 2)), num(h - std::ceil(h / 2)),
                            num(std::ceil(w / 2)), num(h - std::ceil(h / 2)))
             << std::format("%AI5_ArtSize: {} {}\n", num(w), num(h))
             << "%AI5_RulerUnits: 2\n%AI3_DocumentPreview: None\n%AI5_NumLayers: " << _layers << '\n'
             << "%%EndComments\n%%BeginProlog\n%%EndProlog\n%%BeginSetup\n";
        if (!_gradients.empty()) {
            head << _gradients.size() << " Bn\n";
            for (auto const &definition : _gradients) head << definition;
        }
        head << "%%EndSetup\n";
        out.records = head.str() + _body.str() + "%%PageTrailer\n%%Trailer\n%%EOF\n";
        return out;
    }

    double const w = extent_w, h = extent_h, cx = template_x, cy = template_y;
    std::ostringstream head;
    head << "%!PS-Adobe-3.0 \n%%Creator: PROOF\n%%AI8_CreatorVersion: 29.2.1\n%%For: () ()\n"
         << "%%Title: " << title << '\n'
         << "%%Canvassize: 16383\n"
         << std::format("%%BoundingBox: 0 {} {} 0\n%%HiResBoundingBox: 0 {} {} 0\n", num(std::floor(-h)),
                        num(std::ceil(w)), num(-h), num(w))
         << "%%DocumentProcessColors: Cyan Magenta Yellow Black\n";
    ink_comments(head);
    head << "%AI5_FileFormat 14.0\n%AI12_BuildNumber: 116\n%AI3_ColorUsage: Color\n"
         << std::format("%AI3_Cropmarks: 0 {} {} 0\n", num(-h), num(w))
         << std::format("%AI3_TemplateBox: {} {} {} {}\n", num(cx), num(cy), num(cx), num(cy))
         << std::format("%AI3_TileBox: 0 {} {} 0\n", num(-h), num(w))
         << "%AI3_DocumentPreview: None\n%AI5_ArtSize: 14400 14400\n%AI5_RulerUnits: 2\n"
         << "%AI24_LargeCanvasScale: 1\n"
         << "%AI9_ColorModel: " << (cmyk ? 2 : 1) << '\n'
         << "%AI5_ArtFlags: 0 0 0 1 0 0 1 0 0\n%AI5_TargetResolution: 800\n"
         << "%AI5_NumLayers: " << _layers << '\n';
    // Where Illustrator's window looks when the file opens: the first artboard, fitted. Without
    // it the window opened on empty canvas (the owner, 2026-10-17). Its top-left corner in art
    // space, the zoom, then a 1910 by 914 window and constants as Illustrator 29 writes them.
    auto const &first_board = boards.front().second;
    double const bw = first_board.width() * PX_TO_PT, bh = first_board.height() * PX_TO_PT;
    double const zoom = std::clamp(0.9 * std::min(1910 / std::max(bw, 1.0), 914 / std::max(bh, 1.0)), 0.03125, 64.0);
    auto const view = std::format("{} {} {}", num(bw / 2 - 1910 / (2 * zoom)), num(-bh / 2 + 914 / (2 * zoom)), num(zoom));
    head << "%AI17_Begin_Content_if_version_gt:24 4\n"
         << "%AI10_OpenToVie: " << view << " 0 8191 8191 1910 914 18 0 0 5 121 0 0 0 1 1 0 1 1 0 1\n"
         << "%AI17_Alternate_Content\n"
         << "%AI9_OpenToView: " << view << " 1910 914 18 0 0 5 121 0 0 0 1 1 0 1 1 0 1\n"
         // No %AI5_OpenViewLayers: it holds view states per layer that override `Lb`'s, and a
         // copied "7" moved the hidden and locked flags onto other layers.
         << "%AI17_End_Versioned_Content\n"
         << std::format("%%PageOrigin:0 {}\n", num(-h)) << "%%EndComments\n"
         << "%%BeginProlog\n%%EndProlog\n%%BeginSetup\n";

    // The artboards, as the writer recipe's document data.
    head << "%AI9_BeginDocumentData\n%_/Document :\n%_/Dictionary :\n%_0 /Int (CropAreaActive) ,\n%_/Array :\n";
    for (auto const &[name, rect] : boards) {
        double const l = (rect.left() - origin.x()) * PX_TO_PT, t = -(rect.top() - origin.y()) * PX_TO_PT;
        double const r = (rect.right() - origin.x()) * PX_TO_PT, bot = -(rect.bottom() - origin.y()) * PX_TO_PT;
        auto uuid = g_uuid_string_random();
        head << "%_/Dictionary :\n"
             << std::format("%_{} {} /RealPointRelToROrigin\n%_ (PositionPoint1) ,\n", num(l), num(t))
             << std::format("%_{} {} /RealPointRelToROrigin\n%_ (PositionPoint2) ,\n", num(r), num(bot))
             << "%_0 /Bool (IsArtboardDefaultName) ,\n%_0 /Bool (IsArtboardSelected) ,\n%_0 /Int (DisplayMark) ,\n"
             << std::format("%_{} {} /RealPoint\n%_ (RulerOrigin) ,\n", num(std::floor(8191.5 - cx + l)),
                            num(std::floor(8191.5 + cy - t)))
             << "%_(" << uuid << ") /String (ArtboardUUID) ,\n%_1 /Real (PAR) ,\n"
             << "%_" << ps_string(name) << " /UnicodeString (Name) ,\n%_; ,\n";
        g_free(uuid);
    }
    head << "%_; (ArtboardArray) ,\n%_; /Recorded ,\n%_;\n%AI9_EndDocumentData\n";
    if (!_gradients.empty()) {
        head << _gradients.size() << " Bn\n";
        for (auto const &definition : _gradients) head << definition;
    }
    // After the gradients, as Illustrator writes them.
    for (auto const &pattern : _patterns) head << pattern.records;
    head << "%%EndSetup\n";

    out.records = head.str() + _body.str() +
                  "%%PageTrailer\ngsave annotatepage grestore showpage\n%%Trailer\n%%EOF\n";
    return out;
}

} // namespace

WrittenRecords write_records(SPDocument *document, RecordsVersion version)
{
    return Writer(document, version).write();
}

} // namespace Inkscape::Extension::Internal::AiNative
