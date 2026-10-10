// SPDX-License-Identifier: GPL-2.0-or-later
// Numeric story keys adapted from VectorCraft ate.rs (4cf912f), MIT;
// Copyright (c) 2026 ArtCraft Team and the VectorCraft contributors.
#include "ai-native-text.h"
#include "ai-native-lexer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <glib.h>

namespace Inkscape::Extension::Internal::AiNative {
namespace {
constexpr std::size_t MAX_BYTES = 64u << 20, MAX_STORY = 1u << 20, MAX_VALUES = 250000;

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

std::string unpack(std::string_view records)
{
    auto start = records.find("%AI11_BeginTextDocument");
    if (start == records.npos) throw Bad("the text document is missing");
    auto end = records.find("%AI11_EndTextDocument", start);
    if (end == records.npos) throw Bad("the text document doesn't end");
    auto section = records.substr(start, end - start);
    auto from = section.find("/ASCII85Decode");
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
    return out;
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
    if (number("9", 0) != 0) throw Bad("text uses baseline shifts not supported yet");
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

PointText story(Value const &s, Value const &frames, Value const &defaults,
                std::vector<std::string> const &fonts, Geom::Point const &center)
{
    auto const &text = s.get("0").get("0").str();
    if (text.empty() || text.size() > MAX_STORY || text.find('\0') != text.npos ||
        !g_utf8_validate(text.data(), text.size(), nullptr)) throw Bad("invalid or oversized story text");
    auto const &frame_refs = s.get("1").get("0");
    if (frame_refs.list.size() != 1) throw Bad("linked text frames aren't supported yet");
    auto const &frame = frames.at(frame_refs.at(0).get("0").index()).get("0");
    for (auto const &[k, value] : frame.dict) if (k != "0" && k != "2" && k != "97")
        throw Bad("text frame has unsupported geometry or options");
    auto const &carries = frame.get("2");
    if (carries.kind != Value::Dictionary || carries.dict.size() > 1 ||
        (!carries.dict.empty() && !carries.dict.count("2"))) throw Bad("area/path text isn't supported yet");
    auto matrix = carries.dict.empty() ? std::vector<double>{1, 0, 0, 1, 0, 0} : carries.get("2").nums(6);
    auto const det = matrix[0] * matrix[3] - matrix[1] * matrix[2];
    if (!std::isfinite(det) || std::abs(det) < 1e-9 ||
        std::any_of(matrix.begin(), matrix.end(), [](double x) { return std::abs(x) > 1e9; })) throw Bad("invalid text frame matrix");
    std::vector<Value const *> anchors;
    find_nodes(s.get("1").get("2"), "F", anchors);
    if (anchors.size() != 1) throw Bad("text has an unsupported layout frame");
    auto const anchor = anchors[0]->get("0").get("0").nums(2);
    std::vector<Value const *> lines;
    find_nodes(*anchors[0], "L", lines);
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
    PointText out;
    out.to_art = Geom::Affine(matrix[0], -matrix[1], matrix[2], -matrix[3],
                              matrix[4] - 8191.5 + center.x(), 8191.5 + center.y() - matrix[5]);
    for (auto const *line : lines) {
        auto offset = line->dict.count("0") ? line->get("0").get("0").nums(2) : std::vector<double>{0, 0};
        auto const &segments = line->get("6");
        if (std::count_if(segments.list.begin(), segments.list.end(), [](Value const &v) { return v.node("S"); }) != 1)
            throw Bad("text line has multiple layout segments not supported yet");
        auto it = std::find_if(segments.list.begin(), segments.list.end(), [](Value const &v) { return v.node("S"); });
        if (it == segments.list.end()) throw Bad("text line has no supported segment");
        auto segment = it->dict.count("0") ? it->get("0").get("0").nums(2) : std::vector<double>{0, 0};
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
    if (!paragraphs.empty() && paragraph_units != units) throw Bad("paragraph runs don't cover the story");
    std::size_t run = 0, line = 0, at_units = 0, paragraph = 0;
    for (auto const *at = text.data(); at < text.data() + text.size();) {
        auto const cp = g_utf8_get_char(at); auto const *next = g_utf8_next_char(at);
        auto const count = cp > 0xffff ? 2u : 1u;
        while (paragraph < paragraphs.size() && at_units == paragraphs[paragraph].end) ++paragraph;
        if (!paragraphs.empty() && (paragraph == paragraphs.size() || at_units + count > paragraphs[paragraph].end))
            throw Bad("paragraph run cuts a Unicode character or ends early");
        while (run < runs.size() && at_units == runs[run].end) ++run;
        if (run == runs.size() || at_units + count > runs[run].end) throw Bad("text run cuts a Unicode character or ends early");
        if (cp == '\r' || cp == 3 || cp == '\n') {
            ++line;
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
    if (at_units != units || line > out.lines.size() ||
        (line + 1 != out.lines.size() && line != out.lines.size())) throw Bad("text runs or line layout don't cover the story");
    for (std::size_t i = 0; i < out.lines.size(); ++i) {
        auto &l = out.lines[i];
        if (!l.alignment) continue;
        auto const &segments = lines[i]->get("6").list;
        auto const it = std::find_if(segments.begin(), segments.end(), [](Value const &v) { return v.node("S"); });
        if (!it->dict.count("0")) throw Bad("aligned text has no explicit layout offset");
        auto const offset = it->get("0").get("0").nums(2);
        l.origin.x() -= offset[0] / hscale;
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
        auto data = unpack(records);
        auto doc = Reader(data).root();
        std::vector<std::string> fonts;
        for (auto const &font : doc.get("0").get("1").get("0").list) fonts.push_back(font.get("0").get("0").get("0").str());
        auto const &main = doc.get("1");
        auto const &stories = main.get("1");
        if (stories.kind != Value::Array || stories.list.size() > 65536) throw Bad("invalid or oversized story list");
        for (auto const &s : stories.list) {
            try {
                out->stories.push_back(story(s, doc.get("0").get("8").get("0"), main.get("2"), fonts, *center));
                out->errors.emplace_back();
            } catch (Bad const &e) { out->stories.emplace_back(); out->errors.push_back(e.what()); }
        }
    } catch (Bad const &e) { out->error = e.what(); }
    return out;
}
} // namespace Inkscape::Extension::Internal::AiNative
