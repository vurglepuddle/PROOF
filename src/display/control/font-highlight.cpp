// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the pink highlight on what stands in for a missing font or picture. See
 * font-highlight.h.
 */
#include "font-highlight.h"

#include <cstring>

#include <glibmm/main.h>

#include "desktop.h"
#include "display/control/canvas-item-quad.h"
#include "document.h"
#include "extension/internal/ai/ai-native-fonts.h"
#include "libnrtype/Layout-TNG.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "text-editing.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace Inkscape::Display {
namespace {

namespace AiNative = Inkscape::Extension::Internal::AiNative;

// Illustrator's pink. As a highlighter's mark it tints the paper and leaves dark type dark.
constexpr uint32_t PINK = 0xff9fd0ff;

bool near(Geom::Point const &a, Geom::Point const &b)
{
    return Geom::are_near(a, b, 1e-6);
}

} // namespace

std::vector<Geom::Point> font_highlight_shape(SPItem const *text)
{
    std::vector<Geom::Point> out;
    auto const *layout = text ? te_get_layout(text) : nullptr;
    if (!layout) return out;
    for (auto const &[first, last] : AiNative::standing_in(text)) {
        // A box to each run of one style: its corners go round from the top left.
        auto const boxes = layout->createSelectionShape(layout->charIndexToIterator(first),
                                                        layout->charIndexToIterator(last), Geom::identity());
        for (std::size_t i = 0; i + 3 < boxes.size(); i += 4) {
            auto const n = out.size();
            // One that starts where the last one ends, as tall, makes that one longer.
            if (n >= 4 && near(out[n - 3], boxes[i]) && near(out[n - 2], boxes[i + 3])) {
                out[n - 3] = boxes[i + 1];
                out[n - 2] = boxes[i + 2];
            } else {
                out.insert(out.end(), boxes.begin() + i, boxes.begin() + i + 4);
            }
        }
    }
    return out;
}

std::vector<Geom::Point> link_highlight_shape(SPItem const *picture)
{
    std::vector<Geom::Point> out;
    auto const *image = cast<SPImage>(picture);
    if (!image || !image->linkMissing()) return out;
    auto const box = Geom::Rect::from_xywh(image->x.computed, image->y.computed, image->width.computed,
                                           image->height.computed);
    for (unsigned i = 0; i < 4; ++i) out.push_back(box.corner(i));
    return out;
}

FontHighlight::FontHighlight(SPDesktop *desktop)
    : _desktop(desktop)
    , _attribute(g_quark_from_static_string(AiNative::FONT_MISSING_ATTRIBUTE))
    , _href(g_quark_from_static_string("href"))
    , _xlink_href(g_quark_from_static_string("xlink:href"))
{
    auto *prefs = Preferences::get();
    _shown = prefs->getBool(PREFERENCE, true);
    _preference = prefs->createObserver(PREFERENCE, [this](Preferences::Entry const &entry) {
        _shown = entry.getBool(true);
        scan();
    });
}

FontHighlight::~FontHighlight()
{
    setDocument(nullptr);
}

void FontHighlight::setDocument(SPDocument *document)
{
    if (_root) {
        _root->removeSubtreeObserver(*this);
        GC::release(_root);
        _root = nullptr;
    }
    _scan.disconnect();
    _texts.clear();
    _document = document;
    if (!_document) return;
    _root = _document->getReprRoot();
    if (_root) {
        GC::anchor(_root);
        _root->addSubtreeObserver(*this);
    }
    scan();
}

void FontHighlight::notifyChildAdded(XML::Node &, XML::Node &child, XML::Node *)
{
    // What is pasted, placed or brought back by undo can hold such text, or a picture.
    bool found = false;
    sp_repr_visit_descendants(&child, [&](XML::Node *node) {
        auto const *name = node->name();
        found = found || node->attribute(AiNative::FONT_MISSING_ATTRIBUTE) || (name && !strcmp(name, "svg:image"));
        return !found;
    });
    if (found) scan_later();
}

void FontHighlight::notifyAttributeChanged(XML::Node &, GQuark name, Util::ptr_shared, Util::ptr_shared)
{
    // The font was installed, or replaced: the name is gone. Or that was undone. A link
    // that changed may be one to a file that is there, or to one that isn't.
    if (name == _attribute || name == _href || name == _xlink_href) scan_later();
}

void FontHighlight::scan_later()
{
    // The objects of what the XML just gained don't exist yet.
    if (_scan.connected()) return;
    _scan = Glib::signal_idle().connect([this] {
        scan();
        return false;
    });
}

void FontHighlight::scan()
{
    _scan.disconnect();
    _texts.clear();
    if (!_document || !_shown) return;
    auto const watch = [this](SPItem *item) {
        if (_texts.count(item)) return;
        auto &entry = _texts[item];
        // After each change the layout is the new one: the type may have moved, been
        // edited, hidden, or given a font of its own. A picture may have moved or been found.
        entry.modified = item->connectModified([this, item](SPObject *, unsigned) { draw(item); });
        entry.released = item->connectRelease([this, item](SPObject *) { _texts.erase(item); });
        draw(item);
    };
    for (auto const &font : AiNative::missing_fonts(_document)) {
        for (auto *text : font.texts) watch(text);
    }
    for (auto *picture : sp_images_missing(_document)) watch(picture);
}

void FontHighlight::draw(SPItem *text)
{
    auto const found = _texts.find(text);
    if (found == _texts.end()) return;
    auto &quads = found->second.quads;
    std::vector<Geom::Point> corners;
    if (!text->isHidden(_desktop->dkey)) {
        corners = is<SPImage>(text) ? link_highlight_shape(text) : font_highlight_shape(text);
        auto const to_desktop = text->i2dt_affine();
        for (auto &corner : corners) corner *= to_desktop;
    }
    std::size_t const count = corners.size() / 4;
    if (quads.size() > count) quads.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto const *c = &corners[i * 4];
        if (i < quads.size()) {
            quads[i]->set_coords(c[0], c[1], c[2], c[3]);
            continue;
        }
        auto quad = make_canvasitem<CanvasItemQuad>(_desktop->getCanvasSketch(), c[0], c[1], c[2], c[3]);
        quad->set_name("CanvasItemQuad:FontHighlight");
        quad->set_fill(PINK);
        quad->set_highlighter(true);
        quad->set_pickable(false);
        quads.push_back(std::move(quad));
    }
}

} // namespace Inkscape::Display
