// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Type as Illustrator's text documents. See ai-native-writer-text.h.
 *
 * The numbered keys are the ones PROOF's reader reads (ai-native-text.cpp; the key meanings in
 * doc/PROOF-ai-native-records.md). The two normal styles and the document's defaults are
 * Illustrator 30.6's own, as it saved them in a document of the owner's (2026-10-22).
 */
#include "ai-native-writer-text.h"
#include "ai-native-fonts.h"
#include "ai-native-import.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <string_view>

#include <glib.h>
#include <2geom/bezier-curve.h>
#include <2geom/pathvector.h>
#include <2geom/transforms.h>

#include "colors/color.h"
#include "colors/spaces/base.h"
#include "document.h"
#include "helper/geom.h"
#include "libnrtype/Layout-TNG.h"
#include "object/sp-gradient.h"
#include "object/sp-pattern.h"
#include "object/sp-shape.h"
#include "object/sp-shape-reference.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "object/sp-textpath.h"
#include "object/sp-tspan.h"
#include "spot-ink.h"
#include "style.h"
#include "svg/svg-length.h"
#include "svg/svg.h"
#include "type-settings.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

using Type = Colors::Space::Type;
using Features = std::map<int, std::string>;

/// Illustrator 30.6's [Normal Character Style]: every character feature, in the font of index 0.
constexpr std::string_view NORMAL_CHARACTER =
    "/0 0 /1 12.0 /2 false /3 false /4 true /5 27.0 /6 1.0 /7 1.0 /8 0 /9 0.0 /10 0.0 /11 1 /12 0 /13 0 /14 0 "
    "/15 0 /16 0 /17 0.0 /18 true /19 false /20 true /21 false /22 false /23 false /24 false /25 false /26 false "
    "/27 true /28 false /29 false /30 0 /31 false /32 false /33 false /34 false /35 1 /36 0.0 /37 2 /38 0 /39 0 "
    "/40 false /41 2 /42 0 /43 << /0 0.5 >> /44 2 /45 2 /46 7 /47 0 /48 0 /49 -1.0 /50 -1.0 /51 0 /52 false "
    "/53 << /99 /SimplePaint /0 << /0 2 /1 [ 1.0 0.0 0.0 0.0 1.0 ] >> >> "
    "/54 << /99 /SimplePaint /0 << /0 2 /1 [ 1.0 0.0 0.0 0.0 1.0 ] >> >> /55 << /99 /CAITextBlender >> "
    "/56 true /57 false /58 true /59 false /60 false /61 0 /62 0 /63 1.0 /64 4.0 /65 0.0 /66 [ ] /67 [ ] /68 0 "
    "/69 0 /70 0 /71 4 /72 0.0 /73 0.0 /74 false /75 false /76 false /77 true /78 true "
    "/79 << /99 /SimplePaint /0 << /0 1 /1 [ 1.0 1.0 1.0 0.0 ] >> >> /80 false /81 0 /82 3.0 /83 3.0 /84 false "
    "/85 0 /86 << /99 /SimpleCustomFeature >> /87 100.0 /88 true /89 12.0 /90 0 /91 2 /92 0.0 /93 0.0 /94 0.0 "
    "/95 << /0 0.0 /1 0 >> /96 true";

/// Illustrator 30.6's [Normal Paragraph Style].
constexpr std::string_view NORMAL_PARAGRAPH =
    "/0 0 /1 0.0 /2 0.0 /3 0.0 /4 0.0 /5 0.0 /6 1 /7 1.2 /8 0 /9 false /10 6 /11 2 /12 2 /13 0 /14 36.0 /15 true "
    "/16 0.5 /17 [ 0.8 1.0 1.33 ] /18 [ 0.0 0.0 0.0 ] /19 [ 1.0 1.0 1.0 ] /20 6 /21 false /22 0 /23 false /24 0 "
    "/25 0 /27 /nil /26 false /28 /nil /29 false /30 << >> /31 36.0 /32 << >> /33 0 /34 0 /35 0 /36 /nil /37 0 "
    "/38 false /39 0 /40 2 /41 0 /47 false /48 /nil";

/// The share of its size Illustrator sets automatic leading at (the normal paragraph's 7).
constexpr double AUTO_LEADING = 1.2;
/// The sizes Illustrator sets type at, in points.
constexpr double SMALLEST = 0.1, LARGEST = 1296;

/// A real number as the text documents have one: with a point, to five decimals.
std::string real(double v)
{
    auto s = std::format("{:.5f}", v);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s += '0';
    return s == "-0.0" ? "0.0" : s;
}

/// A whole number when it is one, else a real: Illustrator keeps tracking whole.
std::string number(double v)
{
    return v == std::round(v) && std::abs(v) < 1e9 ? std::to_string(std::lround(v)) : real(v);
}

/// A string as the text documents have one: UTF-16BE after its mark, its parentheses and
/// backslashes escaped and every other byte as it is, as Illustrator writes them.
std::string utf16(std::string const &utf8)
{
    std::string out = "(\xfe\xff";
    glong count = 0;
    gunichar2 *units = g_utf8_to_utf16(utf8.c_str(), -1, nullptr, &count, nullptr);
    for (glong i = 0; units && i < count; ++i) {
        for (char const c : {char(units[i] >> 8), char(units[i] & 0xff)}) {
            if (c == '(' || c == ')' || c == '\\') out += '\\';
            out += c;
        }
    }
    g_free(units);
    return out + ")";
}

std::string dict(Features const &entries)
{
    std::string out = "<<";
    for (auto const &[key, value] : entries) out += std::format(" /{} {}", key, value);
    return out + " >>";
}

std::string pair(double x, double y)
{
    return "[ " + real(x) + " " + real(y) + " ]";
}

/// The text documents' encoding: ASCII85 on lines that start with %, ending ~>.
std::string ascii85(std::string const &data)
{
    std::string out = "%";
    std::size_t column = 0;
    auto put = [&](char c) {
        if (column == 100) {
            out += "\n%";
            column = 0;
        }
        out += c;
        ++column;
    };
    for (std::size_t i = 0; i < data.size(); i += 4) {
        auto const n = std::min<std::size_t>(4, data.size() - i);
        std::uint32_t v = 0;
        for (std::size_t j = 0; j < 4; ++j) v = v << 8 | (j < n ? std::uint8_t(data[i + j]) : 0u);
        if (n == 4 && v == 0) {
            put('z');
            continue;
        }
        char digits[5];
        for (int j = 4; j >= 0; --j) {
            digits[j] = char('!' + v % 85);
            v /= 85;
        }
        for (std::size_t j = 0; j <= n; ++j) put(digits[j]);
    }
    return out + "~>";
}

/// Whether the character is of a script read from right to left (as ai-native-text.cpp).
bool reads_from_the_right(gunichar c)
{
    return (c >= 0x590 && c <= 0x8ff) || (c >= 0xfb1d && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfeff) ||
           (c >= 0x10800 && c <= 0x10fff) || (c >= 0x1e800 && c <= 0x1efff);
}

/// Whether a list of numbers (dx, dy, rotate) moves or turns anything.
bool moves(char const *list)
{
    if (!list) return false;
    for (char const *at = list; *at;) {
        char *end = nullptr;
        double const v = g_ascii_strtod(at, &end);
        if (end == at) {
            ++at;
            continue;
        }
        if (v != 0) return true;
        at = end;
    }
    return false;
}

/// Whether an element names a font of its own.
bool declares_font(XML::Node const *repr)
{
    if (repr->attribute("font-family")) return true;
    auto *css = sp_repr_css_attr(repr, "style");
    bool const has = sp_repr_css_property(css, "font-family", nullptr) != nullptr;
    sp_repr_css_attr_unref(css);
    return has;
}

/// A paint on type, as its dictionary; nothing for none, and for what is refused.
std::optional<std::string> type_paint(SPIPaint const &p, SPStyle const *style, double opacity, std::set<std::string> &why)
{
    if (p.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_FILL || p.paintOrigin == SP_CSS_PAINT_ORIGIN_CONTEXT_STROKE) {
        why.insert("context paint on type");
        return {};
    }
    std::optional<Colors::Color> c;
    if (p.paintOrigin == SP_CSS_PAINT_ORIGIN_CURRENT_COLOR) {
        c = style->color.getColor();
    } else if (p.isNone()) {
        return {};
    } else if (p.isColor()) {
        c = p.getColor();
    } else {
        auto *server = p.isPaintserver() && p.href ? p.href->getObject() : nullptr;
        auto *swatch = cast<SPGradient>(server);
        if (swatch && swatch->isSolid()) {
            // Illustrator's type takes an ink by its swatch; the reader doesn't read that yet.
            if (SpotInk::read(swatch->getRepr()) || (swatch->getVector() && SpotInk::read(swatch->getVector()->getRepr()))) {
                why.insert("spot inks on type");
                return {};
            }
            c = swatch->getPreviewAverageColor();
        } else if (is<SPPattern>(server)) {
            why.insert("patterns on type");
            return {};
        } else {
            why.insert("gradients on type");
            return {};
        }
    }
    // A character has no opacity of its own in Illustrator.
    if (c->getOpacity() * opacity < 1.0 - 1e-6) {
        why.insert("translucent fills or strokes on type");
        return {};
    }
    auto channel = [](double v) { return real(std::clamp(v, 0.0, 1.0)); };
    if (c->getSpace() && c->getSpace()->getComponentType() == Type::CMYK) {
        return std::format("<< /99 /SimplePaint /0 << /0 2 /1 [ 1.0 {} {} {} {} ] >> >>", channel((*c)[0]), channel((*c)[1]),
                           channel((*c)[2]), channel((*c)[3]));
    }
    if (auto rgb = c->converted(Type::RGB)) {
        return std::format("<< /99 /SimplePaint /0 << /0 1 /1 [ 1.0 {} {} {} ] >> >>", channel((*rgb)[0]),
                           channel((*rgb)[1]), channel((*rgb)[2]));
    }
    why.insert("colours on type that are neither CMYK nor RGB");
    return {};
}

/// Whether `owner`, or the nearest element above it up to the text that has `name`, says `value`.
bool says(SPObject const *owner, SPText const *text, char const *name, std::string_view value)
{
    for (auto const *o = owner; o; o = o == text ? nullptr : o->parent) {
        if (auto const *v = o->getAttribute(name)) return value == v;
    }
    return false;
}

/// How an import's area type aligns a paragraph: it keeps that in text-align, which Inkscape
/// follows only for lines it lays out in a frame, and places the lines by their anchors.
/// Without one, text-anchor says. Nothing for justified paragraphs.
std::optional<unsigned> frame_alignment(SPObject const *owner, SPText const *text)
{
    bool aligned = false;
    for (auto const *o = owner; o && !aligned; o = o == text ? nullptr : o->parent) aligned = o->style->text_align.set;
    auto const *st = owner->style;
    if (aligned) {
        switch (st->text_align.computed) {
            case SP_CSS_TEXT_ALIGN_JUSTIFY: return {};
            case SP_CSS_TEXT_ALIGN_END:
            case SP_CSS_TEXT_ALIGN_RIGHT: return 1;
            case SP_CSS_TEXT_ALIGN_CENTER: return 2;
            default: return 0;
        }
    }
    return st->text_anchor.computed == SP_CSS_TEXT_ANCHOR_END ? 1 : st->text_anchor.computed == SP_CSS_TEXT_ANCHOR_MIDDLE ? 2 : 0;
}

} // namespace

TextStories::TextStories(SPDocument const *document)
    : _document(document)
{
    // The normal style's font, and the one Illustrator draws what doesn't show in.
    font("MyriadPro-Regular", false, {});
    font("AdobeInvisFont", false, {});
}

std::size_t TextStories::font(std::string const &name, bool truetype, std::vector<double> const &design)
{
    auto key = name;
    for (auto const v : design) key += " " + real(v);
    if (auto const it = _font_at.find(key); it != _font_at.end()) return it->second;
    auto entry = "<< /0 << /99 /CoolTypeFont /0 << /0 " + utf16(name) + " /2 " + (truetype ? "1" : "0");
    if (!design.empty()) {
        // An instance of a variable font: its value on each axis, in 16.16 fixed point.
        entry += " /4 [";
        for (auto const v : design) entry += " " + std::to_string(std::llround(v * 65536));
        entry += " ]";
    }
    entry += " >> >> >>";
    _fonts.push_back(std::move(entry));
    return _font_at[key] = _fonts.size() - 1;
}

WrittenStory TextStories::add(SPText const *text, Geom::Affine const &to_canvas)
{
    WrittenStory out;
    std::set<std::string> why;
    auto finish = [&] {
        out.refused.assign(why.begin(), why.end());
        out.frame.reset();
        return out;
    };
    // Area type: the frame an import kept, or the one shape Inkscape flows the text into, in the
    // text's own coordinates. What an import's frame was too small to show comes after its lines.
    bool const flowed = text->has_shape_inside();
    std::optional<Geom::PathVector> frame;
    std::string overflow;
    if (flowed) {
        auto const &style = *text->style;
        auto const &inside = style.shape_inside.hrefs;
        auto *shape = inside.size() == 1 ? inside[0]->getObject() : nullptr;
        if (inside.size() != 1) {
            why.insert("type flowed into several shapes");
        } else if (!shape) {
            why.insert("type flowed into something that isn't a shape");
        } else {
            if (!shape->curve()) shape->set_shape();
            if (shape->curve()) frame = *shape->curve() * shape->transform;
        }
        if (style.shape_subtract.set && !style.shape_subtract.hrefs.empty()) {
            why.insert("type flowed around other shapes (shape-subtract)");
        }
        if (style.shape_padding.set && std::abs(style.shape_padding.computed) > 1e-9) {
            why.insert("type flowed with padding inside its shape (shape-padding)");
        }
        // The layout leaves out what doesn't fit: there is nothing of it to write.
        if (text->layout.inputTruncated()) why.insert("type that overflows the shape it flows into");
    } else if (text->has_inline_size()) {
        why.insert("auto-wrapped type (inline-size)");
    } else if (auto const *d = text->getAttribute(TEXT_FRAME_ATTRIBUTE)) {
        frame = sp_svg_read_pathv(d);
        if (auto const *more = text->getAttribute(TEXT_OVERFLOW_ATTRIBUTE)) overflow = more;
    }
    bool const area = frame.has_value();
    if (!text->is_horizontal()) why.insert("vertical type");
    // What the layout doesn't keep of each character: pieces moved, turned or stretched.
    std::function<void(SPObject const *)> look = [&](SPObject const *o) {
        if (is<SPTextPath>(o)) why.insert("type on a path");
        if (moves(o->getAttribute("dx")) || moves(o->getAttribute("dy")) || moves(o->getAttribute("rotate"))) {
            why.insert("characters moved or turned one by one (dx, dy, rotate)");
        }
        if (o->getAttribute("textLength")) why.insert("type stretched to a length (textLength)");
        if (auto const *style = o->style) {
            if (style->direction.computed == SP_CSS_DIRECTION_RTL) why.insert("type that reads from right to left");
            if (style->unicode_bidi.computed != SP_CSS_UNICODE_BIDI_NORMAL) why.insert("type with its reading order overridden");
        }
        for (auto const &child : o->children) look(&child);
    };
    look(text);
    double const det = to_canvas.det();
    if (!std::isfinite(det) || std::abs(det) < 1e-12) why.insert("type that can't be placed");
    if (!why.empty()) return finish();
    // A uniform scale goes into the sizes: Illustrator shows type's own size as set. A turn as
    // Illustrator writes one, to five decimals, is a scale a few millionths from 1: none.
    double s = std::sqrt(std::abs(det));
    if (std::abs(s - 1) < 1e-5) s = 1;
    auto const matrix = Geom::Scale(1 / s) * to_canvas;
    // Area type's frame is turned and moved, not stretched: Illustrator lays it out in the frame
    // as it is, at the type's own size.
    std::optional<Geom::Path> outline;
    if (area) {
        if (std::abs(matrix[0] * matrix[0] + matrix[1] * matrix[1] - 1) > 1e-4 ||
            std::abs(matrix[2] * matrix[2] + matrix[3] * matrix[3] - 1) > 1e-4 ||
            std::abs(matrix[0] * matrix[2] + matrix[1] * matrix[3]) > 1e-4) {
            why.insert("area type stretched or skewed");
        }
        auto const pv = pathv_to_linear_and_cubic_beziers(*frame);
        auto const bounds = pv.size() == 1 ? pv.boundsExact() : Geom::OptRect();
        if (!bounds || bounds->width() * s < 0.01 || bounds->height() * s < 0.01) {
            why.insert("area type in a frame that isn't one outline");
        } else {
            outline = pv[0];
            outline->close(true);
        }
        if (!why.empty()) return finish();
    }

    // The characters, line by line. A line is the layout's chunk: plain SVG text (an import's)
    // sets each of its lines as a chunk of one layout line, by its own x and y; Inkscape's lines,
    // and those it flows into a shape, are layout lines of a chunk each.
    struct Char
    {
        std::string utf8;
        unsigned units;
        SPObject const *owner;
    };
    struct Line
    {
        Geom::Point anchor;  ///< The x it is aligned to, on its baseline.
        double left = 0;     ///< Where its first character starts.
        unsigned alignment = 0;
        unsigned layout_line = 0;
        unsigned paragraph = 0; ///< The layout's paragraph.
        /// A line feed or a line separator typed at its end, which Inkscape's layout keeps as a
        /// character of the line inside its paragraph (white-space:pre, as for type in a shape).
        std::optional<bool> typed_break; ///< True for a line separator.
        std::vector<Char> chars;
    };
    std::map<unsigned, Line> lines;
    auto const &layout = text->layout;
    for (auto it = layout.begin(); it != layout.end() && why.empty(); it.nextCharacter()) {
        SPObject *source = nullptr;
        Glib::ustring::iterator at;
        layout.getSourceOfCharacter(it, &source, &at);
        auto const *string = cast<SPString>(source);
        // A break between lines is a character of the line's element, with nothing to draw.
        if (!string && !it.hasGlyph()) continue;
        if (!string || !string->parent) {
            why.insert("type with characters that aren't text");
            break;
        }
        gunichar const c = *at;
        // Breaks are the layout's lines. One typed at the end of a line ends it there.
        if (c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029) {
            if (auto const typed = lines.find(layout.chunkIndex(it)); typed != lines.end()) typed->second.typed_break = c == 0x2028;
            continue;
        }
        if (c == '\t') {
            why.insert("tabs in type");
            break;
        }
        if (c < 0x20 || c == 0x7f) {
            why.insert("control characters in type");
            break;
        }
        if (reads_from_the_right(c)) {
            why.insert("letters of scripts read from right to left");
            break;
        }
        auto [found, fresh] = lines.try_emplace(layout.chunkIndex(it));
        auto &line = found->second;
        if (fresh) {
            line.anchor = layout.chunkPlacement(it);
            line.left = layout.characterAnchorPoint(it)[Geom::X];
            line.layout_line = layout.lineIndex(it);
            line.paragraph = layout.paragraphIndex(it);
            // Inkscape aligns the lines it flows into a shape by their paragraph. An import's area
            // type keeps its paragraphs' alignment in text-align, the lines placed by their anchors.
            std::optional<unsigned> alignment;
            if (area && !flowed) {
                alignment = frame_alignment(string->parent, text);
            } else {
                auto const a = layout.paragraphAlignment(it);
                if (a != Text::Layout::FULL) alignment = a == Text::Layout::RIGHT ? 1 : a == Text::Layout::CENTER ? 2 : 0;
            }
            if (!alignment) {
                why.insert("justified type");
                break;
            }
            line.alignment = *alignment;
        }
        gchar bytes[8];
        auto const n = g_unichar_to_utf8(c, bytes);
        line.chars.push_back({std::string(bytes, n), c > 0xffff ? 2u : 1u, string->parent});
    }
    if (!why.empty()) return finish();
    if (lines.empty() && overflow.find_first_not_of('\n') == std::string::npos) {
        out.empty = true;
        return out;
    }
    // Point type has one anchor, its lines aligned to it, each its leading below the one before.
    // Area type's lines lie where the frame put them.
    enum class End { Paragraph, Break, Wrap };
    struct Row
    {
        Line const *line; ///< Nothing for an empty line between two others.
        double y;
        End end = End::Paragraph;
    };
    std::vector<Row> rows;
    auto const first = lines.empty() ? Geom::Point() : lines.begin()->second.anchor;
    for (auto it = lines.begin(); it != lines.end(); ++it) {
        auto const &line = it->second;
        if (!area && std::abs(line.anchor.x() - first.x()) * s > 1e-3) why.insert("lines of type that don't share one anchor");
        if (it != lines.begin()) {
            auto const &before = std::prev(it)->second;
            double const down = (line.anchor.y() - before.anchor.y()) * s;
            if (std::abs(down) <= 0.01) {
                why.insert("lines of type set in pieces");
            } else if (down < 0) {
                why.insert("lines of type that don't each lie below the one before");
            }
            // Empty lines of Inkscape's are layout lines without a chunk, evenly between.
            auto const steps = std::max(1u, line.layout_line - std::min(line.layout_line, before.layout_line));
            for (unsigned k = 1; k < steps; ++k) {
                rows.push_back({nullptr, before.anchor.y() + (line.anchor.y() - before.anchor.y()) * k / steps});
            }
        }
        rows.push_back({&line, line.anchor.y()});
    }
    if (!why.empty()) return finish();
    // An empty line of plain SVG text is an element placed by its own y with nothing in it.
    for (auto const &child : text->children) {
        auto const *span = cast<SPTSpan>(&child);
        if (!span || span->role != SP_TSPAN_ROLE_UNSPECIFIED || !child.getAttribute("y")) continue;
        bool typed = false;
        std::function<void(SPObject const *)> look_in = [&](SPObject const *o) {
            if (auto const *string = cast<SPString>(o); string && !string->string.empty()) typed = true;
            for (auto const &inner : o->children) look_in(&inner);
        };
        look_in(span);
        SVGLength y;
        if (typed || !y.read(child.getAttribute("y"))) continue;
        auto const at = std::find_if(rows.begin(), rows.end(), [&](Row const &r) { return r.y > y.computed; });
        if (at != rows.begin() && at != rows.end() && (at - 1)->y < y.computed) {
            rows.insert(at, {nullptr, y.computed, says(&child, text, LINE_END_ATTRIBUTE, "break") ? End::Break : End::Paragraph});
        }
    }
    // How each line ends. The layout's paragraphs, and lines that end with a line feed, end with
    // a typed end. Inside one, Inkscape's own lines in a shape are where it wrapped them; an
    // import's lines say how they ended.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto &row = rows[i];
        if (!row.line) continue;
        if (row.line->typed_break) {
            row.end = *row.line->typed_break ? End::Break : End::Paragraph;
            continue;
        }
        auto const *next = i + 1 < rows.size() ? rows[i + 1].line : nullptr;
        if (next && next->paragraph != row.line->paragraph) continue;
        auto const *last = row.line->chars.back().owner;
        if (says(last, text, LINE_END_ATTRIBUTE, "break")) {
            row.end = End::Break;
        } else if (area && i + 1 < rows.size() && (flowed || says(last, text, LINE_END_ATTRIBUTE, "wrap"))) {
            row.end = End::Wrap;
        }
    }
    // The story ends its last paragraph, unless an import's last line ends where its frame
    // stopped showing the rest, which comes after it.
    bool const hidden = overflow.find_first_not_of('\n') != std::string::npos;
    if (!rows.empty()) {
        auto &last = rows.back();
        auto const *owner = last.line ? last.line->chars.back().owner : nullptr;
        last.end = hidden && owner && says(owner, text, LINE_END_ATTRIBUTE, "wrap")    ? End::Wrap
                 : hidden && owner && says(owner, text, LINE_END_ATTRIBUTE, "break") ? End::Break
                                                                                       : End::Paragraph;
    }

    // Each owner's features, but for leading, which is the line's.
    auto const settings = TypeSettings::of(_document);
    std::map<SPObject const *, std::optional<Features>> known;
    auto features_of = [&](SPObject const *owner) -> std::optional<Features> const & {
        auto [it, fresh] = known.try_emplace(owner);
        if (!fresh) return it->second;
        auto const *st = owner->style;
        Features f;
        std::set<std::string> own;
        // The font: the one a font stands in for, else the one that sets it.
        std::optional<std::size_t> index;
        for (auto const *o = owner; o; o = o->parent) {
            auto const *repr = o->getRepr();
            if (repr && repr->attribute(FONT_MISSING_ATTRIBUTE)) {
                if (stands_in(repr)) {
                    std::vector<double> design;
                    if (auto const *axes = repr->attribute(FONT_MISSING_AXES_ATTRIBUTE)) {
                        for (char const *at = axes; *at;) {
                            char *end = nullptr;
                            double const v = g_ascii_strtod(at, &end);
                            if (end == at) {
                                ++at;
                                continue;
                            }
                            design.push_back(v);
                            at = end;
                        }
                    }
                    index = font(repr->attribute(FONT_MISSING_ATTRIBUTE), false, design);
                }
                break;
            }
            // A font named further down isn't the one stood in for further up.
            if ((repr && declares_font(repr)) || o == text) break;
        }
        if (!index) {
            if (!st->font_variation_settings.normal) own.insert("variable font instances on type");
            std::string reason;
            if (auto const ps = postscript_font(st, reason)) {
                index = font(ps->name, ps->truetype, {});
            } else {
                own.insert(reason);
            }
        }
        if (index) f[0] = std::to_string(*index);
        double const size = st->font_size.computed * s;
        if (!(size >= SMALLEST && size <= LARGEST)) own.insert("type sizes Illustrator can't set (under 0.1 or over 1296 pt)");
        f[1] = real(size);
        // Superscript and subscript keep the type's size; the layout sets them smaller.
        auto const position = st->font_variant_position.computed;
        double share = 1;
        if (position == SP_CSS_FONT_VARIANT_POSITION_SUPER) {
            f[13] = "1";
            share = settings.superscript_size;
        } else if (position == SP_CSS_FONT_VARIANT_POSITION_SUB) {
            f[13] = "2";
            share = settings.subscript_size;
        } else {
            f[13] = "0";
        }
        // Tracking, in thousandths of the size the characters are set at.
        double tracking = 0;
        if (!st->letter_spacing.normal && st->font_size.computed > 0) {
            tracking = std::round(st->letter_spacing.computed / (st->font_size.computed * share) * 1e6) / 1e3;
        }
        if (!(tracking >= -1000 && tracking <= 10000)) own.insert("tracking Illustrator can't set");
        f[8] = number(tracking);
        if (!st->word_spacing.normal && st->word_spacing.computed != 0) own.insert("word spacing on type");
        // A shift is from the baseline of what holds the span, and adds to that one's.
        double shift = 0;
        bool underline = false, strikethrough = false;
        for (auto const *o = owner; o; o = o->parent) {
            auto const &b = o->style->baseline_shift;
            if (b.set && b.type == SP_BASELINE_SHIFT_LITERAL && b.literal != SP_CSS_BASELINE_SHIFT_BASELINE) {
                own.insert("superscript or subscript made by a baseline shift");
            } else if (b.set) {
                shift += b.computed;
            }
            auto const &d = o->style->text_decoration_line;
            if (d.set) {
                underline = underline || d.underline;
                strikethrough = strikethrough || d.line_through;
                if (d.overline || d.blink) own.insert("overlines on type");
            }
            if (o->style->text_decoration_style.set && !o->style->text_decoration_style.solid) {
                own.insert("underlines that aren't solid");
            }
            if (o->style->text_decoration_color.set) own.insert("underlines in colours of their own");
            if (o != text && o->style->opacity.value < SP_SCALE24_MAX) own.insert("transparency on part of a text");
            if (o == text) break;
        }
        f[9] = real(shift * s);
        f[15] = strikethrough ? "1" : "0";
        f[16] = underline ? "1" : "0";
        // Kerning by the font's own pairs, or none.
        std::string features = st->font_feature_settings.value() ? st->font_feature_settings.value() : "normal";
        std::erase_if(features, [](char c) { return c == ' ' || c == '\'' || c == '"'; });
        if (features == "normal" || features.empty()) {
            f[11] = "1";
        } else if (features == "kern0" || features == "kernoff") {
            f[11] = "0";
        } else {
            own.insert("OpenType feature settings on type");
        }
        auto const ligatures = st->font_variant_ligatures.computed;
        if (ligatures & (SP_CSS_FONT_VARIANT_LIGATURES_DISCRETIONARY | SP_CSS_FONT_VARIANT_LIGATURES_HISTORICAL)) {
            own.insert("discretionary or historical ligatures");
        }
        f[18] = ligatures & SP_CSS_FONT_VARIANT_LIGATURES_COMMON ? "true" : "false";
        f[20] = ligatures & SP_CSS_FONT_VARIANT_LIGATURES_CONTEXTUAL ? "true" : "false";
        if (st->font_variant_numeric.computed != SP_CSS_FONT_VARIANT_NUMERIC_NORMAL ||
            st->font_variant_east_asian.computed != SP_CSS_FONT_VARIANT_EAST_ASIAN_NORMAL ||
            st->font_variant_alternates.computed != SP_CSS_FONT_VARIANT_ALTERNATES_NORMAL) {
            own.insert("OpenType figure, alternate or East Asian forms on type");
        }
        // Capitals: 0 as typed, 1 small caps, 2 all caps. The story keeps what was typed.
        auto const transform = st->text_transform.computed;
        auto const caps = st->font_variant_caps.computed;
        if (transform != SP_CSS_TEXT_TRANSFORM_NONE && transform != SP_CSS_TEXT_TRANSFORM_UPPERCASE) {
            own.insert("type set in lower case or capitalised");
        }
        if (caps != SP_CSS_FONT_VARIANT_CAPS_NORMAL && caps != SP_CSS_FONT_VARIANT_CAPS_SMALL) {
            own.insert("capitals other than all caps and small caps");
        }
        if (transform == SP_CSS_TEXT_TRANSFORM_UPPERCASE && caps == SP_CSS_FONT_VARIANT_CAPS_SMALL) {
            own.insert("all caps and small caps together");
        }
        f[12] = transform == SP_CSS_TEXT_TRANSFORM_UPPERCASE ? "2" : caps == SP_CSS_FONT_VARIANT_CAPS_SMALL ? "1" : "0";
        // Fill and stroke. Illustrator strokes characters over their fill.
        auto const fill = type_paint(st->fill, st, st->fill_opacity.value / double(SP_SCALE24_MAX), own);
        f[56] = fill ? "true" : "false";
        if (fill) f[53] = *fill;
        std::optional<std::string> stroke;
        if (!st->stroke.isNone() && st->stroke_width.computed > 0) {
            stroke = type_paint(st->stroke, st, st->stroke_opacity.value / double(SP_SCALE24_MAX), own);
        }
        f[57] = stroke ? "true" : "false";
        if (stroke) {
            f[54] = *stroke;
            f[63] = real(st->stroke_width.computed * s);
            f[64] = real(st->stroke_miterlimit.value);
            if (st->stroke_linecap.computed != SP_STROKE_LINECAP_BUTT || st->stroke_linejoin.computed != SP_STROKE_LINEJOIN_MITER) {
                own.insert("stroke caps or joins on type other than butt and miter");
            }
            for (auto const v : st->stroke_dasharray.get_computed()) {
                if (v > 0) own.insert("dashed strokes on type");
            }
            if (st->paint_order.layer[0] == SP_CSS_PAINT_ORDER_STROKE) own.insert("strokes painted under type's fill");
        }
        if (!own.empty()) {
            why.insert(own.begin(), own.end());
            return it->second;
        }
        it->second = std::move(f);
        return it->second;
    };

    // The story: what was typed, a return ending each paragraph and a line break each line
    // typed inside one, in runs of one set of features. A paragraph run ends at either, as
    // Illustrator writes them.
    std::string typed, paragraph_runs, style_runs, laid;
    std::optional<std::string> run_features;
    std::size_t run_units = 0, paragraph_units = 0;
    std::optional<Row const *> paragraph_start;
    auto flush = [&] {
        if (run_units) style_runs += std::format(" << /0 << /0 << /0 {} /5 0 /6 {} >> >> /1 {} >>", utf16(""), *run_features, run_units);
        run_units = 0;
    };
    auto append = [&](std::string const &features, unsigned units) {
        if (run_features != features) {
            flush();
            run_features = features;
        }
        run_units += units;
    };
    auto end_paragraph = [&] {
        auto const *row = *paragraph_start;
        auto const alignment = row->line ? row->line->alignment : 0u;
        bool const hyphenate = area && !flowed && row->line && says(row->line->chars.front().owner, text, HYPHENATE_ATTRIBUTE, "true");
        paragraph_runs += std::format(" << /0 << /0 << /0 {} /5 << /0 {}{} >> /6 0 >> >> /1 {} >>", utf16(""), alignment,
                                      hyphenate ? " /9 true" : "", paragraph_units);
        paragraph_units = 0;
        paragraph_start.reset();
    };
    // Where the layout PROOF's reader reads starts: point type's anchor, area type's frame's
    // top-left corner. Lines are placed down and across from it.
    auto const origin = area ? outline->boundsExact()->min() : first;
    std::optional<Features> last_features;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto const &p = rows[i];
        if (!paragraph_start) paragraph_start = &p;
        // Its leading: down from the line before; the first takes the second's.
        std::optional<double> leading;
        if (i > 0) leading = (p.y - rows[i - 1].y) * s;
        else if (rows.size() > 1) leading = (rows[1].y - p.y) * s;
        std::vector<Char> const empty{{"", 0, text}};
        auto const &chars = p.line ? p.line->chars : empty;
        double largest = 0;
        for (auto const &c : chars) largest = std::max(largest, c.owner->style->font_size.computed * s);
        bool const automatic = !leading || std::abs(*leading - AUTO_LEADING * largest) < 1e-3;
        std::size_t units = 0;
        for (auto const &c : chars) {
            auto const &own = features_of(c.owner);
            if (!own) continue;
            auto f = *own;
            f[4] = automatic ? "true" : "false";
            if (!automatic) f[5] = real(*leading);
            typed += c.utf8;
            units += c.units;
            // What ends the line is in the features of its last character.
            if (&c == &chars.back() && p.end != End::Wrap) {
                typed += p.end == End::Break ? '\x03' : '\r';
                ++units;
                append(dict(f), c.units + 1);
            } else {
                append(dict(f), c.units);
            }
            if (&c == &chars.back()) last_features = std::move(f);
        }
        if (!why.empty()) return finish();
        paragraph_units += units;
        if (p.end != End::Wrap) end_paragraph();
        // The line where PROOF sets it, for PROOF's reader: down from where the layout starts,
        // and where it starts across from there.
        double const across = p.line ? (p.line->left - origin.x()) * s : 0.0;
        laid += "<< /99 /L";
        if (area || i > 0) laid += " /0 << /0 " + pair(0, (p.y - origin.y()) * s) + " >>";
        laid += std::format(" /6 [ << /99 /S /15 << /0 {} /2 0 /5 false >> /0 << /0 {} >> >> ] >> ", units, pair(across, 0));
    }
    // What an import's frame was too small to show: its paragraphs, in the last character's features.
    Row const none{nullptr, 0};
    if (hidden) {
        if (!last_features) last_features = features_of(text);
        if (!last_features) return finish();
        auto const features = dict(*last_features);
        auto const *after = rows.empty() ? &none : &rows.back();
        if (!paragraph_start) paragraph_start = after;
        if (overflow.back() != '\n') overflow += '\n';
        for (auto const *at = overflow.c_str(); *at; at = g_utf8_next_char(at)) {
            gunichar const c = g_utf8_get_char(at);
            if (c == '\n') {
                typed += '\r';
                append(features, 1);
                ++paragraph_units;
                end_paragraph();
                paragraph_start = after;
                continue;
            }
            if (c < 0x20 || c == 0x7f || reads_from_the_right(c)) {
                why.insert("type the frame doesn't show that can't be written");
                return finish();
            }
            gchar bytes[8];
            typed.append(bytes, g_unichar_to_utf8(c, bytes));
            append(features, c > 0xffff ? 2 : 1);
            paragraph_units += c > 0xffff ? 2 : 1;
        }
        paragraph_start.reset();
    }
    flush();

    auto const frame_index = _frames.size();
    if (area) {
        // The outline, each segment four points (a start, two handles and an end) in the
        // story's own space; Illustrator's kind for area type (1) and its gutters.
        std::string points;
        for (auto const &c : *outline) {
            auto const *cubic = dynamic_cast<Geom::CubicBezier const *>(&c);
            for (auto const &q : {c.initialPoint(), cubic ? (*cubic)[1] : c.initialPoint(), cubic ? (*cubic)[2] : c.finalPoint(),
                                  c.finalPoint()}) {
                points += " " + real(q.x() * s) + " " + real(q.y() * s);
            }
        }
        // Where its first baseline goes: by the rule an import's frame had (Illustrator's
        // default, Ascent, unless it says), and for type Inkscape flowed into a shape where
        // Inkscape set it, fixed (rule 0) that far below the frame's top. Illustrator 29.2.1's
        // rules, found by having it lay out each (2026-10-23): 0 fixed at the minimum, 1 Ascent,
        // 2 cap height, 3 leading, 4 x-height, 5 em box, 6 legacy.
        std::string first_baseline;
        if (auto const *rule = text->getAttribute(FIRST_BASELINE_ATTRIBUTE)) {
            char *end = nullptr, *stop = nullptr;
            auto const kind = std::strtoul(rule, &end, 10);
            double const minimum = g_ascii_strtod(end, &stop);
            if (end != rule && kind <= 6 && stop != end && std::isfinite(minimum)) {
                first_baseline = std::format(" /10 << /0 {} /1 {} >>", kind, real(minimum));
            }
        } else if (flowed && !rows.empty()) {
            first_baseline = std::format(" /10 << /0 0 /1 {} >>", real(std::max(0.0, (rows.front().y - origin.y()) * s)));
        }
        _frames.push_back(std::format("<< /1 << /0 [{} ] >> /2 << /0 1 /2 [ {} {} {} {} {} {} ] /7 18.0 /8 18.0{} >> >>", points,
                                      real(matrix[0]), real(matrix[1]), real(matrix[2]), real(matrix[3]), real(matrix[4]),
                                      real(matrix[5]), first_baseline));
        out.frame = *outline;
    } else {
        _frames.push_back(std::format("<< /0 {} /2 << /2 [ {} {} {} {} {} {} ] >> >>", pair(first.x() * s, first.y() * s),
                                      real(matrix[0]), real(matrix[1]), real(matrix[2]), real(matrix[3]), real(matrix[4]),
                                      real(matrix[5])));
    }
    out.story = _stories.size();
    _stories.push_back(std::format(
        "<< /0 << /0 {} /5 << /0 [{} ] >> /6 << /0 [{} ] >> /10 << /0 4 /2 true >> >> "
        "/1 << /0 [ << /0 {} >> ] /2 [ << /99 /PC /5 0 /6 [ << /99 /F /10 {} /0 << /0 {} >> /5 2 "
        "/6 [ << /99 /R /6 [ << /99 /R /5 2 /6 [ {}] >> ] >> ] >> ] >> ] >> >>",
        utf16(typed), paragraph_runs, style_runs, frame_index, frame_index, pair(origin.x() * s, origin.y() * s), laid));
    return out;
}

std::string TextStories::document(bool typed) const
{
    auto const settings = TypeSettings::of(_document);
    std::string out = " /98 << /0 14 >> /0 << /1 << /0 [";
    for (auto const &f : _fonts) out += " " + f;
    out += " ] /1 [";
    for (std::size_t i = 0; i < _fonts.size(); ++i) out += std::format(" << /0 {} >>", i);
    out += " ] >> /5 << /0 [ << /0 << /0 " + utf16("[Normal Character Style]") + " /6 << " + std::string(NORMAL_CHARACTER) +
           " >> >> >> ] /1 [ << /0 0 >> ] >> /6 << /0 [ << /0 << /0 " + utf16("[Normal Paragraph Style]") + " /5 << " +
           std::string(NORMAL_PARAGRAPH) + " >> >> >> ] /1 [ << /0 0 >> ] >>";
    if (typed && !_frames.empty()) {
        out += " /8 << /0 [";
        for (auto const &f : _frames) out += " << /0 " + f + " >>";
        out += " ] >>";
    }
    // How the document sets superscripts, subscripts and made-up small capitals.
    out += std::format(" >> /1 << /0 << /1 0 /2 0 /3 {} /4 {} /5 {} /6 {} /7 {} /8 true /11 6 /16 true /17 0 >>",
                       real(settings.superscript_size), real(settings.superscript_shift), real(settings.subscript_size),
                       real(settings.subscript_shift), real(settings.small_caps_size));
    if (typed) {
        out += " /1 [";
        for (auto const &story : _stories) out += " " + story;
        out += " ]";
    }
    out += " /2 << " + std::string(NORMAL_CHARACTER) + " >> /3 << " + std::string(NORMAL_PARAGRAPH) + " >> >>";
    return out;
}

std::string TextStories::section(Geom::Point const &ruler_origin) const
{
    return "%AI11_BeginTextDocument\n/AI11TextDocument : /ASCII85Decode ,\n" + ascii85(document(true)) + "\n" +
           real(ruler_origin.x()) + " " + real(ruler_origin.y()) + " /RulerOrigin ,\n;\n" +
           "/AI11UndoFreeTextDocument : /ASCII85Decode ,\n" + ascii85(document(false)) + "\n;\n%AI11_EndTextDocument\n";
}

} // namespace Inkscape::Extension::Internal::AiNative
