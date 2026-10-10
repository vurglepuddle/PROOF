// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PROOF: the pink highlight on type set in a font that stands in for one that isn't
 * installed, as Illustrator shows it.
 *
 * It is drawn over the canvas of one window, line by line where the type is. It is no part
 * of the drawing: nothing that is exported, printed or saved has it.
 */
#ifndef SEEN_INKSCAPE_DISPLAY_FONT_HIGHLIGHT_H
#define SEEN_INKSCAPE_DISPLAY_FONT_HIGHLIGHT_H

#include <map>
#include <memory>
#include <vector>

#include <2geom/point.h>
#include <sigc++/scoped_connection.h>

#include "display/control/canvas-item-ptr.h"
#include "preferences.h"
#include "xml/node-observer.h"

class SPDesktop;
class SPDocument;
class SPItem;

namespace Inkscape {
class CanvasItemQuad;

namespace Display {

/// Where a text's type is set in a font standing in for another: four corners to each
/// stretch of a line, in the text's own coordinates. Stretches that meet end to end on a
/// line are one.
std::vector<Geom::Point> font_highlight_shape(SPItem const *text);

class FontHighlight final : private XML::NodeObserver
{
public:
    /// Whether the highlight shows: a preference, on unless switched off.
    static constexpr char const *PREFERENCE = "/options/font/missinghighlight";

    explicit FontHighlight(SPDesktop *desktop);
    ~FontHighlight() override;

    /// The document the window shows; none while it shows none.
    void setDocument(SPDocument *document);

private:
    struct Text
    {
        std::vector<CanvasItemPtr<CanvasItemQuad>> quads;
        sigc::scoped_connection modified, released;
    };

    // The document's XML says when text may have begun or stopped being set in such a font.
    void notifyChildAdded(XML::Node &node, XML::Node &child, XML::Node *prev) override;
    void notifyAttributeChanged(XML::Node &node, GQuark name, Util::ptr_shared old_value,
                                Util::ptr_shared new_value) override;

    void scan_later();
    void scan();              ///< Finds the document's texts that have such type.
    void draw(SPItem *text);  ///< Puts one text's highlight where its type now is.

    SPDesktop *_desktop;
    SPDocument *_document = nullptr;
    XML::Node *_root = nullptr;
    std::map<SPItem *, Text> _texts;
    sigc::scoped_connection _scan;
    bool _shown = true;
    PrefObserver _preference;
    GQuark _attribute;
};

} // namespace Display
} // namespace Inkscape

#endif // SEEN_INKSCAPE_DISPLAY_FONT_HIGHLIGHT_H
