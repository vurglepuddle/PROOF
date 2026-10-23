// SPDX-License-Identifier: GPL-2.0-or-later
// Numeric story keys adapted from VectorCraft ate.rs (4cf912f), MIT;
// Copyright (c) 2026 ArtCraft Team and the VectorCraft contributors.
#include "ai-native-text.h"
#include "ai-native-lexer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
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
/// The middle of Illustrator's canvas, on which the text document places its stories.
constexpr double CANVAS_CENTRE = 8191.5;

std::string_view text_section(std::string_view records)
{
    auto start = records.find("%AI11_BeginTextDocument");
    if (start == records.npos) throw Bad("the text document is missing");
    auto end = records.find("%AI11_EndTextDocument", start);
    if (end == records.npos) throw Bad("the text document doesn't end");
    return records.substr(start, end - start);
}

/**
 * Where the art's 0,0 lies on the canvas the stories are placed on, when the text section
 * says: `x y /RulerOrigin ,` after the document of what was typed. The canvas is 16383
 * points square, y down.
 */
std::optional<Geom::Point> ruler_origin(std::string_view section)
{
    constexpr std::string_view name = "/RulerOrigin";
    for (auto at = section.find(name); at != section.npos; at = section.find(name, at + name.size())) {
        // The two numbers before the name, on its line. The packed documents' lines hold any
        // characters: one that isn't two numbers and the name isn't this entry.
        auto start = section.find_last_of("\r\n", at);
        start = start == section.npos ? 0 : start + 1;
        auto line = section.substr(start, at - start);
        if (line.substr(0, 2) == "%_") line.remove_prefix(2);
        double values[2];
        int count = 0;
        bool only_numbers = true;
        while (!line.empty() && only_numbers) {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            if (line.empty()) break;
            auto const end = line.find_first_of(" \t");
            auto const word = line.substr(0, end);
            double value = 0;
            auto const [stop, ec] = std::from_chars(word.data(), word.data() + word.size(), value);
            if (ec != std::errc() || stop != word.data() + word.size() || !std::isfinite(value) || count == 2) {
                only_numbers = false;
            } else {
                values[count++] = value;
            }
            line.remove_prefix(end == line.npos ? line.size() : end);
        }
        if (only_numbers && count == 2 && std::abs(values[0]) < 1e9 && std::abs(values[1]) < 1e9) {
            return Geom::Point(values[0], values[1]);
        }
    }
    return {};
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

/// A font as the text document lists it.
struct Font
{
    std::string name;
    std::vector<double> design; ///< An instance of a variable font: its axis values.
};

/// A font's entry in the text document's list: its PostScript name (0) and, for an instance
/// of a variable font, where it is on each of the font's axes (4), in 16.16 fixed point.
Font font_entry(Value const &entry)
{
    Font out;
    out.name = entry.get("0").str();
    auto const design = entry.dict.find("4");
    if (design != entry.dict.end() && design->second.kind == Value::Array && design->second.list.size() <= 64) {
        for (auto const &v : design->second.list) {
            // Values that aren't numbers leave the name to say which instance it is.
            if (v.kind != Value::Number || !std::isfinite(v.number) || std::abs(v.number) > 2147483648.0) return {out.name, {}};
            out.design.push_back(v.number / 65536.0);
        }
    }
    return out;
}

/// What a paint a character names is, when it isn't a plain colour.
struct NamedPaint
{
    bool is_pattern = false; ///< A pattern; else a gradient.
    /// For a pattern: which, and where. Nothing when it is placed in a way that isn't read.
    std::optional<TextPattern> pattern;
};

/// The pattern a `(name) px py sx sy angle rf r k ka [a b c d tx ty] p` line places, when
/// it is placed the way Illustrator 9 and later place one: by its matrix alone.
std::optional<TextPattern> placed_pattern(std::string_view line)
{
    if (line.empty() || line.front() != '(') return {};
    TextPattern out;
    std::size_t at = 1;
    for (; at < line.size() && line[at] != ')'; ++at) {
        if (line[at] == '\\') {
            // A parenthesis or a backslash in the name; anything else isn't taken.
            if (at + 1 >= line.size() || (line[at + 1] != '(' && line[at + 1] != ')' && line[at + 1] != '\\')) return {};
            ++at;
        }
        out.name.push_back(line[at]);
    }
    if (at >= line.size() || out.name.empty() || out.name.size() > 256) return {};
    std::vector<double> numbers;
    bool in_matrix = false, had_matrix = false;
    std::vector<double> matrix;
    for (++at; at < line.size();) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
        if (at >= line.size()) break;
        if (line[at] == '[' || line[at] == ']') {
            if ((line[at] == '[') == in_matrix || (line[at] == '[' && had_matrix)) return {};
            in_matrix = line[at] == '[';
            had_matrix = true;
            ++at;
            continue;
        }
        auto const end = std::min(line.find_first_of(" \t[]", at), line.size());
        auto const word = std::string(line.substr(at, end - at));
        at = end;
        if (word == "p" || word == "P") break;
        char *stop = nullptr;
        double const value = g_ascii_strtod(word.c_str(), &stop);
        if (stop != word.c_str() + word.size() || !std::isfinite(value)) return {};
        (in_matrix ? matrix : numbers).push_back(value);
    }
    constexpr double AT_REST[] = {0, 0, 1, 1, 0, 0, 0, 0, 0};
    if (in_matrix || matrix.size() != 6 || numbers.size() != 9) return {};
    for (std::size_t i = 0; i < 9; ++i) {
        if (std::abs(numbers[i] - AT_REST[i]) > 1e-9) return {};
    }
    out.matrix = Geom::Affine(matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]);
    if (std::abs(out.matrix.det()) < 1e-12 || std::any_of(matrix.begin(), matrix.end(), [](double v) { return std::abs(v) > 1e9; })) return {};
    return out;
}

/**
 * The paints the records define by name that aren't a plain colour.
 *
 * A character's paint can name a style of the document's (`/5 (Anon …)` beside its
 * colour). The styles are among the records, each a `/KnownStyle :` with its `/Name` and,
 * in a `/SimpleStyle`, the operators that set its paint: `Bg` for a gradient, `p` or `P`
 * for a pattern. For those two the colour the text document keeps beside the name is
 * black, whatever the paint looks like. A pattern's line also says which pattern, and
 * where its tile lies.
 */
std::map<std::string, NamedPaint> named_paints(std::string_view records)
{
    std::map<std::string, NamedPaint> out;
    constexpr std::string_view BEGIN = "/KnownStyle :";
    for (auto at = records.find(BEGIN); at != records.npos && out.size() < 65536; at = records.find(BEGIN, at)) {
        at += BEGIN.size();
        std::string name;
        std::optional<NamedPaint> kind;
        bool patterned = false;
        // Line by line to the style's end; a style is a few lines long.
        for (unsigned lines = 0; lines < 4096 && at < records.size(); ++lines) {
            auto const end = std::min(records.find_first_of("\r\n", at), records.size());
            auto line = records.substr(at, end - at);
            at = end + 1;
            if (line.substr(0, 2) == "%_") line.remove_prefix(2);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
            constexpr std::string_view NAME = ") /Name ,";
            if (line == "/Def ;" || line.substr(0, BEGIN.size()) == BEGIN) break;
            if (line.size() > NAME.size() + 1 && line.front() == '(' && line.substr(line.size() - NAME.size()) == NAME) {
                name = std::string(line.substr(1, line.size() - NAME.size() - 1));
            } else if (line.size() > 3 && line.substr(line.size() - 3) == " Bg") {
                if (!kind) kind.emplace();
            } else if (line.size() > 2 && (line.substr(line.size() - 2) == " p" || line.substr(line.size() - 2) == " P")) {
                // A style has one paint: a second pattern in it is nothing this reads.
                kind.emplace();
                kind->is_pattern = true;
                if (!patterned) kind->pattern = placed_pattern(line);
                patterned = true;
            }
        }
        if (kind && !name.empty() && name.size() <= 256) out[name] = *kind;
    }
    return out;
}

/// What a text document says for all its stories.
struct Setup
{
    std::vector<Font> fonts;
    /// Superscript and subscript: their size and their distance from the baseline (the
    /// text document's settings 3 to 6). Small capitals a font doesn't have: the size of
    /// the capitals that stand for them (7). Each a share of the type's size; Illustrator's
    /// own when the text document says nothing.
    TypeSettings type;
    /// For the document of type that appearances drew: what the document of typed text
    /// says, which is what the whole document is set by.
    TypeSettings const *document = nullptr;
    /// The named paints of the records that aren't a plain colour (named_paints).
    std::map<std::string, NamedPaint> const *paints = nullptr;
    Value const *paragraph_styles = nullptr;   ///< Resource 6 of this text document.
    Value const *paragraph_defaults = nullptr; ///< Main dictionary 3.
};

using StyleLayers = std::vector<Value const *>;

/// Most specific first. Keep references to the bounded parsed document instead of
/// copying dictionaries for every run. A declaration is inline, or a resource index;
/// key 6 links to the parent style and key 5 holds its property dictionary.
StyleLayers paragraph_layers(Value const *declaration, Setup const &setup,
                             bool &expanded, std::size_t &budget)
{
    StyleLayers layers;
    std::vector<std::size_t> seen;
    while (declaration) {
        if (layers.size() >= 64) throw Bad("paragraph style inheritance is too deep");
        if (declaration->kind == Value::Number) {
            auto const index = declaration->index();
            if (std::find(seen.begin(), seen.end(), index) != seen.end()) throw Bad("paragraph style inheritance is cyclic");
            seen.push_back(index);
            if (!setup.paragraph_styles) throw Bad("paragraph style resource is missing");
            declaration = &setup.paragraph_styles->get("0").at(index).get("0");
            expanded = true;
        }
        auto const &properties = declaration->get("5");
        if (properties.kind != Value::Dictionary) throw Bad("invalid paragraph style properties");
        layers.push_back(&properties);
        auto const parent = declaration->dict.find("6");
        declaration = parent == declaration->dict.end() ? nullptr : &parent->second;
        if (declaration && declaration->kind != Value::Number) throw Bad("invalid paragraph parent style reference");
    }
    if (setup.paragraph_defaults) {
        if (setup.paragraph_defaults->kind != Value::Dictionary) throw Bad("invalid default paragraph style");
        layers.push_back(setup.paragraph_defaults);
    }
    if (layers.size() > budget) throw Bad("paragraph style resolution exceeds its limits");
    budget -= layers.size();
    return layers;
}

Value const *layer_value(StyleLayers const &layers, char const *key)
{
    for (auto const *layer : layers) {
        auto const it = layer->dict.find(key);
        if (it != layer->dict.end()) return &it->second;
    }
    return nullptr;
}

TextStyle style(Value const &own, Value const &defaults, Setup const &setup,
                StyleLayers const &paragraph = {})
{
    if (own.kind != Value::Dictionary || defaults.kind != Value::Dictionary) throw Bad("invalid character style dictionary");
    auto const &fonts = setup.fonts;
    auto find = [&](char const *k) -> Value const * {
        if (auto const it = own.dict.find(k); it != own.dict.end()) return &it->second;
        if (auto const inherited = layer_value(paragraph, k)) return inherited;
        auto const it = defaults.dict.find(k);
        return it == defaults.dict.end() ? nullptr : &it->second;
    };
    auto pick = [&](char const *k) -> Value const & {
        auto const value = find(k);
        return value ? *value : defaults.get(k);
    };
    auto number = [&](char const *k, double fallback) {
        if (!find(k)) return fallback;
        return pick(k).num();
    };
    TextStyle out;
    auto const font = pick("0").index();
    if (font >= fonts.size() || fonts[font].name.empty()) throw Bad("text has no known font name");
    out.font = fonts[font].name; out.design = fonts[font].design; out.size = pick("1").num();
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
        if (!find(k)) return true;
        auto const &v = pick(k);
        return v.kind != Value::Boolean || v.boolean;
    };
    out.ligatures = on("18");
    out.contextual = on("20");
    out.tracking = number("8", 0);
    if (std::abs(out.tracking) > 1e6) throw Bad("invalid text tracking");
    // Capitals (12): 0 as typed, 1 small caps, 2 all caps. The story keeps what was typed.
    auto const caps = number("12", 0);
    if (caps != 0 && caps != 1 && caps != 2) throw Bad("text has an unsupported character style feature");
    out.all_caps = caps == 2;
    out.small_caps = caps == 1;
    // A document has one size for small capitals, and one each for the two positions.
    auto const &whole = setup.document ? *setup.document : setup.type;
    if (out.small_caps && setup.type.small_caps_size != whole.small_caps_size) {
        throw Bad("type an appearance drew has small caps at a size that isn't the document's");
    }
    // Strikethrough (15) and underline (16): 0 none, 1 drawn. An underline has other
    // places in vertical type (2), which nothing here sets.
    auto const strikethrough = number("15", 0), underline = number("16", 0);
    if ((strikethrough != 0 && strikethrough != 1) || (underline != 0 && underline != 1)) {
        throw Bad("text has an unsupported character style feature");
    }
    out.strikethrough = strikethrough == 1;
    out.underline = underline == 1;
    // Position (13): 0 on the line, 1 superscript, 2 subscript.
    auto const position = number("13", 0);
    if (position != 0 && position != 1 && position != 2) throw Bad("text has an unsupported character style feature");
    out.position = int(position);
    if (out.position == 1) {
        if (setup.type.superscript_size != whole.superscript_size || setup.type.superscript_shift != whole.superscript_shift) {
            throw Bad("type an appearance drew has superscript at a size or place that isn't the document's");
        }
        out.position_size = setup.type.superscript_size;
    } else if (out.position == 2) {
        if (setup.type.subscript_size != whole.subscript_size || setup.type.subscript_shift != whole.subscript_shift) {
            throw Bad("type an appearance drew has subscript at a size or place that isn't the document's");
        }
        out.position_size = setup.type.subscript_size;
    }
    auto paint = [&](char const *k, double &opacity) -> Color {
        // A paint that names a style of the document's is that style's. Where that is a
        // plain colour, it is the colour beside the name. A gradient isn't drawn on type:
        // Illustrator sets the characters black, the colour that is kept for one. A
        // pattern is drawn, and isn't read here.
        if (auto const name = pick(k).dict.find("5"); name != pick(k).dict.end() && name->second.kind == Value::String && setup.paints) {
            if (auto const named = setup.paints->find(name->second.string); named != setup.paints->end()) {
                if (!named->second.is_pattern) {
                    out.gradient = true;
                } else if (!named->second.pattern) {
                    throw Bad("type painted with a pattern placed in a way that isn't supported");
                } else {
                    (std::string_view(k) == "53" ? out.fill_pattern : out.stroke_pattern) = named->second.pattern;
                }
            }
        }
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
    for (auto const *k : {"2", "3", "10", "14", "17", "19", "21", "22", "23", "24", "25", "26", "28", "29", "30", "31", "32", "33", "34"}) {
        if (find(k)) {
            auto const &v = pick(k);
            if ((v.kind == Value::Boolean && v.boolean) || (v.kind == Value::Number && v.num() != 0))
                throw Bad("text has an unsupported character style feature");
        }
    }
    return out;
}

/// Whether the character is of a script read from right to left.
bool reads_from_the_right(gunichar c)
{
    return (c >= 0x590 && c <= 0x8ff) || (c >= 0xfb1d && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfeff) ||
           (c >= 0x10800 && c <= 0x10fff) || (c >= 0x1e800 && c <= 0x1efff);
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
Geom::PathVector frame_outline(Value const &points, bool keep_parameters = false)
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
        if (!keep_parameters && p[1] == p[0] && p[2] == p[3]) {
            path->appendNew<Geom::LineSegment>(p[3]);
        } else {
            path->appendNew<Geom::CubicBezier>(p[1], p[2], p[3]);
        }
    }
    finish();
    return out;
}

PointText story(Value const &s, Value const &frames, Value const &defaults,
                Setup const &setup, Geom::Point const &center)
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
    bool const on_path = carries.dict.count("0") && carries.get("0").num() == 2;
    if (carries.dict.count("0") && !area && !on_path) throw Bad("unsupported text frame kind");
    for (auto const &[k, value] : frame.dict) {
        if (k != "2" && k != "97" && k != (area || on_path ? "1" : "0")) throw Bad("text frame has unsupported geometry or options");
    }
    for (auto const &[k, value] : carries.dict) {
        if (k == "2" || (area && (k == "0" || k == "7" || k == "8" || k == "10")) ||
            (on_path && (k == "0" || k == "6" || k == "11"))) continue;
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
                              matrix[4] - CANVAS_CENTRE + center.x(), CANVAS_CENTRE + center.y() - matrix[5]);
    if (area) {
        out.frame = frame_outline(frame.get("1").get("0"));
        // The rule that places the first baseline, kept for laying the frame out again; the
        // lines are placed as recorded. Rules Illustrator doesn't have are left out.
        if (auto const rule = carries.dict.find("10"); rule != carries.dict.end() && rule->second.kind == Value::Dictionary) {
            auto const kind = rule->second.dict.find("0"), minimum = rule->second.dict.find("1");
            bool const known = kind != rule->second.dict.end() && kind->second.kind == Value::Number &&
                               kind->second.number >= 0 && kind->second.number <= 6 && kind->second.number == std::floor(kind->second.number);
            bool const sane = minimum == rule->second.dict.end() ||
                              (minimum->second.kind == Value::Number && std::isfinite(minimum->second.number) && std::abs(minimum->second.number) < 1e5);
            if (known && sane) {
                out.first_baseline = std::pair(unsigned(kind->second.number), minimum == rule->second.dict.end() ? 0.0 : minimum->second.number);
            }
        }
    }
    if (on_path) {
        PathText path;
        // Illustrator 29.2.1 recalculates 4/18 and drops 6/22 when supplied alone; their
        // recorded values are not an unsupported effect. Keep validated scalar options as
        // provenance. Explicit /6 false enables automatic spacing; omitting /6 or setting it
        // true does not. The visual gate still decides whether the approximation fits.
        // Actual flips, effects and non-baseline alignments remain unsupported.
        if (auto const it = carries.dict.find("11"); it != carries.dict.end()) {
            if (it->second.kind != Value::Dictionary) throw Bad("invalid path type options");
            for (auto const &[key, value] : it->second.dict) {
                bool const numeric = key == "4" || key == "18" || key == "22" || key == "1" || key == "2";
                bool const flag = key == "0" || key == "6" || key == "7";
                if (numeric) {
                    auto const n = value.num(); // Includes type and finiteness checks.
                    if ((key == "1" || key == "2") && n != 0) throw Bad("nondefault path type options aren't supported yet");
                    char buffer[64];
                    auto const converted = std::to_chars(buffer, buffer + sizeof(buffer), n);
                    if (converted.ec != std::errc{}) throw Bad("invalid path type options");
                    path.source_options += "/" + key + " " + std::string(buffer, converted.ptr) + " ";
                } else if (flag) {
                    auto const b = value.flag();
                    if (key != "6" && b) throw Bad("nondefault path type options aren't supported yet");
                    path.source_options += "/" + key + (b ? " true " : " false ");
                } else {
                    throw Bad("nondefault path type options aren't supported yet");
                }
            }
            if (!path.source_options.empty()) path.source_options.pop_back();
            auto const automatic = it->second.dict.find("6");
            path.automatic_spacing = automatic != it->second.dict.end() && !automatic->second.flag();
        }
        // Even a straight Illustrator segment retains cubic parameterisation:
        // t=0.25 is 15.625% along a cubic with handles at its endpoints.
        path.outline = frame_outline(frame.get("1").get("0"), true);
        auto bounds = carries.get("6").nums(2);
        if (path.outline.size() != 1) throw Bad("invalid or wrapped path type interval");
        auto const &geometry = path.outline[0];
        // The brackets are places on Illustrator's own segments, each a cubic: the outline closes
        // with a straight line where the last is one with its handles on its ends, timed otherwise.
        auto const &points = frame.get("1").get("0").list;
        std::vector<Geom::CubicBezier> segments;
        for (std::size_t i = 0; i < points.size(); i += 8) {
            Geom::Point p[4];
            for (int k = 0; k < 4; ++k) p[k] = {points[i + 2 * k].num(), points[i + 2 * k + 1].num()};
            segments.emplace_back(p[0], p[1], p[2], p[3]);
        }
        double const n = segments.size();
        auto length_to = [&](double at) {
            double d = 0;
            auto const whole = std::min<std::size_t>(std::size_t(std::floor(at)), segments.size());
            for (std::size_t i = 0; i < whole; ++i) d += segments[i].length(0.001);
            if (whole < segments.size() && at > whole) {
                d += std::unique_ptr<Geom::Curve>(segments[whole].portion(0, at - whole))->length(0.001);
            }
            return d;
        };
        if (geometry.closed() && std::isfinite(bounds[0]) && std::isfinite(bounds[1]) && bounds[0] != bounds[1] &&
            std::abs(bounds[1] - bounds[0]) <= n + 1e-6 && std::abs(bounds[0]) <= 2 * n && std::abs(bounds[1]) <= 2 * n) {
            bool const whole_turn = std::abs(std::abs(bounds[1] - bounds[0]) - n) < 1e-8;
            // Round a closed path both brackets are places on the loop: the end can be written past
            // the last segment (the start plus the run) or before the start (VectorCraft
            // `1abc5d40`). The type runs on from the start, round past the path's own start when
            // the end comes before it, a whole turn when they meet.
            for (auto &b : bounds) b -= std::floor(b / n) * n;
            path.start = length_to(bounds[0]);
            path.end = length_to(bounds[1]);
            if (whole_turn) path.end = path.start + length_to(n);
            else if (path.end <= path.start) path.end += length_to(n);
        } else {
            if (bounds[0] < 0 || bounds[1] <= bounds[0] || bounds[1] > n) throw Bad("invalid or wrapped path type interval");
            path.start = length_to(bounds[0]);
            path.end = length_to(bounds[1]);
        }
        if (!std::isfinite(path.start) || !std::isfinite(path.end) || path.end - path.start < 1e-6 || path.end > 1e9)
            throw Bad("invalid path type length");
        out.path = std::move(path);
    }
    // A frame nothing was typed in has no layout: there is nothing of it to draw.
    if (anchors.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c == '\r' || c == '\n' || c == 3 || c == 0; })) {
        out.empty = true;
        return out;
    }
    if (text.empty()) throw Bad("invalid or oversized story text");
    bool const unlaid_path = on_path && anchors.empty();
    if (anchors.size() != 1 && !unlaid_path) throw Bad("text has an unsupported layout frame");
    auto const anchor = on_path ? std::vector<double>{0, 0} : anchors[0]->get("0").get("0").nums(2);
    std::vector<Value const *> lines;
    std::vector<Laid> laid;
    if (area) {
        laid_lines(*anchors[0], Geom::Point(anchor[0], anchor[1]), 0, laid);
        for (auto const &l : laid) lines.push_back(l.line);
    } else if (!unlaid_path) {
        find_nodes(*anchors[0], "L", lines);
    }
    if ((!unlaid_path && lines.empty()) || lines.size() > 65536) throw Bad("text has no supported lines");
    if (unlaid_path) out.lines.push_back({Geom::Point(0, 0), {}}); // A wholly clipped story has no cached /F or /L.
    // Each run of glyphs says where it was set (8: left, top, right, bottom).
    std::vector<Value const *> glyph_runs;
    if (!unlaid_path) find_nodes(*anchors[0], "G", glyph_runs);
    for (auto const *g : glyph_runs) {
        auto const box = g->dict.find("8");
        if (box == g->dict.end() || box->second.kind != Value::Array || box->second.list.size() != 4 || out.set.size() >= 65536) continue;
        double b[4];
        bool sane = true;
        for (int i = 0; i < 4; ++i) {
            auto const &v = box->second.list[i];
            sane = sane && v.kind == Value::Number && std::isfinite(v.number) && std::abs(v.number) < 1e9;
            b[i] = v.number;
        }
        if (sane) out.set.emplace_back(Geom::Point(b[0], b[1]), Geom::Point(b[2], b[3]));
    }
    struct Paragraph { std::size_t end; unsigned alignment; bool rtl; bool world_ready; bool justified; bool hyphenate;
        std::array<double, 3> indents; StyleLayers characters, features; };
    std::vector<Paragraph> paragraphs;
    std::size_t paragraph_units = 0, layer_budget = MAX_VALUES;
    auto add_paragraph = [&](Value const *declaration, std::size_t count) {
        auto const layers = paragraph_layers(declaration, setup, out.paragraph_styles_expanded, layer_budget);
        auto number = [&](char const *key) {
            auto const value = layer_value(layers, key);
            return value ? value->index() : 0;
        };
        StyleLayers characters;
        for (auto const *p : layers) {
            if (auto const it = p->dict.find("32"); it != p->dict.end()) {
                if (it->second.kind != Value::Dictionary) throw Bad("invalid paragraph character defaults");
                characters.push_back(&it->second);
            }
        }
        // How its lines are aligned (0): 0 left, 1 right, 2 centred, and justified with
        // the last line left (3), right (4), centred (5) or justified too (6).
        auto align = number("0");
        if (align > 6) throw Bad("text has an unsupported paragraph alignment");
        bool const justified = align > 2;
        // A line of point type has no measure to be justified to: it is set as the last
        // line of its paragraph would be, and one justified too from its left end.
        if (justified && area) throw Bad("justified paragraph alignment isn't supported yet");
        if (justified) align = align == 4 ? 1 : align == 5 ? 2 : 0;
        // Direction (33) and composer (35) are independent. The Middle Eastern composer
        // reorders/shapes scripts in LTR paragraphs too; it is not a paragraph direction.
        auto const direction = number("33");
        if (direction > 1) throw Bad("text has an unsupported paragraph direction");
        auto const composer = number("35");
        if (composer > 1) throw Bad("text has an unsupported paragraph composer");
        // Illustrator's alignment is relative to the paragraph's reading direction.
        // TextLine keeps physical left/right anchors, independently of that direction.
        if (direction == 1 && align != 2) align = 1 - align;
        // Whether it is hyphenated (9) where area type wraps it: Illustrator then draws a hyphen
        // that isn't typed, and lays it out again so.
        auto const hyphenate = layer_value(layers, "9");
        std::array<double, 3> indents{};
        for (unsigned i = 0; i < indents.size(); ++i) {
            auto const *value = layer_value(layers, std::to_string(i + 1).c_str());
            if (!value) continue;
            indents[i] = value->num();
            if (!std::isfinite(indents[i]) || std::abs(indents[i]) > 1e6) throw Bad("invalid paragraph indent");
        }
        if (!count || count > MAX_STORY * 2 - paragraph_units) throw Bad("invalid paragraph run length");
        paragraph_units += count;
        paragraphs.push_back({paragraph_units, unsigned(align), direction == 1, composer == 1, justified,
                              hyphenate && hyphenate->kind == Value::Boolean && hyphenate->boolean, indents, std::move(characters), layers});
    };
    if (s.get("0").dict.count("5")) {
        for (auto const &paragraph : s.get("0").get("5").get("0").list) {
            add_paragraph(&paragraph.get("0").get("0"), paragraph.get("1").index());
        }
        if (paragraphs.empty()) throw Bad("text has no paragraph runs");
    } else {
        std::size_t count = 0;
        for (auto const *at = text.data(); at < text.data() + text.size(); at = g_utf8_next_char(at)) {
            count += g_utf8_get_char(at) > 0xffff ? 2 : 1;
        }
        add_paragraph(nullptr, count);
    }
    // A text object reads one way: its paragraphs are one to what sets them.
    out.rtl = paragraphs.front().rtl;
    out.world_ready = paragraphs.front().world_ready;
    for (auto const &p : paragraphs) {
        if (p.rtl != out.rtl) throw Bad("paragraphs that read both ways in one story aren't supported yet");
        if (p.world_ready != out.world_ready) throw Bad("paragraphs with different composers in one story aren't supported yet");
    }
    // Area type: how many characters Illustrator put in each line, their ends included.
    std::vector<std::size_t> line_units;
    auto segment_of = [](Value const &line) -> Value const & {
        auto const &segments = line.get("6");
        if (std::count_if(segments.list.begin(), segments.list.end(), [](Value const &v) { return v.node("S"); }) != 1)
            throw Bad("text line has multiple layout segments not supported yet");
        return *std::find_if(segments.list.begin(), segments.list.end(), [](Value const &v) { return v.node("S"); });
    };
    // Whether Illustrator put a line's letters in order for scripts that read from the right:
    // its composer for those leaves what it worked out with the segment's character count
    // (15, key 7). Its other composer sets such letters from the left as typed, backwards.
    std::vector<bool> ordered;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto const *line = lines[i];
        auto const &found = segment_of(*line);
        auto const count = found.dict.find("15");
        ordered.push_back(count != found.dict.end() && count->second.kind == Value::Dictionary && count->second.dict.count("7"));
        if (area) {
            // Where the frame wrapped it, unless a return or a line break ends it (below).
            out.lines.push_back({laid[i].at + offset_of(found), {}});
            out.lines.back().end = TextLine::End::Wrap;
            auto const &counts = found.get("15");
            auto const extra = counts.dict.find("2"), mappings = counts.dict.find("1");
            // Other layouts also use /2 (notably bidi caches). An automatic hyphen
            // has one extra glyph AND a one-to-two expansion of the last character.
            if (extra != counts.dict.end() && extra->second.index() == 1 &&
                mappings != counts.dict.end() && mappings->second.kind == Value::Array) {
                for (auto const &map : mappings->second.list) {
                    if (map.kind == Value::Dictionary && map.dict.count("0") && map.dict.count("1") && map.dict.count("2") &&
                        map.get("0").index() == 2 && map.get("1").index() + 1 == counts.get("0").index() && map.get("2").index() == 2)
                        out.lines.back().automatic_hyphen = true;
                }
            }
            line_units.push_back(found.get("15").get("0").index());
            continue;
        }
        auto offset = line->dict.count("0") ? line->get("0").get("0").nums(2) : std::vector<double>{0, 0};
        auto segment = found.dict.count("0") ? found.get("0").get("0").nums(2) : std::vector<double>{0, 0};
        if (on_path) {
            if (offset[0] != 0 || offset[1] != 0 || segment[1] != 0)
                throw Bad("path type has unsupported layout offsets");
            out.lines.push_back({Geom::Point(0, 0), {}});
        } else {
            out.lines.push_back({Geom::Point(anchor[0] + offset[0] + segment[0], anchor[1] + offset[1]), {}});
        }
    }
    struct Run { std::size_t end; TextStyle style; };
    std::vector<Run> runs;
    struct RawRun { std::size_t end; Value const *own; };
    std::vector<RawRun> raw_runs;
    Value inherited;
    inherited.kind = Value::Dictionary;
    std::size_t units = 0;
    auto const &content = s.get("0");
    if (!content.dict.count("6")) {
        // A story with no character overrides has no run list. Illustrator uses the
        // paragraph's character defaults, then the document's, including ends.
        // An explicitly present but broken run list must still fail below.
        for (auto const *at = text.data(); at < text.data() + text.size(); at = g_utf8_next_char(at)) {
            units += g_utf8_get_char(at) > 0xffff ? 2 : 1;
        }
        raw_runs.push_back({units, &inherited});
    } else {
        for (auto const &run : content.get("6").get("0").list) {
            auto count = run.get("1").index();
            if (!count || count > MAX_STORY * 2 - units) throw Bad("invalid text run length");
            units += count;
            raw_runs.push_back({units, &run.get("0").get("0").get("6")});
        }
    }
    if (raw_runs.empty()) throw Bad("text has no character runs");
    if (paragraph_units != units) throw Bad("paragraph runs don't cover the story");
    // Character overrides can span paragraph boundaries. Intersect the two run
    // lists so each span inherits its own paragraph's character defaults.
    std::size_t resolved_units = 0, paragraph_index = 0;
    for (auto const &r : raw_runs) {
        while (resolved_units < r.end) {
            auto const &p = paragraphs[paragraph_index];
            auto const end = std::min(r.end, p.end);
            if (runs.size() >= MAX_VALUES) throw Bad("resolved character runs exceed their limits");
            if (p.characters.size() + 1 > layer_budget) throw Bad("character style resolution exceeds its limits");
            layer_budget -= p.characters.size() + 1;
            runs.push_back({end, style(*r.own, defaults, setup, p.characters)});
            resolved_units = end;
            if (end == p.end) ++paragraph_index;
        }
    }
    auto const hscale = runs[0].style.horizontal_scale, vscale = runs[0].style.vertical_scale;
    for (auto const &r : runs) {
        if (r.style.horizontal_scale != hscale || r.style.vertical_scale != vscale)
            throw Bad("text has different character scales within one story, not supported yet");
    }
    if (on_path) {
        if (hscale != 1 || vscale != 1) throw Bad("scaled characters on a path aren't supported yet");
        for (auto const &r : runs) {
            if (r.style.baseline_shift != 0 || r.style.position || r.style.underline || r.style.strikethrough)
                throw Bad("shifted or decorated path type isn't supported yet");
        }
    }
    // A common character scale is an editable affine on the story. Compensate
    // explicit baseline positions so glyphs scale without moving their anchors.
    out.to_art = Geom::Scale(hscale, vscale) * out.to_art;
    for (auto &l : out.lines) l.origin *= Geom::Scale(1 / hscale, 1 / vscale);
    out.frame *= Geom::Scale(1 / hscale, 1 / vscale);
    for (auto &r : out.set) r *= Geom::Scale(1 / hscale, 1 / vscale);
    if (area) {
        std::size_t laid_units = 0;
        for (auto const n : line_units) laid_units += n;
        // Fewer is a frame too small for its text: the rest isn't laid out.
        if (laid_units > units) throw Bad("text runs or line layout don't cover the story");
    }
    std::size_t run = 0, line = 0, at_units = 0, paragraph = 0, line_end = area ? line_units[0] : 0;
    bool in_order = false, as_typed = false;
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
                // The hidden suffix currently retains text, rather than individual paragraph
                // declarations. Don't silently assign another paragraph's margins to it.
                if (!out.lines.empty()) for (unsigned i = 0; i < 3; ++i) {
                    if (paragraphs[paragraph].indents[i] / hscale != out.lines.back().indents[i])
                        throw Bad("hidden paragraphs with different indents aren't supported yet");
                }
                // What the frame is too small to show: kept, not drawn.
                if (cp == '\r' || cp == 3 || cp == '\n') out.overflow.push_back('\n'); else if (cp) out.overflow.append(at, next - at);
                at_units += count; at = next;
                continue;
            }
            if (at_units + count > line_end) throw Bad("text line cuts a Unicode character or ends early");
        }
        // A line takes its paragraph's alignment and hyphenation, and its end from the return or
        // the line break that ends it.
        if (line < out.lines.size()) {
            auto &l = out.lines[line];
            l.paragraph_alignment = paragraphs[paragraph].alignment;
            l.hyphenate = paragraphs[paragraph].hyphenate;
            for (unsigned i = 0; i < 3; ++i) l.indents[i] = paragraphs[paragraph].indents[i] / hscale;
            if (cp == '\r' || cp == '\n') l.end = TextLine::End::Paragraph;
            else if (cp == 3) l.end = TextLine::End::Break;
        }
        if (cp == 0) {
            // A character that sets nothing; it only counts.
        } else if (cp == '\r' || cp == 3 || cp == '\n') {
            if (!area) ++line;
        } else {
            if (on_path && (cp == '\t' || reads_from_the_right(cp)))
                throw Bad("tabs or bidirectional type on a path aren't supported yet");
            if (cp == '\t') {
                if (line >= out.lines.size()) throw Bad("tab line layout ends early");
                auto const &p = paragraphs[paragraph];
                if (p.world_ready) throw Bad("tab stops with the Middle Eastern composer aren't supported yet");
                if (p.rtl || p.alignment || p.justified) throw Bad("tab stops in non-left-aligned paragraphs aren't supported yet");
                Text::LeftTabStops tabs;
                tabs.origin = anchor[0] / hscale;
                if (auto v = layer_value(p.features, "31")) tabs.interval = v->num();
                tabs.interval /= hscale;
                if (auto v = layer_value(p.features, "30")) {
                    if (v->kind != Value::Dictionary || (!v->dict.empty() && (v->dict.size() != 1 || !v->dict.count("0"))))
                        throw Bad("invalid paragraph tab stops");
                    if (!v->dict.empty()) {
                        auto const &list = v->get("0");
                        if (list.kind != Value::Array || list.list.size() > 1024) throw Bad("invalid or excessive paragraph tab stops");
                        for (auto const &stop : list.list) {
                            for (auto const &[key, value] : stop.dict) {
                                if (key != "0" && key != "1" && key != "2" && key != "3") throw Bad("unsupported tab stop settings");
                            }
                            if (stop.get("1").index() != 0) throw Bad("non-left tab stops aren't supported yet");
                            if (stop.dict.count("2") && !stop.get("2").str().empty()) throw Bad("tab stop leaders aren't supported yet");
                            if (stop.dict.count("3")) stop.get("3").str();
                            tabs.positions.push_back(stop.get("0").num() / hscale);
                        }
                    }
                }
                if (!tabs.valid()) throw Bad("invalid or excessive paragraph tab stops");
                auto &own = out.lines[line].tabs;
                if (own && *own != tabs) throw Bad("one line has conflicting paragraph tab stops");
                own = std::move(tabs);
            }
            if (line >= out.lines.size() || (cp < 32 && cp != '\t')) throw Bad("text line count or control character isn't supported");
            // Hebrew, Arabic and the other scripts read from the right are put in order by
            // one of Illustrator's two composers, which leaves what it worked out with the
            // line. The other sets them from the left as they were typed: backwards. The
            // story says which it was, to be set the same way; a story with both can't be.
            if (reads_from_the_right(cp)) {
                (ordered[line] ? in_order : as_typed) = true;
                if (in_order && as_typed) {
                    throw Bad("letters read from the right that Illustrator put in order in one line and not in another aren't supported yet");
                }
                // Nothing says how a paragraph that reads from the right sets them unordered.
                if (as_typed && out.rtl) {
                    throw Bad("letters read from the right that Illustrator set as typed in a paragraph read from the right aren't supported yet");
                }
            }
            auto &pieces = out.lines[line].runs;
            if (pieces.empty() && !paragraphs.empty()) out.lines[line].alignment = paragraphs[paragraph].alignment;
            // Keeping source run boundaries preserves editable character styles.
            if (pieces.empty() || at_units == (run ? runs[run - 1].end : 0)) pieces.push_back({{}, runs[run].style});
            pieces.back().text.append(at, next - at);
        }
        at_units += count; at = next;
    }
    if (at_units != units) throw Bad("text runs or line layout don't cover the story");
    out.unordered = as_typed;
    if ((as_typed || in_order) && std::any_of(out.lines.begin(), out.lines.end(), [](auto const &l) { return bool(l.tabs); }))
        throw Bad("tab stops with bidirectional text aren't supported yet");
    if (!area && (line > out.lines.size() || (line + 1 != out.lines.size() && line != out.lines.size()))) {
        throw Bad("text runs or line layout don't cover the story");
    }
    // Paragraphs with nothing in them past the frame's end hold nothing to keep.
    if (out.overflow.find_first_not_of('\n') == std::string::npos) out.overflow.clear();

    for (auto const &line : out.lines) {
        if (line.automatic_hyphen && (line.end != TextLine::End::Wrap || !line.hyphenate || line.runs.empty() || out.rtl || in_order || as_typed))
            throw Bad("unsupported automatic hyphen at an area line end");
    }
    if (on_path) {
        if (out.lines.size() != 1 || paragraphs.size() != 1 || out.rtl || out.world_ready || paragraphs[0].justified)
            throw Bad("multiple or justified paragraphs on a path aren't supported yet");
        out.lines[0].alignment = paragraphs[0].alignment;
        return out;
    }

    // Every line starts where its glyphs do. Centred or right-aligned lines are anchored at
    // their middle or right end instead, to stay there when edited. A text object has one
    // anchor for all its lines (they are one paragraph to what sets them, which takes its
    // alignment from the first), so that is done only when every line with characters has
    // the same alignment and each says where its anchor is: point type by its segment's
    // offset from the story's anchor, area type by the middle or right end of its line's
    // box, when its glyphs' own extent agrees that that is where it sits (a line that keeps
    // a space at its end doesn't).
    // Illustrator centres a line with the spaces at its end, as point type's offsets show
    // (half the run's width, its space included), and so does the layout of type that says it
    // is aligned as Illustrator aligns it (LINE_ALIGNMENT_ATTRIBUTE, which the importer gives
    // such point type). That has no rule for letters read from the right, whose line ends are
    // elsewhere.
    bool const spaces_aligned = !area && !out.rtl && !in_order && !as_typed;
    std::optional<unsigned> common;
    bool together = true;
    bool const unsure = std::any_of(paragraphs.begin(), paragraphs.end(), [](Paragraph const &p) { return p.justified; });
    for (auto const &l : out.lines) {
        if (l.runs.empty()) continue;
        together = together && (!common || *common == l.alignment);
        common = l.alignment;
    }
    std::vector<double> moves(out.lines.size(), 0.0); // From each line's start to its anchor.
    // RTL point type's return has an advance before the visible text, even when
    // physically left-aligned. Its segment starts before the paragraph anchor;
    // use that anchor just as for centred/right-aligned point type. The return's
    // width is a font metric, not a fixed correction to apply to the glyphs.
    together = together && common && (*common != 0 || (!area && out.rtl));
    for (std::size_t i = 0; together && i < out.lines.size(); ++i) {
        if (out.lines[i].runs.empty()) continue;
        // A space at a line's end is part of what Illustrator centres, and not of what is
        // centred here unless it says so (above): such a line is only exact from its start.
        auto const &last = out.lines[i].runs.back().text;
        if (!spaces_aligned && !last.empty() && g_unichar_isspace(g_utf8_get_char(g_utf8_prev_char(last.data() + last.size())))) {
            together = false;
            break;
        }
        auto const &found = segment_of(*lines[i]);
        if (!area) {
            if (!found.dict.count("0")) {
                if (*common == 0) continue;
                // What says a justified paragraph's lines end at the right or the middle
                // doesn't say where a line of point type starts: it stays at its start.
                if (unsure) {
                    together = false;
                    break;
                }
                throw Bad("aligned text has no explicit layout offset");
            }
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
    out.illustrator_alignment = together && spaces_aligned;
    return out;
}
} // namespace

std::shared_ptr<TextDocument const> read_text_document(std::string_view records, std::optional<Geom::Point> center)
{
    auto out = std::make_shared<TextDocument>();
    try {
        auto const section = text_section(records);
        // The stories lie on Illustrator's canvas, and the section says where the art's 0,0 is
        // on it. Without that, the middle of the canvas is taken to be the document's centre,
        // which holds for an artboard that was never moved or resized from one side: in one
        // file in eight of those seen the two differ.
        if (auto const origin = ruler_origin(section)) {
            center = Geom::Point(CANVAS_CENTRE - origin->x(), origin->y() - CANVAS_CENTRE);
        }
        if (!center) throw Bad("text has no template centre for placement");
        if (!std::isfinite(center->x()) || !std::isfinite(center->y()) ||
            std::abs(center->x()) > 1e9 || std::abs(center->y()) > 1e9) throw Bad("invalid text template centre");
        auto const paints = named_paints(records);
        // The stories of one text document, each read on its own.
        auto read = [&](std::string const &data, std::vector<std::optional<PointText>> &stories_out,
                        std::vector<std::string> &errors, bool typed) {
            auto doc = Reader(data).root();
            Setup setup;
            if (!typed) setup.document = &out->type;
            setup.paints = &paints;
            for (auto const &font : doc.get("0").get("1").get("0").list) setup.fonts.push_back(font_entry(font.get("0").get("0")));
            auto const &main = doc.get("1");
            if (auto const it = doc.get("0").dict.find("6"); it != doc.get("0").dict.end()) setup.paragraph_styles = &it->second;
            if (auto const it = main.dict.find("3"); it != main.dict.end()) setup.paragraph_defaults = &it->second;
            if (auto const settings = main.dict.find("0"); settings != main.dict.end() && settings->second.kind == Value::Dictionary) {
                // Illustrator takes sizes up to twice the type's own; a shift can be none.
                auto share = [&](char const *k, double &value, bool size) {
                    auto const it = settings->second.dict.find(k);
                    if (it == settings->second.dict.end()) return;
                    auto const &v = it->second;
                    if (v.kind != Value::Number || !(v.number >= 0) || v.number > 4 || (size && v.number == 0)) {
                        throw Bad("invalid superscript, subscript or small caps setting");
                    }
                    value = v.number;
                };
                share("3", setup.type.superscript_size, true);
                share("4", setup.type.superscript_shift, false);
                share("5", setup.type.subscript_size, true);
                share("6", setup.type.subscript_shift, false);
                share("7", setup.type.small_caps_size, true);
            }
            if (typed) out->type = setup.type;
            auto const &stories = main.get("1");
            if (stories.kind != Value::Array || stories.list.size() > 65536) throw Bad("invalid or oversized story list");
            for (auto const &s : stories.list) {
                try {
                    stories_out.push_back(story(s, doc.get("0").get("8").get("0"), main.get("2"), setup, *center));
                    errors.emplace_back();
                } catch (Bad const &e) { stories_out.emplace_back(); errors.push_back(e.what()); }
            }
        };
        std::size_t at = 0;
        read(unpack(section, at), out->stories, out->errors, true);
        // After the document of what was typed comes, when appearances drew type, the document
        // of that type. Without it, or when it can't be read, such type has no stories.
        auto const drawn = section.find("/AI11UndoFreeTextDocument", at);
        if (drawn != section.npos) {
            try {
                at = drawn;
                read(unpack(section, at), out->drawn, out->drawn_errors, false);
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
    if (a.empty != b.empty || a.rtl != b.rtl || a.world_ready != b.world_ready ||
        a.unordered != b.unordered || a.lines.size() != b.lines.size()) return false;
    if (bool(a.path) != bool(b.path)) return false;
    if (a.path && (a.path->start != b.path->start || a.path->end != b.path->end ||
                   a.path->automatic_spacing != b.path->automatic_spacing ||
                   a.path->source_options != b.path->source_options ||
                   a.path->outline * a.to_art != b.path->outline * b.to_art)) return false;
    auto same_style = [](TextStyle const &s, TextStyle const &t) {
        return s.font == t.font && s.design == t.design && s.size == t.size && s.fill == t.fill && s.stroke == t.stroke &&
               s.fill_opacity == t.fill_opacity && s.stroke_opacity == t.stroke_opacity &&
               (!s.stroke || s.stroke_width == t.stroke_width) && s.tracking == t.tracking &&
               s.horizontal_scale == t.horizontal_scale && s.vertical_scale == t.vertical_scale &&
               s.baseline_shift == t.baseline_shift && s.kerning == t.kerning && s.ligatures == t.ligatures &&
               s.contextual == t.contextual && s.all_caps == t.all_caps && s.small_caps == t.small_caps &&
               s.underline == t.underline && s.strikethrough == t.strikethrough && s.position == t.position &&
               s.gradient == t.gradient && s.fill_pattern == t.fill_pattern && s.stroke_pattern == t.stroke_pattern;
    };
    // Scaled and turned alike, and (below) each line in the same place on the art.
    if (!Geom::are_near(a.to_art.withoutTranslation(), b.to_art.withoutTranslation(), 1e-6)) return false;
    for (std::size_t i = 0; i < a.lines.size(); ++i) {
        auto const &la = a.lines[i], &lb = b.lines[i];
        if (la.runs.size() != lb.runs.size() || la.alignment != lb.alignment || la.tabs != lb.tabs || la.indents != lb.indents ||
            la.automatic_hyphen != lb.automatic_hyphen) return false;
        if (!la.runs.empty() && !Geom::are_near(la.origin * a.to_art, lb.origin * b.to_art, 0.01)) return false;
        for (std::size_t r = 0; r < la.runs.size(); ++r) {
            if (la.runs[r].text != lb.runs[r].text || !same_style(la.runs[r].style, lb.runs[r].style)) return false;
        }
    }
    return true;
}
} // namespace Inkscape::Extension::Internal::AiNative
