// SPDX-License-Identifier: GPL-2.0-or-later
// Numeric story keys adapted from VectorCraft ate.rs (4cf912f), MIT;
// Copyright (c) 2026 ArtCraft Team and the VectorCraft contributors.
#include "ai-native-text.h"
#include "ai-native-lexer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <glib.h>
#include <2geom/affine.h>
#include <2geom/bezier-curve.h>
#include <2geom/path.h>
#include <2geom/transforms.h>

namespace Inkscape::Extension::Internal::AiNative {
namespace {
// A document of some hundred pages has several hundred stories and a few hundred thousand
// values in each of its two text documents.
constexpr std::size_t MAX_BYTES = 64u << 20, MAX_STORY = 1u << 20, MAX_VALUES = 2000000;

struct Bad : std::runtime_error { using std::runtime_error::runtime_error; };

struct Value
{
    enum Kind { Number, String, Name, Boolean, Array, Dictionary } kind = Number;
    double number = 0;
    bool boolean = false;
    std::string string;
    std::vector<Value> list;
    std::map<std::string, Value> dict;
    Value const &get(std::string const &key) const {
        auto it = dict.find(key);
        if (kind != Dictionary || it == dict.end()) throw Bad("text dictionary is incomplete");
        return it->second;
    }
    Value const &at(std::size_t i) const {
        if (kind != Array || i >= list.size()) throw Bad("text array is incomplete");
        return list[i];
    }
    double num() const {
        if (kind != Number || !std::isfinite(number)) throw Bad("invalid text number");
        return number;
    }
    std::size_t index() const {
        auto n = num();
        if (n < 0 || n > MAX_STORY * 2 || n != std::floor(n)) throw Bad("invalid text index or run length");
        return std::size_t(n);
    }
    std::string const &str() const {
        if (kind != String) throw Bad("invalid text string");
        return string;
    }
    bool flag() const {
        if (kind != Boolean) throw Bad("invalid text flag");
        return boolean;
    }
    bool node(char const *name) const {
        auto it = dict.find("99");
        return it != dict.end() && it->second.kind == Name && it->second.string == name;
    }
    std::vector<double> nums(std::size_t n) const {
        if (kind != Array || list.size() != n) throw Bad("invalid text coordinates");
        std::vector<double> out;
        for (auto const &v : list) out.push_back(v.num());
        return out;
    }
};

class Reader
{
    std::string_view _s;
    std::size_t _at = 0, _budget = MAX_VALUES;
    void space() { while (_at < _s.size() && (g_ascii_isspace(_s[_at]) || _s[_at] == '\0')) ++_at; }
    bool take(std::string_view s) {
        if (_s.substr(_at, s.size()) != s) return false;
        _at += s.size(); return true;
    }
    std::string word() {
        auto start = _at;
        while (_at < _s.size() && !g_ascii_isspace(_s[_at]) && _s[_at] != '\0' &&
               std::string_view("<>()[]/").find(_s[_at]) == std::string_view::npos) ++_at;
        if (_at == start) throw Bad("invalid text token");
        return std::string(_s.substr(start, _at - start));
    }
    std::string name() { space(); if (!take("/")) throw Bad("invalid text key"); return word(); }
    std::string string() {
        std::string bytes;
        unsigned nested = 0;
        while (_at < _s.size()) {
            char c = _s[_at++];
            if (c == ')' && nested == 0) {
                if (bytes.size() >= 2 && std::uint8_t(bytes[0]) == 0xfe && std::uint8_t(bytes[1]) == 0xff) {
                    GError *error = nullptr;
                    gsize written = 0;
                    auto *utf8 = g_convert(bytes.data() + 2, bytes.size() - 2, "UTF-8", "UTF-16BE", nullptr, &written, &error);
                    if (!utf8) { if (error) g_error_free(error); throw Bad("invalid UTF-16 text"); }
                    std::string out(utf8, written); g_free(utf8); return out;
                }
                return text_of(bytes);
            }
            if (c == '(') { if (++nested > 64) throw Bad("text string is nested too deeply"); }
            else if (c == ')') --nested;
            else if (c == '\\') {
                if (_at == _s.size()) throw Bad("unfinished text escape");
                c = _s[_at++];
                if (c >= '0' && c <= '7') {
                    unsigned v = c - '0';
                    for (unsigned i = 0; i < 2 && _at < _s.size() && _s[_at] >= '0' && _s[_at] <= '7'; ++i) v = v * 8 + (_s[_at++] - '0');
                    c = char(v & 255);
                } else {
                    if (c == '\r' || c == '\n') { if (c == '\r' && _at < _s.size() && _s[_at] == '\n') ++_at; continue; }
                    switch (c) { case 'n': c = '\n'; break; case 'r': c = '\r'; break;
                        case 't': c = '\t'; break; case 'b': c = '\b'; break; case 'f': c = '\f'; break; }
                }
            }
            bytes.push_back(c);
            if (bytes.size() > MAX_STORY * 2) throw Bad("text string exceeds the limit");
        }
        throw Bad("unfinished text string");
    }
    Value value(unsigned depth) {
        space();
        if (depth > 64 || !_budget-- || _at == _s.size()) throw Bad("text document exceeds its limits or ends early");
        Value v;
        if (take("<<")) {
            v.kind = Value::Dictionary;
            for (;;) {
                space(); if (take(">>")) return v;
                auto k = name(); auto child = value(depth + 1);
                if (!v.dict.emplace(std::move(k), std::move(child)).second) throw Bad("duplicate text dictionary key");
            }
        }
        if (take("[")) {
            v.kind = Value::Array;
            for (;;) { space(); if (take("]")) return v; v.list.push_back(value(depth + 1)); }
        }
        if (take("(")) { v.kind = Value::String; v.string = string(); return v; }
        if (take("/")) { v.kind = Value::Name; v.string = word(); return v; }
        auto w = word();
        if (w == "true" || w == "false") { v.kind = Value::Boolean; v.boolean = w == "true"; }
        else { auto n = parse_number(w); if (!n) throw Bad("invalid text number"); v.number = *n; }
        return v;
    }
public:
    explicit Reader(std::string_view s) : _s(s) {}
    Value root() {
        Value out; out.kind = Value::Dictionary;
        for (;;) {
            space(); if (_at == _s.size()) return out;
            auto k = name(); auto v = value(0);
            if (!out.dict.emplace(std::move(k), std::move(v)).second) throw Bad("duplicate text document key");
        }
    }
};

/// The text documents' section of the records.
std::string_view text_section(std::string_view records)
{
    auto start = records.find("%AI11_BeginTextDocument");
    if (start == records.npos) throw Bad("the text document is missing");
    auto end = records.find("%AI11_EndTextDocument", start);
    if (end == records.npos) throw Bad("the text document doesn't end");
    return records.substr(start, end - start);
}

/// The ASCII85 block that starts at or after `from` in `section`, decoded; `from` is left
/// after it.
std::string unpack(std::string_view section, std::size_t &from)
{
    from = section.find("/ASCII85Decode", from);
    if (from == section.npos) throw Bad("the text encoding isn't supported");
    from += std::string_view("/ASCII85Decode").size();
    while (from < section.size() && g_ascii_isspace(section[from])) ++from;
    if (from == section.size() || section[from++] != ',') throw Bad("invalid text encoding marker");
    auto finish = section.find("~>", from);
    if (finish == section.npos || finish - from > MAX_BYTES * 2) throw Bad("the encoded text exceeds its limit or doesn't end");
    std::string out;
    std::uint64_t acc = 0;
    unsigned digits = 0;
    bool line_start = false;
    auto emit = [&](unsigned count) {
        if (acc > UINT32_MAX || out.size() + count > MAX_BYTES) throw Bad("invalid or oversized ASCII85 text");
        for (unsigned j = 0; j < count; ++j) out.push_back(char((acc >> (24 - 8 * j)) & 255));
        acc = 0; digits = 0;
    };
    for (auto at = from; at < finish; ++at) {
        char c = section[at];
        if (c == '\r' || c == '\n') { line_start = true; continue; }
        if (line_start && c == '%') { line_start = false; continue; }
        if (g_ascii_isspace(c)) continue;
        line_start = false;
        if (c == 'z' && digits == 0) emit(4);
        else {
            if (c < '!' || c > 'u') throw Bad("invalid ASCII85 text");
            acc = acc * 85 + unsigned(c - '!');
            if (++digits == 5) emit(4);
        }
    }
    if (digits == 1) throw Bad("incomplete ASCII85 text");
    if (digits) { auto count = digits - 1; while (digits++ < 5) acc = acc * 85 + 84; emit(count); }
    from = finish + 2;
    return out;
}

/// Whether `text` is UTF-8 throughout. A story can hold a NUL (it sets nothing), which
/// glib's own check takes for the end.
bool utf8_with_nuls(std::string const &text)
{
    for (std::size_t at = 0; at < text.size();) {
        auto const end = std::min(text.find('\0', at), text.size());
        if (end > at && !g_utf8_validate(text.data() + at, end - at, nullptr)) return false;
        at = end + 1;
    }
    return true;
}

void find_nodes(Value const &v, char const *name, std::vector<Value const *> &out)
{
    if (v.node(name)) out.push_back(&v);
    for (auto const &[key, child] : v.dict) find_nodes(child, name, out);
    for (auto const &child : v.list) find_nodes(child, name, out);
}

TextStyle style(Value const &own, Value const &defaults, std::vector<std::string> const &fonts)
{
    auto pick = [&](char const *k) -> Value const & {
        auto it = own.dict.find(k); return it == own.dict.end() ? defaults.get(k) : it->second;
    };
    auto number = [&](char const *k, double fallback) {
        if (!own.dict.count(k) && !defaults.dict.count(k)) return fallback;
        return pick(k).num();
    };
    TextStyle out;
    auto const font = pick("0").index();
    if (font >= fonts.size() || fonts[font].empty()) throw Bad("text has no known font name");
    out.font = fonts[font]; out.size = pick("1").num();
    if (out.font.size() > 256 || std::any_of(out.font.begin(), out.font.end(), [](unsigned char c) {
            return !g_ascii_isalnum(c) && c != '-' && c != '_' && c != '+' && c != '.';
        })) throw Bad("invalid PostScript font name");
    if (out.size <= 0 || out.size > 1e6) throw Bad("invalid text size");
    out.horizontal_scale = number("6", 1); out.vertical_scale = number("7", 1);
    if (out.horizontal_scale < 0.01 || out.horizontal_scale > 100 ||
        out.vertical_scale < 0.01 || out.vertical_scale > 100) throw Bad("invalid or excessive text scale");
    out.baseline_shift = number("9", 0);
    if (std::abs(out.baseline_shift) > 1e5) throw Bad("invalid text baseline shift");
    // How pairs are kerned (11), and the two substitutions fonts make unasked that
    // Illustrator lets a run switch off: standard ligatures (18), contextual alternates (20).
    auto const kerning = number("11", 1);
    if (kerning != 0 && kerning != 1 && kerning != 2 && kerning != 3) throw Bad("text is kerned in a way that isn't supported");
    out.kerning = int(kerning);
    auto on = [&](char const *k) {
        if (!own.dict.count(k) && !defaults.dict.count(k)) return true;
        auto const &v = pick(k);
        return v.kind != Value::Boolean || v.boolean;
    };
    out.ligatures = on("18");
    out.contextual = on("20");
    out.tracking = number("8", 0);
    if (std::abs(out.tracking) > 1e6) throw Bad("invalid text tracking");
    auto paint = [&](char const *k, double &opacity) -> Color {
        auto const &p = pick(k).get("0");
        auto kind = p.get("0").index(); auto const &values = p.get("1");
        auto const n = kind == 0 ? 2u : kind == 1 ? 4u : kind == 2 ? 5u : 0u;
        if (!n) throw Bad("text uses an unsupported paint (including named inks)");
        auto v = values.nums(n); opacity = v[0];
        if (std::any_of(v.begin(), v.end(), [](double x) { return x < 0 || x > 1; })) throw Bad("invalid text paint channels");
        return kind == 0 ? Color::gray(v[1]) : kind == 1 ? Color::rgb(v[1], v[2], v[3]) : Color::cmyk(v[1], v[2], v[3], v[4]);
    };
    if (pick("56").flag()) out.fill = paint("53", out.fill_opacity);
    if (pick("57").flag()) out.stroke = paint("54", out.stroke_opacity);
    out.stroke_width = number("63", 1);
    if (out.stroke_width < 0 || out.stroke_width > 1e6) throw Bad("invalid text stroke width");
    // Features beyond the supported style subset must not be silently normalised.
    for (auto const *k : {"2", "3", "10", "12", "13", "14", "15", "16", "17", "19", "21", "22", "23", "24", "25", "26", "28", "29", "30", "31", "32", "33", "34"}) {
        if (own.dict.count(k) || defaults.dict.count(k)) {
            auto const &v = pick(k);
            if ((v.kind == Value::Boolean && v.boolean) || (v.kind == Value::Number && v.num() != 0))
                throw Bad("text has an unsupported character style feature");
        }
    }
    return out;
}

/// A line of area type as Illustrator laid it out.
struct Laid
{
    Value const *line;
    Geom::Point at; ///< Its origin, from the frame's anchor.
};

/// A node's own offset from what holds it (`/0 << /0 [x y] >>`), nothing when it has none.
Geom::Point offset_of(Value const &node)
{
    if (node.kind != Value::Dictionary || !node.dict.count("0")) return {};
    auto const o = node.get("0").get("0").nums(2);
    return {o[0], o[1]};
}

/// The lines under `v` (the frame, or a row or column in it), each from the frame's anchor:
/// the offsets of what holds a line add up.
void laid_lines(Value const &v, Geom::Point const &at, unsigned depth, std::vector<Laid> &out)
{
    auto const kids = v.dict.find("6");
    if (depth > 8 || kids == v.dict.end() || kids->second.kind != Value::Array) {
        throw Bad("area type has a layout that isn't supported yet");
    }
    for (auto const &k : kids->second.list) {
        auto const here = at + offset_of(k);
        if (k.node("L")) {
            out.push_back({&k, here});
        } else if (k.node("R")) {
            laid_lines(k, here, depth + 1, out);
        } else {
            throw Bad("area type has a layout that isn't supported yet");
        }
        if (out.size() > 65536) throw Bad("text has too many lines");
    }
}

/// A frame's outline: its segments one after another, each four points (a start, two
/// handles and an end).
Geom::PathVector frame_outline(Value const &points)
{
    if (points.kind != Value::Array || points.list.empty() || points.list.size() % 8 || points.list.size() > 8 * 8192) {
        throw Bad("invalid text frame outline");
    }
    Geom::PathVector out;
    std::optional<Geom::Path> path;
    auto finish = [&] {
        if (!path) return;
        if (Geom::distance(path->initialPoint(), path->finalPoint()) < 1e-6) path->close(true);
        out.push_back(std::move(*path));
        path.reset();
    };
    for (std::size_t i = 0; i < points.list.size(); i += 8) {
        Geom::Point p[4];
        for (int k = 0; k < 4; ++k) {
            p[k] = {points.list[i + 2 * k].num(), points.list[i + 2 * k + 1].num()};
            if (std::abs(p[k].x()) > 1e9 || std::abs(p[k].y()) > 1e9) throw Bad("invalid text frame outline");
        }
        if (!path || Geom::distance(path->finalPoint(), p[0]) > 1e-6) {
            finish();
            path = Geom::Path(p[0]);
        }
        if (p[1] == p[0] && p[2] == p[3]) {
            path->appendNew<Geom::LineSegment>(p[3]);
        } else {
            path->appendNew<Geom::CubicBezier>(p[1], p[2], p[3]);
        }
    }
    finish();
    return out;
}

PointText story(Value const &s, Value const &frames, Value const &defaults,
                std::vector<std::string> const &fonts, Geom::Point const &center)
{
    auto const &text = s.get("0").get("0").str();
    if (text.size() > MAX_STORY || !utf8_with_nuls(text)) throw Bad("invalid or oversized story text");
    auto const &frame_refs = s.get("1").get("0");
    if (frame_refs.list.size() != 1) throw Bad("linked text frames aren't supported yet");
    auto const &frame = frames.at(frame_refs.at(0).get("0").index()).get("0");
    auto const &carries = frame.get("2");
    if (carries.kind != Value::Dictionary) throw Bad("text frame has unsupported geometry or options");
    // What a frame carries says its kind (0): nothing for point type, 1 for area type. Area
    // type then has an outline (the frame's 1) where point type has its point (0), and
    // carries its gutters (7, 8) and its first-baseline rule (10): the lines are placed as
    // Illustrator laid them out, so neither is needed to draw them.
    bool const area = carries.dict.count("0") && carries.get("0").num() == 1;
    if (carries.dict.count("0") && !area) throw Bad("type on a path isn't supported yet");
    for (auto const &[k, value] : frame.dict) {
        if (k != "2" && k != "97" && k != (area ? "1" : "0")) throw Bad("text frame has unsupported geometry or options");
    }
    for (auto const &[k, value] : carries.dict) {
        if (k == "2" || (area && (k == "0" || k == "7" || k == "8" || k == "10"))) continue;
        throw Bad(area ? "area type has options that aren't supported yet" : "area/path text isn't supported yet");
    }
    auto matrix = carries.dict.count("2") ? carries.get("2").nums(6) : std::vector<double>{1, 0, 0, 1, 0, 0};
    auto const det = matrix[0] * matrix[3] - matrix[1] * matrix[2];
    if (!std::isfinite(det) || std::abs(det) < 1e-9 ||
        std::any_of(matrix.begin(), matrix.end(), [](double x) { return std::abs(x) > 1e9; })) throw Bad("invalid text frame matrix");
    std::vector<Value const *> anchors;
    if (s.get("1").dict.count("2")) find_nodes(s.get("1").get("2"), "F", anchors);
    PointText out;
    out.area = area;
    out.to_art = Geom::Affine(matrix[0], -matrix[1], matrix[2], -matrix[3],
                              matrix[4] - 8191.5 + center.x(), 8191.5 + center.y() - matrix[5]);
    if (area) out.frame = frame_outline(frame.get("1").get("0"));
    // A frame nothing was typed in has no layout: there is nothing of it to draw.
    if (anchors.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c == '\r' || c == '\n' || c == 3 || c == 0; })) {
        out.empty = true;
        return out;
    }
    if (text.empty()) throw Bad("invalid or oversized story text");
    if (anchors.size() != 1) throw Bad("text has an unsupported layout frame");
    auto const anchor = anchors[0]->get("0").get("0").nums(2);
    std::vector<Value const *> lines;
    std::vector<Laid> laid;
    if (area) {
        laid_lines(*anchors[0], Geom::Point(anchor[0], anchor[1]), 0, laid);
        for (auto const &l : laid) lines.push_back(l.line);
    } else {
        find_nodes(*anchors[0], "L", lines);
    }
    if (lines.empty() || lines.size() > 65536) throw Bad("text has no supported lines");
    struct Paragraph { std::size_t end; unsigned alignment; };
    std::vector<Paragraph> paragraphs;
    std::size_t paragraph_units = 0;
    if (s.get("0").dict.count("5")) {
        for (auto const &paragraph : s.get("0").get("5").get("0").list) {
            auto const &p = paragraph.get("0").get("0").get("5");
            auto const align = p.dict.count("0") ? p.get("0").index() : 0;
            if (align > 2) throw Bad("justified paragraph alignment isn't supported yet");
            auto const count = paragraph.get("1").index();
            if (!count || count > MAX_STORY * 2 - paragraph_units) throw Bad("invalid paragraph run length");
            paragraph_units += count;
            paragraphs.push_back({paragraph_units, unsigned(align)});
        }
        if (paragraphs.empty()) throw Bad("text has no paragraph runs");
    }
    // Area type: how many characters Illustrator put in each line, their ends included.
    std::vector<std::size_t> line_units;
    auto segment_of = [](Value const &line) -> Value const & {
        auto const &segments = line.get("6");
        if (std::count_if(segments.list.begin(), segments.list.end(), [](Value const &v) { return v.node("S"); }) != 1)
            throw Bad("text line has multiple layout segments not supported yet");
        return *std::find_if(segments.list.begin(), segments.list.end(), [](Value const &v) { return v.node("S"); });
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto const *line = lines[i];
        auto const &found = segment_of(*line);
        if (area) {
            out.lines.push_back({laid[i].at + offset_of(found), {}});
            line_units.push_back(found.get("15").get("0").index());
            continue;
        }
        auto offset = line->dict.count("0") ? line->get("0").get("0").nums(2) : std::vector<double>{0, 0};
        auto segment = found.dict.count("0") ? found.get("0").get("0").nums(2) : std::vector<double>{0, 0};
        out.lines.push_back({Geom::Point(anchor[0] + offset[0] + segment[0], anchor[1] + offset[1]), {}});
    }
    struct Run { std::size_t end; TextStyle style; };
    std::vector<Run> runs;
    std::size_t units = 0;
    for (auto const &run : s.get("0").get("6").get("0").list) {
        auto count = run.get("1").index();
        if (!count || count > MAX_STORY * 2 - units) throw Bad("invalid text run length");
        units += count;
        runs.push_back({units, style(run.get("0").get("0").get("6"), defaults, fonts)});
    }
    if (runs.empty()) throw Bad("text has no character runs");
    auto const hscale = runs[0].style.horizontal_scale, vscale = runs[0].style.vertical_scale;
    for (auto const &r : runs) {
        if (r.style.horizontal_scale != hscale || r.style.vertical_scale != vscale)
            throw Bad("text has different character scales within one story, not supported yet");
    }
    // A common character scale is an editable affine on the story. Compensate
    // explicit baseline positions so glyphs scale without moving their anchors.
    out.to_art = Geom::Scale(hscale, vscale) * out.to_art;
    for (auto &l : out.lines) l.origin *= Geom::Scale(1 / hscale, 1 / vscale);
    out.frame *= Geom::Scale(1 / hscale, 1 / vscale);
    if (!paragraphs.empty() && paragraph_units != units) throw Bad("paragraph runs don't cover the story");
    if (area) {
        std::size_t laid_units = 0;
        for (auto const n : line_units) laid_units += n;
        // Fewer is a frame too small for its text: the rest isn't laid out.
        if (laid_units > units) throw Bad("text runs or line layout don't cover the story");
    }
    std::size_t run = 0, line = 0, at_units = 0, paragraph = 0, line_end = area ? line_units[0] : 0;
    for (auto const *at = text.data(); at < text.data() + text.size();) {
        auto const cp = g_utf8_get_char(at); auto const *next = g_utf8_next_char(at);
        auto const count = cp > 0xffff ? 2u : 1u;
        while (paragraph < paragraphs.size() && at_units == paragraphs[paragraph].end) ++paragraph;
        if (!paragraphs.empty() && (paragraph == paragraphs.size() || at_units + count > paragraphs[paragraph].end))
            throw Bad("paragraph run cuts a Unicode character or ends early");
        while (run < runs.size() && at_units == runs[run].end) ++run;
        if (run == runs.size() || at_units + count > runs[run].end) throw Bad("text run cuts a Unicode character or ends early");
        if (area) {
            // A line ends where its count does: where Illustrator wrapped it, typed or not.
            while (line < out.lines.size() && at_units == line_end) {
                if (++line < out.lines.size()) line_end += line_units[line];
            }
            if (line == out.lines.size()) {
                // What the frame is too small to show: kept, not drawn.
                if (cp == '\r' || cp == 3 || cp == '\n') out.overflow.push_back('\n'); else if (cp) out.overflow.append(at, next - at);
                at_units += count; at = next;
                continue;
            }
            if (at_units + count > line_end) throw Bad("text line cuts a Unicode character or ends early");
        }
        if (cp == 0) {
            // A character that sets nothing; it only counts.
        } else if (cp == '\r' || cp == 3 || cp == '\n') {
            if (!area) ++line;
        } else {
            if (line >= out.lines.size() || cp < 32) throw Bad("text line count or control character isn't supported");
            auto &pieces = out.lines[line].runs;
            if (pieces.empty() && !paragraphs.empty()) out.lines[line].alignment = paragraphs[paragraph].alignment;
            // Keeping source run boundaries preserves editable character styles.
            if (pieces.empty() || at_units == (run ? runs[run - 1].end : 0)) pieces.push_back({{}, runs[run].style});
            pieces.back().text.append(at, next - at);
        }
        at_units += count; at = next;
    }
    if (at_units != units) throw Bad("text runs or line layout don't cover the story");
    if (!area && (line > out.lines.size() || (line + 1 != out.lines.size() && line != out.lines.size()))) {
        throw Bad("text runs or line layout don't cover the story");
    }
    // Paragraphs with nothing in them past the frame's end hold nothing to keep.
    if (out.overflow.find_first_not_of('\n') == std::string::npos) out.overflow.clear();

    // Every line starts where its glyphs do. Centred or right-aligned lines are anchored at
    // their middle or right end instead, to stay there when edited. A text object has one
    // anchor for all its lines (they are one paragraph to what sets them, which takes its
    // alignment from the first), so that is done only when every line with characters has
    // the same alignment and each says where its anchor is: point type by its segment's
    // offset from the story's anchor, area type by the middle or right end of its line's
    // box, when its glyphs' own extent agrees that that is where it sits (a line that keeps
    // a space at its end doesn't).
    std::optional<unsigned> common;
    bool together = true;
    for (auto const &l : out.lines) {
        if (l.runs.empty()) continue;
        together = together && (!common || *common == l.alignment);
        common = l.alignment;
    }
    std::vector<double> moves(out.lines.size(), 0.0); // From each line's start to its anchor.
    together = together && common && *common != 0;
    for (std::size_t i = 0; together && i < out.lines.size(); ++i) {
        if (out.lines[i].runs.empty()) continue;
        // A space at a line's end is part of what Illustrator centres, and not of what is
        // centred here: such a line is only exact from its start.
        auto const &last = out.lines[i].runs.back().text;
        if (!last.empty() && g_unichar_isspace(g_utf8_get_char(g_utf8_prev_char(last.data() + last.size())))) {
            together = false;
            break;
        }
        auto const &found = segment_of(*lines[i]);
        if (!area) {
            if (!found.dict.count("0")) throw Bad("aligned text has no explicit layout offset");
            moves[i] = -offset_of(found).x();
            continue;
        }
        if (!lines[i]->dict.count("1") || !found.dict.count("6")) {
            together = false;
            break;
        }
        auto const box = lines[i]->get("1").nums(4);
        double const start = offset_of(found).x();
        std::optional<double> width;
        for (auto const &g : found.get("6").list) {
            if (g.node("G") && g.dict.count("1")) {
                width = std::max(width.value_or(0.0), offset_of(g).x() + g.get("1").nums(4)[2]);
            }
        }
        double const target = *common == 2 ? (box[0] + box[2]) / 2 : box[2];
        together = width && std::abs((*common == 2 ? start + *width / 2 : start + *width) - target) <= 0.02;
        moves[i] = target - start;
    }
    for (std::size_t i = 0; i < out.lines.size(); ++i) {
        out.lines[i].alignment = together ? *common : 0u;
        if (together) out.lines[i].origin.x() += moves[i] / hscale;
    }
    return out;
}
} // namespace

std::shared_ptr<TextDocument const> read_text_document(std::string_view records, std::optional<Geom::Point> center)
{
    auto out = std::make_shared<TextDocument>();
    try {
        if (!center) throw Bad("text has no template centre for placement");
        if (!std::isfinite(center->x()) || !std::isfinite(center->y()) ||
            std::abs(center->x()) > 1e9 || std::abs(center->y()) > 1e9) throw Bad("invalid text template centre");
        auto const section = text_section(records);
        // The stories of one text document, each read on its own.
        auto read = [&](std::string const &data, std::vector<std::optional<PointText>> &stories_out,
                        std::vector<std::string> &errors) {
            auto doc = Reader(data).root();
            std::vector<std::string> fonts;
            for (auto const &font : doc.get("0").get("1").get("0").list) fonts.push_back(font.get("0").get("0").get("0").str());
            auto const &main = doc.get("1");
            auto const &stories = main.get("1");
            if (stories.kind != Value::Array || stories.list.size() > 65536) throw Bad("invalid or oversized story list");
            for (auto const &s : stories.list) {
                try {
                    stories_out.push_back(story(s, doc.get("0").get("8").get("0"), main.get("2"), fonts, *center));
                    errors.emplace_back();
                } catch (Bad const &e) { stories_out.emplace_back(); errors.push_back(e.what()); }
            }
        };
        std::size_t at = 0;
        read(unpack(section, at), out->stories, out->errors);
        // After the document of what was typed comes, when appearances drew type, the document
        // of that type. Without it, or when it can't be read, such type has no stories.
        auto const drawn = section.find("/AI11UndoFreeTextDocument", at);
        if (drawn != section.npos) {
            try {
                at = drawn;
                read(unpack(section, at), out->drawn, out->drawn_errors);
            } catch (Bad const &) {
                out->drawn.clear();
                out->drawn_errors.clear();
            }
        }
    } catch (Bad const &e) { out->error = e.what(); }
    return out;
}

bool same_drawing(PointText const &a, PointText const &b)
{
    if (a.empty != b.empty || a.lines.size() != b.lines.size()) return false;
    auto same_style = [](TextStyle const &s, TextStyle const &t) {
        return s.font == t.font && s.size == t.size && s.fill == t.fill && s.stroke == t.stroke &&
               s.fill_opacity == t.fill_opacity && s.stroke_opacity == t.stroke_opacity &&
               (!s.stroke || s.stroke_width == t.stroke_width) && s.tracking == t.tracking &&
               s.horizontal_scale == t.horizontal_scale && s.vertical_scale == t.vertical_scale &&
               s.baseline_shift == t.baseline_shift && s.kerning == t.kerning && s.ligatures == t.ligatures &&
               s.contextual == t.contextual;
    };
    // Scaled and turned alike, and (below) each line in the same place on the art.
    if (!Geom::are_near(a.to_art.withoutTranslation(), b.to_art.withoutTranslation(), 1e-6)) return false;
    for (std::size_t i = 0; i < a.lines.size(); ++i) {
        auto const &la = a.lines[i], &lb = b.lines[i];
        if (la.runs.size() != lb.runs.size() || la.alignment != lb.alignment) return false;
        if (!la.runs.empty() && !Geom::are_near(la.origin * a.to_art, lb.origin * b.to_art, 0.01)) return false;
        for (std::size_t r = 0; r < la.runs.size(); ++r) {
            if (la.runs[r].text != lb.runs[r].text || !same_style(la.runs[r].style, lb.runs[r].style)) return false;
        }
    }
    return true;
}
} // namespace Inkscape::Extension::Internal::AiNative
