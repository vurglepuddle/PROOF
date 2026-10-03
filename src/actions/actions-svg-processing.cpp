// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for pre-processing svg, used in extensions
 *
 * Copyright (C) 2002-2023 Authors
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#include "actions-svg-processing.h"

#include <iostream>

#include <giomm.h>
#include <glibmm/i18n.h>

#include "actions-helper.h"


#include "colors/document-colors.h"
#include "document.h"
#include "inkscape-application.h"
#include "style.h"

#include "object/object-set.h"
#include "object/sp-defs.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "path-chemistry.h"
#include "path/path-outline.h"
#include "svg/svg-box.h"
#include "svg/svg.h"
#include "xml/attribute-record.h"
#include "xml/node.h"

/*
 * Removes all sodipodi and inkscape elements and attributes from an xml tree.
 * used to make plain svg output.
 */
void prune_inkscape_from_node(Inkscape::XML::Node *repr)
{
    if (repr) {
        if ( repr->type() == Inkscape::XML::NodeType::ELEMENT_NODE ) {
            std::vector<gchar const*> attrsRemoved;
            for ( const auto & it : repr->attributeList()) {
                const gchar* attrName = g_quark_to_string(it.key);
                if ((strncmp("inkscape:", attrName, 9) == 0) || (strncmp("sodipodi:", attrName, 9) == 0)) {
                    attrsRemoved.push_back(attrName);
                }
            }
            // Can't change the set we're iterating over while we are iterating.
            for (auto & it : attrsRemoved) {
                repr->removeAttribute(it);
            }
        }

        std::vector<Inkscape::XML::Node *> nodesRemoved;
        for (auto child = repr->firstChild(); child; child = child->next()) {
            if((strncmp("inkscape:", child->name(), 9) == 0) || strncmp("sodipodi:", child->name(), 9) == 0) {
                nodesRemoved.push_back(child);
            } else {
                prune_inkscape_from_node(child);
            }
        }
        for (auto & it : nodesRemoved) {
            repr->removeChild(it);
        }
    }
}

/*
 * Similar to the above prune, but used on all documents to remove problematic elements
 * for example Adobe's i:pgf tag; only removes known garbage tags.
 */
static void prune_proprietary_from_node( Inkscape::XML::Node *repr )
{
    if (repr) {
        std::vector<Inkscape::XML::Node *> nodesRemoved;
        for (auto child = repr->firstChild(); child; child = child->next() ) {
            if((strncmp("i:pgf", child->name(), 5) == 0)) {
                nodesRemoved.push_back(child);
                g_warning( "An Adobe proprietary tag was found which is known to cause issues. It was removed before saving.");
            } else {
                prune_proprietary_from_node(child);
            }
        }
        for (auto & it : nodesRemoved) {
            repr->removeChild(it);
        }
    }
}

/**
 *  \return    None
 *
 *  \brief     Create new markers where necessary to simulate the SVG 2 marker attribute 'orient'
 *             value 'auto-start-reverse'.
 *
 *  \param     repr  The current element to check.
 *  \param     defs  A pointer to the <defs> element.
 *  \param     property  Which property to check, either 'marker' or 'marker-start'.
 *
 */
static void remove_marker_auto_start_reverse(Inkscape::XML::Node *repr,
                                             Inkscape::XML::Node *defs,
                                             Glib::ustring const &property)
{
    for (auto child = repr->firstChild(); child; child = child->next() ) {
        remove_marker_auto_start_reverse(child, defs, property);
    }

    SPCSSAttr* css = sp_repr_css_attr (repr, "style");
    Glib::ustring value = sp_repr_css_property (css, property.c_str(), "");

    if (value.empty())
        return;

    // Find reference <marker>
    static Glib::RefPtr<Glib::Regex> regex = Glib::Regex::create("url\\(#([^\\)]*)\\)");
    Glib::MatchInfo matchInfo;
    regex->match(value, matchInfo);

    if (matchInfo.matches()) {

        auto marker_name = matchInfo.fetch(1).raw();
        Inkscape::XML::Node *marker = sp_repr_lookup_child (defs, "id", marker_name.c_str());
        if (marker) {

            // Does marker use "auto-start-reverse"?
            if (strncmp(marker->attribute("orient"), "auto-start-reverse", 17)==0) {

                // See if a reversed marker already exists.
                auto marker_name_reversed = marker_name + "_reversed";
                Inkscape::XML::Node *marker_reversed =
                    sp_repr_lookup_child (defs, "id", marker_name_reversed.c_str());

                if (!marker_reversed) {

                    // No reversed marker, need to create!
                    marker_reversed = repr->document()->createElement("svg:marker");

                    // Copy attributes
                    for (const auto & iter : marker->attributeList()) {
                        marker_reversed->setAttribute(g_quark_to_string(iter.key), iter.value);
                    }

                    // Override attributes
                    marker_reversed->setAttribute("id", marker_name_reversed);
                    marker_reversed->setAttribute("orient", "auto");

                    // Find transform
                    const char* refX = marker_reversed->attribute("refX");
                    const char* refY = marker_reversed->attribute("refY");
                    std::string transform = "rotate(180";
                    if (refX) {
                        transform += ",";
                        transform += refX;

                        if (refY) {
                            if (refX) {
                                transform += ",";
                                transform += refY;
                            } else {
                                transform += ",0,";
                                transform += refY;
                            }
                        }
                    }
                    transform += ")";

                    // We can't set a transform on a marker... must create group first.
                    Inkscape::XML::Node *group = repr->document()->createElement("svg:g");
                    group->setAttribute("transform", transform);
                    marker_reversed->addChild(group, nullptr);

                    // Copy all marker content to group.
                    for (auto child = marker->firstChild() ; child != nullptr ; child = child->next() ) {
                        auto new_child = child->duplicate(repr->document());
                        group->addChild(new_child, nullptr);
                        new_child->release();
                    }

                    // Add new marker to <defs>.
                    defs->addChild(marker_reversed, marker);
                    marker_reversed->release();
                 }

                // Change url to reference reversed marker.
                std::string marker_url("url(#" + marker_name_reversed + ")");
                sp_repr_css_set_property(css, "marker-start", marker_url.c_str());

                // Also fix up if property is marker shorthand.
                if (property == "marker") {
                    std::string marker_old_url("url(#" + marker_name + ")");
                    sp_repr_css_unset_property(css, "marker");
                    sp_repr_css_set_property(css, "marker-mid", marker_old_url.c_str());
                    sp_repr_css_set_property(css, "marker-end", marker_old_url.c_str());
                }

                sp_repr_css_set(repr, css, "style");

            } // Uses auto-start-reverse
        }
    }
}

// Called by remove_marker_context_paint() for each property value ("marker", "marker-start", ...).
void remove_marker_context_paint (Inkscape::XML::Node *repr, Inkscape::XML::Node *defs, Glib::ustring property)
{
    // Value of 'marker', 'marker-start', ... property.
    std::string value("url(#");
    value += repr->attribute("id");
    value += ")";

    // Generate a list of elements that reference this marker.
    std::vector<Inkscape::XML::Node *> to_fix_fill_stroke =
        sp_repr_lookup_property_many(repr->root(), property, value);

    for (auto it: to_fix_fill_stroke) {

        // Figure out value of fill... could be inherited.
        SPCSSAttr* css = sp_repr_css_attr_inherited (it, "style");
        Glib::ustring fill   = sp_repr_css_property (css, "fill",   "");
        Glib::ustring stroke = sp_repr_css_property (css, "stroke", "");

        // Name of new marker./
        Glib::ustring marker_fixed_id = repr->attribute("id");
        if (!fill.empty()) {
            marker_fixed_id += "_F" + fill;
        }
        if (!stroke.empty()) {
            marker_fixed_id += "_S" + stroke;
        }

        {
            // Replace characters from color value that are invalid in ids
            gchar *normalized_id = g_strdup(marker_fixed_id.c_str());
            g_strdelimit(normalized_id, "#%", '-');
            g_strdelimit(normalized_id, "(), \n\t\r", '.');
            marker_fixed_id = normalized_id;
            g_free(normalized_id);
        }

        // See if a fixed marker already exists.
        // Could be more robust, assumes markers are direct children of <defs>.
        Inkscape::XML::Node* marker_fixed = sp_repr_lookup_child(defs, "id", marker_fixed_id.c_str());

        if (!marker_fixed) {

            // Need to create new marker.

            marker_fixed = repr->duplicate(repr->document());
            marker_fixed->setAttribute("id", marker_fixed_id);

            // This needs to be turned into a function that fixes all descendents.
            for (auto child = marker_fixed->firstChild() ; child != nullptr ; child = child->next()) {
                // Find style.
                SPCSSAttr* css = sp_repr_css_attr ( child, "style" );

                Glib::ustring fill2   = sp_repr_css_property (css, "fill",   "");
                if (fill2 == "context-fill" ) {
                    sp_repr_css_set_property (css, "fill", fill.c_str());
                }
                if (fill2 == "context-stroke" ) {
                    sp_repr_css_set_property (css, "fill", stroke.c_str());
                }

                Glib::ustring stroke2 = sp_repr_css_property (css, "stroke", "");
                if (stroke2 == "context-fill" ) {
                    sp_repr_css_set_property (css, "stroke", fill.c_str());
                }
                if (stroke2 == "context-stroke" ) {
                    sp_repr_css_set_property (css, "stroke", stroke.c_str());
                }

                sp_repr_css_set(child, css, "style");
                sp_repr_css_attr_unref(css);
            }

            defs->addChild(marker_fixed, repr);
            marker_fixed->release();
        }

        Glib::ustring marker_value = "url(#" + marker_fixed_id + ")";
        sp_repr_css_set_property (css, property.c_str(), marker_value.c_str());
        sp_repr_css_set (it, css, "style");
        sp_repr_css_attr_unref(css);
    }
}  

void remove_marker_context_paint(Inkscape::XML::Node *repr, Inkscape::XML::Node *defs)
{
    for (auto child = repr->firstChild(); child; child = child->next() ) {
        remove_marker_context_paint(child, defs);
    }

    if (strncmp("svg:marker", repr->name(), 10) == 0) {

        if (!repr->attribute("id")) {

            std::cerr << "remove_marker_context_paint: <marker> without 'id'!" << std::endl;

        } else {

            // First see if we need to do anything.
            bool need_to_fix = false;

            // This needs to be turned into a function that searches all descendents.
            for (auto child = repr->firstChild() ; child != nullptr ; child = child->next()) {

                // Find style.
                SPCSSAttr* css = sp_repr_css_attr ( child, "style" );
                Glib::ustring fill   = sp_repr_css_property (css, "fill",   "");
                Glib::ustring stroke = sp_repr_css_property (css, "stroke", "");
                if (fill   == "context-fill"   ||
                    fill   == "context-stroke" ||
                    stroke == "context-fill"   ||
                    stroke == "context-stroke" ) {
                    need_to_fix = true;
                    break;
                }
                sp_repr_css_attr_unref(css);
            }

            if (need_to_fix) {

                // Now we need to search document for all elements that use this marker.
                remove_marker_context_paint (repr, defs, "marker");
                remove_marker_context_paint (repr, defs, "marker-start");
                remove_marker_context_paint (repr, defs, "marker-mid");
                remove_marker_context_paint (repr, defs, "marker-end");
            }
        }
    }
}

/*
 * Recursively insert SVG 1.1 fallback for SVG 2 text (ignored by SVG 2 renderers including ours).
 * Notes:
 *   Text must have been layed out. Access via old document.
 */
void insert_text_fallback(Inkscape::XML::Node *repr, const SPDocument *original_doc, Inkscape::XML::Node *defs)
{
    if (repr) {

        if (strncmp("svg:text", repr->name(), 8) == 0) {

            auto id = repr->attribute("id");
            // std::cout << "insert_text_fallback: found text!  id: " << (id?id:"null") << std::endl;

            // We need to get original SPText object to access layout.
            SPText* text = static_cast<SPText *>(original_doc->getObjectById( id ));
            if (text == nullptr) {
                std::cerr << "insert_text_fallback: bad cast" << std::endl;
                return;
            }

            if (!text->has_inline_size() &&
                !text->has_shape_inside()) {
                // No SVG 2 text, nothing to do.
                return;
            }

            // We will keep this text node but replace all children.
            // Text object must be visible for the text calculatons to work
            bool was_hidden = text->isHidden();
            text->setHidden(false);
            text->rebuildLayout();

            // For text in a shape, We need to unset 'text-anchor' or SVG 1.1 fallback won't work.
            // Note 'text' here refers to original document while 'repr' refers to new document copy.
            if (text->has_shape_inside()) {
                SPCSSAttr *css = sp_repr_css_attr(repr, "style" );
                sp_repr_css_unset_property(css, "text-anchor");
                sp_repr_css_set(repr, css, "style");
                sp_repr_css_attr_unref(css);
            }

            // We need to put trailing white space into its own tspan for inline size so
            // it is excluded during calculation of line position in SVG 1.1 renderers.
            bool trim = text->has_inline_size() &&
                !(text->style->text_anchor.computed == SP_CSS_TEXT_ANCHOR_START);

            // Make a list of children to delete at end:
            std::vector<Inkscape::XML::Node *> old_children;
            for (auto child = repr->firstChild(); child; child = child->next()) {
                old_children.push_back(child);
            }

            // For round-tripping, xml:space (or 'white-space:pre') must be set.
            repr->setAttribute("xml:space", "preserve");

            double text_x = repr->getAttributeDouble("x", 0.0);
            double text_y = repr->getAttributeDouble("y", 0.0);
            // std::cout << "text_x: " << text_x << " text_y: " << text_y << std::endl;

            // Loop over all lines in layout.
            for (auto it = text->layout.begin() ; it != text->layout.end() ; ) {

                // Create a <tspan> with 'x' and 'y' for each line.
                Inkscape::XML::Node *line_tspan = repr->document()->createElement("svg:tspan");

                // This could be useful if one wants to edit in an old version of Inkscape but we
                // need to check if it breaks anything:
                // line_tspan->setAttribute("sodipodi:role", "line");

                // Hide overflow tspan (one line of text).
                if (text->layout.isHidden(it)) {
                    line_tspan->setAttribute("style", "visibility:hidden");
                }

                Geom::Point line_anchor_point = text->layout.characterAnchorPoint(it);
                double line_x = line_anchor_point[Geom::X];
                double line_y = line_anchor_point[Geom::Y];

                // std::cout << "  line_anchor_point: " << line_anchor_point << std::endl;
                if (line_tspan->childCount() == 0) {
                    if (text->is_horizontal()) {
                        // std::cout << "  horizontal: " << text_x << " " << line_anchor_point[Geom::Y] << std::endl;
                        if (text->has_inline_size()) {
                            // We use text_x as this is the reference for 'text-anchor'
                            // (line_x is the start of the line which gives wrong position when 'text-anchor' not start).
                            line_tspan->setAttributeSvgDouble("x", text_x);
                        } else {
                            // shape-inside (we don't have to worry about 'text-anchor').
                            line_tspan->setAttributeSvgDouble("x", line_x);
                        }
                        line_tspan->setAttributeSvgDouble("y", line_y); // FIXME: this will pick up the wrong end of counter-directional runs
                    } else {
                        // std::cout << "  vertical:   " << line_anchor_point[Geom::X] << " " << text_y << std::endl;
                        line_tspan->setAttributeSvgDouble("x", line_x); // FIXME: this will pick up the wrong end of counter-directional runs
                        if (text->has_inline_size()) {
                            line_tspan->setAttributeSvgDouble("y", text_y);
                        } else {
                            line_tspan->setAttributeSvgDouble("y", line_y);
                        }
                    }
                }

                // Inside line <tspan>, create <tspan>s for each change of style or shift. (No shifts in SVG 2 flowed text.)
                // For simple lines, this creates an unneeded <tspan> but so be it.
                Inkscape::Text::Layout::iterator it_line_end = it;
                it_line_end.nextStartOfLine();

                // Find last span in line so we can put trailing whitespace in its own tspan for SVG 1.1 fallback.
                Inkscape::Text::Layout::iterator it_last_span = it;
                it_last_span.nextStartOfLine();
                it_last_span.prevStartOfSpan();

                Glib::ustring trailing_whitespace;

                // Loop over chunks in line
                while (it != it_line_end) {

                    Inkscape::XML::Node *span_tspan = repr->document()->createElement("svg:tspan");

                    // use kerning to simulate justification and whatnot
                    Inkscape::Text::Layout::iterator it_span_end = it;
                    it_span_end.nextStartOfSpan();
                    Inkscape::Text::Layout::OptionalTextTagAttrs attrs;
                    text->layout.simulateLayoutUsingKerning(it, it_span_end, &attrs);

                    // 'dx' and 'dy' attributes are used to simulated justified text.
                    if (!text->is_horizontal()) {
                        std::swap(attrs.dx, attrs.dy);
                    }
                    TextTagAttributes(attrs).writeTo(span_tspan);
                    SPObject *source_obj = nullptr;
                    Glib::ustring::iterator span_text_start_iter;
                    text->layout.getSourceOfCharacter(it, &source_obj, &span_text_start_iter);

                    // Set tspan style
                    Glib::ustring style_text = (is<SPString>(source_obj) ? source_obj->parent : source_obj)
                                                   ->style->writeIfDiff(text->style);
                    if (!style_text.empty()) {
                        span_tspan->setAttributeOrRemoveIfEmpty("style", style_text);
                    }

                    // If this tspan has no attributes, discard it and add content directly to parent element.
                    if (span_tspan->attributeList().empty()) {
                        Inkscape::GC::release(span_tspan);
                        span_tspan = line_tspan;
                    } else {
                        line_tspan->appendChild(span_tspan);
                        Inkscape::GC::release(span_tspan);
                    }

                    // Add text node
                    auto str = cast<SPString>(source_obj);
                    if (str) {
                        Glib::ustring *string = &(str->string); // TODO fixme: dangerous, unsafe premature-optimization
                        SPObject *span_end_obj = nullptr;
                        Glib::ustring::iterator span_text_end_iter;
                        text->layout.getSourceOfCharacter(it_span_end, &span_end_obj, &span_text_end_iter);
                        if (span_end_obj != source_obj) {
                            if (it_span_end == text->layout.end()) {
                                span_text_end_iter = span_text_start_iter;
                                for (int i = text->layout.iteratorToCharIndex(it_span_end) - text->layout.iteratorToCharIndex(it) ; i ; --i)
                                    ++span_text_end_iter;
                            } else
                                span_text_end_iter = string->end();    // spans will never straddle a source boundary
                        }

                        if (span_text_start_iter != span_text_end_iter) {
                            Glib::ustring new_string;
                            while (span_text_start_iter != span_text_end_iter)
                                new_string += *span_text_start_iter++;    // grr. no substr() with iterators

                            if (it == it_last_span && trim) {
                                // Found last span in line
                                const auto s = new_string.find_last_not_of(" \t"); // Any other white space characters needed?
                                trailing_whitespace = new_string.substr(s+1, new_string.length());
                                new_string.erase(s+1);
                            }

                            Inkscape::XML::Node *new_text = repr->document()->createTextNode(new_string.c_str());
                            span_tspan->appendChild(new_text);
                            Inkscape::GC::release(new_text);
                            // std::cout << "  new_string: |" << new_string << "|" << std::endl;
                        }
                    }
                    it = it_span_end;
                }

                // Add line tspan to document
                repr->appendChild(line_tspan);
                Inkscape::GC::release(line_tspan);

                // For center and end justified text, we need to remove any spaces and put them
                // into a separate tspan (alignment is done by "text chunk" and spaces at ends of
                // line will mess this up).
                if (trim && trailing_whitespace.length() != 0) {

                    Inkscape::XML::Node *space_tspan = repr->document()->createElement("svg:tspan");
                    // Set either 'x' or 'y' to force a new text chunk. To do: this really should
                    // be positioned at the end of the line (overhanging).
                    if (text->is_horizontal()) {
                        space_tspan->setAttributeSvgDouble("y", line_y);
                    } else {
                        space_tspan->setAttributeSvgDouble("x", line_x);
                    }
                    Inkscape::XML::Node *space = repr->document()->createTextNode(trailing_whitespace.c_str());
                    space_tspan->appendChild(space);
                    Inkscape::GC::release(space);
                    line_tspan->appendChild(space_tspan);
                    Inkscape::GC::release(space_tspan);
                }

            }

            for (auto i: old_children) {
                repr->removeChild (i);
            }

	    text->setHidden(was_hidden);
            return; // No need to look at children of <text>
        }

        for (auto child = repr->firstChild(); child; child = child->next() ) {
            insert_text_fallback (child, original_doc, defs);
        }
    }
}

void insert_mesh_polyfill(Inkscape::XML::Node *repr)
{
    if (repr) {

        Inkscape::XML::Node *defs = sp_repr_lookup_name (repr, "svg:defs");

        if (defs == nullptr) {
            // We always put meshes in <defs>, no defs -> no mesh.
            return;
        }

        bool has_mesh = false;
        for (auto child = defs->firstChild(); child; child = child->next()) {
            if (strncmp("svg:meshgradient", child->name(), 16) == 0) {
                has_mesh = true;
                break;
            }
        }

        Inkscape::XML::Node *script = sp_repr_lookup_child (repr, "id", "mesh_polyfill");

        if (has_mesh && script == nullptr) {

            script = repr->document()->createElement("svg:script");
            script->setAttribute ("id",   "mesh_polyfill");
            script->setAttribute ("type", "text/javascript");
            repr->root()->appendChild(script); // Must be last

            // Insert JavaScript via raw string literal.
            Glib::ustring js =
#include "extension/internal/polyfill/mesh_compressed.include"
;

            Inkscape::XML::Node *script_text = repr->document()->createTextNode(js.c_str());
            script->appendChild(script_text);
        }
    }
}
void insert_hatch_polyfill(Inkscape::XML::Node *repr)
{
    if (repr) {

        Inkscape::XML::Node *defs = sp_repr_lookup_name (repr, "svg:defs");

        if (defs == nullptr) {
            // We always put meshes in <defs>, no defs -> no mesh.
            return;
        }

        bool has_hatch = false;
        for (auto child = defs->firstChild(); child; child = child->next()) {
            if (strncmp("svg:hatch", child->name(), 16) == 0) {
                has_hatch = true;
                break;
            }
        }

        Inkscape::XML::Node *script = sp_repr_lookup_child (repr, "id", "hatch_polyfill");

        if (has_hatch && script == nullptr) {

            script = repr->document()->createElement("svg:script");
            script->setAttribute ("id",   "hatch_polyfill");
            script->setAttribute ("type", "text/javascript");
            repr->root()->appendChild(script); // Must be last

            // Insert JavaScript via raw string literal.
            Glib::ustring js =
#include "extension/internal/polyfill/hatch_compressed.include"
;

            Inkscape::XML::Node *script_text = repr->document()->createTextNode(js.c_str());
            script->appendChild(script_text);
        }
    }
}

/**
 * Appends a visual box, and an optional geometric box to each SPItem recursively.
 *
 * This means groups also end up with boxes and any other item where one can be made.
 */
void insert_bounding_boxes(SPItem *item)
{
    for (auto& child: item->childList(false)) {
        if (auto child_item = cast<SPItem>(child)) {
            insert_bounding_boxes(child_item);
        }
    }
    auto const scale = item->document->getDocumentScale().inverse();
    auto const vbox = item->visualBounds(item->i2doc_affine() * scale);
    auto const gbox = item->geometricBounds(item->i2doc_affine() * scale);
    item->setAttributeOrRemoveIfEmpty("inkscape:visualbox", SVGBox(vbox).write());
    if (gbox != vbox) {
        item->setAttributeOrRemoveIfEmpty("inkscape:geometricbox", SVGBox(gbox).write());
    }
}

/**
 * Appends the shape path, if available, to any SPShape recursively.
 */
void insert_path_data(SPItem *item)
{
    Geom::PathVector fill;
    Geom::PathVector stroke;
    if (item_find_paths(item, fill, stroke)) {
        item->setAttribute("inkscape:d", sp_svg_write_path(fill));
    } else {
        for (auto& child: item->childList(false)) {
            if (auto child_item = cast<SPItem>(child)) {
                insert_path_data(child_item);
            }
        }
    }
}

/**
 * Makes paths more predictable for better processing
 */
void normalize_all_paths(Inkscape::XML::Node *node)
{
    if (auto attr = node->attribute("d")) {
        node->setAttribute("d", sp_svg_write_path(sp_svg_read_pathv(attr), true));
    }
    for (auto child = node->firstChild(); child; child = child->next() ) {
        normalize_all_paths(child);
    }
}

const Glib::ustring SECTION = NC_("Action Section", "Processing");

std::vector<std::vector<Glib::ustring>> doc_svg_processing_actions =
{
    // clang-format off
    {"doc.set-svg-version-1",            N_("Set SVG Version to 1.1"),       SECTION, N_("Set the document's SVG version to 1.1") },
    {"doc.set-svg-version-2",            N_("Set SVG Version to 2.0"),       SECTION, N_("Set the document's SVG version to 2.0") },
    {"doc.set-inkscape-version",         N_("Set Inkscape Version"),         SECTION, N_("Add the Inkscape version to the document") },
    {"doc.prune-inkscape-namespaces",    N_("Prune Inkscape Namespaces"),    SECTION, N_("Remove any Inkscape-specific SVG data") },
    {"doc.prune-proprietary-namespaces", N_("Prune Proprietary Namespaces"), SECTION, N_("Remove any known proprietary SVG data") },

    {"doc.reverse-auto-start-markers",   N_("Reverse Auto Start Markers"),   SECTION, N_("Remove auto start positions from markers") },
    {"doc.remove-all-transforms",        N_("Try to Remove All Transforms"), SECTION, N_("Attempt to remove all transforms from all shapes") },
    {"doc.remove-marker-context-paint",  N_("Remove Marker Context Paint"),  SECTION, N_("Remove context paints from markers") },

    {"doc.insert-text-fallback",         N_("Insert Text Fallback"),         SECTION, N_("Replace SVG2 text with SVG1.1 text") },
    {"doc.insert-color-fallback",        N_("Insert Color Fallback"),        SECTION, N_("Add RGB fallbacks for ICC and CMYK colors") },
    {"doc.insert-mesh-polyfill",         N_("Insert Mesh Polyfill"),         SECTION, N_("Insert JavaScript for rendering meshes") },
    {"doc.insert-hatch-polyfill",        N_("Insert Hatch Polyfill"),        SECTION, N_("Insert JavaScript for rendering hatches") },

    {"doc.all-clones-to-objects",        N_("Unlink All Clones"),            SECTION, N_("Recursively unlink all clones and symbols") },
    {"doc.all-objects-to-paths",         N_("All Objects to Paths"),         SECTION, N_("Turn all shapes recursively into path elements") },
    {"doc.add-strokes-to-paths",         N_("All Strokes to Paths"),         SECTION, N_("Turn all strokes recursively into fill-only paths") },
    {"doc.normalize-all-paths",          N_("Normalize Path Data"),          SECTION, N_("Make all paths absolute and predictable") },

    {"doc.insert-bounding-boxes",        N_("Annotate all Bounding Boxes"),  SECTION, N_("Annotate every shape and group with its current bounding box (not kept up to date)") },
    {"doc.insert-path-data",             N_("Annotate all Shape Paths"),     SECTION, N_("Annotate every non-path shape with their equivalent path string (not kept up to date)") },

    {"doc.vacuum-defs",                  N_("Clean up Document"),            SECTION, N_("Remove unused definitions (gradients, etc.)") },
    // clang-format on
};

void add_actions_processing(SPDocument* doc)
{

    auto group = doc->getActionGroup();
    // clang-format off
    group->add_action("set-svg-version-2",            [doc]() {
        auto rroot = doc->getReprRoot();
        rroot->setAttribute("standalone", "no");
        rroot->setAttribute("version", "2.0");
    });
    group->add_action("set-svg-version-1",            [doc]() {
        auto rroot = doc->getReprRoot();
        rroot->setAttribute("version", "1.1");
    });
    group->add_action("set-inkscape-version",         [doc]() {
        doc->getRoot()->updateDocVersion();
    });
    group->add_action("prune-inkscape-namespaces",    [doc]() { prune_inkscape_from_node(doc->getReprRoot()); });
    group->add_action("prune-proprietary-namespaces", [doc]() { prune_proprietary_from_node(doc->getReprRoot()); });
    group->add_action("reverse-auto-start-markers",   [doc]() {
        // Do marker start for efficiency reasons
        remove_marker_auto_start_reverse(doc->getReprRoot(), doc->getDefs()->getRepr(), "marker-start");
        remove_marker_auto_start_reverse(doc->getReprRoot(), doc->getDefs()->getRepr(), "marker");
    });
    group->add_action("remove-all-transforms", [doc]() {
        doc->getRoot()->removeTransformsRecursively(doc->getRoot());
    });

    group->add_action("remove-marker-context-paint",  [doc]() { remove_marker_context_paint(doc->getReprRoot(), doc->getDefs()->getRepr()); });
    group->add_action("insert-text-fallback",         [doc]() { insert_text_fallback(doc->getReprRoot(), doc->getOriginalDocument()); });
    group->add_action("insert-color-fallback",        [doc]() { Inkscape::Colors::DocumentColors::insertFallbacks(doc); });
    group->add_action("insert-mesh-polyfill",         [doc]() { insert_mesh_polyfill(doc->getReprRoot()); });
    group->add_action("insert-hatch-polyfill",        [doc]() { insert_hatch_polyfill(doc->getReprRoot()); });
    group->add_action("all-clones-to-objects",        [doc]() {
        auto selection = Inkscape::ObjectSet(doc);
        selection.set(doc->getRoot());
        selection.unlinkRecursive(true, false, true);
    });
    group->add_action("all-objects-to-paths",         [doc]() {
        std::vector<SPItem*> selected;
        std::vector<Inkscape::XML::Node*> to_select;
        sp_item_list_to_curves({doc->getRoot()}, selected, to_select, false);
    });
    group->add_action("add-strokes-to-paths",         [doc]() {
        item_to_paths(doc->getRoot());
    });
    group->add_action("normalize-all-paths",       [doc]() { normalize_all_paths(doc->getReprRoot()); });
    group->add_action("insert-bounding-boxes",     [doc]() { insert_bounding_boxes(doc->getRoot()); });
    group->add_action("insert-path-data",          [doc]() { insert_path_data(doc->getRoot()); });
    group->add_action("vacuum-defs",               [doc]() { doc->vacuumDocument(); });
    // clang-format on

    // Note: This will only work for the first ux to load, possible problem.
    auto app = InkscapeApplication::instance();
    if (!app) { // i.e. Inkview
        return;
    }
    app->get_action_extra_data().add_data(doc_svg_processing_actions);
}

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
