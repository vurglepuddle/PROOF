// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: how a document sets superscripts, subscripts and small capitals. See
 * type-settings.h.
 */
#include "type-settings.h"

#include <cmath>
#include <cstring>
#include <string>

#include <glib.h>

#include "document.h"
#include "svg/css-ostringstream.h"
#include "xml/node.h"

namespace Inkscape {
namespace {

constexpr char const *SUPERSCRIPT = "proof:superscript";
constexpr char const *SUBSCRIPT = "proof:subscript";
constexpr char const *SMALL_CAPS = "proof:small-caps";

constexpr double MOST = 4.0; // Illustrator stops at 2; a little room costs nothing.

bool size_ok(double v)
{
    return std::isfinite(v) && v > 0 && v <= MOST;
}

bool shift_ok(double v)
{
    return std::isfinite(v) && v >= 0 && v <= MOST;
}

/// The numbers of an attribute's value, when it is `count` of them and nothing else.
bool numbers(char const *value, double *out, unsigned count)
{
    if (!value) return false;
    for (unsigned i = 0; i < count; ++i) {
        while (*value == ' ') ++value;
        char *end = nullptr;
        out[i] = g_ascii_strtod(value, &end);
        if (end == value) return false;
        value = end;
    }
    while (*value == ' ') ++value;
    return !*value;
}

std::string number(double v)
{
    CSSOStringStream os;
    os << v;
    return os.str();
}

} // namespace

bool TypeSettings::valid() const
{
    return size_ok(superscript_size) && shift_ok(superscript_shift) && size_ok(subscript_size) &&
           shift_ok(subscript_shift) && size_ok(small_caps_size);
}

TypeSettings TypeSettings::of(SPDocument const *document)
{
    TypeSettings out;
    auto const *root = document ? const_cast<SPDocument *>(document)->getReprRoot() : nullptr;
    if (!root) return out;
    double v[2];
    if (numbers(root->attribute(SUPERSCRIPT), v, 2) && size_ok(v[0]) && shift_ok(v[1])) {
        out.superscript_size = v[0];
        out.superscript_shift = v[1];
    }
    if (numbers(root->attribute(SUBSCRIPT), v, 2) && size_ok(v[0]) && shift_ok(v[1])) {
        out.subscript_size = v[0];
        out.subscript_shift = v[1];
    }
    if (numbers(root->attribute(SMALL_CAPS), v, 1) && size_ok(v[0])) {
        out.small_caps_size = v[0];
    }
    return out;
}

void TypeSettings::write(SPDocument *document) const
{
    auto *root = document ? document->getReprRoot() : nullptr;
    if (!root || !valid()) return;
    TypeSettings const usual;
    auto set = [&](char const *name, bool same, std::string const &value) {
        if (same) {
            root->removeAttribute(name);
        } else {
            root->setAttribute(name, value);
        }
    };
    set(SUPERSCRIPT, superscript_size == usual.superscript_size && superscript_shift == usual.superscript_shift,
        number(superscript_size) + " " + number(superscript_shift));
    set(SUBSCRIPT, subscript_size == usual.subscript_size && subscript_shift == usual.subscript_shift,
        number(subscript_size) + " " + number(subscript_shift));
    set(SMALL_CAPS, small_caps_size == usual.small_caps_size, number(small_caps_size));
}

bool TypeSettings::is_attribute(char const *name)
{
    return name && (!std::strcmp(name, SUPERSCRIPT) || !std::strcmp(name, SUBSCRIPT) || !std::strcmp(name, SMALL_CAPS));
}

} // namespace Inkscape
