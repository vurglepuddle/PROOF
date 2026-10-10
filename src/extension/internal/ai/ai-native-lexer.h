// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the scanner of Illustrator's native records.
 *
 * Illustrator's own copy of a document (a PDF-based .ai's AIPrivateData streams,
 * an Illustrator EPS after %%EOF, or an old PostScript .ai) is a PostScript-like
 * program of operators. This splits it into tokens. Nothing is executed.
 *
 * - A line starting with "%_" holds tokens like any other line; they are marked
 *   hidden (readers of the printed format skip them as comments).
 * - Any other line starting with "%" is a comment token (the section markers), and so
 *   is a "%" straight after "%_": an object written on hidden lines keeps its markers
 *   and its image data there ("%_%AI5_BeginRaster", "%_%%BeginData: n").
 *   A "%" later in a line starts a comment that is skipped, except a gradient
 *   definition's "%_BS", "%_Bs" and "%_Br" ending a line: a hidden operator.
 * - The binary image data after "%%BeginData:" and "XI" is one data token, and
 *   placed-object previews are skipped whole.
 *
 * Ported from VectorCraft's scanner (crates/eps/src/import/ai/lex.rs at 4cf912f),
 * Copyright (c) 2026 ArtCraft Team and the VectorCraft contributors, MIT licence;
 * see LICENSES/MIT-VectorCraft.txt.
 */
#ifndef SEEN_EXTENSION_INTERNAL_AI_NATIVE_LEXER_H
#define SEEN_EXTENSION_INTERNAL_AI_NATIVE_LEXER_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Inkscape::Extension::Internal::AiNative {

struct Token
{
    enum class Kind
    {
        Number,  ///< A finite number.
        String,  ///< A literal (...) or hexadecimal <...> string, decoded into `string`.
        Name,    ///< A literal /name, without its slash, in `view`.
        Word,    ///< An operator (any other word, brackets and braces included), in `view`.
        Comment, ///< A comment line without its first '%', in `view`.
        Data,    ///< An image's samples after XI, in `view`.
    };

    Kind kind = Kind::Word;
    double number = 0.0;
    std::string_view view;
    std::string string;
    bool hidden = false; ///< Read from a "%_" line (comments and data after "%_" too).
};

class Lexer
{
public:
    explicit Lexer(std::string_view src);

    /// The next token, or nothing at the end.
    std::optional<Token> next();

    /// Skip past the next `needle` (data that isn't tokens, such as ASCII85 after a
    /// /Binary dictionary entry). Returns what was skipped.
    std::string_view skip_past(std::string_view needle);

    std::size_t position() const { return _pos; }

private:
    bool at_line_start() const;
    std::string_view line();
    std::optional<Token> special(std::string_view line);
    std::string_view word();
    std::string literal_string();

    std::string_view _src;
    std::size_t _pos = 0;
    bool _hidden = false;
    std::size_t _hidden_from = 0; ///< Where the tokens of the current "%_" line start.
};

/// A finite number token ("12", "-3.5", ".5", "1e-3"), or nothing.
std::optional<double> parse_number(std::string_view word);

/// Decode hexadecimal digits (whitespace ignored, an odd last digit padded with 0).
/// Nothing when a character isn't a digit.
std::optional<std::string> hex_decode(std::string_view digits);

} // namespace Inkscape::Extension::Internal::AiNative

#endif // SEEN_EXTENSION_INTERNAL_AI_NATIVE_LEXER_H
