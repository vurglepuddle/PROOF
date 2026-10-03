// SPDX-License-Identifier: GPL-2.0-or-later
#include "ctrl-handle-styling.h"

#include <optional>
#include <ranges>
#include <boost/functional/hash.hpp>
#include <glibmm/fileutils.h>
#include <glibmm/i18n.h>
#include <glibmm/regex.h>

#include "3rdparty/libcroco/src/cr-selector.h"
#include "3rdparty/libcroco/src/cr-doc-handler.h"
#include "3rdparty/libcroco/src/cr-string.h"
#include "3rdparty/libcroco/src/cr-term.h"
#include "3rdparty/libcroco/src/cr-parser.h"
#include "3rdparty/libcroco/src/cr-rgb.h"
#include "3rdparty/libcroco/src/cr-utils.h"

#include "display/cairo-utils.h" // argb32_from_rgba()
#include "display/control/canvas-item-enums.h"
#include "io/resource.h"
#include "util/delete-with.h"
using Inkscape::Util::delete_with;

namespace Inkscape::Handles {
namespace {

/**
 * State needed for parsing (between functions).
 */
struct ParsingState
{
    Css &result;
    std::vector<std::pair<Style *, int>> selected_handles;
};

/**
 * Get the parsing state from the document handler.
 */
ParsingState &get_parsing_state(CRDocHandler *a_handler)
{
    return *reinterpret_cast<ParsingState *>(a_handler->app_data);
}

/**
 * Conversion maps for ctrl types (CSS parsing).
 */
std::unordered_map<std::string, CanvasItemCtrlType> const ctrl_type_map = {
    {"*", CANVAS_ITEM_CTRL_TYPE_DEFAULT},
    {".inkscape-adj-handle", CANVAS_ITEM_CTRL_TYPE_ADJ_HANDLE},
    {".inkscape-adj-skew", CANVAS_ITEM_CTRL_TYPE_ADJ_SKEW},
    {".inkscape-adj-rotate", CANVAS_ITEM_CTRL_TYPE_ADJ_ROTATE},
    {".inkscape-adj-center", CANVAS_ITEM_CTRL_TYPE_ADJ_CENTER},
    {".inkscape-adj-salign", CANVAS_ITEM_CTRL_TYPE_ADJ_SALIGN},
    {".inkscape-adj-calign", CANVAS_ITEM_CTRL_TYPE_ADJ_CALIGN},
    {".inkscape-adj-malign", CANVAS_ITEM_CTRL_TYPE_ADJ_MALIGN},
    {".inkscape-anchor", CANVAS_ITEM_CTRL_TYPE_ANCHOR},
    {".inkscape-point", CANVAS_ITEM_CTRL_TYPE_POINT},
    {".inkscape-rotate", CANVAS_ITEM_CTRL_TYPE_ROTATE},
    {".inkscape-margin", CANVAS_ITEM_CTRL_TYPE_MARGIN},
    {".inkscape-center", CANVAS_ITEM_CTRL_TYPE_CENTER},
    {".inkscape-sizer", CANVAS_ITEM_CTRL_TYPE_SIZER},
    {".inkscape-shaper", CANVAS_ITEM_CTRL_TYPE_SHAPER},
    {".inkscape-marker", CANVAS_ITEM_CTRL_TYPE_MARKER},
    {".inkscape-lpe", CANVAS_ITEM_CTRL_TYPE_LPE},
    {".inkscape-node-auto", CANVAS_ITEM_CTRL_TYPE_NODE_AUTO},
    {".inkscape-node-cusp", CANVAS_ITEM_CTRL_TYPE_NODE_CUSP},
    {".inkscape-node-smooth", CANVAS_ITEM_CTRL_TYPE_NODE_SMOOTH},
    {".inkscape-node-symmetrical", CANVAS_ITEM_CTRL_TYPE_NODE_SYMMETRICAL},
    {".inkscape-mesh", CANVAS_ITEM_CTRL_TYPE_MESH},
    {".inkscape-invisible", CANVAS_ITEM_CTRL_TYPE_INVISIPOINT},
    {".inkscape-guide-handle", CANVAS_ITEM_CTRL_TYPE_GUIDE_HANDLE},
    {".inkscape-pointer", CANVAS_ITEM_CTRL_TYPE_POINTER},
    {".inkscape-move", CANVAS_ITEM_CTRL_TYPE_MOVE},
    {".proof-box-corner", CANVAS_ITEM_CTRL_TYPE_BOX_CORNER},
    {".proof-box-center", CANVAS_ITEM_CTRL_TYPE_BOX_CENTER},
    {".proof-box-radius", CANVAS_ITEM_CTRL_TYPE_BOX_RADIUS},
    {".inkscape-selection-rect", RUBBERBAND_RECT},
    {".inkscape-selection-lasso", RUBBERBAND_TOUCHPATH},
    {".inkscape-selection-path.selector", RUBBERBAND_TOUCHPATH_SELECT},
    {".inkscape-selection-path.eraser", RUBBERBAND_TOUCHPATH_ERASER},
    {".inkscape-selection-path.paint-bucket", RUBBERBAND_TOUCHPATH_FLOOD},
    {".inkscape-selection-touchrect", RUBBERBAND_TOUCHRECT},
    {".inkscape-selection-deselect", RUBBERBAND_DESELECT},
    {".inkscape-selection-deselect.selector", RUBBERBAND_TOUCHPATH_DESELECT},
    {".inkscape-selection-invert", RUBBERBAND_INVERT},
    {".inkscape-selection-invert.selector", RUBBERBAND_TOUCHPATH_INVERT},
};

/**
 * Conversion maps for ctrl shapes (CSS parsing).
 */
std::unordered_map<std::string, CanvasItemCtrlShape> const ctrl_shape_map = {
    {"'square'", CANVAS_ITEM_CTRL_SHAPE_SQUARE},
    {"'diamond'", CANVAS_ITEM_CTRL_SHAPE_DIAMOND},
    {"'circle'", CANVAS_ITEM_CTRL_SHAPE_CIRCLE},
    {"'triangle'", CANVAS_ITEM_CTRL_SHAPE_TRIANGLE},
    {"'triangle-angled'", CANVAS_ITEM_CTRL_SHAPE_TRIANGLE_ANGLED},
    {"'cross'", CANVAS_ITEM_CTRL_SHAPE_CROSS},
    {"'plus'", CANVAS_ITEM_CTRL_SHAPE_PLUS},
    {"'plus'", CANVAS_ITEM_CTRL_SHAPE_PLUS},
    {"'pivot'", CANVAS_ITEM_CTRL_SHAPE_PIVOT},
    {"'arrow'", CANVAS_ITEM_CTRL_SHAPE_DARROW},
    {"'skew-arrow'", CANVAS_ITEM_CTRL_SHAPE_SARROW},
    {"'curved-arrow'", CANVAS_ITEM_CTRL_SHAPE_CARROW},
    {"'side-align'", CANVAS_ITEM_CTRL_SHAPE_SALIGN},
    {"'corner-align'", CANVAS_ITEM_CTRL_SHAPE_CALIGN},
    {"'middle-align'", CANVAS_ITEM_CTRL_SHAPE_MALIGN}
};

struct Exception
{
    Glib::ustring msg;
};

void log_error(Glib::ustring const &err, CRParsingLocation const &loc)
{
    std::cerr << loc.line << ':' << loc.column << ": " << err << std::endl;
}

std::string get_string(CRTerm const *term)
{
    auto const cstr = delete_with<g_free>(cr_term_to_string(term));
    if (!cstr) {
        throw Exception{_("Empty or improper value, skipped")};
    }
    return reinterpret_cast<char *>(cstr.get());
}

CanvasItemCtrlShape parse_shape(CRTerm const *term)
{
    auto const str = get_string(term);
    auto const it = ctrl_shape_map.find(str);
    if (it == ctrl_shape_map.end()) {
        throw Exception{Glib::ustring::compose(_("Unrecognized shape '%1'"), str)};
    }
    return it->second;
}

uint32_t parse_rgb(CRTerm const *term)
{
    auto const rgb = delete_with<cr_rgb_destroy>(cr_rgb_new());
    auto const status = cr_rgb_set_from_term(rgb.get(), term);
    if (status != CR_OK) {
        throw Exception{Glib::ustring::compose(_("Unrecognized color '%1'"), get_string(term))};
    }
    return Display::AssembleARGB32(255, rgb->red, rgb->green, rgb->blue);
}

float parse_opacity(CRTerm const *term)
{
    auto const num = term->content.num;
    if (!num) {
        throw Exception{Glib::ustring::compose(_("Invalid opacity '%1'"), get_string(term))};
    }

    double value;
    if (num->type == NUM_PERCENTAGE) {
        value = num->val / 100.0f;
    } else if (num->type == NUM_GENERIC) {
        value = num->val;
    } else {
        throw Exception{Glib::ustring::compose(_("Invalid opacity units '%1'"), get_string(term))};
    }

    if (value > 1 || value < 0) {
        throw Exception{Glib::ustring::compose(_("Opacity '%1' out of range"), get_string(term))};
    }

    return value;
}

float parse_width(CRTerm const *term)
{
    // Assuming px value only, which stays the same regardless of the size of the handles.
    auto const num = term->content.num;
    if (!num) {
        throw Exception{Glib::ustring::compose(_("Invalid width '%1'"), get_string(term))};
    }

    float value;
    if (num->type == NUM_LENGTH_PX) {
        value = static_cast<float>(num->val);
    } else {
        throw Exception{Glib::ustring::compose(_("Invalid width units '%1'"), get_string(term))};
    }

    return value;
}

float parse_scale(CRTerm const *term)
{
    auto const num = term->content.num;
    if (!num) {
        throw Exception{Glib::ustring::compose(_("Invalid scale '%1'"), get_string(term))};
    }

    double value;
    if (num->type == NUM_PERCENTAGE) {
        value = num->val / 100.0f;
    } else if (num->type == NUM_GENERIC) {
        value = num->val;
    } else {
        throw Exception{Glib::ustring::compose(_("Invalid scale units '%1'"), get_string(term))};
    }

    if (value > 100 || value < 0) {
        throw Exception{Glib::ustring::compose(_("Scale '%1' out of range"), get_string(term))};
    }

    return value;
}

template <auto parse, auto member>
auto setter = +[] (CRDocHandler *handler, CRTerm const *term, bool important)
{
    auto &state = get_parsing_state(handler);
    auto const value = parse(term);
    for (auto &[handle, specificity] : state.selected_handles) {
        (handle->*member).setProperty(value, specificity + 100000 * important);
    }
};

/**
 * Lookup table for setting properties.
 */
std::unordered_map<std::string, void(*)(CRDocHandler *, CRTerm const *, bool)> const property_map = {
    {"shape",           setter<parse_shape,   &Style::shape>},
    {"fill",            setter<parse_rgb,     &Style::fill>},
    {"stroke",          setter<parse_rgb,     &Style::stroke>},
    {"outline",         setter<parse_rgb,     &Style::outline>},
    {"opacity",         setter<parse_opacity, &Style::opacity>},
    {"fill-opacity",    setter<parse_opacity, &Style::fill_opacity>},
    {"stroke-opacity",  setter<parse_opacity, &Style::stroke_opacity>},
    {"outline-opacity", setter<parse_opacity, &Style::outline_opacity>},
    {"stroke-width",    setter<parse_width,   &Style::stroke_width>},
    {"outline-width",   setter<parse_width,   &Style::outline_width>},
    {"scale",           setter<parse_scale,   &Style::scale>},
    {"size-extra",      setter<parse_width,   &Style::size_extra>},
    {"stroke-scale",    setter<parse_scale,   &Style::stroke_scale>},
};

/**
 * Parses the CSS selector for handles.
 */
std::optional<std::pair<TypeState, int>> configure_selector(CRSelector *a_selector)
{
    auto log_unrecognised = [&] (char const *selector) {
        log_error(Glib::ustring::compose(_("Unrecognized selector '%1'"), selector),
                  a_selector->location);
    };

    cr_simple_sel_compute_specificity(a_selector->simple_sel);
    int specificity = a_selector->simple_sel->specificity;

    auto const selector_str = reinterpret_cast<char const *>(cr_simple_sel_one_to_string(a_selector->simple_sel));
    auto const tokens = Glib::Regex::split_simple(":", selector_str);
    auto const type_it = tokens.empty() ? ctrl_type_map.end() : ctrl_type_map.find(tokens.front());
    if (type_it == ctrl_type_map.end()) {
        log_unrecognised(selector_str);
        return {};
    }

    auto selector = TypeState{type_it->second};
    for (auto &tok : tokens | std::views::drop(1)) {
        if (tok == "*") {
            continue;
        } else if (tok == "selected") {
            selector.selected = true;
        } else if (tok == "hover") {
            specificity++;
            selector.hover = true;
        } else if (tok == "click") {
            specificity++;
            selector.click = true;
        } else {
            log_unrecognised(tok.c_str());
            return {};
        }
    }

    return {{ selector, specificity }};
}

bool fits(TypeState const &selector, TypeState const &handle)
{
    // Type must match for non-default selectors.
    if (selector.type != CANVAS_ITEM_CTRL_TYPE_DEFAULT && selector.type != handle.type) {
        return false;
    }
    // Any state set in selector must be set in handle.
    return !((selector.selected && !handle.selected) ||
             (selector.hover && !handle.hover) ||
             (selector.click && !handle.click));
}

/**
 * Selects fitting handles from all handles based on selector.
 */
void set_selectors(CRDocHandler *a_handler, CRSelector *a_selector, bool is_users)
{
    auto &state = get_parsing_state(a_handler);
    while (a_selector) {
        if (auto const ret = configure_selector(a_selector)) {
            auto const &[selector, specificity] = *ret;
            for (auto &[handle, style] : state.result.style_map) {
                if (fits(selector, handle)) {
                    state.selected_handles.emplace_back(&style, specificity + 10000 * is_users);
                }
            }
        }
        a_selector = a_selector->next;
    }
}

template <bool is_users>
void set_selectors(CRDocHandler *a_handler, CRSelector *a_selector)
{
    set_selectors(a_handler, a_selector, is_users);
}

/**
 * Parse and set the properties for selected handles.
 */
void set_properties(CRDocHandler *a_handler, CRString *a_name, CRTerm *a_value, gboolean a_important)
{
    auto log_error_local = [&] (Glib::ustring const &err) {
        log_error(err, a_value->location);
    };

    auto const property = cr_string_peek_raw_str(a_name);
    if (!property) {
        log_error_local(_("Empty or improper property, skipped."));
        return;
    }

    auto const it = property_map.find(property);
    if (it == property_map.end()) {
        log_error_local(Glib::ustring::compose(_("Unrecognized property '%1'"), property));
        return;
    }

    try {
        it->second(a_handler, a_value, a_important);
    } catch (Exception const &e) {
        log_error_local(e.msg);
    }
}

/**
 * Clean-up for selected handles vector.
 */
void clear_selectors(CRDocHandler *a_handler, CRSelector *a_selector)
{
    auto &state = get_parsing_state(a_handler);
    state.selected_handles.clear();
}

uint32_t combine_rgb_a(uint32_t rgb, float a)
{
    EXTRACT_ARGB32(rgb, _, r, g, b)
    return Display::AssembleARGB32(r, g, b, a * 255);
}

} // namespace

uint32_t Style::getFill() const
{
    return combine_rgb_a(fill(), fill_opacity() * opacity());
}

uint32_t Style::getStroke() const
{
    return combine_rgb_a(stroke(), stroke_opacity() * opacity());
}

uint32_t Style::getOutline() const
{
    return combine_rgb_a(outline(), outline_opacity() * opacity());
}

Css parse_css(const std::string& css_file_name)
{
    Css result;

    for (int type_i = CANVAS_ITEM_CTRL_TYPE_DEFAULT; type_i < LAST_ITEM_CANVAS_ITEM_CTRL_TYPE; type_i++) {
        auto type = static_cast<CanvasItemCtrlType>(type_i);
        for (auto state_bits = 0; state_bits < 8; state_bits++) {
            bool selected = state_bits & (1 << 2);
            bool hover = state_bits & (1 << 1);
            bool click = state_bits & (1 << 0);
            result.style_map[TypeState{type, selected, hover, click}] = {};
        }
    }

    ParsingState state{result};

    auto sac = delete_with<cr_doc_handler_destroy>(cr_doc_handler_new());
    sac->app_data = &state;
    sac->property = set_properties;
    sac->end_selector = clear_selectors;

    auto parse = [&] (IO::Resource::Domain domain) {
        auto const css_path = IO::Resource::get_path_string(domain, IO::Resource::UIS, css_file_name.c_str());
        if (Glib::file_test(css_path, Glib::FileTest::EXISTS)) {
            auto parser = delete_with<cr_parser_destroy>(cr_parser_new_from_file(reinterpret_cast<unsigned char const *>(css_path.c_str()), CR_UTF_8));
            cr_parser_set_sac_handler(parser.get(), sac.get());
            cr_parser_parse(parser.get());
        }
    };

    auto import = +[](CRDocHandler* a_handler, GList* a_media_list, CRString* a_uri, CRString* a_uri_default_ns, CRParsingLocation* a_location){
        g_return_if_fail(a_handler && a_uri && a_uri->stryng && a_uri->stryng->str);
        auto domain = IO::Resource::SYSTEM; // import files form installation folder
        auto fname = a_uri->stryng->str;
        auto const css_path = IO::Resource::get_path_string(domain, IO::Resource::UIS, fname);
        if (Glib::file_test(css_path, Glib::FileTest::EXISTS)) {
            auto parser = delete_with<cr_parser_destroy>(cr_parser_new_from_file(reinterpret_cast<unsigned char const *>(css_path.c_str()), CR_UTF_8));
            cr_parser_set_sac_handler(parser.get(), a_handler);
            cr_parser_parse(parser.get());
        }
    };

    sac->import_style = import;

    sac->start_selector = set_selectors<false>;
    parse(IO::Resource::SYSTEM);

    sac->start_selector = set_selectors<true>;
    parse(IO::Resource::USER);

    return result;
}

} // namespace Inkscape::Handles

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
