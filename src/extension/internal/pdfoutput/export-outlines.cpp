// SPDX-License-Identifier: GPL-2.0-or-later
/** @file Outlining on an export copy, preserving cutting layers and inks. */

#include "export-outlines.h"

#include <algorithm>
#include <vector>
#include <glibmm/ustring.h>

#include "document.h"
#include "object/sp-clippath.h"
#include "object/sp-flowtext.h"
#include "object/sp-gradient.h"
#include "object/sp-item-group.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "object/sp-use.h"
#include "path-chemistry.h"
#include "path/path-outline.h"
#include "preferences.h"
#include "spot-ink.h"
#include "style.h"
#include "text-editing.h"

namespace Inkscape::Extension::Internal::PdfBuilder {

bool is_cut_name(std::string_view name)
{
    static constexpr char const *names[] = {
        "cut", "cuts", "cutline", "cutlines", "cutcontour", "cutter", "cutting", "thrucut", "kisscut",
        "die", "dies", "dieline", "dielines", "diecut", "perf", "perfs", "perforation", "crease",
        "creases", "score", "scoreline", "fold", "foldline"
    };
    Glib::ustring word;
    auto matches = [&] { return std::any_of(std::begin(names), std::end(names), [&](auto n) { return word == n; }); };
    Glib::ustring const input{std::string(name)};
    std::vector<gunichar> const chars(input.begin(), input.end());
    for (unsigned i = 0; i < chars.size(); ++i) {
        auto const c = chars[i];
        bool const camel = i && g_unichar_isupper(c) &&
            (g_unichar_islower(chars[i - 1]) || (g_unichar_isupper(chars[i - 1]) &&
             i + 1 < chars.size() && g_unichar_islower(chars[i + 1])));
        if (!g_unichar_isalnum(c) || camel) {
            if (matches()) return true;
            word.clear();
        }
        if (g_unichar_isalnum(c)) word += g_unichar_tolower(c);
    }
    return matches();
}

namespace {

bool cutting_ink(SPObject *object)
{
    auto swatch = cast<SPGradient>(object->style ? object->style->getStrokePaintServer() : nullptr);
    if (!swatch || !swatch->isSolid()) return false;
    auto ink = SpotInk::read(swatch->getRepr());
    if (!ink && swatch->getVector()) ink = SpotInk::read(swatch->getVector()->getRepr());
    return ink && is_cut_name(ink->name);
}

bool has_cutting_ink(SPObject *object)
{
    if (cutting_ink(object)) return true;
    for (auto &child : object->children) {
        if (has_cutting_ink(&child)) return true;
    }
    return false;
}

bool has_stroke(SPObject *object)
{
    return object->style && !object->style->stroke.isNone() && object->style->stroke_width.computed > 0;
}

bool has_stroked_child(SPObject *object)
{
    if (has_stroke(object)) return true;
    for (auto &child : object->children) {
        if (has_stroked_child(&child)) return true;
    }
    return false;
}

void unlink_clones(SPObject *object)
{
    if (auto use = cast<SPUse>(object)) {
        auto const id = std::string(use->getId() ? use->getId() : "(unnamed)");
        object = use->unlink();
        if (!object) {
            g_warning("PDF outlines: couldn't unlink clone '%s'; it is left unchanged.", id.c_str());
            return;
        }
    }
    for (auto child : object->childList(false)) unlink_clones(child);
}

void outline(SPObject *object, bool text, bool strokes)
{
    if (!object) return;
    // A clip uses geometry alone; expanding its children's strokes would widen it.
    if (is<SPClipPath>(object)) strokes = false;
    if (auto layer = cast<SPGroup>(object); layer && layer->isLayer() && layer->label() && is_cut_name(layer->label())) return;
    if (is<SPUse>(object) || (!is<SPGroup>(object) && cutting_ink(object))) return;

    if (auto item = cast<SPItem>(object); item && (is<SPText>(item) || is<SPFlowtext>(item))) {
        // Text is converted as a whole; preserve it if a span carries a cutting ink.
        if (has_cutting_ink(item)) return;
        if (text || (strokes && has_stroked_child(item))) {
            auto doc = object->document;
            auto const id = std::string(item->getId() ? item->getId() : "(unnamed)");
            te_update_layout_now_recursive(item);
            std::vector<SPItem *> selected;
            std::vector<XML::Node *> converted;
            sp_item_list_to_curves({item}, selected, converted);
            if (converted.empty()) {
                g_warning("PDF outlines: couldn't convert text '%s'.", id.c_str());
                return;
            }
            for (auto repr : converted) {
                if (strokes) outline(doc->getObjectByRepr(repr), false, true);
            }
        }
        return;
    }
    if (is<SPGroup>(object) || !is<SPItem>(object)) {
        for (auto child : object->childList(false)) outline(child, text, strokes);
    } else if (strokes && has_stroke(object)) {
        auto item = cast<SPItem>(object);
        auto const id = std::string(item->getId() ? item->getId() : "(unnamed)");
        if (!item_to_paths(item)) g_warning("PDF outlines: couldn't convert stroke on '%s'; it is left as a stroke.", id.c_str());
    }
}

} // namespace

void outline_for_export(SPDocument *document, bool text, bool strokes)
{
    if (!text && !strokes) return;
    auto prefs = Preferences::get();
    char const *scale_stroke = "/options/transform/stroke";
    bool const was_set = prefs->hasPref(scale_stroke);
    bool const previous = prefs->getBool(scale_stroke, true);
    auto restore = scope_exit([&] {
        if (was_set) prefs->setBool(scale_stroke, previous);
        else prefs->remove(scale_stroke);
    });
    // Stroke to Path enables stroke scaling while replacing transformed objects.
    prefs->setBool(scale_stroke, true);
    document->ensureUpToDate();
    // Detach clones before changing their sources, including clones on cutting layers.
    unlink_clones(document->getRoot());
    document->ensureUpToDate();
    outline(document->getRoot(), text, strokes);
    document->ensureUpToDate();
}

} // namespace Inkscape::Extension::Internal::PdfBuilder
