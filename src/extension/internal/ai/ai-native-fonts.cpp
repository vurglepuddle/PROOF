// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the installed font for a font an Illustrator text document names. See
 * ai-native-fonts.h.
 */
#include "ai-native-fonts.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <fontconfig/fontconfig.h>
#include <glib.h>
#include <hb-ot.h>
#include <pango/pangofc-fontmap.h>

#include "document.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "libnrtype/font-utils.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "object/sp-string.h"
#include "object/sp-text.h"
#include "style.h"
#include "style-text.h"
#include "svg/css-ostringstream.h"
#include "text-editing.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Extension::Internal::AiNative {
namespace {

struct Axis
{
    std::string tag; ///< As the font has it: four characters.
    double min, def, max;
};

/// A variable font's named instance, under the names it may go by.
struct Instance
{
    std::string given;   ///< The PostScript name the font gives it, when it gives one.
    std::string derived; ///< The one made from its style's name.
    std::vector<double> at;
};

struct VariableFont
{
    std::string prefix; ///< What the PostScript names of its instances start with.
    std::vector<Axis> axes;
    std::vector<Instance> instances; ///< In the font's order.
    // Of the face the system lists for the font itself.
    std::string family;
    PangoStyle style = PANGO_STYLE_NORMAL;
};

std::string number(double v)
{
    CSSOStringStream os;
    os << v;
    return os.str();
}

/// What a made-up PostScript name keeps of a name: its ASCII letters and digits.
std::string letters_and_digits(std::string_view s)
{
    std::string out;
    for (char c : s) {
        if (g_ascii_isalnum(c)) out.push_back(c);
    }
    return out;
}

std::string name_of(hb_face_t *face, hb_ot_name_id_t id)
{
    if (id == HB_OT_NAME_ID_INVALID) return {};
    char text[512];
    unsigned size = sizeof text;
    hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, &size, text);
    return std::string(text, std::min<std::size_t>(size, sizeof text - 1));
}

/// The axes and named instances of a font file's face; nothing when it isn't variable.
std::optional<VariableFont> variable_font(char const *file, unsigned face_index)
{
    auto *blob = hb_blob_create_from_file_or_fail(file);
    if (!blob) return {};
    auto *face = hb_face_create(blob, face_index);
    hb_blob_destroy(blob);
    std::optional<VariableFont> out;
    unsigned count = hb_ot_var_get_axis_count(face);
    if (count && count <= 64) {
        std::vector<hb_ot_var_axis_info_t> infos(count);
        hb_ot_var_get_axis_infos(face, 0, &count, infos.data());
        out.emplace();
        for (auto const &info : infos) {
            char tag[5] = {};
            hb_tag_to_string(info.tag, tag);
            out->axes.push_back({tag, info.min_value, info.default_value, info.max_value});
        }
        // Technical Note 5902: the prefix the font names, else its family's name.
        for (auto id : {hb_ot_name_id_t(HB_OT_NAME_ID_VARIATIONS_PS_PREFIX),
                        hb_ot_name_id_t(HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY), hb_ot_name_id_t(HB_OT_NAME_ID_FONT_FAMILY)}) {
            out->prefix = letters_and_digits(name_of(face, id));
            if (!out->prefix.empty()) break;
        }
        auto const instances = std::min(hb_ot_var_get_named_instance_count(face), 4096u);
        for (unsigned i = 0; i < instances; ++i) {
            std::vector<float> at(count);
            unsigned n = count;
            hb_ot_var_named_instance_get_design_coords(face, i, &n, at.data());
            Instance instance;
            if (n == count) instance.at.assign(at.begin(), at.end());
            instance.given = name_of(face, hb_ot_var_named_instance_get_postscript_name_id(face, i));
            auto const style = letters_and_digits(name_of(face, hb_ot_var_named_instance_get_subfamily_name_id(face, i)));
            if (!style.empty()) instance.derived = out->prefix + "-" + style;
            out->instances.push_back(std::move(instance));
        }
    }
    hb_face_destroy(face);
    return out;
}

FcConfig *config()
{
    auto *map = pango_context_get_font_map(FontFactory::get().get_font_context());
    return pango_fc_font_map_get_config(PANGO_FC_FONT_MAP(map));
}

/// The face the system knows by exactly this PostScript name.
FcPattern *exact(std::string const &name)
{
    auto *pattern = FcNameParse(reinterpret_cast<FcChar8 const *>((":postscriptname=" + name).c_str()));
    if (!pattern) return nullptr;
    // These must be called before FcFontMatch, see the fontconfig documentation.
    FcConfigSubstitute(config(), pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    auto *match = FcFontMatch(config(), pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) return nullptr;
    // The nearest font is returned whatever its name.
    FcChar8 *found = nullptr;
    if (FcPatternGetString(match, FC_POSTSCRIPT_NAME, 0, &found) != FcResultMatch || name != reinterpret_cast<char const *>(found)) {
        FcPatternDestroy(match);
        return nullptr;
    }
    return match;
}

/// Every installed variable font. Listed once: opening each font file takes a moment.
std::vector<VariableFont> const &variable_fonts()
{
    static std::vector<VariableFont> const fonts = [] {
        std::vector<VariableFont> out;
        auto *all = FcPatternCreate();
        auto *wanted = FcObjectSetBuild(FC_FILE, FC_INDEX, FC_VARIABLE, FC_FAMILY, FC_FAMILYLANG, FC_STYLE, FC_SLANT,
                                        FC_WEIGHT, FC_WIDTH, nullptr);
        auto *set = FcFontList(config(), all, wanted);
        FcObjectSetDestroy(wanted);
        FcPatternDestroy(all);
        if (!set) return out;
        // A variable font is listed several times: as the font itself, once for each named
        // instance (numbered in the upper half of its index), and once more as a whole.
        struct Listed
        {
            bool variable = false;
            FcPattern *itself = nullptr;
        };
        std::map<std::pair<std::string, int>, Listed> files;
        for (int i = 0; i < set->nfont; ++i) {
            FcChar8 *file = nullptr;
            int index = 0;
            if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch) continue;
            FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &index);
            FcBool whole = FcFalse;
            FcPatternGetBool(set->fonts[i], FC_VARIABLE, 0, &whole);
            auto &listed = files[{reinterpret_cast<char const *>(file), index & 0xffff}];
            if (whole || (index >> 16)) {
                listed.variable = true;
            } else {
                listed.itself = set->fonts[i];
            }
        }
        for (auto const &[key, listed] : files) {
            if (!listed.variable || !listed.itself) continue;
            auto font = variable_font(key.first.c_str(), key.second);
            if (!font || font->prefix.empty()) continue;
            auto *desc = pango_fc_font_description_from_pattern(listed.itself, false);
            if (auto const *family = desc ? sp_font_description_get_family(desc) : nullptr) {
                font->family = family;
                font->style = pango_font_description_get_style(desc);
                out.push_back(std::move(*font));
            }
            if (desc) pango_font_description_free(desc);
        }
        FcFontSetDestroy(set);
        return out;
    }();
    return fonts;
}

/// The axis values a made-up name spells out after the prefix: "_", a value and an axis's
/// tag, for each axis that is off its default.
std::optional<std::vector<double>> spelled_out(std::string_view rest, std::vector<Axis> const &axes)
{
    std::vector<double> out;
    for (auto const &axis : axes) out.push_back(axis.def);
    if (rest.empty()) return {};
    while (!rest.empty()) {
        if (rest.front() != '_') return {};
        rest.remove_prefix(1);
        auto const piece = rest.substr(0, rest.find('_'));
        rest.remove_prefix(piece.size());
        std::size_t digits = 0;
        while (digits < piece.size() && (g_ascii_isdigit(piece[digits]) || piece[digits] == '.' || (!digits && piece[digits] == '-'))) ++digits;
        if (!digits || digits == piece.size()) return {};
        auto const text = std::string(piece.substr(0, digits));
        char *end = nullptr;
        auto const value = g_ascii_strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || !std::isfinite(value)) return {};
        auto const axis = std::find_if(axes.begin(), axes.end(), [&](Axis const &a) {
            return letters_and_digits(a.tag) == piece.substr(digits);
        });
        if (axis == axes.end()) return {};
        out[axis - axes.begin()] = value;
    }
    return out;
}

/// The instance of `font` that `name` and `design` say, as a value for each axis.
std::optional<std::vector<double>> instance_of(VariableFont const &font, std::string const &name,
                                              std::vector<double> const &design)
{
    std::optional<std::vector<double>> at;
    if (design.size() == font.axes.size()) {
        at = design;
    } else {
        for (auto const &instance : font.instances) {
            if (!instance.at.empty() && (instance.given == name || instance.derived == name)) at = instance.at;
        }
        if (!at && name.size() > font.prefix.size()) {
            at = spelled_out(std::string_view(name).substr(font.prefix.size()), font.axes);
        }
    }
    if (!at) return {};
    for (std::size_t i = 0; i < at->size(); ++i) {
        if (!std::isfinite((*at)[i])) return {};
        // A font keeps these in 16.16 fixed point: a ten-thousandth is as fine as they get.
        (*at)[i] = std::clamp(std::round((*at)[i] * 1e4) / 1e4, font.axes[i].min, font.axes[i].max);
    }
    return at;
}

/// Whether `face` is this instance: Pango chooses a family's face by weight, width and
/// slant, and a family can hold ordinary fonts beside its variable one.
bool is_instance(FontInstance const &face, VariableFont const &font, std::vector<double> const &at)
{
    auto *hb = pango_font_get_hb_font(face.get_font());
    if (!hb) return false;
    unsigned count = hb_ot_var_get_axis_count(hb_font_get_face(hb));
    if (count != font.axes.size()) return false;
    std::vector<hb_ot_var_axis_info_t> infos(count);
    hb_ot_var_get_axis_infos(hb_font_get_face(hb), 0, &count, infos.data());
    unsigned set = 0;
    auto const *got = hb_font_get_var_coords_design(hb, &set);
    if (set != count || !got) return false;
    for (unsigned i = 0; i < count; ++i) {
        char tag[5] = {};
        hb_tag_to_string(infos[i].tag, tag);
        if (font.axes[i].tag != tag || std::abs(got[i] - at[i]) > 0.01 + 1e-4 * std::abs(at[i])) return false;
    }
    return true;
}

PangoStretch stretch_of(double width)
{
    constexpr std::pair<double, PangoStretch> named[] = {
        {50, PANGO_STRETCH_ULTRA_CONDENSED},  {62.5, PANGO_STRETCH_EXTRA_CONDENSED}, {75, PANGO_STRETCH_CONDENSED},
        {87.5, PANGO_STRETCH_SEMI_CONDENSED}, {100, PANGO_STRETCH_NORMAL},           {112.5, PANGO_STRETCH_SEMI_EXPANDED},
        {125, PANGO_STRETCH_EXPANDED},        {150, PANGO_STRETCH_EXTRA_EXPANDED},   {200, PANGO_STRETCH_ULTRA_EXPANDED}};
    auto best = named[4];
    for (auto const &n : named) {
        if (std::abs(n.first - width) < std::abs(best.first - width)) best = n;
    }
    return best.second;
}

/// The declarations for `desc`, and for the instance of a variable font its axis values.
std::string declarations(PangoFontDescription *desc, std::map<std::string, double> const &variations)
{
    Glib::ustring family = sp_font_description_get_family(desc);
    css_font_family_quote(family);
    auto *spec = pango_font_description_to_string(desc);
    Glib::ustring specification = spec ? spec : "";
    g_free(spec);
    // As the font controls write an instance: its weight apart from the other axes.
    if (!variations.empty()) specification = canonize_fontspec(specification);
    css_quote(specification);
    auto css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "font-family", family.c_str());
    sp_repr_css_set_property(css, "-inkscape-font-specification", specification.c_str());
    sp_repr_css_set_property(css, "font-weight", std::to_string(pango_font_description_get_weight(desc)).c_str());
    auto const slant = pango_font_description_get_style(desc);
    sp_repr_css_set_property(css, "font-style", slant == PANGO_STYLE_ITALIC ? "italic" : slant == PANGO_STYLE_OBLIQUE ? "oblique" : "normal");
    constexpr char const *stretch[] = {"ultra-condensed", "extra-condensed", "condensed", "semi-condensed", "normal",
                                      "semi-expanded", "expanded", "extra-expanded", "ultra-expanded"};
    sp_repr_css_set_property(css, "font-stretch", stretch[std::clamp(int(pango_font_description_get_stretch(desc)), 0, 8)]);
    if (!variations.empty()) {
        std::string settings;
        for (auto const &[tag, value] : variations) settings += (settings.empty() ? "'" : ", '") + tag + "' " + number(value);
        sp_repr_css_set_property(css, "font-variation-settings", settings.c_str());
    }
    Glib::ustring result;
    sp_repr_css_write_string(css, result);
    sp_repr_css_attr_unref(css);
    return result.empty() ? std::string{} : result.raw() + ";";
}

std::shared_ptr<FontInstance> load(PangoFontDescription *desc)
{
    try {
        return FontFactory::get().Face(desc, false);
    } catch (std::runtime_error const &) {
        return {};
    }
}

/// `desc` made the instance of `font` at these axis values, and loaded.
std::optional<FontMatch> instance(PangoFontDescription *desc, VariableFont const &font, std::vector<double> const &at)
{
    // Every axis is said, whatever its value: the face Pango picks for the family can be a
    // named instance, which starts from that instance's values.
    std::map<std::string, double> variations;
    std::string pango;
    for (std::size_t i = 0; i < at.size(); ++i) {
        auto const &tag = font.axes[i].tag;
        variations[tag] = at[i];
        pango += (pango.empty() ? "" : ",") + tag + "=" + number(at[i]);
        // The properties that say the same as an axis follow it.
        if (tag == "wght") pango_font_description_set_weight(desc, PangoWeight(std::clamp(int(std::lround(at[i])), 1, 1000)));
        if (tag == "wdth") pango_font_description_set_stretch(desc, stretch_of(at[i]));
    }
    pango_font_description_set_variations(desc, pango.c_str());
    FontMatch out;
    out.variable = true;
    out.css = declarations(desc, variations);
    out.face = load(desc);
    if (out.css.empty() || !out.face || !is_instance(*out.face, font, at)) return {};
    return out;
}
} // namespace

std::optional<FontMatch> find_font(std::string const &name, std::vector<double> const &design)
{
    std::optional<FontMatch> out;
    if (auto *match = exact(name)) {
        auto *desc = pango_fc_font_description_from_pattern(match, false);
        FcChar8 *file = nullptr;
        int index = 0;
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        std::optional<VariableFont> font;
        if (desc && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            font = variable_font(reinterpret_cast<char const *>(file), index & 0xffff);
        }
        // The face Pango loads for the font's family, weight, width and slant.
        auto ordinary = [&] {
            out.emplace();
            out->css = declarations(desc, {});
            out->face = load(desc);
            if (out->css.empty() || !out->face) out.reset();
        };
        if (!desc || !sp_font_description_get_family(desc)) {
            // Nothing Pango can load.
        } else if (!font) {
            ordinary();
        } else {
            // The name is the font's own, or one of its named instances: the instance the
            // system numbers it, unless the text document says where on the axes it is.
            std::optional<std::vector<double>> at;
            if (design.size() == font->axes.size()) {
                at = instance_of(*font, name, design);
            } else if (unsigned const numbered = index >> 16; numbered && numbered <= font->instances.size()) {
                at = instance_of(*font, name, font->instances[numbered - 1].at);
            } else {
                at.emplace();
                for (auto const &axis : font->axes) at->push_back(axis.def);
            }
            if (at && at->size() == font->axes.size()) out = instance(desc, *font, *at);
            if (!out) {
                // The family also has an ordinary font of this weight, width and slant, and
                // Pango loads that one: it is the font by this name, as it was before
                // instances were told apart.
                pango_font_description_free(desc);
                desc = pango_fc_font_description_from_pattern(match, false);
                if (desc && sp_font_description_get_family(desc)) ordinary();
            }
        }
        if (desc) pango_font_description_free(desc);
        FcPatternDestroy(match);
        return out;
    }

    // No face has the name: an instance of a variable font, named after the font's prefix.
    VariableFont const *best = nullptr;
    std::vector<double> best_at;
    bool const italic = name.find("Italic") != std::string::npos || name.find("Oblique") != std::string::npos;
    for (auto const &font : variable_fonts()) {
        auto const n = font.prefix.size();
        if (name.compare(0, n, font.prefix) != 0 || (name.size() > n && name[n] != '-' && name[n] != '_')) continue;
        auto at = instance_of(font, name, design);
        if (!at) continue;
        // A family in two files, upright and italic, can share its prefix: the instance's
        // own name tells them apart, and failing that the slant it mentions.
        bool const named = std::any_of(font.instances.begin(), font.instances.end(), [&](Instance const &i) {
            return i.given == name || i.derived == name;
        });
        bool const slanted = font.style != PANGO_STYLE_NORMAL;
        if (!best || named || (slanted == italic && (best->style != PANGO_STYLE_NORMAL) != italic)) {
            best = &font;
            best_at = std::move(*at);
        }
        if (named) break;
    }
    if (!best) return {};
    auto *desc = pango_font_description_new();
    pango_font_description_set_family(desc, best->family.c_str());
    pango_font_description_set_style(desc, best->style);
    out = instance(desc, *best, best_at);
    pango_font_description_free(desc);
    return out;
}

namespace {

/// The properties that say which font sets a text.
constexpr char const *FONT_PROPERTIES[] = {"font-family", "-inkscape-font-specification", "font-weight",
                                          "font-style", "font-stretch", "font-variation-settings"};

/// `declarations` without the one named.
std::string without(std::string const &declarations, char const *property)
{
    auto *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, declarations.c_str());
    sp_repr_css_unset_property(css, property);
    Glib::ustring result;
    sp_repr_css_write_string(css, result);
    sp_repr_css_attr_unref(css);
    return result.empty() ? std::string{} : result.raw() + ";";
}

/// Whether the element's own style says which font sets it.
bool declares_font(XML::Node const *node)
{
    auto *css = sp_repr_css_attr(node, "style");
    bool const declares = sp_repr_css_property(css, "font-family", nullptr) || sp_repr_css_property(css, "font", nullptr) ||
                          sp_repr_css_property(css, "-inkscape-font-specification", nullptr);
    sp_repr_css_attr_unref(css);
    return declares;
}

/// The declarations of `chosen` put in place of the element's own font, which then names
/// no font that isn't installed.
void set_font(XML::Node *node, SPCSSAttr const *chosen)
{
    auto *css = sp_repr_css_attr(node, "style");
    for (auto const *property : FONT_PROPERTIES) sp_repr_css_unset_property(css, property);
    sp_repr_css_merge(css, const_cast<SPCSSAttr *>(chosen));
    sp_repr_css_set(node, css, "style");
    sp_repr_css_attr_unref(css);
    node->removeAttribute(FONT_MISSING_ATTRIBUTE);
    node->removeAttribute(FONT_MISSING_AXES_ATTRIBUTE);
}

/// Every element of the document that names a font that isn't installed.
std::vector<XML::Node *> naming_fonts(SPDocument *document)
{
    std::vector<XML::Node *> out;
    if (!document || !document->getReprRoot()) return out;
    sp_repr_visit_descendants(document->getReprRoot(), [&](XML::Node *node) {
        if (node->attribute(FONT_MISSING_ATTRIBUTE)) out.push_back(node);
        return true;
    });
    return out;
}

} // namespace

std::optional<PostScriptFont> postscript_font(SPStyle const *style, std::string &why)
{
    auto *asked = ink_font_description_from_style(style);
    if (!asked) {
        why = "type whose font can't be found";
        return {};
    }
    // The first family the style names: the one its type is in when that is installed.
    std::string first = sp_font_description_get_family(asked) ? sp_font_description_get_family(asked) : "";
    first = first.substr(0, first.find(','));
    first.erase(0, first.find_first_not_of(" \t'\""));
    first.erase(first.find_last_not_of(" \t'\"") + 1);
    // A generic family is whatever font the system gives for it.
    bool const generic = std::ranges::any_of(std::array{"serif", "sans-serif", "sans", "monospace", "cursive", "fantasy", "system-ui"},
                                             [&](char const *g) { return g_ascii_strcasecmp(g, first.c_str()) == 0; });
    auto const face = load(asked);
    auto *got = face && face->get_font() ? pango_font_describe(face->get_font()) : nullptr;
    std::optional<PostScriptFont> out;
    if (!got) {
        why = "type whose font can't be loaded";
    } else if (auto const *family = sp_font_description_get_family(got);
               !generic && (!family || g_ascii_strcasecmp(family, first.c_str()) != 0)) {
        why = "type in a font that isn't installed (" + first + ")";
    } else if (pango_font_description_get_weight(asked) - pango_font_description_get_weight(got) >= 200) {
        why = "type made bold for a font that has no bold (" + first + ")";
    } else if (pango_font_description_get_style(asked) != PANGO_STYLE_NORMAL &&
               pango_font_description_get_style(got) == PANGO_STYLE_NORMAL) {
        why = "type slanted for a font that has no italic (" + first + ")";
    } else if (auto *hb = pango_font_get_hb_font(face->get_font()); !hb) {
        why = "type whose font can't be loaded";
    } else {
        auto *hb_face = hb_font_get_face(hb);
        auto const name = name_of(hb_face, HB_OT_NAME_ID_POSTSCRIPT_NAME);
        if (hb_ot_var_has_data(hb_face)) {
            // Which of its instances Illustrator would take for the name isn't qualified.
            why = "type in a variable font (" + first + ")";
        } else if (name.empty()) {
            why = "type in a font without a PostScript name (" + first + ")";
        } else {
            auto *outlines = hb_face_reference_table(hb_face, HB_TAG('g', 'l', 'y', 'f'));
            out = PostScriptFont{name, hb_blob_get_length(outlines) > 0};
            hb_blob_destroy(outlines);
        }
    }
    if (got) pango_font_description_free(got);
    pango_font_description_free(asked);
    return out;
}

/// Still set in the font the import gave it in place of its own.
bool stands_in(XML::Node const *node)
{
    if (!node || !node->attribute(FONT_MISSING_ATTRIBUTE)) return false;
    auto *css = sp_repr_css_attr(node, "style");
    bool const untouched = !sp_repr_css_property(css, "-inkscape-font-specification", nullptr);
    sp_repr_css_attr_unref(css);
    return untouched;
}

std::optional<FontMatch> substitute_font()
{
    std::optional<FontMatch> out;
    for (auto const *name : {"MyriadPro-Regular", "ArialMT"}) {
        if (!out) out = find_font(name);
    }
    if (!out) {
        out.emplace();
        auto *desc = pango_font_description_from_string("sans-serif");
        out->css = declarations(desc, {});
        out->face = FontFactory::get().Face(desc);
        pango_font_description_free(desc);
    }
    out->css = without(out->css, "-inkscape-font-specification");
    if (out->css.empty() || !out->face) return {};
    return out;
}

std::vector<MissingFont> missing_fonts(SPDocument *document)
{
    std::vector<MissingFont> out;
    for (auto *node : naming_fonts(document)) {
        if (!stands_in(node)) continue;
        std::string const name = node->attribute(FONT_MISSING_ATTRIBUTE);
        auto font = std::find_if(out.begin(), out.end(), [&](MissingFont const &f) { return f.name == name; });
        if (font == out.end()) font = out.insert(out.end(), MissingFont{name, {}});
        // The text object the element is of.
        SPObject *object = document->getObjectByRepr(node);
        while (object && !is_top_level_text_object(object)) object = object->parent;
        auto *text = cast<SPItem>(object);
        if (text && std::find(font->texts.begin(), font->texts.end(), text) == font->texts.end()) font->texts.push_back(text);
    }
    return out;
}

unsigned restore_fonts(SPDocument *document)
{
    unsigned restored = 0;
    for (auto *node : naming_fonts(document)) {
        std::optional<FontMatch> font;
        if (stands_in(node)) {
            std::vector<double> design;
            if (auto const *axes = node->attribute(FONT_MISSING_AXES_ATTRIBUTE)) {
                gchar **values = g_strsplit(axes, " ", 65);
                for (gchar **v = values; *v; ++v) {
                    if (**v) design.push_back(g_ascii_strtod(*v, nullptr));
                }
                g_strfreev(values);
            }
            font = find_font(node->attribute(FONT_MISSING_ATTRIBUTE), design);
            if (!font) continue; // Still not installed.
            auto *css = sp_repr_css_attr_new();
            sp_repr_css_attr_add_from_string(css, font->css.c_str());
            set_font(node, css);
            sp_repr_css_attr_unref(css);
            ++restored;
        }
        // Its own font again, or one chosen by hand: either way no longer one standing in.
        node->removeAttribute(FONT_MISSING_ATTRIBUTE);
        node->removeAttribute(FONT_MISSING_AXES_ATTRIBUTE);
    }
    return restored;
}

unsigned replace_font(SPDocument *document, std::string const &name, std::string const &declarations)
{
    auto *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, declarations.c_str());
    unsigned replaced = 0;
    // Without these the text would be no font's, or go on counting as one standing in.
    if (sp_repr_css_property(css, "font-family", nullptr) && sp_repr_css_property(css, "-inkscape-font-specification", nullptr)) {
        // What holds the text may be an instance of a variable font: an ordinary font
        // says it has no axes of its own to be set.
        if (!sp_repr_css_property(css, "font-variation-settings", nullptr)) {
            sp_repr_css_set_property(css, "font-variation-settings", "normal");
        }
        for (auto *node : naming_fonts(document)) {
            if (!stands_in(node) || name != node->attribute(FONT_MISSING_ATTRIBUTE)) continue;
            set_font(node, css);
            ++replaced;
        }
    }
    sp_repr_css_attr_unref(css);
    return replaced;
}

std::vector<std::pair<unsigned, unsigned>> standing_in(SPItem const *text)
{
    std::vector<std::pair<unsigned, unsigned>> out;
    if (!text) return out;
    // An element's font sets what is inside it, down to an element that says a font again.
    auto visit = [&](auto const &self, SPObject const *object, bool standing) -> void {
        if (auto const *string = cast<SPString>(object)) {
            if (!standing || string->string.empty()) return;
            unsigned const start = sp_text_get_length_upto(text, string);
            unsigned const end = start + string->string.length();
            if (!out.empty() && out.back().second == start) {
                out.back().second = end;
            } else {
                out.emplace_back(start, end);
            }
            return;
        }
        if (auto const *node = object->getRepr()) {
            if (node->attribute(FONT_MISSING_ATTRIBUTE)) {
                standing = stands_in(node);
            } else if (declares_font(node)) {
                standing = false;
            }
        }
        for (auto const &child : object->children) self(self, &child, standing);
    };
    visit(visit, text, false);
    return out;
}

} // namespace Inkscape::Extension::Internal::AiNative
