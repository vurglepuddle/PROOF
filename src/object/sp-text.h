// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_TEXT_H
#define SEEN_SP_TEXT_H

/*
 * SVG <text> and <tspan> implementation
 *
 * Author:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "sp-item.h"
#include "sp-string.h"
#include "text-tag-attributes.h"
#include "libnrtype/style-attachments.h"
#include "style.h"

/* Text specific flags */
#define SP_TEXT_CONTENT_MODIFIED_FLAG SP_OBJECT_USER_MODIFIED_FLAG_A
#define SP_TEXT_LAYOUT_MODIFIED_FLAG SP_OBJECT_USER_MODIFIED_FLAG_A

class SPShape;

/* SPText */
class SPText final : public SPItem {
public:
	SPText();
	~SPText() override;
    int tag() const override { return tag_of<decltype(*this)>; }

    /** Converts the text object to its component curves */
    Geom::PathVector getNormalizedBpath() const;

    /** Completely recalculates the layout. */
    void rebuildLayout();

    //semiprivate:  (need to be accessed by the C-style functions still)
    TextTagAttributes attributes;
    Inkscape::Text::Layout layout;
    std::unordered_map<unsigned, Inkscape::Text::StyleAttachments> view_style_attachments;

    /** when the object is transformed it's nicer to change the font size
    and coordinates when we can, rather than just applying a matrix
    transform. is_root is used to indicate to the function that it should
    extend zero-length position vectors to length 1 in order to record the
    new position. This is necessary to convert from objects whose position is
    completely specified by transformations. */
    static void _adjustCoordsRecursive(SPItem *item, Geom::Affine const &m, double ex, bool is_root = true);
    static void _adjustFontsizeRecursive(SPItem *item, double ex, bool is_root = true);
    /**
    This two functions are useful because layout calculations need text visible for example
    Calculating a invisible char position object or pasting text with paragraphs that overflow
    shape defined. I have doubts about transform into a toggle function*/
    void show_shape_inside();
    void hide_shape_inside();

    /** discards the drawing objects representing this text. */
    void _clearFlow(Inkscape::DrawingGroup *in_arena);

    bool _optimizeTextpathText = false;

    /** Union all exclusion shapes. */
    std::unique_ptr<Shape> getExclusionShape() const;
    /** Add a single inclusion shape with padding */
    std::unique_ptr<Shape> getInclusionShape(SPShape *shape) const;
    /** Get the location where the first letter would flow into this shape **/
    static Geom::Point getFirstInsertionPosition(SPShape *shape, bool ltr = true);

    /** Compute the final effective shapes:
     *  All inclusion shapes shrunk by the padding,
     *  from which we subtract the exclusion shapes expanded by their padding.
     *
     *  @return A vector of owning pointers to the effective shapes.
     */
    std::vector<std::unique_ptr<Shape>> makeEffectiveShapes() const;

    std::optional<Geom::Point> getBaselinePoint() const;

private:

    /** Initializes layout from <text> (i.e. this node). */
    void _buildLayoutInit();

    /** Recursively walks the xml tree adding tags and their contents. The
    non-trivial code does two things: firstly, it manages the positioning
    attributes and their inheritance rules, and secondly it keeps track of line
    breaks and makes sure both that they are assigned the correct SPObject and
    that we don't get a spurious extra one at the end of the flow. */
    unsigned _buildLayoutInput(SPObject *object, Inkscape::Text::Layout::OptionalTextTagAttrs const &parent_optional_attrs, unsigned parent_attrs_offset, bool in_textpath);

    /** Find first x/y values which may be in a descendent element. */
    SVGLength* _getFirstXLength();
    SVGLength* _getFirstYLength();
    SPCSSAttr *css;

    static std::unique_ptr<Shape> makeInclusionShape(SPShape *shape, std::optional<double> padding = {});

  public:
    /** Optimize textpath text on next set_transform. */
    void optimizeTextpathText() {_optimizeTextpathText = true;}

    void build(SPDocument* doc, Inkscape::XML::Node* repr) override;
    void release() override;
    void child_added(Inkscape::XML::Node* child, Inkscape::XML::Node* ref) override;
    void remove_child(Inkscape::XML::Node* child) override;
    void set(SPAttr key, const char* value) override;
    void update(SPCtx* ctx, unsigned int flags) override;
    void modified(unsigned int flags) override;
    Inkscape::XML::Node* write(Inkscape::XML::Document* doc, Inkscape::XML::Node* repr, unsigned int flags) override;

    Geom::OptRect bbox(Geom::Affine const &transform, SPItem::BBoxType type) const override;
    void print(SPPrintContext *ctx) override;
    const char* typeName() const override;
    const char* displayName() const override;
    char* description() const override;
    Inkscape::DrawingItem* show(Inkscape::Drawing &drawing, unsigned int key, unsigned int flags) override;
    void hide(unsigned int key) override;
    void snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const override;
    Geom::Affine set_transform(Geom::Affine const &transform, Inkscape::StrokeTransform *preview = nullptr) override;
    void getLinked(std::vector<SPObject *> &objects, LinkedObjectNature direction = LinkedObjectNature::ANY) const override;

    // For 'inline-size', need to also remove any 'x' and 'y' added by SVG 1.1 fallback.
    void remove_svg11_fallback();

    void newline_to_sodipodi(); // 'inline-size' to Inkscape multi-line text.
    void sodipodi_to_newline(); // Inkscape mult-line text to SVG 2 text.

    bool is_horizontal() const;
    bool has_inline_size() const;
    bool has_shape_inside() const;
    Geom::OptRect get_frame();                        // Gets inline-size or shape-inside frame.
    Inkscape::XML::Node* get_first_rectangle();       // Gets first shape-inside rectangle (if it exists).
    SPItem *get_first_shape_dependency();
    const std::vector<SPItem *> get_all_shape_dependencies() const;
    void remove_newlines();                           // Removes newlines in text.

    double length() const;
    int resolve_flip_offset_multiplier() const;
    double resolve_alignment_offset_multiplier() const;
};

SPText *create_text_at_position(SPGroup *parent, SPCSSAttr *css, Geom::Point pos, std::optional<double> inline_size = {});
SPText *create_text_in_rectangle(SPGroup *parent, SPCSSAttr *css, Geom::Rect rect);
SPText *create_text_in_shape(SPGroup *parent, SPCSSAttr *css, SPShape *shape);

#endif

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
