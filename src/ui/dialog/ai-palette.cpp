// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: read the swatches of an Illustrator document. See ai-palette.h.
 *
 * Palette records, one statement per line:
 *   c m y k k                          process CMYK colour      } followed by
 *   gray g / r g b Xa                  process gray / RGB       } "(name)" and "Pc"
 *   v... (ink) tint type Xk            global process colour    }
 *   v... (ink) tint type Xx            spot ink                 }
 *   n (group name) n Pg                start of a swatch group
 * For Xk/Xx, type 0 gives c m y k; type 1 gives c m y k r g b and type 2 gives
 * c m y k L a b, where the RGB or Lab values are the authored definition.
 * The tint operand is inverted: 0 means full strength.
 */
#include "ai-palette.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <glib.h>

#include "colors/color.h"
#include "colors/spaces/enum.h"
#include "colors/spaces/lab.h"
#include "ui/dialog/global-palettes.h"

namespace Inkscape::UI::Dialog {
namespace {

using Colors::Color;
using Colors::Space::Type;

struct Token
{
    enum Kind { Number, String, Operator } kind;
    double number = 0;
    std::string text; // raw bytes for strings, operator name otherwise
};

std::vector<Token> tokenize(std::string_view line)
{
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < line.size()) {
        char const c = line[i];
        if (c == ' ' || c == '\t') {
            ++i;
        } else if (c == '(') {
            std::string bytes;
            int depth = 1;
            ++i;
            while (i < line.size() && depth) {
                char ch = line[i];
                if (ch == '\\' && i + 1 < line.size()) {
                    char const next = line[i + 1];
                    if (next >= '0' && next <= '7') {
                        int value = 0;
                        std::size_t j = i + 1;
                        for (; j < line.size() && j < i + 4 && line[j] >= '0' && line[j] <= '7'; ++j) {
                            value = value * 8 + (line[j] - '0');
                        }
                        bytes += static_cast<char>(value & 0xff);
                        i = j;
                        continue;
                    }
                    switch (next) {
                        case 'n': bytes += '\n'; break;
                        case 'r': bytes += '\r'; break;
                        case 't': bytes += '\t'; break;
                        case 'b': bytes += '\b'; break;
                        case 'f': bytes += '\f'; break;
                        default: bytes += next; break;
                    }
                    i += 2;
                    continue;
                }
                if (ch == '(') {
                    ++depth;
                } else if (ch == ')' && !--depth) {
                    ++i;
                    break;
                }
                bytes += ch;
                ++i;
            }
            out.push_back({Token::String, 0, std::move(bytes)});
        } else {
            auto j = i;
            while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '(') {
                ++j;
            }
            std::string word(line.substr(i, j - i));
            char *end = nullptr;
            double value = g_ascii_strtod(word.c_str(), &end);
            if (end && *end == '\0' && !word.empty()) {
                out.push_back({Token::Number, value, {}});
            } else {
                out.push_back({Token::Operator, 0, std::move(word)});
            }
            i = j;
        }
    }
    return out;
}

/// Names may be UTF-16BE (with BOM), UTF-8 or Windows-1252.
std::string decode_name(std::string const &raw)
{
    auto convert = [&](char const *from, std::size_t skip) -> std::string {
        gsize written = 0;
        gchar *utf8 = g_convert(raw.data() + skip, raw.size() - skip, "UTF-8", from, nullptr, &written, nullptr);
        if (!utf8) {
            return {};
        }
        std::string result(utf8, written);
        g_free(utf8);
        return result;
    };
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xfe && static_cast<unsigned char>(raw[1]) == 0xff) {
        return convert("UTF-16BE", 2);
    }
    if (g_utf8_validate(raw.data(), raw.size(), nullptr)) {
        return raw;
    }
    auto result = convert("CP1252", 0);
    return result.empty() ? convert("ISO-8859-1", 0) : result;
}

Color lab_color(double l, double a, double b)
{
    using Lab = Colors::Space::Lab;
    auto const span = Lab::MAX_SCALE - Lab::MIN_SCALE;
    return Color(Type::LAB, {l / Lab::LUMA_SCALE, (a - Lab::MIN_SCALE) / span, (b - Lab::MIN_SCALE) / span});
}

struct Pending
{
    std::optional<Color> color;
    bool spot = false;
    double tint = 1.0;
    std::string ink; // ink name from the Xx/Xk operator
    bool skip = false;
};

} // namespace

bool parse_ai_palette(std::string_view records, PaletteFileData &palette)
{
    auto const begin = records.find("%AI5_BeginPalette");
    if (begin == std::string_view::npos) {
        return false;
    }
    auto end = records.find("%AI5_EndPalette", begin);
    auto section = records.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);

    Pending pending;
    bool alternate_branch = false;
    std::size_t pos = 0;
    while (pos < section.size()) {
        auto next = section.find_first_of("\r\n", pos);
        auto line = section.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
        pos = next == std::string_view::npos ? section.size() : next + 1;

        // Versioned content: keep the branch for current Illustrator, skip its fallback copy.
        if (line.starts_with("%AI17_Begin_Content_if_version") || line.starts_with("%AI17_End_Versioned_Content")) {
            alternate_branch = false;
            continue;
        }
        if (line.starts_with("%AI17_Alternate_Content")) {
            alternate_branch = true;
            continue;
        }
        if (alternate_branch || line.empty() || line.front() == '%') {
            continue;
        }

        auto tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }
        std::vector<double> numbers;
        std::vector<std::string const *> strings;
        for (auto const &token : tokens) {
            if (token.kind == Token::Number) {
                numbers.push_back(token.number);
            } else if (token.kind == Token::String) {
                strings.push_back(&token.text);
            }
        }
        auto const &last = tokens.back();

        if (last.kind == Token::String && tokens.size() == 1) {
            // "(name)" line: the swatch's name, closing its definition.
            if (pending.color && !pending.skip) {
                auto color = *pending.color;
                if (pending.spot) {
                    color.setName(pending.ink.empty() ? decode_name(last.text) : pending.ink);
                    palette.colors.emplace_back(PaletteFileData::SpotColor{std::move(color), pending.tint});
                } else {
                    color.setName(decode_name(last.text));
                    palette.colors.emplace_back(std::move(color));
                }
            }
            pending = {};
            continue;
        }
        if (last.kind != Token::Operator) {
            continue;
        }
        auto const &op = last.text;
        if (op == "Pg" && !strings.empty()) {
            palette.colors.emplace_back(PaletteFileData::GroupStart{.name = decode_name(*strings.front())});
            pending = {};
        } else if (op == "k" && numbers.size() >= 4) {
            auto n = numbers.size();
            pending.color = Color(Type::CMYK, {numbers[n - 4], numbers[n - 3], numbers[n - 2], numbers[n - 1]});
        } else if (op == "g" && !numbers.empty()) {
            pending.color = Color(Type::Gray, {numbers.back()});
        } else if (op == "Xa" && numbers.size() >= 3) {
            auto n = numbers.size();
            pending.color = Color(Type::RGB, {numbers[n - 3], numbers[n - 2], numbers[n - 1]});
        } else if ((op == "Xx" || op == "Xk") && !strings.empty() && numbers.size() >= 2) {
            auto const type = static_cast<int>(numbers[numbers.size() - 1]);
            auto const ai_tint = numbers[numbers.size() - 2];
            std::vector<double> v(numbers.begin(), numbers.end() - 2);
            pending.color.reset();
            if (type == 0 && v.size() >= 4) {
                pending.color = Color(Type::CMYK, {v[0], v[1], v[2], v[3]});
            } else if (type == 1 && v.size() >= 7) {
                pending.color = Color(Type::RGB, {v[4], v[5], v[6]});
            } else if (type == 2 && v.size() >= 7) {
                pending.color = lab_color(v[4], v[5], v[6]);
            }
            pending.spot = op == "Xx";
            pending.tint = std::clamp(1.0 - ai_tint, 0.0, 1.0);
            pending.ink = decode_name(*strings.front());
        } else if (op == "Xs" || op == "Bb" || op == "Bg" || op == "BB" || op == "Bh" || op == "p") {
            pending.skip = true; // registration, gradient and pattern swatches are not palette colours
        } else if (op == "Pc") {
            pending = {};
        }
    }
    return true;
}

} // namespace Inkscape::UI::Dialog
