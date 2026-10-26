// SPDX-License-Identifier: GPL-2.0-or-later
#include "json.h"

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace Inkscape::MCP {
namespace {

[[noreturn]] void invalid() { throw std::runtime_error("Invalid JSON"); }

void utf8(std::string_view s)
{
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i++]);
        if (c < 0x80) continue;
        unsigned count, cp, minimum;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; cp = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; cp = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; cp = c & 7; minimum = 0x10000; }
        else invalid();
        if (s.size() - i < count) invalid();
        while (count--) {
            auto next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xc0) != 0x80) invalid();
            cp = (cp << 6) | (next & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) invalid();
    }
}

class Parser
{
public:
    explicit Parser(std::string_view s) : input(s) {}
    Json run() { auto result = read(0); space(); if (pos != input.size()) invalid(); return result; }

private:
    std::string_view input;
    std::size_t pos = 0;
    void space() { while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\n' || input[pos] == '\r' || input[pos] == '\t')) ++pos; }
    bool take(char c) { if (pos < input.size() && input[pos] == c) { ++pos; return true; } return false; }
    void need(char c) { if (!take(c)) invalid(); }
    unsigned hex()
    {
        unsigned n = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos == input.size()) invalid();
            char c = input[pos++];
            unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (d == 16) invalid();
            n = n * 16 + d;
        }
        return n;
    }
    std::string string()
    {
        need('"');
        std::string out;
        while (pos < input.size()) {
            auto c = static_cast<unsigned char>(input[pos++]);
            if (c == '"') return out;
            if (c < 32) invalid();
            if (c != '\\') { out += char(c); continue; }
            if (pos == input.size()) invalid();
            switch (input[pos++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex();
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        need('\\'); need('u');
                        unsigned low = hex();
                        if (low < 0xdc00 || low > 0xdfff) invalid();
                        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) invalid();
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                    else if (cp < 0x10000) { out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                    else { out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                    break;
                }
                default: invalid();
            }
        }
        invalid();
    }
    Json read(unsigned depth)
    {
        space();
        if (pos == input.size()) invalid();
        if (input[pos] == '{' || input[pos] == '[') {
            if (depth == 32) invalid();
            bool object = take('{');
            if (!object) need('[');
            space();
            Json::Object map;
            Json::Array array;
            char end = object ? '}' : ']';
            if (!take(end)) {
                do {
                    space();
                    std::string key;
                    if (object) { key = string(); space(); need(':'); }
                    auto v = read(depth + 1);
                    if (object) { if (!map.emplace(std::move(key), std::move(v)).second) invalid(); }
                    else array.emplace_back(std::move(v));
                    space();
                    if (take(end)) return object ? Json(std::move(map)) : Json(std::move(array));
                } while (take(','));
                invalid();
            }
            return object ? Json(std::move(map)) : Json(std::move(array));
        }
        if (input[pos] == '"') return string();
        for (auto literal : {"true", "false", "null"}) {
            auto s = std::string_view(literal);
            if (input.substr(pos, s.size()) == s) {
                pos += s.size();
                return s == "null" ? Json() : Json(s == "true");
            }
        }
        auto start = pos;
        take('-');
        if (!take('0')) {
            if (pos == input.size() || input[pos] < '1' || input[pos] > '9') invalid();
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
        }
        auto digits = [&] {
            auto begin = pos;
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            if (begin == pos) invalid();
        };
        if (take('.')) digits();
        if (take('e') || take('E')) { if (!take('+')) take('-'); digits(); }
        double n;
        auto [end, ec] = std::from_chars(input.data() + start, input.data() + pos, n);
        if (ec != std::errc() || end != input.data() + pos || !std::isfinite(n)) invalid();
        return n;
    }
};

void emit(std::string &out, Json const &v, unsigned depth)
{
    if (depth > 32 || out.size() > RESPONSE_LIMIT) throw std::runtime_error("JSON output limit exceeded");
    if (v.is<std::nullptr_t>()) out += "null";
    else if (v.is<bool>()) out += v.get<bool>() ? "true" : "false";
    else if (v.is<double>()) {
        auto n = v.get<double>();
        if (!std::isfinite(n)) invalid();
        char buf[64];
        auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), n);
        if (ec != std::errc()) invalid();
        out.append(buf, end);
    } else if (v.is<std::string>()) {
        auto const &s = v.get<std::string>();
        utf8(s);
        out += '"';
        for (auto c : s) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (static_cast<unsigned char>(c) < 32) {
                out += "\\u00";
                out += "0123456789abcdef"[(static_cast<unsigned char>(c) >> 4)];
                out += "0123456789abcdef"[c & 15];
            } else out += c;
        }
        out += '"';
    } else if (v.is<Json::Array>()) {
        out += '[';
        bool first = true;
        for (auto const &item : v.get<Json::Array>()) { if (!first) out += ','; first = false; emit(out, item, depth + 1); }
        out += ']';
    } else {
        out += '{';
        bool first = true;
        for (auto const &[key, item] : v.get<Json::Object>()) {
            if (!first) out += ',';
            first = false;
            emit(out, Json(key), depth + 1); out += ':'; emit(out, item, depth + 1);
        }
        out += '}';
    }
}
} // namespace

Json const &Json::at(std::string_view key) const
{
    if (!is<Object>()) invalid();
    auto it = get<Object>().find(key);
    if (it == get<Object>().end()) invalid();
    return it->second;
}
bool Json::contains(std::string_view key) const { return is<Object>() && get<Object>().contains(key); }
Json parse(std::string_view input)
{
    if (input.size() > REQUEST_LIMIT) throw std::runtime_error("JSON input limit exceeded");
    utf8(input);
    return Parser(input).run();
}
std::string serialize(Json const &value)
{
    std::string out;
    emit(out, value, 0);
    if (out.size() > RESPONSE_LIMIT) throw std::runtime_error("JSON output limit exceeded");
    return out;
}
} // namespace Inkscape::MCP
