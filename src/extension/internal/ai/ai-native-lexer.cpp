// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the scanner of Illustrator's native records. See ai-native-lexer.h.
 *
 * Ported from VectorCraft's scanner (crates/eps/src/import/ai/lex.rs at 4cf912f),
 * Copyright (c) 2026 ArtCraft Team and the VectorCraft contributors, MIT licence;
 * see LICENSES/MIT-VectorCraft.txt.
 */
#include "ai-native-lexer.h"

#include <array>
#include <charconv>
#include <cmath>

namespace Inkscape::Extension::Internal::AiNative {
namespace {

/// Binary sections skipped whole: (begin, end) markers.
constexpr std::array<std::pair<std::string_view, std::string_view>, 2> SKIPPED{{
    {"%AI26_BeginPlacedObjectPreview", "%AI26_EndPlacedObjectPreview"},
    {"%%BeginDocument", "%%EndDocument"},
}};

constexpr std::string_view BEGIN_DATA = "%%BeginData:";
constexpr std::string_view END_DATA = "%%EndData";

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\x0c' || c == '\0';
}

bool is_eol(char c)
{
    return c == '\r' || c == '\n';
}

bool is_delim(char c)
{
    switch (c) {
        case '(': case ')': case '<': case '>': case '[': case ']': case '{': case '}': case '/': case '%':
            return true;
        default:
            return false;
    }
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::optional<double> parse_number(std::string_view w)
{
    if (w.empty()) {
        return {};
    }
    char const first = w.front();
    if (!((first >= '0' && first <= '9') || first == '+' || first == '-' || first == '.')) {
        return {};
    }
    for (char c : w) {
        bool const alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (alpha && c != 'e' && c != 'E') {
            return {};
        }
    }
    // from_chars rejects a leading '+'.
    if (first == '+') {
        w.remove_prefix(1);
        if (w.empty() || w.front() == '-' || w.front() == '+') {
            return {};
        }
    }
    double value = 0.0;
    auto const [end, ec] = std::from_chars(w.data(), w.data() + w.size(), value);
    if (ec != std::errc() || end != w.data() + w.size() || !std::isfinite(value)) {
        return {};
    }
    return value;
}

std::optional<std::string> hex_decode(std::string_view digits)
{
    std::string out;
    out.reserve(digits.size() / 2 + 1);
    int high = -1;
    for (char c : digits) {
        if (is_space(c)) {
            continue;
        }
        int const v = hex_value(c);
        if (v < 0) {
            return {};
        }
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<char>((high << 4) | v));
            high = -1;
        }
    }
    if (high >= 0) {
        out.push_back(static_cast<char>(high << 4));
    }
    return out;
}

Lexer::Lexer(std::string_view src)
    : _src(src)
{}

bool Lexer::at_line_start() const
{
    return _pos == 0 || is_eol(_src[_pos - 1]);
}

std::string_view Lexer::line()
{
    auto const start = _pos;
    while (_pos < _src.size() && !is_eol(_src[_pos])) {
        ++_pos;
    }
    return _src.substr(start, _pos - start);
}

std::string_view Lexer::skip_past(std::string_view needle)
{
    auto const start = _pos;
    auto const at = _src.find(needle, _pos);
    _pos = at == std::string_view::npos ? _src.size() : at + needle.size();
    return _src.substr(start, _pos - start);
}

std::string_view Lexer::word()
{
    auto const start = _pos;
    while (_pos < _src.size() && !is_space(_src[_pos]) && !is_delim(_src[_pos])) {
        ++_pos;
    }
    return _src.substr(start, _pos - start);
}

std::string Lexer::literal_string()
{
    std::string out;
    std::size_t depth = 0;
    while (_pos < _src.size()) {
        char const c = _src[_pos++];
        if (c == '(') {
            ++depth;
            out.push_back(c);
        } else if (c == ')') {
            if (depth == 0) {
                break;
            }
            --depth;
            out.push_back(c);
        } else if (c == '\\') {
            if (_pos >= _src.size()) {
                break;
            }
            char const e = _src[_pos++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '\r':
                    if (_pos < _src.size() && _src[_pos] == '\n') {
                        ++_pos;
                    }
                    break;
                case '\n':
                    break;
                default:
                    if (e >= '0' && e <= '7') {
                        unsigned v = e - '0';
                        for (int i = 0; i < 2 && _pos < _src.size() && _src[_pos] >= '0' && _src[_pos] <= '7'; ++i) {
                            v = v * 8 + (_src[_pos++] - '0');
                        }
                        out.push_back(static_cast<char>(v & 0xff));
                    } else {
                        out.push_back(e);
                    }
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<Token> Lexer::special(std::string_view l)
{
    for (auto const &[begin, end] : SKIPPED) {
        if (l.substr(0, begin.size()) == begin) {
            auto const at = _src.find(end, _pos);
            _pos = at == std::string_view::npos ? _src.size() : at;
            line();
            Token t;
            t.kind = Token::Kind::Comment;
            t.view = l.substr(1);
            return t;
        }
    }
    if (l.substr(0, BEGIN_DATA.size()) != BEGIN_DATA) {
        return {};
    }
    auto rest = l.substr(BEGIN_DATA.size());
    while (!rest.empty() && is_space(rest.front())) {
        rest.remove_prefix(1);
    }
    auto const count_end = rest.find_first_of(" \t");
    std::size_t count = 0;
    auto const count_text = rest.substr(0, count_end);
    auto const [ptr, ec] = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
    if (ec != std::errc() || ptr != count_text.data() + count_text.size()) {
        return {};
    }
    // The count starts after the comment's line end character (a following '\n' counts).
    auto const from = _pos + 1;
    if (from > _src.size()) {
        return {};
    }
    auto skip = from;
    while (skip < _src.size() && is_space(_src[skip])) {
        ++skip;
    }
    if (_src.substr(skip, 2) != "XI") {
        return {};
    }
    auto data = skip + 2;
    if (_src.substr(data, 2) == "\r\n") {
        data += 2;
    } else if (data < _src.size() && is_eol(_src[data])) {
        data += 1;
    }
    // The count leaves out an alpha channel: the data ends where %%EndData starts.
    auto const capped = std::min(count, _src.size());
    auto const declared = std::max(std::min(from + capped, _src.size()), data);
    auto const lo = std::max(declared >= 4 ? declared - 4 : 0, data);
    auto const span = capped > (_src.size() / 2) ? _src.size() : capped * 2 + 64;
    auto const hi = std::min(declared + span, _src.size());
    auto const window = _src.substr(lo, hi - lo);
    auto const found = window.find(END_DATA);
    auto const end = found == std::string_view::npos ? declared : lo + found;
    _pos = end;
    Token t;
    t.kind = Token::Kind::Data;
    t.view = _src.substr(data, end - data);
    return t;
}

std::optional<Token> Lexer::next()
{
    while (_pos < _src.size()) {
        char const b = _src[_pos];
        if (is_eol(b)) {
            ++_pos;
            _hidden = false;
            continue;
        }
        if (is_space(b)) {
            ++_pos;
            continue;
        }
        if (b == '%') {
            if (at_line_start()) {
                if (_src.substr(_pos, 2) == "%_") {
                    _pos += 2;
                    _hidden = true;
                    continue;
                }
                auto const l = line();
                if (auto t = special(l)) {
                    return t;
                }
                Token t;
                t.kind = Token::Kind::Comment;
                t.view = l.substr(1);
                return t;
            }
            line(); // a comment later in a line
            continue;
        }
        Token t;
        t.hidden = _hidden;
        ++_pos;
        switch (b) {
            case '(':
                t.kind = Token::Kind::String;
                t.string = literal_string();
                return t;
            case '<': {
                auto const close = _src.find('>', _pos);
                auto const end = close == std::string_view::npos ? _src.size() : close;
                t.kind = Token::Kind::String;
                // Malformed digits read as an empty string.
                t.string = hex_decode(_src.substr(_pos, end - _pos)).value_or(std::string());
                _pos = std::min(end + 1, _src.size());
                return t;
            }
            case '[': case ']': case '{': case '}': case ')': case '>':
                t.kind = Token::Kind::Word;
                t.view = _src.substr(_pos - 1, 1);
                return t;
            case '/':
                t.kind = Token::Kind::Name;
                t.view = word();
                return t;
            default: {
                --_pos;
                auto const w = word();
                if (auto n = parse_number(w)) {
                    t.kind = Token::Kind::Number;
                    t.number = *n;
                } else {
                    t.kind = Token::Kind::Word;
                    t.view = w;
                }
                return t;
            }
        }
    }
    return {};
}

} // namespace Inkscape::Extension::Internal::AiNative
