// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Miscellaneous operations on selected items.
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Frank Felfe <innerspace@iname.com>
 *   MenTaLguY <mental@rydia.net>
 *   bulia byak <buliabyak@users.sf.net>
 *   Andrius R. <knutux@gmail.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Martin Sucha <martin.sucha-inkscape@jts-sro.sk>
 *   Abhishek Sharma
 *   Kris De Gussem <Kris.DeGussem@gmail.com>
 *   Tavmjong Bah <tavmjong@free.fr> (Symbol additions)
 *   Adrian Boguszewski
 *   Marc Jeanmougin
 *
 * Copyright (C) 1999-2016 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "selection-chemistry.h"

#include <cstring>
#include <deque>
#include <map>
#include <ranges>
#include <string>
#include <glibmm/i18n.h>
#include <2geom/numeric/symmetric-matrix-fs-operation.h>

#include "actions/actions-tools.h" // Switching tools
#include "context-fns.h"
#include "desktop-style.h"
#include "desktop.h"
#include "display/cairo-utils.h"
#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-drawing.h"
#include "path/path-curve.h"
#include "display/drawing.h"
#include "document-undo.h"
#include "file.h"
#include "filter-chemistry.h"
#include "gradient-drag.h"
#include "helper/pixbuf-ops.h"
#include "layer-manager.h"
#include "live_effects/effect.h"
#include "live_effects/lpeobject.h"
#include "message-stack.h"
#include "object/box3d.h"
#include "object/object-set.h"
#include "object/persp3d.h"
#include "object/sp-clippath.h"
#include "object/sp-conn-end.h"
#include "object/sp-defs.h"
#include "object/sp-ellipse.h"
#include "object/sp-flowregion.h"
#include "object/sp-flowtext.h"
#include "object/sp-image.h"
#include "object/sp-item-transform.h"
#include "object/sp-item.h"
#include "object/sp-line.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-marker.h"
#include "object/sp-mask.h"
#include "object/sp-namedview.h"
#include "object/sp-offset.h"
#include "object/sp-path.h"
#include "object/sp-pattern.h"
#include "object/sp-polyline.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-rect.h"
#include "object/sp-root.h"
#include "object/sp-spiral.h"
#include "object/sp-star.h"
#include "object/sp-symbol.h"
#include "object/sp-textpath.h"
#include "object/sp-tref.h"
#include "object/sp-tspan.h"
#include "object/sp-use.h"
#include "page-manager.h"
#include "path-chemistry.h"
#include "selection.h"
#include "style.h"
#include "svg/svg.h"
#include "text-chemistry.h"
#include "text-editing.h"
#include "ui/clipboard.h"
#include "ui/icon-names.h"
#include "ui/tool/control-point-selection.h"
#include "ui/tool/multi-path-manipulator.h"
#include "ui/tool/path-manipulator.h"
#include "ui/tools/connector-tool.h"
#include "ui/tools/dropper-tool.h"
#include "ui/tools/gradient-tool.h"
#include "ui/tools/node-tool.h"
#include "ui/tools/text-tool.h"
#include "ui/widget/canvas.h" // is_dragging()
#include "xml/href-attribute-helper.h"
#include "xml/rebase-hrefs.h"

// TODO FIXME: This should be moved into preference repr
SPCycleType SP_CYCLING = SP_CYCLE_FOCUS;

using Inkscape::DocumentUndo;
using Geom::X;
using Geom::Y;
using Inkscape::UI::Tools::GradientTool;
using Inkscape::UI::Tools::NodeTool;
using Inkscape::UI::Tools::TextTool;
using namespace Inkscape;

/* The clipboard handling is in ui/clipboard.cpp now. There are some legacy functions left here,
because the layer manipulation code uses them. It should be rewritten specifically
for that purpose. */


// helper for printing error messages, regardless of whether we have a GUI or not
// If desktop == NULL, errors will be shown on stderr
static void
selection_display_message(SPDesktop *desktop, Inkscape::MessageType msgType, Glib::ustring const &msg)
{
    if (desktop) {
        desktop->messageStack()->flash(msgType, msg);
    } else {
        if (msgType == Inkscape::IMMEDIATE_MESSAGE ||
            msgType == Inkscape::WARNING_MESSAGE ||
            msgType == Inkscape::ERROR_MESSAGE) {
            g_printerr("%s\n", msg.c_str());
        }
    }
}

namespace Inkscape {

void SelectionHelper::selectAll(SPDesktop *dt)
{
    NodeTool *nt = dynamic_cast<NodeTool*>(dt->getTool());
    if (nt) {
        if (!nt->_multipath->empty()) {
            nt->_multipath->selectSubpaths();
            return;
        }
    }
    sp_edit_select_all(dt);
}

void SelectionHelper::selectAllInAll(SPDesktop *dt)
{
    NodeTool *nt = dynamic_cast<NodeTool*>(dt->getTool());
    if (nt) {
        nt->_selected_nodes->selectAll();
    } else {
        sp_edit_select_all_in_all_layers(dt);
    }
}

void SelectionHelper::selectNone(SPDesktop *dt)
{
    NodeTool *nt = dynamic_cast<NodeTool*>(dt->getTool());
    if (nt && !nt->_selected_nodes->empty()) {
        nt->_selected_nodes->clear();
    } else if (!dt->getSelection()->isEmpty()) {
        dt->getSelection()->clear();
    }
    // PROOF: with nothing selected, Inkscape switched to the selection tool here. Escape and
    // Deselect never change the tool in Illustrator, so the current tool stays.
}

void SelectionHelper::selectSameFillStroke(SPDesktop *dt)
{
    sp_select_same_fill_stroke_style(dt, true, true, true);
}

void SelectionHelper::selectSameFillColor(SPDesktop *dt)
{
    sp_select_same_fill_stroke_style(dt, true, false, false);
}

void SelectionHelper::selectSameStrokeColor(SPDesktop *dt)
{
    sp_select_same_fill_stroke_style(dt, false, true, false);
}

void SelectionHelper::selectSameStrokeStyle(SPDesktop *dt)
{
    sp_select_same_fill_stroke_style(dt, false, false, true);
}

void SelectionHelper::selectSameObjectType(SPDesktop *dt)
{
    sp_select_same_object_type(dt);
}

void SelectionHelper::invert(SPDesktop *dt)
{
    NodeTool *nt = dynamic_cast<NodeTool*>(dt->getTool());
    if (nt) {
        nt->_multipath->invertSelectionInSubpaths();
    } else {
        sp_edit_invert(dt);
    }
}

void SelectionHelper::invertAllInAll(SPDesktop *dt)
{
    NodeTool *nt = dynamic_cast<NodeTool*>(dt->getTool());
    if (nt) {
        nt->_selected_nodes->invertSelection();
    } else {
        sp_edit_invert_in_all_layers(dt);
    }
}

void SelectionHelper::reverse(SPDesktop *dt)
{
    // TODO make this a virtual method of event context!
    NodeTool *nt = dynamic_cast<NodeTool*>(dt->getTool());
    if (nt) {
        nt->_multipath->reverseSubpaths();
    } else {
        dt->getSelection()->pathReverse();
    }
}

/*
 * Fixes the current selection, removing locked objects from it
 */
void SelectionHelper::fixSelection(SPDesktop *dt)
{
    if (!dt) {
        return;
    }

    Inkscape::Selection *selection = dt->getSelection();

    std::vector<SPItem*> items;

    auto selList = selection->items();

    for (auto item : selList | std::views::reverse) {
        if (item &&
            !dt->layerManager().isLayer(item) &&
            !item->isLocked())
        {
            items.push_back(item);
        }
    }

    selection->setList(items);
}

} // namespace Inkscape

/**
 * Copies repr and its inherited css style elements, along with the accumulated transform 'full_t',
 * then prepends the copy to 'clip'.
 */
static void sp_selection_copy_one(Inkscape::XML::Node *repr, Geom::Affine full_t, std::vector<Inkscape::XML::Node*> &clip, Inkscape::XML::Document* xml_doc)
{
    Inkscape::XML::Node *copy = repr->duplicate(xml_doc);

    // copy complete inherited style
    SPCSSAttr *css = sp_repr_css_attr_inherited(repr, "style");
    sp_repr_css_set(copy, css, "style");
    sp_repr_css_attr_unref(css);

    // write the complete accumulated transform passed to us
    // (we're dealing with unattached repr, so we write to its attr
    // instead of using sp_item_set_transform)
    copy->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(full_t));

    clip.insert(clip.begin(),copy);
}

static void sp_selection_copy_impl(std::vector<SPItem*> const &items, std::vector<Inkscape::XML::Node*> &clip, Inkscape::XML::Document* xml_doc)
{
    // Sort items:
    std::vector<SPItem*> sorted_items(items);
    sort(sorted_items.begin(),sorted_items.end(),sp_object_compare_position_bool);

    // Copy item reprs:
    for (auto item : sorted_items) {
        if (item) {
            sp_selection_copy_one(item->getRepr(), item->i2doc_affine(), clip, xml_doc);
        } else {
            g_assert_not_reached();
        }
    }
    reverse(clip.begin(),clip.end());
}

// TODO check if parent parameter should be changed to SPItem, of if the code should handle non-items.
static std::vector<Inkscape::XML::Node *> sp_selection_paste_impl(SPDocument *doc, SPObject *parent,
                                                                  std::vector<Inkscape::XML::Node *> &clip,
                                                                  Inkscape::XML::Node *after = nullptr)
{
    assert(!after || after->parent() == parent->getRepr());
    assert(!parent->cloned);

    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    auto parentItem = cast<SPItem>(parent);
    g_assert(parentItem);

    std::vector<Inkscape::XML::Node*> copied;
    // add objects to document
    for (auto repr : clip) {
        Inkscape::XML::Node *copy = repr->duplicate(xml_doc);

        // premultiply the item transform by the accumulated parent transform in the paste layer
        auto const local = parentItem->i2doc_affine();
        if (!local.isIdentity()) {
            char const *t_str = copy->attribute("transform");
            Geom::Affine item_t;
            if (t_str) {
                sp_svg_transform_read(t_str, &item_t);
            }
            item_t *= local.inverse();
            // (we're dealing with unattached repr, so we write to its attr instead of using sp_item_set_transform)
            copy->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(item_t));
            if (copy->attribute("inkscape:original-d")) {
                copy->setAttribute("d", copy->attribute("inkscape:original-d"));
            }
        }

        parent->getRepr()->addChild(copy, after);
        after = copy;

        copied.push_back(copy);
        Inkscape::GC::release(copy);
    }
    return copied;
}

static void sp_selection_delete_impl(std::vector<SPItem*> const &items, bool propagate = true, bool propagate_descendants = true)
{
    for (auto item : items) {
        sp_object_ref(item, nullptr);
    }
    for (auto item : items) {
        item->deleteObject(propagate, propagate_descendants);
        sp_object_unref(item, nullptr);
    }
}

void ObjectSet::deleteItems(bool skip_undo)
{
    if (isEmpty() && !skip_undo) {
        selection_display_message(desktop(),Inkscape::WARNING_MESSAGE, _("<b>Nothing</b> was deleted."));
        return;
    }

    auto selected = items_vector();
    clear();
    sp_selection_delete_impl(selected);

    if (skip_undo) {
        return;
    }

    if (SPDesktop *dt = desktop()) {
        dt->layerManager().currentLayer()->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);

        /* A tool may have set up private information in its selection context
         * that depends on desktop items.  I think the only sane way to deal with
         * this currently is to reset the tool which will reset its
         * associated selection context.  For example: deleting an object
         * while moving it around the canvas.
         * We copy the string so we donʼt pass reference to member of the tool that is being reset.
         */
        dt->setTool(std::string{dt->getTool()->getPrefsPath()});
    }

    if(document()) {
        DocumentUndo::done(document(), RC_("Undo", "Delete"), INKSCAPE_ICON("edit-delete"));
    }
}



static void add_ids_recursive(std::vector<const gchar *> &ids, SPObject *obj)
{
    if (obj) {
        ids.push_back(obj->getId());

        if (is<SPGroup>(obj)) {
            for (auto& child: obj->children) {
                add_ids_recursive(ids, &child);
            }
        }
    }
}

void ObjectSet::duplicate(bool suppressDone, bool duplicateLayer, bool includeHidden)
{
    if(duplicateLayer && !desktop() ){
        //TODO: understand why layer management is tied to desktop and not to document.
        return;
    }

    SPDocument *doc = document();

    if(!doc)
        return;

    Inkscape::XML::Document* xml_doc = doc->getReprDoc();

    // check if something is selected
    if (isEmpty() && !duplicateLayer) {
        selection_display_message(desktop(),Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to duplicate."));
        return;
    }

    std::vector<Inkscape::XML::Node *> reprs;
    if (duplicateLayer) {
        reprs = {desktop()->layerManager().currentLayer()->getRepr()};
    } else {
        reprs = xmlNodes_vector();
    }

    clear();

    std::vector<SPItem *> items;
    for(auto old_repr : reprs) {
        auto item = cast<SPItem>(doc->getObjectByRepr(old_repr));
        if (item) {
            items.push_back(item);
            auto lpeitem = cast<SPLPEItem>(item);
            if (lpeitem) {
                for (auto satellite : lpeitem->get_satellites(false, true, true)) {
                    if (satellite) {
                        auto item2 = cast<SPItem>(satellite);
                        if (item2 && std::find(items.begin(), items.end(), item2) == items.end()) {
                            items.push_back(item2);
                        }
                    }
                }
            }
        }
    }
    for(auto item : items) {
        if (std::find(reprs.begin(), reprs.end(), item->getRepr()) == reprs.end()) {
            reprs.push_back(item->getRepr());
        }
    }
    // sorting items from different parents sorts each parent's subset without possibly mixing
    // them, just what we need
    sort(reprs.begin(),reprs.end(),sp_repr_compare_position_bool);

    std::vector<const gchar *> old_ids;
    std::vector<const gchar *> new_ids;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool relink_clones = prefs->getBool("/options/relinkclonesonduplicate/value");
    const bool fork_livepatheffects = prefs->getBool("/options/forklpeonduplicate/value", true);

    // check ref-d shapes, split in defs|internal|external
    // add external & defs to reprs
    auto text_refs = text_categorize_refs(doc, reprs.begin(), reprs.end(),
            static_cast<text_ref_t>(TEXT_REF_DEF | TEXT_REF_EXTERNAL | TEXT_REF_INTERNAL));
    for (auto const &ref : text_refs) {
        if (ref.second == TEXT_REF_DEF || ref.second == TEXT_REF_EXTERNAL) {
            reprs.push_back(doc->getObjectById(ref.first)->getRepr());
        }
    }
    // Re-sort after adding text reference dependencies so duplication preserves stacking order.
    sort(reprs.begin(), reprs.end(), sp_repr_compare_position_bool);

    std::vector<Inkscape::XML::Node*> copies;
    for(auto old_repr : reprs) {
        Inkscape::XML::Node *parent = old_repr->parent();
        Inkscape::XML::Node *copy = old_repr->duplicate(xml_doc);

        if (!duplicateLayer || sp_repr_is_def(old_repr)) {
            parent->appendChild(copy);
        } else if (sp_repr_is_layer(old_repr)) {
            parent->addChild(copy, old_repr);
        } else {
            // duplicateLayer, non-layer, non-def
            // external nodes -- append to new layer
            // text_relink will ignore extra nodes in layer children
            copies[0]->appendChild(copy);
        }
        SPObject *old_obj = doc->getObjectByRepr(old_repr);
        SPObject *new_obj = doc->getObjectByRepr(copy);
        if (old_obj && new_obj && !old_obj->_tmpsuccessor) {
            old_obj->setTmpSuccessor(new_obj);
        }
        if (relink_clones) {
            add_ids_recursive(old_ids, old_obj);
            add_ids_recursive(new_ids, new_obj);
        }

        copies.push_back(copy);
        Inkscape::GC::release(copy);
    }

    // Relink copied text nodes to copied reference shapes
    text_relink_refs(text_refs, reprs.begin(), reprs.end(), copies.begin());

    // copies contains def nodes, we don't want that in our selection
    std::vector<Inkscape::XML::Node*> newsel;
    if (!duplicateLayer) {
        // compute newsel, by removing def nodes from copies
        for (auto node : copies) {
            // hide on duple this is done to dont show autoselected hidden LPE items satellites
            // is only a make up if at any point we think is better keep selected items reselected on duple
            // please roll back or make some more loops to handle well, keep as it for speed
            // and simplicity
            auto itm = cast<SPItem>(doc->getObjectByRepr(node));
            if (!sp_repr_is_def(node) && includeHidden || (!itm || !itm->isHidden())) {
                newsel.push_back(node);
            }
        }
    }

    if (relink_clones) {

        g_assert(old_ids.size() == new_ids.size());

        for (unsigned int i = 0; i < old_ids.size(); i++) {
            const gchar *id = old_ids[i];
            SPObject *old_clone = doc->getObjectById(id);
            auto use = cast<SPUse>(old_clone);
            auto offset = cast<SPOffset>(old_clone);
            auto text = cast<SPText>(old_clone);
            auto path = cast<SPPath>(old_clone);
            if (use) {
                SPItem *orig = use->get_original();
                if (!orig) // orphaned
                    continue;
                for (unsigned int j = 0; j < old_ids.size(); j++) {
                    if (!strcmp(orig->getId(), old_ids[j])) {
                        // we have both orig and clone in selection, relink
                        // std::cout << id  << " old, its ori: " << orig->getId() << "; will relink:" << new_ids[i] << " to " << new_ids[j] << "\n";
                        SPObject *new_clone = doc->getObjectById(new_ids[i]);
                        new_clone->setAttribute("xlink:href", Glib::ustring("#") + new_ids[j]);
                        new_clone->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
                    }
                }
            } else if (offset) {
                gchar *source_href = offset->sourceHref;
                for (guint j = 0; j < old_ids.size(); j++) {
                    if (source_href && source_href[0]=='#' && !strcmp(source_href+1, old_ids[j])) {
                        doc->getObjectById(new_ids[i])->setAttribute("xlink:href", Glib::ustring("#") + new_ids[j]);
                    }
                }
            } else if (text) {
                auto textpath = cast<SPTextPath>(text->firstChild());
                if (!textpath) continue;
                const gchar *source_href = sp_textpath_get_path_item(textpath)->getId();
                for (guint j = 0; j < old_ids.size(); j++) {
                    if (!strcmp(source_href, old_ids[j])) {
                        doc->getObjectById(new_ids[i])->firstChild()->setAttribute("xlink:href", Glib::ustring("#") + new_ids[j]);
                    }
                }
            } else if (path) {
                if (old_clone->getAttribute("inkscape:connection-start") != nullptr) {
                    const char *old_start = old_clone->getAttribute("inkscape:connection-start");
                    const char *old_end = old_clone->getAttribute("inkscape:connection-end");
                    SPObject *new_clone = doc->getObjectById(new_ids[i]);
                    for (guint j = 0; j < old_ids.size(); j++) {
                        if(old_start == Glib::ustring("#") + old_ids[j]) {
                            new_clone->setAttribute("inkscape:connection-start", Glib::ustring("#") + new_ids[j]);
                        }
                        if(old_end == Glib::ustring("#") + old_ids[j]) {
                            new_clone->setAttribute("inkscape:connection-end", Glib::ustring("#") + new_ids[j]);
                        }
                    }
                }
            }
        }
    }
    for (auto node : copies) {
        if (fork_livepatheffects) {
            SPObject *new_obj = doc->getObjectByRepr(node);
            auto newLPEObj = cast<SPLPEItem>(new_obj);
            if (newLPEObj) {
                // force always fork
                newLPEObj->forkPathEffectsIfNecessary(1, true, true);
                sp_lpe_item_update_patheffect(newLPEObj, false, true, true);
            }
        }
    }
    for(auto old_repr : reprs) {
        SPObject *old_obj = doc->getObjectByRepr(old_repr);
        if (old_obj) {
            old_obj->fixTmpSuccessors();
            old_obj->unsetTmpSuccessor();
        }
    }

    if (!duplicateLayer) {
        setReprList(newsel);
        if ( !suppressDone ) {
            DocumentUndo::done(document(), RC_("Undo", "Duplicate"), INKSCAPE_ICON("edit-duplicate"));
        }
    } else {
        if ( !suppressDone ) {
            DocumentUndo::done(document(), RC_("Undo", "Duplicate"), INKSCAPE_ICON("edit-duplicate"));
        }
        SPObject* new_layer = doc->getObjectByRepr(copies[0]);

        if (auto label = new_layer->label()) {
            if (std::string(label).find("copy") == std::string::npos) {
                gchar* name = g_strdup_printf(_("%s copy"), label);
                desktop()->layerManager().renameLayer(new_layer, name, true);
                g_free(name);
            }
        }
    }

}

void sp_edit_clear_all(Inkscape::Selection *selection)
{
    if (!selection)
        return;

    auto desktop = selection->desktop();
    SPDocument *doc = desktop->getDocument();
    selection->clear();

    auto group = desktop->layerManager().currentLayer();
    g_return_if_fail(group);
    std::vector<SPItem*> items = group->item_list();

    for(auto & item : items){
        item->deleteObject();
    }

    DocumentUndo::done(doc, RC_("Undo", "Delete all"), "");
}

/*
 * Return a list of SPItems that are the children of 'list'
 *
 * list - source list of items to search in
 * desktop - desktop associated with the source list
 * exclude - list of items to exclude from result
 * onlyvisible - TRUE includes only items visible on canvas
 * onlysensitive - TRUE includes only non-locked items
 * ingroups - TRUE to recursively get grouped items children
 */
static void get_all_items_recursive(std::vector<SPItem*> &list, SPObject *from, SPDesktop *desktop, bool onlyvisible, bool onlysensitive, bool ingroups, std::vector<SPItem*> const &exclude)
{
    for (auto &child : from->children) {
        auto item = cast<SPItem>(&child);
        if (item &&
            !desktop->layerManager().isLayer(item) &&
            (!onlysensitive || !item->isLocked()) &&
            (!onlyvisible || !desktop->itemIsHidden(item)) &&
            (exclude.empty() || std::find(exclude.begin(), exclude.end(), &child) == exclude.end()))
        {
            list.emplace_back(item);
        }

        if (ingroups || (item && desktop->layerManager().isLayer(item))) {
            get_all_items_recursive(list, &child, desktop, onlyvisible, onlysensitive, ingroups, exclude);
        }
    }
}

std::vector<SPItem*> get_all_items(SPObject *from, SPDesktop *desktop, bool onlyvisible, bool onlysensitive, bool ingroups, std::vector<SPItem*> const &exclude)
{
    std::vector<SPItem*> list;
    get_all_items_recursive(list, from, desktop, onlyvisible, onlysensitive, ingroups, exclude);
    std::reverse(list.begin(), list.end()); // Todo: For compatibility; is it necessary?
    return list;
}

static void sp_edit_select_all_full(SPDesktop *dt, bool force_all_layers, bool invert)
{
    if (!dt)
        return;

    Inkscape::Selection *selection = dt->getSelection();

    auto layer = dt->layerManager().currentLayer();
    g_return_if_fail(layer);

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    PrefsSelectionContext inlayer = (PrefsSelectionContext) prefs->getInt("/options/kbselection/inlayer", PREFS_SELECTION_LAYER);
    bool onlyvisible = prefs->getBool("/options/kbselection/onlyvisible", true);
    bool onlysensitive = prefs->getBool("/options/kbselection/onlysensitive", true);

    std::vector<SPItem *> exclude;
    if (invert) {
        exclude = selection->items_vector();
    }

    if (force_all_layers) {
        inlayer = PREFS_SELECTION_ALL;
    }

    std::vector<SPItem *> items;

    switch (inlayer) {
        case PREFS_SELECTION_LAYER: {
            if ((onlysensitive && layer->isLocked()) || (onlyvisible && dt->itemIsHidden(layer))) {
                return;
            }

            for (auto item : layer->item_list() | std::views::reverse) {
                if (!onlysensitive || !item->isLocked()) {
                    if (!onlyvisible || !dt->itemIsHidden(item)) {
                        if (!dt->layerManager().isLayer(item)) {
                            if (!invert || exclude.end() == std::find(exclude.begin(),exclude.end(),item)) {
                                items.push_back(item); // leave it in the list
                            }
                        }
                    }
                }
            }

            break;
        }
        case PREFS_SELECTION_LAYER_RECURSIVE: {
            items = get_all_items(dt->layerManager().currentLayer(), dt, onlyvisible, onlysensitive, FALSE, exclude);
            break;
        }
        default: {
            items = get_all_items(dt->layerManager().currentRoot(), dt, onlyvisible, onlysensitive, FALSE, exclude);
            break;
        }
    }

    selection->setList(items);
}

void sp_edit_select_all(SPDesktop *desktop)
{
    sp_edit_select_all_full(desktop, false, false);
}

void sp_edit_select_all_in_all_layers(SPDesktop *desktop)
{
    sp_edit_select_all_full(desktop, true, false);
}

void sp_edit_invert(SPDesktop *desktop)
{
    sp_edit_select_all_full(desktop, false, true);
}

void sp_edit_invert_in_all_layers(SPDesktop *desktop)
{
    sp_edit_select_all_full(desktop, true, true);
}

Inkscape::XML::Node* ObjectSet::group(bool is_anchor) {
    SPDocument *doc = document();
    if(!doc)
        return nullptr;
    if (isEmpty()) {
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Select <b>some objects</b> to group."));
        return nullptr;
    }
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();
    Inkscape::XML::Node *group = xml_doc->createElement(is_anchor ? "svg:a" : "svg:g");

    auto p = xmlNodes_vector();
    std::sort(p.begin(), p.end(), sp_repr_compare_position_bool);
    clear();

    // Remember the position and parent of the topmost object.
    Inkscape::XML::Node *topmost = p.back();
    Inkscape::XML::Node *topmost_parent = topmost->parent();

    // Add as close to the top as we can get it
    topmost_parent->addChild(group, topmost);

    for(auto current : p){
        if (current->parent() == topmost_parent) {

            Inkscape::XML::Node *spnew = current->duplicate(xml_doc);
            sp_repr_unparent(current);
            group->appendChild(spnew);
            Inkscape::GC::release(spnew);

        } else { // move it to topmost_parent first
            std::vector<Inkscape::XML::Node*> temp_clip;

            // At this point, current may already have no item, due to its being a clone whose original is already moved away
            // So we copy it artificially calculating the transform from its repr->attr("transform") and the parent transform
            gchar const *t_str = current->attribute("transform");
            Geom::Affine item_t(Geom::identity());
            if (t_str)
                sp_svg_transform_read(t_str, &item_t);
            auto parent_item = cast<SPItem>(doc->getObjectByRepr(current->parent()));
            assert(parent_item);
            item_t *= parent_item->i2doc_affine();
            // FIXME: when moving both clone and original from a transformed group (either by
            // grouping into another parent, or by cut/paste) the transform from the original's
            // parent becomes embedded into original itself, and this affects its clones. Fix
            // this by remembering the transform diffs we write to each item into an array and
            // then, if this is clone, looking up its original in that array and pre-multiplying
            // it by the inverse of that original's transform diff.

            sp_selection_copy_one(current, item_t, temp_clip, xml_doc);
            sp_repr_unparent(current);

            // paste into topmost_parent (temporarily)
            std::vector<Inkscape::XML::Node*> copied = sp_selection_paste_impl(doc, doc->getObjectByRepr(topmost_parent), temp_clip);
            if (!temp_clip.empty())temp_clip.clear() ;
            if (!copied.empty()) { // if success,
                // take pasted object (now in topmost_parent)
                Inkscape::XML::Node *in_topmost = copied.back();
                // make a copy
                Inkscape::XML::Node *spnew = in_topmost->duplicate(xml_doc);
                // remove pasted
                sp_repr_unparent(in_topmost);
                // put its copy into group
                group->appendChild(spnew);
                Inkscape::GC::release(spnew);
                copied.clear();
            }
        }
    }

    set(doc->getObjectByRepr(group));

    return group;
}

void ObjectSet::popFromGroup(){
    if (isEmpty()) {
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("<b>No objects selected</b> to pop out of group."));
        return;
    }

    std::set<SPObject*> grandparents;

    for (auto *obj : items()) {
        auto parent_group = cast<SPGroup>(obj->parent);
        if (!parent_group || !parent_group->parent || SP_IS_LAYER(parent_group)) {
            selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Selection <b>not in a group</b>."));
            return;
        }
        grandparents.insert(parent_group->parent);
    }

    assert(!grandparents.empty());

    if (grandparents.size() > 1) {
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE,
                                  _("Objects in selection must have the same grandparents."));
        return;
    }

    toLayer(*grandparents.begin());

    if(document())
        DocumentUndo::done(document(), RC_("Undo", "Pop selection from group"), INKSCAPE_ICON("object-ungroup-pop-selection"));

}

/**
 * Finds the first clone in `objects` which references an item in `groups`.
 * The search is recursive, the children of `objects` are searched as well.
 * Return NULL if no such clone is found.
 */
template <typename Objects>
static SPUse *find_clone_to_group(Objects &&objects, std::set<SPGroup *> const &groups)
{
    assert(!groups.count(nullptr));

    for (auto *obj : objects) {
        if (auto *use = cast<SPUse>(obj)) {
            if (auto root = use->root()) {
                if (groups.count(cast_unsafe<SPGroup>(root->clone_original))) {
                    return use;
                }
            }
        }

        if (auto *use = find_clone_to_group(obj->childList(false), groups)) {
            return use;
        }
    }

    return nullptr;
}

/**
 * Ungroup all groups in an object set.
 *
 * Clones of ungrouped groups will be unlinked.
 *
 * Children of groups will not be ungrouped (operation is not recursive).
 *
 * Unlinked clones and children of ungrouped groups will be added to the object set.
 */
static void ungroup_impl(ObjectSet *set)
{
    auto groups_range = set->groups();
    auto const groups = std::set<SPGroup *>(groups_range.begin(), groups_range.end());

    while (auto *use = find_clone_to_group(set->items(), groups)) {
        bool const readd = set->includes(use);
        auto const unlinked = use->unlink();
        if (readd) {
            set->add(unlinked, true);
        }
    }

    std::vector<SPItem *> children;

    for (auto *group : groups) {
        sp_item_group_ungroup(group, children);
    }

    set->addList(children);
}

void ObjectSet::ungroup(bool skip_undo)
{
    if (isEmpty()) {
        if(desktop())
            selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Select a <b>group</b> to ungroup."));
        return;
    }

    if (groups().empty()) {
        if (desktop()) {
            selection_display_message(desktop(), Inkscape::ERROR_MESSAGE, _("<b>No groups</b> to ungroup in the selection."));
        }
        return;
    }

    ungroup_impl(this);
    if (document() && !skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "Ungroup"), INKSCAPE_ICON("object-ungroup"));
    }
}

/**
 * Keep ungrouping until there are no more groups.
 */
void ObjectSet::ungroup_all(bool skip_undo)
{
    std::size_t last = 0;
    while (size() != last) {
        last = size();
        ungroup(skip_undo);
    }
}

/** If items in the list have a common parent, return it, otherwise return NULL */
static SPGroup *
sp_item_list_common_parent_group(decltype(ObjectSet{}.items()) items)
{
    if (items.empty()) {
        return nullptr;
    }
    SPObject *parent = items.front()->parent;
    // Strictly speaking this CAN happen, if user selects <svg> from Inkscape::XML editor
    if (!is<SPGroup>(parent)) {
        return nullptr;
    }
    for (auto item : items) {
        if (item == items.front()) {
            continue;
        }
        if (item->parent != parent) {
            return nullptr;
        }
    }

    return cast<SPGroup>(parent);
}

/** Finds out the minimum common bbox of the selected items. */
static Geom::OptRect
enclose_items(std::vector<SPItem*> const &items)
{
    g_assert(!items.empty());

    Geom::OptRect r;
    for (auto item : items) {
        r.unionWith(item->documentVisualBounds());
    }
    return r;
}

// TODO determine if this is intentionally different from SPObject::getPrev()
static SPObject *prev_sibling(SPObject *child)
{
    SPObject *prev = nullptr;
    if ( child && cast<SPGroup>(child->parent) ) {
        prev = child->getPrev();
    }
    return prev;
}

void ObjectSet::raise(bool skip_undo){

    if(isEmpty()){
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to raise."));
        return;
    }

    SPGroup const *group = sp_item_list_common_parent_group(items());
    if (!group) {
        if(desktop())
            selection_display_message(desktop(), Inkscape::ERROR_MESSAGE, _("You cannot raise/lower objects from <b>different groups</b> or <b>layers</b>."));
        return;
    }

    auto items_copy = items_vector();
    Inkscape::XML::Node *grepr = const_cast<Inkscape::XML::Node *>(items_copy.front()->parent->getRepr());

    /* Construct reverse-ordered list of selected children. */
    auto rev = items_copy;
    std::sort(rev.begin(), rev.end(), sp_item_repr_compare_position_bool);

    // Determine the common bbox of the selected items.
    Geom::OptRect selected = enclose_items(items_copy);

    // Iterate over all objects in the selection (starting from top).
    if (selected) {
        for (auto child : rev) {
            // for each selected object, find the next sibling
            for (SPObject *newref = child->getNext(); newref; newref = newref->getNext()) {
                // if the sibling is an item AND overlaps our selection,
                auto newItem = cast<SPItem>(newref);
                if (newItem) {
                    Geom::OptRect newref_bbox = newItem->documentVisualBounds();
                    if ( newref_bbox && selected->intersects(*newref_bbox) ) {
                        // AND if it's not one of our selected objects,
                        if ( std::find(items_copy.begin(),items_copy.end(),newref)==items_copy.end()) {
                            // move the selected object after that sibling
                            grepr->changeOrder(child->getRepr(), newref->getRepr());
                        }
                        break;
                    }
                }
            }
        }
    }
    if (document() && !skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "Raise"), INKSCAPE_ICON("selection-raise"));
    }
}


void ObjectSet::raiseToTop(bool skip_undo) {
    if (isEmpty()) {
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to raise."));
        return;
    }

    SPGroup const *group = sp_item_list_common_parent_group(items());
    if (!group) {
        selection_display_message(desktop(), Inkscape::ERROR_MESSAGE, _("You cannot raise/lower objects from <b>different groups</b> or <b>layers</b>."));
        return;
    }

    auto rl = xmlNodes_vector();
    std::sort(rl.begin(), rl.end(), sp_repr_compare_position_bool);

    for (auto repr : rl) {
        repr->setPosition(-1);
    }
    if (document() && !skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "Raise to top"), INKSCAPE_ICON("selection-top"));
    }
}

void ObjectSet::lower(bool skip_undo)
{
    if(isEmpty()){
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to lower."));
        return;
    }

    SPGroup const *group = sp_item_list_common_parent_group(items());
    if (!group) {
        selection_display_message(desktop(), Inkscape::ERROR_MESSAGE, _("You cannot raise/lower objects from <b>different groups</b> or <b>layers</b>."));
        return;
    }

    auto items_copy = items_vector();
    Inkscape::XML::Node *grepr = const_cast<Inkscape::XML::Node *>(items_copy.front()->parent->getRepr());

    // Determine the common bbox of the selected items.
    Geom::OptRect selected = enclose_items(items_copy);

    /* Construct direct-ordered list of selected children. */
    auto rev = items_copy;
    std::sort(rev.begin(), rev.end(), sp_item_repr_compare_position_bool);

    // Iterate over all objects in the selection (starting from top).
    if (selected) {
        for (auto child : rev | std::views::reverse) {
            // for each selected object, find the prev sibling
            for (SPObject *newref = prev_sibling(child); newref; newref = prev_sibling(newref)) {
                // if the sibling is an item AND overlaps our selection,
                auto newItem = cast<SPItem>(newref);
                if (newItem) {
                    Geom::OptRect ref_bbox = newItem->documentVisualBounds();
                    if ( ref_bbox && selected->intersects(*ref_bbox) ) {
                        // AND if it's not one of our selected objects,
                        if (std::find(items_copy.begin(), items_copy.end(), newref) == items_copy.end()) {
                            // move the selected object before that sibling
                            if (auto put_after = prev_sibling(newref))
                                grepr->changeOrder(child->getRepr(), put_after->getRepr());
                            else
                                child->getRepr()->setPosition(0);
                        }
                        break;
                    }
                }
            }
        }
    }
    if(document() && !skip_undo)
        DocumentUndo::done(document(), RC_("Undo", "Lower"), INKSCAPE_ICON("selection-lower"));
}


void ObjectSet::lowerToBottom(bool skip_undo)
{
    if(!document())
        return;
    if (isEmpty()) {
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to lower to bottom."));
        return;
    }

    SPGroup const *group = sp_item_list_common_parent_group(items());
    if (!group) {
        selection_display_message(desktop(), Inkscape::ERROR_MESSAGE, _("You cannot raise/lower objects from <b>different groups</b> or <b>layers</b>."));
        return;
    }

    auto rl = xmlNodes_vector();
    std::sort(rl.begin(), rl.end(), sp_repr_compare_position_bool);

    for (auto const repr : rl | std::views::reverse) {
        int minpos = 0;
        auto const pp = document()->getObjectByRepr(repr->parent());
        g_assert(is<SPGroup>(pp));
        for (auto &pc : pp->children) {
            if (is<SPItem>(&pc)) {
                break;
            }
            minpos++;
        }
        repr->setPosition(minpos);
    }
    if (document() && !skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "Lower to bottom"), INKSCAPE_ICON("selection-bottom"));
    }
}

void ObjectSet::stackUp(bool skip_undo) {
    if (isEmpty()) {
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to stack up."));
        return;
    }

    auto selection = items_vector();
    std::sort(selection.begin(), selection.end(), sp_item_repr_compare_position_bool);

    for (auto item : selection | std::views::reverse) {
        if (!item->raiseOne()) { // stop if top was reached
            if(document() && !skip_undo) {
                DocumentUndo::cancel(document());
            }
            selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("We hit top."));
            return;
        }
    }

    if (document() && !skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "stack up"), INKSCAPE_ICON("layer-raise"));
    }
}

void ObjectSet::stackDown(bool skip_undo) {
    if (isEmpty()) {
        selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to stack down."));
        return;
    }

    auto selection = items_vector();
    std::sort(selection.begin(), selection.end(), sp_item_repr_compare_position_bool);

    for (auto item : selection) {
        if (!item->lowerOne()) { // stop if bottom was reached
            if (document() && !skip_undo) {
                DocumentUndo::cancel(document());
            }
            selection_display_message(desktop(), Inkscape::WARNING_MESSAGE, _("We hit bottom."));
            return;
        }
    }

    if (document() && !skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "stack down"), INKSCAPE_ICON("layer-lower"));
    }
}

void ObjectSet::cut()
{
    copy();

    // Text and Node tools have their own CUT responses instead of deleteItems
    if (auto text_tool = dynamic_cast<TextTool*>(_desktop->getTool())) {
        if (text_tool->deleteSelection()) {
            DocumentUndo::done(desktop()->getDocument(), RC_("Undo", "Cut text"), INKSCAPE_ICON("draw-text"));
            return;
        }
    }

    auto node_tool = dynamic_cast<Inkscape::UI::Tools::NodeTool *>(desktop()->getTool());
    if (node_tool && node_tool->_selected_nodes) {
        auto prefs = Preferences::get();
        // This takes care of undo internally
        node_tool->_multipath->deleteNodes((Inkscape::UI::NodeDeleteMode)prefs->getInt("/tools/node/delete-mode-cut", (int)Inkscape::UI::NodeDeleteMode::gap_lines));
        return;
    }

    deleteItems();
}

/**
 * \pre item != NULL
 */
SPCSSAttr *
take_style_from_item(SPObject *object)
{
    // CPPIFY:
    // This function should only take SPItems, but currently SPString is not an Item.

    // write the complete cascaded style, context-free
    SPCSSAttr *css = sp_css_attr_from_object(object, SP_STYLE_FLAG_IFSET);
    if (!css) {
        return nullptr;
    }

    if ((is<SPGroup>(object) && object->firstChild()) ||
        (is<SPText>(object) && object->firstChild() && !object->firstChild()->getNext())) {
        // if this is a text with exactly one tspan child, merge the style of that tspan as well
        // If this is a group, merge the style of its topmost (last) child with style
        for (auto &element : object->children | std::views::reverse) {
            if (element.style) {
                if (auto temp = sp_css_attr_from_object(&element, SP_STYLE_FLAG_IFSET)) {
                    sp_repr_css_merge(css, temp);
                    sp_repr_css_attr_unref(temp);
                }
                break;
            }
        }
    }

    // Remove black-listed properties (those that should not be used in a default style)
    css = sp_css_attr_unset_blacklist(css);

    if (!(is<SPText>(object) || is<SPTSpan>(object) || is<SPTRef>(object) || is<SPString>(object))) {
        // do not copy text properties from non-text objects, it's confusing
        css = sp_css_attr_unset_text(css);
    }


    if (auto item = cast<SPItem>(object)) {
        // FIXME: also transform gradient/pattern fills, by forking? NO, this must be nondestructive
        double ex = item->i2doc_affine().descrim();
        if (ex != 1.0) {
            css = sp_css_attr_scale(css, ex);
        }
    }

    return css;
}

void ObjectSet::copy()
{
    Inkscape::UI::ClipboardManager *cm = Inkscape::UI::ClipboardManager::get();
    cm->copy(this);
}

void sp_selection_paste(SPDesktop *desktop, bool in_place, bool on_page)
{
    Inkscape::UI::ClipboardManager *cm = Inkscape::UI::ClipboardManager::get();
    if (cm->paste(desktop, in_place, on_page)) {
        DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Paste"), INKSCAPE_ICON("edit-paste"));
    }
}

void ObjectSet::pasteStyle()
{
    Inkscape::UI::ClipboardManager *cm = Inkscape::UI::ClipboardManager::get();
    if (cm->pasteStyle(this)) {
        DocumentUndo::done(document(), RC_("Undo", "Paste style"), INKSCAPE_ICON("edit-paste-style"));
    }
}

void ObjectSet::pastePathEffect()
{
    Inkscape::UI::ClipboardManager *cm = Inkscape::UI::ClipboardManager::get();
    if (cm->pastePathEffect(this)) {
        DocumentUndo::done(document(), RC_("Undo", "Paste live path effect"), "");
    }
}


static void sp_selection_remove_livepatheffect_impl(SPItem *item)
{
    if ( auto lpeitem = cast<SPLPEItem>(item) ) {
        if ( lpeitem->hasPathEffect() ) {
            lpeitem->removeAllPathEffects(false);
        }
    }
}

void ObjectSet::removeLPE()
{

    // check if something is selected
    if (isEmpty()) {
        if(desktop())
        desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to remove live path effects from."));
        return;
    }
    auto list= items();
    for (auto item : list) {
        sp_selection_remove_livepatheffect_impl(item);

    }

    if (document()) {
        DocumentUndo::done(document(), RC_("Undo", "Remove live path effect"), "");
    }
}

void ObjectSet::removeFilter()
{
    // check if something is selected
    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to remove filters from."));
        return;
    }

    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_unset_property(css, "filter");
    if (SPDesktop *d = desktop()) {
        sp_desktop_set_style(this, desktop(), css);
        // Refreshing the current tool (by switching to same tool)
        // will refresh tool's private information in it's selection context that
        // depends on desktop items.
        set_active_tool (d, get_active_tool(d));
    } else {
        auto list = items();
        for (auto itemlist : list) {
            sp_desktop_apply_css_recursive(itemlist, css, true);
        }
    }
    sp_repr_css_attr_unref(css);
    if (document()) {
        DocumentUndo::done(document(), RC_("Undo", "Remove filter"), "");
    }
}


void ObjectSet::pasteSize(bool apply_x, bool apply_y)
{
    Inkscape::UI::ClipboardManager *cm = Inkscape::UI::ClipboardManager::get();
    if (cm->pasteSize(this, false, apply_x, apply_y)) {
        DocumentUndo::done(document(), RC_("Undo", "Paste size"), INKSCAPE_ICON("edit-paste-size"));
    }
}

void ObjectSet::pasteSizeSeparately(bool apply_x, bool apply_y)
{
    Inkscape::UI::ClipboardManager *cm = Inkscape::UI::ClipboardManager::get();
    if (cm->pasteSize(this, true, apply_x, apply_y)) {
        DocumentUndo::done(document(), RC_("Undo", "Paste size separately"), INKSCAPE_ICON("edit-paste-size-separately"));
    }
}

/**
 * Ensures that the clones of objects are not modified when moving objects between layers.
 * Calls the same function as ungroup
 */
void sp_selection_change_layer_maintain_clones(std::vector<SPItem*> const &items,SPObject *where)
{
    for (auto item : items) {
        if (item) {
            auto oldparent = cast<SPItem>(item->parent);
            auto newparent = cast<SPItem>(where);
            sp_item_group_ungroup_handle_clones(item,
                    (oldparent->i2doc_affine())
                    *((newparent->i2doc_affine()).inverse()));
        }
    }
}

void ObjectSet::toNextLayer(bool skip_undo)
{
    if (!desktop()) {
        return;
    }
    SPDesktop *dt=desktop(); //TODO make it desktop-independent

    // check if something is selected
    if (isEmpty()) {
        dt->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to move to the layer above."));
        return;
    }

    auto items_copy = items_vector();

    bool no_more = false; // Set to true, if no more layers above
    SPObject *next=Inkscape::next_layer(dt->layerManager().currentRoot(), dt->layerManager().currentLayer());
    if (next) {
        clear();
        sp_selection_change_layer_maintain_clones(items_copy,next);
        std::vector<Inkscape::XML::Node*> temp_clip;
        sp_selection_copy_impl(items_copy, temp_clip, dt->doc()->getReprDoc());
        sp_selection_delete_impl(items_copy, false, false);
        next=Inkscape::next_layer(dt->layerManager().currentRoot(), dt->layerManager().currentLayer()); // Fixes bug 1482973: crash while moving layers
        std::vector<Inkscape::XML::Node*> copied;
        if (next) {
            copied = sp_selection_paste_impl(dt->getDocument(), next, temp_clip);
        } else {
            copied = sp_selection_paste_impl(dt->getDocument(), dt->layerManager().currentLayer(), temp_clip);
            no_more = true;
        }
        setReprList(copied);
        if (next) dt->layerManager().setCurrentLayer(next);
        if ( !skip_undo ) {
            DocumentUndo::done(dt->getDocument(), RC_("Undo", "Raise to next layer"), INKSCAPE_ICON("selection-move-to-layer-above"));
        }
    } else {
        no_more = true;
    }

    if (no_more) {
        dt->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("No more layers above."));
    }

}

void ObjectSet::toPrevLayer(bool skip_undo)
{
    if (!desktop()) {
        return;
    }
    SPDesktop *dt=desktop(); //TODO make it desktop-independent

    // check if something is selected
    if (isEmpty()) {
        dt->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to move to the layer below."));
        return;
    }

    auto items_copy = items_vector();

    bool no_more = false; // Set to true, if no more layers below
    SPObject *next=Inkscape::previous_layer(dt->layerManager().currentRoot(), dt->layerManager().currentLayer());
    if (next) {
        clear();
        sp_selection_change_layer_maintain_clones(items_copy,next);
        std::vector<Inkscape::XML::Node*> temp_clip;
        sp_selection_copy_impl(items_copy, temp_clip, dt->doc()->getReprDoc()); // we're in the same doc, so no need to copy defs
        sp_selection_delete_impl(items_copy, false, false);
        next=Inkscape::previous_layer(dt->layerManager().currentRoot(), dt->layerManager().currentLayer()); // Fixes bug 1482973: crash while moving layers
        std::vector<Inkscape::XML::Node*> copied;
        if (next) {
            copied = sp_selection_paste_impl(dt->getDocument(), next, temp_clip);
        } else {
            copied = sp_selection_paste_impl(dt->getDocument(), dt->layerManager().currentLayer(), temp_clip);
            no_more = true;
        }
        setReprList( copied);
        if (next) dt->layerManager().setCurrentLayer(next);
        if ( !skip_undo ) {
            DocumentUndo::done(dt->getDocument(), RC_("Undo", "Lower to previous layer"), INKSCAPE_ICON("selection-move-to-layer-below"));
        }
    } else {
        no_more = true;
    }

    if (no_more) {
        dt->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("No more layers below."));
    }
}

/**
 * Move selection to group `moveto`, after the last child of `moveto` (if it has any children).
 *
 * @param moveto Layer to move to
 * @param skip_undo Don't call DocumentUndo::done
 *
 * @pre moveto is of type SPItem (or even SPGroup?)
 */
void ObjectSet::toLayer(SPObject *moveto)
{
    if(!document())
        return;

    if (!moveto || !moveto->getRepr()) {
        g_warning("%s moveto is NULL", __func__);
        g_assert_not_reached();
        return;
    }

    toLayer(moveto, moveto->getRepr()->lastChild());
}

/**
 * Move selection to group `moveto`, after child `after`.
 */
void ObjectSet::toLayer(SPObject *moveto, Inkscape::XML::Node *after)
{
    assert(moveto);
    assert(!after || after->parent() == moveto->getRepr());
    assert(document());

    SPDesktop *dt = desktop();

    // check if something is selected
    if (isEmpty()) {
        if(dt)
            dt->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to move."));
        return;
    }

    /* Make sure after is not in the selected group.
     * Iterate after's siblings backwards, finding the nearest that
     * isn't selected. This is important for positioning in the layer.
     */
    while (after && includes(after)) {
        after = after->prev();
    }

    auto items_copy = items_vector();

    if (moveto) {
        clear();
        sp_selection_change_layer_maintain_clones(items_copy,moveto);
        std::vector<Inkscape::XML::Node*> temp_clip;
        sp_selection_copy_impl(items_copy, temp_clip, document()->getReprDoc()); // we're in the same doc, so no need to copy defs
        sp_selection_delete_impl(items_copy, false, false);
        std::vector<Inkscape::XML::Node*> copied = sp_selection_paste_impl(document(), moveto, temp_clip, after);

        setReprList(copied);
        if (!temp_clip.empty()) temp_clip.clear();
        if (moveto && dt) dt->layerManager().setCurrentLayer(moveto);
    }
}

static bool
object_set_contains_original(SPItem *item, ObjectSet *set)
{
    bool contains_original = false;

    SPItem *item_use = item;
    SPItem *item_use_first = item;
    auto use = cast<SPUse>(item_use);
    while (use && item_use && !contains_original)
    {
        item_use = use->get_original();
        use = cast<SPUse>(item_use);
        contains_original |= set->includes(item_use);
        if (item_use == item_use_first)
            break;
    }

    // If it's a tref, check whether the object containing the character
    // data is part of the selection
    auto tref = cast<SPTRef>(item);
    if (!contains_original && tref) {
        contains_original = set->includes(tref->getObjectReferredTo());
    }

    return contains_original;
}


static bool
object_set_contains_both_clone_and_original(ObjectSet *set)
{
    bool clone_with_original = false;
    auto items = set->items();
    for (auto item : items) {
        if (item) {
            clone_with_original |= object_set_contains_original(item, set);
            if (clone_with_original)
                break;
        }
    }
    return clone_with_original;
}

/**
 * Reapply the same transform again.
 */
void ObjectSet::reapplyAffine()
{
    auto cached = _last_affine;
    applyAffine(_last_affine);
    _last_affine = cached;
}

void ObjectSet::clearLastAffine()
{
    _last_affine = Geom::identity(); // Clear last affine
}

/** Apply matrix to the selection.  \a set_i2d is normally true, which means objects are in the
original transform, synced with their reprs, and need to jump to the new transform in one go. A
value of set_i2d==false is only used by seltrans when it's dragging objects live (not outlines); in
that case, items are already in the new position, but the repr is in the old, and this function
then simply updates the repr from item->transform.
 */

void ObjectSet::applyAffine(Geom::Affine const &affine, bool set_i2d, bool compensate,
                                bool adjust_transf_center)
{
    if (isEmpty())
        return;

    _last_affine = affine;

    auto items_copy = items();
    std::deque<SPItem *> ordered_items;
    for (auto item : items_copy) {
        auto clonelpe = cast<SPLPEItem>(item);
        if (clonelpe && clonelpe->hasPathEffectOfType(Inkscape::LivePathEffect::CLONE_ORIGINAL)) {
            ordered_items.push_front(item);
        } else {
            ordered_items.push_back(item);
        }
    }
    for (auto item : ordered_items) {
        if (is<SPRoot>(item) ) {
            // An SVG element cannot have a transform. We could change 'x' and 'y' in response
            // to a translation... but leave that for another day.
            if(desktop())
                desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Cannot transform an embedded SVG."));
            break;
        }

        Geom::Point old_center(0,0);
        if (set_i2d && item->isCenterSet())
            old_center = item->getCenter();

        // If we're moving a connector, we want to detach it
        // from shapes that aren't part of the selection, but
        // leave it attached if they are
        if (Inkscape::UI::Tools::cc_item_is_connector(item)) {
            auto path = cast<SPPath>(item);
            if (path) {
                SPItem *attItem[2] = {nullptr, nullptr};
                path->connEndPair.getAttachedItems(attItem);
                for (int n = 0; n < 2; ++n) {
                    if (!includes(attItem[n])) {
                        sp_conn_end_detach(item, n);
                    }
                }
            } else {
                g_assert_not_reached();
            }
        }

        // "clones are unmoved when original is moved" preference
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        int compensation = prefs->getInt("/options/clonecompensation/value", SP_CLONE_COMPENSATION_UNMOVED);
        bool prefs_unmoved = (compensation == SP_CLONE_COMPENSATION_UNMOVED);
        bool prefs_parallel = (compensation == SP_CLONE_COMPENSATION_PARALLEL);

        SiblingState sibling_state = getSiblingState(item);

        /* If this is a clone and it's selected along with its original, do not move it;
         * it will feel the transform of its original and respond to it itself.
         * Without this, a clone is doubly transformed, very unintuitive.
         *
         * Same for textpath if we are also doing ANY transform to its path: do not touch textpath,
         * letters cannot be squeezed or rotated anyway, they only refill the changed path.
         * Same for linked offset if we are also moving its source: do not move it. */
        if (sibling_state == SiblingState::SIBLING_TEXT_PATH) {
            // Restore item->transform field from the repr, in case it was changed by seltrans.
            item->readAttr(SPAttr::TRANSFORM);
        } else if (sibling_state == SiblingState::SIBLING_TEXT_FLOW_FRAME) {
            // apply the inverse of the region's transform to the <use> so that the flow remains
            // the same (even though the output itself gets transformed)
            for (auto& region: item->children) {
                if (is<SPFlowregion>(&region) || is<SPFlowregionExclude>(&region)) {
                    for (auto& itm: region.children) {
                        auto use = cast<SPUse>(&itm);
                        if ( use ) {
                            use->doWriteTransform(item->transform.inverse(), nullptr, compensate);
                        }
                    }
                }
            }
        } else if (sibling_state == SiblingState::SIBLING_CLONE_ORIGINAL || sibling_state == SiblingState::SIBLING_OFFSET_SOURCE) {
            // We are transforming a clone along with its original. The below matrix juggling is
            // necessary to ensure that they transform as a whole, i.e. the clone's induced
            // transform and its move compensation are both cancelled out.

            // restore item->transform field from the repr, in case it was changed by seltrans
            item->readAttr(SPAttr::TRANSFORM);

            // calculate the matrix we need to apply to the clone to cancel its induced transform from its original
            Geom::Affine parent2dt;
            {
                auto parentItem = cast<SPItem>(item->parent);
                if (parentItem) {
                    parent2dt = parentItem->i2dt_affine();
                } else {
                    g_assert_not_reached();
                }
            }
            Geom::Affine t = parent2dt * affine * parent2dt.inverse();
            Geom::Affine t_inv = t.inverse();
            Geom::Affine result = t_inv * item->transform * t;

            if (sibling_state == SiblingState::SIBLING_CLONE_ORIGINAL && (prefs_parallel || prefs_unmoved) && affine.isTranslation()) {
                // we need to cancel out the move compensation, too

                // find out the clone move, same as in sp_use_move_compensate
                Geom::Affine parent;
                {
                    auto use = cast<SPUse>(item);
                    if (use) {
                        parent = use->get_parent_transform();
                    } else {
                        g_assert_not_reached();
                    }
                }
                Geom::Affine clone_move = parent.inverse() * t * parent;

                if (prefs_parallel) {
                    Geom::Affine move = result * clone_move * t_inv;
                    item->doWriteTransform(move, &move, compensate);

                } else if (prefs_unmoved) {
                    //if (is<SPUse>(sp_use_get_original(cast<SPUse>(item))))
                    //    clone_move = Geom::identity();
                    Geom::Affine move = result * clone_move;
                    item->doWriteTransform(move, &t, compensate);
                }

            } else if (sibling_state == SiblingState::SIBLING_OFFSET_SOURCE && (prefs_parallel || prefs_unmoved) && affine.isTranslation()){
                Geom::Affine parent = item->transform;
                Geom::Affine offset_move = parent.inverse() * t * parent;

                if (prefs_parallel) {
                    Geom::Affine move = result * offset_move * t_inv;
                    item->doWriteTransform(move, &move, compensate);

                } else if (prefs_unmoved) {
                    Geom::Affine move = result * offset_move;
                    item->doWriteTransform(move, &t, compensate);
                }

            } else {
                // just apply the result
                item->doWriteTransform(result, &t, compensate);
            }
        } else if (sibling_state == SiblingState::SIBLING_TEXT_SHAPE_INSIDE) {
            item->readAttr(SPAttr::TRANSFORM);

        } else {
            if (set_i2d) {
                item->set_i2d_affine(item->i2dt_affine() * (Geom::Affine)affine);
            }
            item->doWriteTransform(item->transform, nullptr, compensate);
        }

        if (adjust_transf_center) { // The transformation center should not be touched in case of pasting or importing, which is allowed by this if clause
            // if we're moving the actual object, not just updating the repr, we can transform the
            // center by the same matrix (only necessary for non-translations)
            if (set_i2d && item->isCenterSet() && !(affine.isTranslation() || affine.isIdentity())) {
                item->setCenter(old_center * affine);
                item->updateRepr();
            }
        }
    }

    // For each perspective with a box in selection, check whether all boxes are selected and
    // unlink all non-selected boxes.
    for (auto *persp : perspList()) {
        if (persp) {
            if (!persp->has_all_boxes_in_selection(this)) {
                // create a new perspective as a copy of the current one
                Persp3D *transf_persp = Persp3D::create_copy(*persp);
                for (auto *selboxe : box3DList(persp)) {
                    selboxe->switch_perspectives(persp, transf_persp);
                }
                persp = transf_persp;
            }
            persp->apply_affine_transformation(affine);
        }
    }
}

void ObjectSet::removeTransform()
{
    auto items = xmlNodes();
    for (auto item : items) {
        item->removeAttribute("transform");
    }

    if (document()) {
        DocumentUndo::done(document(), RC_("Undo", "Remove transform"), "");
    }
}

void ObjectSet::setScaleAbsolute(double x0, double x1,double y0, double y1)
{
    if (isEmpty())
        return;

    Geom::OptRect bbox = visualBounds();
    if ( !bbox ) {
        return;
    }

    Geom::Translate const p2o(-bbox->min());

    Geom::Scale const newSize(x1 - x0,
                              y1 - y0);
    Geom::Scale const scale( newSize * Geom::Scale(bbox->dimensions()).inverse() );
    Geom::Translate const o2n(x0, y0);
    Geom::Affine const final( p2o * scale * o2n );

    applyAffine(final);
}

void ObjectSet::scaleRelative(Geom::Point const &align, Geom::Scale const &scale)
{
    if (isEmpty())
        return;

    Geom::OptRect bbox = visualBounds();

    if ( !bbox ) {
        return;
    }

    // FIXME: ARBITRARY LIMIT: don't try to scale above 1 Mpx, it won't display properly and will crash sooner or later anyway
    if ( bbox->dimensions()[Geom::X] * scale[Geom::X] > 1e6  ||
         bbox->dimensions()[Geom::Y] * scale[Geom::Y] > 1e6 )
    {
        return;
    }

    Geom::Translate const n2d(-align);
    Geom::Translate const d2n(align);
    Geom::Affine const final( n2d * scale * d2n );
    applyAffine(final);
}

void ObjectSet::rotateRelative(Geom::Point const &center, double angle_degrees)
{
    Geom::Translate const d2n(center);
    Geom::Translate const n2d(-center);
    Geom::Rotate const rotate(Geom::Rotate::from_degrees(angle_degrees));
    Geom::Affine const final( Geom::Affine(n2d) * rotate * d2n );
    applyAffine(final);
}

void ObjectSet::skewRelative(Geom::Point const &align, double dx, double dy)
{
    Geom::Translate const d2n(align);
    Geom::Translate const n2d(-align);
    Geom::Affine const skew(1, dy,
                            dx, 1,
                            0, 0);
    Geom::Affine const final( n2d * skew * d2n );
    applyAffine(final);
}

void ObjectSet::moveRelative(Geom::Point const &move, bool compensate)
{
    applyAffine(Geom::Affine(Geom::Translate(move)), true, compensate);
}

void ObjectSet::moveRelative(double dx, double dy)
{
    applyAffine(Geom::Affine(Geom::Translate(dx, dy)));
}

/*
 * Selects all the visible items with the same fill and/or stroke color/style as the items in the current selection
 *
 * Params:
 * desktop - set the selection on this desktop
 * fill - select objects matching fill
 * stroke - select objects matching stroke
 */
void sp_select_same_fill_stroke_style(SPDesktop *desktop, gboolean fill, gboolean stroke, gboolean style)
{
    if (!desktop) {
        return;
    }

    if (!fill && !stroke && !style) {
        return;
    }

    Inkscape::Selection *selection = desktop->getSelection();

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool inlayersame = prefs->getBool("/options/selection/samelikeall", false);
    bool onlyvisible = prefs->getBool("/options/kbselection/onlyvisible", true);
    bool onlysensitive = prefs->getBool("/options/kbselection/onlysensitive", true);

    SPObject *root = desktop->layerManager().currentRoot();
    bool ingroup = true;

    // Apply the same layer logic to select same as used for select all.
    if (inlayersame) {
        PrefsSelectionContext inlayer = (PrefsSelectionContext)prefs->getInt("/options/kbselection/inlayer", PREFS_SELECTION_LAYER);
        if (PREFS_SELECTION_ALL != inlayer) {
            root = selection->activeContext();
            ingroup = (inlayer == PREFS_SELECTION_LAYER_RECURSIVE);
        }
    }

    std::vector<SPItem*> all_list = get_all_items(root, desktop, onlyvisible, onlysensitive, ingroup);
    std::vector<SPItem*> all_matches;

    auto items = selection->items();

    std::vector<SPItem*> tmp;
    for (auto iter : all_list) {
        if(!is<SPGroup>(iter)){
            tmp.push_back(iter);
        }
    }
    all_list=tmp;

    for (auto sel : items) {
        std::vector<SPItem*> matches = all_list;
        if (fill && stroke && style) {
            matches = sp_get_same_style(sel, matches);
        }
        else if (fill) {
            matches = sp_get_same_style(sel, matches, SP_FILL_COLOR);
        }
        else if (stroke) {
            matches = sp_get_same_style(sel, matches, SP_STROKE_COLOR);
        }
        else if (style) {
            matches = sp_get_same_style(sel, matches,SP_STROKE_STYLE_ALL);
        }
        all_matches.insert(all_matches.end(), matches.begin(),matches.end());
    }

    selection->clear();
    selection->setList(all_matches);

}


/*
 * Selects all the visible items with the same object type as the items in the current selection
 *
 * Params:
 * desktop - set the selection on this desktop
 */
void sp_select_same_object_type(SPDesktop *desktop)
{
    if (!desktop) {
        return;
    }


    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool onlyvisible = prefs->getBool("/options/kbselection/onlyvisible", true);
    bool onlysensitive = prefs->getBool("/options/kbselection/onlysensitive", true);
    bool ingroups = TRUE;
    auto matches = get_all_items(desktop->layerManager().currentRoot(), desktop, onlyvisible, onlysensitive, ingroups);

    Inkscape::Selection *selection = desktop->getSelection();

    auto items= selection->items();
    for (auto sel : items) {
        if (sel) {
            matches = sp_get_same_object_type(sel, matches);
        } else {
            g_assert_not_reached();
        }
    }

    selection->clear();
    selection->setList(matches);

}



/*
 * Find all items in src list that have the same fill or stroke style as sel
 * Return the list of matching items
 */
std::vector<SPItem*> sp_get_same_fill_or_stroke_color(SPItem *sel, std::vector<SPItem*> &src, SPSelectStrokeStyleType type)
{
    std::vector<SPItem*> matches ;
    gboolean match = false;

    SPIPaint *sel_paint = sel->style->getFillOrStroke(type == SP_FILL_COLOR);

    for (std::vector<SPItem*>::const_reverse_iterator i=src.rbegin();i!=src.rend();++i) {
        SPItem *iter = *i;
        if (iter) {
            SPIPaint *iter_paint = iter->style->getFillOrStroke(type == SP_FILL_COLOR);
            match = false;
            if (sel_paint->isColor() && iter_paint->isColor()
                && (sel_paint->getColor().isSimilar(iter_paint->getColor()))) {
                match = true;
            } else if (sel_paint->isPaintserver() && iter_paint->isPaintserver()) {

                SPPaintServer *sel_server =
                    (type == SP_FILL_COLOR) ? sel->style->getFillPaintServer() : sel->style->getStrokePaintServer();
                SPPaintServer *iter_server =
                    (type == SP_FILL_COLOR) ? iter->style->getFillPaintServer() : iter->style->getStrokePaintServer();

                auto check_gradient = [] (SPGradient const *g) {
                    return is<SPLinearGradient>(g) || is<SPRadialGradient>(g) || g->getVector()->isSwatch();
                };

                SPGradient *sel_gradient, *iter_gradient;
                SPPattern *sel_pattern, *iter_pattern;

                if ((sel_gradient = cast<SPGradient>(sel_server)) &&
                    (iter_gradient = cast<SPGradient>(iter_server)) &&
                    check_gradient(sel_gradient) &&
                    check_gradient(iter_gradient))
                {
                    SPGradient *sel_vector = sel_gradient->getVector();
                    SPGradient *iter_vector = iter_gradient->getVector();
                    if (sel_vector == iter_vector) {
                        match = true;
                    }

                } else if ((sel_pattern = cast<SPPattern>(sel_server)) &&
                           (iter_pattern = cast<SPPattern>(iter_server))) {
                    SPPattern *sel_pat = sel_pattern->rootPattern();
                    SPPattern *iter_pat = iter_pattern->rootPattern();
                    if (sel_pat == iter_pat) {
                        match = true;
                    }
                }
            } else if (sel_paint->isNone() && iter_paint->isNone()) {
                match = true;
            } else if (sel_paint->isNoneSet() && iter_paint->isNoneSet()) {
                match = true;
            }

            if (match) {
                matches.push_back(iter);
            }
        } else {
            g_assert_not_reached();
        }
    }

    return matches;
}

static bool item_type_match (SPItem *i, SPItem *j)
{
    if (is<SPRect>(i)) {
        return ( is<SPRect>(j) );

    } else if (is<SPGenericEllipse>(i)) {
        return (is<SPGenericEllipse>(j));

    } else if (is<SPStar>(i) || is<SPPolygon>(i)) {
        return (is<SPStar>(j) || is<SPPolygon>(j)) ;

    } else if (is<SPSpiral>(i)) {
        return (is<SPSpiral>(j));

    } else if (is<SPPath>(i) || is<SPLine>(i) || is<SPPolyLine>(i)) {
        return (is<SPPath>(j) || is<SPLine>(j) || is<SPPolyLine>(j));

    } else if (is<SPText>(i) || is<SPFlowtext>(i) || is<SPTSpan>(i) || is<SPTRef>(i)) {
        return (is<SPText>(j) || is<SPFlowtext>(j) || is<SPTSpan>(j) || is<SPTRef>(j));

    }  else if (is<SPUse>(i)) {
        return (is<SPUse>(j)) ;

    } else if (is<SPImage>(i)) {
        return (is<SPImage>(j));

    } else if (is<SPOffset>(i) && cast_unsafe<SPOffset>(i)->sourceHref) {   // Linked offset
        return (is<SPOffset>(j) && cast_unsafe<SPOffset>(j)->sourceHref);

    }  else if (is<SPOffset>(i) && !cast_unsafe<SPOffset>(i)->sourceHref) { // Dynamic offset
        return is<SPOffset>(j) && !cast_unsafe<SPOffset>(j)->sourceHref;

    }

    return false;
}

/*
 * Find all items in src list that have the same object type as sel by type
 * Return the list of matching items
 */
std::vector<SPItem*> sp_get_same_object_type(SPItem *sel, std::vector<SPItem*> &src)
{
    std::vector<SPItem*> matches;

    for (std::vector<SPItem*>::const_reverse_iterator i=src.rbegin();i!=src.rend();++i) {
        SPItem *item = *i;
        if (item && item_type_match(sel, item) && !item->cloned) {
            matches.push_back(item);
        }
    }
    return matches;
}

/*
 * Find all items in src list that have the same stroke style as sel by type
 * Return the list of matching items
 */
std::vector<SPItem*> sp_get_same_style(SPItem *sel, std::vector<SPItem*> &src, SPSelectStrokeStyleType type)
{
    std::vector<SPItem*> matches;
    bool match = false;

    SPStyle *sel_style = sel->style;

    if (type == SP_FILL_COLOR || type == SP_STYLE_ALL) {
        src = sp_get_same_fill_or_stroke_color(sel, src, SP_FILL_COLOR);
    }
    if (type == SP_STROKE_COLOR || type == SP_STYLE_ALL) {
        src = sp_get_same_fill_or_stroke_color(sel, src, SP_STROKE_COLOR);
    }

    /*
     * Stroke width needs to handle transformations, so call this function
     * to get the transformed stroke width
     */
    std::vector<SPItem*> objects;
    SPStyle *sel_style_for_width = nullptr;
    if (type == SP_STROKE_STYLE_WIDTH || type == SP_STROKE_STYLE_ALL || type==SP_STYLE_ALL ) {
        objects.push_back(sel);
        sel_style_for_width = new SPStyle(SP_ACTIVE_DOCUMENT);
        objects_query_strokewidth (objects, sel_style_for_width);
    }
    bool match_g;
    for (auto iter : src) {
        if (iter) {
            match_g=true;
            SPStyle *iter_style = iter->style;
            match = true;

            if (type == SP_STROKE_STYLE_WIDTH|| type == SP_STROKE_STYLE_ALL|| type==SP_STYLE_ALL) {
                match = (sel_style->stroke_width.set == iter_style->stroke_width.set);
                if (sel_style->stroke_width.set && iter_style->stroke_width.set) {
                    std::vector<SPItem*> objects;
                    objects.insert(objects.begin(),iter);
                    SPStyle tmp_style(SP_ACTIVE_DOCUMENT);
                    objects_query_strokewidth (objects, &tmp_style);

                    if (sel_style_for_width) {
                        match = (sel_style_for_width->stroke_width.computed == tmp_style.stroke_width.computed);
                    }
                }
            }
            match_g = match_g && match;
            if (type == SP_STROKE_STYLE_DASHES|| type == SP_STROKE_STYLE_ALL || type==SP_STYLE_ALL) {
                match = (sel_style->stroke_dasharray.set == iter_style->stroke_dasharray.set);
                if (sel_style->stroke_dasharray.set && iter_style->stroke_dasharray.set) {
                    match = (sel_style->stroke_dasharray == iter_style->stroke_dasharray);
                }
            }
            match_g = match_g && match;
            if (type == SP_STROKE_STYLE_MARKERS|| type == SP_STROKE_STYLE_ALL|| type==SP_STYLE_ALL) {
                match = true;
                int len = sizeof(sel_style->marker)/sizeof(SPIString);
                for (int i = 0; i < len; i++) {
                    if (g_strcmp0(sel_style->marker_ptrs[i]->value(),
                                  iter_style->marker_ptrs[i]->value())) {
                        match = false;
                        break;
                    }
                }
            }
            match_g = match_g && match;
            if (match_g) {
                while (iter->cloned) iter=cast<SPItem>(iter->parent);
                matches.insert(matches.begin(),iter);
            }
        } else {
            g_assert_not_reached();
        }
    }

    if( sel_style_for_width != nullptr ) delete sel_style_for_width;
    return matches;
}

void ObjectSet::move(double dx, double dy)
{
    if (isEmpty()) {
        return;
    }

    moveRelative(dx, dy);

    if (document()) {
        if (dx == 0) {
            DocumentUndo::maybeDone(document(), "selector:move:vertical", RC_("Undo", "Move vertically"), INKSCAPE_ICON("tool-pointer"));
        } else if (dy == 0) {
            DocumentUndo::maybeDone(document(), "selector:move:horizontal", RC_("Undo", "Move horizontally"), INKSCAPE_ICON("tool-pointer"));
        } else {
            DocumentUndo::done(document(), RC_("Undo", "Move"), INKSCAPE_ICON("tool-pointer"));
        }
    }
}

void ObjectSet::move(double dx, double dy, bool rotated)
{
    if (rotated) {
        double const rotation = desktop()->current_rotation().angle();
        double const rdx = std::cos(rotation) * dx + std::sin(rotation) * dy;
        double const rdy = -std::sin(rotation) * dx + std::cos(rotation) * dy;

        move(rdx, rdy);
    } else {
        move(dx, dy);
    }
}

void ObjectSet::move(double dx, double dy, bool rotated, bool screen)
{
    if (screen) {
        moveScreen(dx, dy, rotated);
    } else {
        move(dx, dy, rotated);
    }
}

void ObjectSet::moveScreen(double dx, double dy)
{
    if (isEmpty() || !desktop()) {
        return;
    }

    // same as ObjectSet::move but divide deltas by zoom factor
    gdouble const zoom = desktop()->current_zoom();
    gdouble const zdx = dx / zoom;
    gdouble const zdy = dy / zoom;
    moveRelative(zdx, zdy);

    SPDocument *doc = document();
    if (dx == 0) {
        DocumentUndo::maybeDone(doc, "selector:move:vertical", RC_("Undo", "Move vertically by pixels"), INKSCAPE_ICON("tool-pointer"));
    } else if (dy == 0) {
        DocumentUndo::maybeDone(doc, "selector:move:horizontal", RC_("Undo", "Move horizontally by pixels"), INKSCAPE_ICON("tool-pointer"));
    } else {
        DocumentUndo::done(doc, RC_("Undo", "Move"), INKSCAPE_ICON("tool-pointer"));
    }
}

void ObjectSet::moveScreen(double dx, double dy, bool rotated)
{
    if (rotated) {
        double const rotation = desktop()->current_rotation().angle();
        double const rdx = std::cos(rotation) * dx + std::sin(rotation) * dy;
        double const rdy = -std::sin(rotation) * dx + std::cos(rotation) * dy;

        moveScreen(rdx, rdy);
    } else {
        moveScreen(dx, dy);
    }
}

struct Forward {
    typedef SPObject *Iterator;

    static Iterator children(SPObject *o) { return o->firstChild(); }
    static Iterator siblings_after(SPObject *o) { return o->getNext(); }
    static void dispose(Iterator i) {}

    static SPObject *object(Iterator i) { return i; }
    static Iterator next(Iterator i) { return i->getNext(); }
    static bool isNull(Iterator i) {return (!i);}
};

struct ListReverse {
    typedef std::list<SPObject *> *Iterator;

    static Iterator children(SPObject *o) {
        return make_list(o, nullptr);
    }
    static Iterator siblings_after(SPObject *o) {
        return make_list(o->parent, o);
    }
    static void dispose(Iterator i) {
        delete i;
    }

    static SPObject *object(Iterator i) {
        return *(i->begin());
    }
    static Iterator next(Iterator i) { i->pop_front(); return i; }

    static bool isNull(Iterator i) {return i->empty();}

private:
    static std::list<SPObject *> *make_list(SPObject *object, SPObject *limit) {
        auto list = new std::list<SPObject *>;
        for (auto &child: object->children) {
            if (&child == limit) {
                break;
            }
            list->push_front(&child);
        }
        return list;
    }
};



template <typename D>
SPItem *next_item(SPDesktop *desktop, std::vector<SPObject *> &path, SPObject *root,
                  bool only_in_viewport, PrefsSelectionContext inlayer, bool onlyvisible, bool onlysensitive)
{
    typename D::Iterator children;
    typename D::Iterator iter;

    SPItem *found=nullptr;

    if (!path.empty()) {
        SPObject *object=path.back();
        path.pop_back();
        g_assert(object->parent == root);
        if (desktop->layerManager().isLayer(object)) {
            found = next_item<D>(desktop, path, object, only_in_viewport, inlayer, onlyvisible, onlysensitive);
        }
        iter = children = D::siblings_after(object);
    } else {
        iter = children = D::children(root);
    }

    while ( !D::isNull(iter) && !found ) {
        SPObject *object=D::object(iter);
        if (desktop->layerManager().isLayer(object)) {
            if (PREFS_SELECTION_LAYER != inlayer) { // recurse into sublayers
                std::vector<SPObject *> empt;
                found = next_item<D>(desktop, empt, object, only_in_viewport, inlayer, onlyvisible, onlysensitive);
            }
        } else {
            auto item = cast<SPItem>(object);
            if ( item &&
                 ( !only_in_viewport || desktop->isWithinViewport(item) ) &&
                 ( !onlyvisible || !desktop->itemIsHidden(item)) &&
                 ( !onlysensitive || !item->isLocked()) &&
                 !desktop->layerManager().isLayer(item) )
            {
                found = item;
            }
        }
        iter = D::next(iter);
    }

    D::dispose(children);

    return found;
}


template <typename D>
SPItem *next_item_from_list(SPDesktop *desktop, std::vector<SPItem*> const &items,
                            SPObject *root, bool only_in_viewport, PrefsSelectionContext inlayer, bool onlyvisible, bool onlysensitive)
{
    SPObject *current=root;
    for(auto item : items) {
        if ( root->isAncestorOf(item) &&
             ( !only_in_viewport || desktop->isWithinViewport(item) ) )
        {
            current = item;
            break;
        }
    }

    std::vector<SPObject *> path;
    while ( current != root ) {
        path.push_back(current);
        current = current->parent;
    }

    SPItem *next;
    // first, try from the current object
    next = next_item<D>(desktop, path, root, only_in_viewport, inlayer, onlyvisible, onlysensitive);

    if (!next) { // if we ran out of objects, start over at the root
        std::vector<SPObject *> empt;
        next = next_item<D>(desktop, empt, root, only_in_viewport, inlayer, onlyvisible, onlysensitive);
    }

    return next;
}

void
sp_selection_item_next(SPDesktop *desktop)
{
    g_return_if_fail(desktop != nullptr);
    Inkscape::Selection *selection = desktop->getSelection();

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    PrefsSelectionContext inlayer = (PrefsSelectionContext)prefs->getInt("/options/kbselection/inlayer", PREFS_SELECTION_LAYER);
    bool onlyvisible = prefs->getBool("/options/kbselection/onlyvisible", true);
    bool onlysensitive = prefs->getBool("/options/kbselection/onlysensitive", true);

    SPObject *root;
    if (PREFS_SELECTION_ALL != inlayer) {
        root = selection->activeContext();
    } else {
        root = desktop->layerManager().currentRoot();
    }

    auto vec = selection->items_vector();
    SPItem *item=next_item_from_list<Forward>(desktop, vec, root, SP_CYCLING == SP_CYCLE_VISIBLE, inlayer, onlyvisible, onlysensitive);

    if (item) {
        selection->set(item, PREFS_SELECTION_LAYER_RECURSIVE == inlayer);
        if ( SP_CYCLING == SP_CYCLE_FOCUS ) {
            scroll_to_show_item(desktop, item);
        }
    }
}

void
sp_selection_item_prev(SPDesktop *desktop)
{
    SPDocument *document = desktop->getDocument();
    g_return_if_fail(document != nullptr);
    g_return_if_fail(desktop != nullptr);
    Inkscape::Selection *selection = desktop->getSelection();

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    PrefsSelectionContext inlayer = (PrefsSelectionContext) prefs->getInt("/options/kbselection/inlayer", PREFS_SELECTION_LAYER);
    bool onlyvisible = prefs->getBool("/options/kbselection/onlyvisible", true);
    bool onlysensitive = prefs->getBool("/options/kbselection/onlysensitive", true);

    SPObject *root;
    if (PREFS_SELECTION_ALL != inlayer) {
        root = selection->activeContext();
    } else {
        root = desktop->layerManager().currentRoot();
    }

    auto vec = selection->items_vector();
    SPItem *item=next_item_from_list<ListReverse>(desktop, vec, root, SP_CYCLING == SP_CYCLE_VISIBLE, inlayer, onlyvisible, onlysensitive);

    if (item) {
        selection->set(item, PREFS_SELECTION_LAYER_RECURSIVE == inlayer);
        if ( SP_CYCLING == SP_CYCLE_FOCUS ) {
            scroll_to_show_item(desktop, item);
        }
    }
}

void sp_selection_next_patheffect_param(SPDesktop * dt)
{
    if (!dt) return;

    Inkscape::Selection *selection = dt->getSelection();
    if ( selection && !selection->isEmpty() ) {
        SPItem *item = selection->singleItem();
        if ( auto lpeitem = cast<SPLPEItem>(item) ) {
            if (lpeitem->hasPathEffect()) {
                lpeitem->editNextParamOncanvas(dt);
            } else {
                dt->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("The selection has no applied path effect."));
            }
        }
    }
}

void ObjectSet::editMask(bool /*clip*/)
{
    return;
}




/**
 * If \a item is not entirely visible then adjust visible area to centre on the centre on of
 * \a item.
 */
void scroll_to_show_item(SPDesktop *desktop, SPItem *item)
{
    auto dbox = desktop->get_display_area();
    Geom::OptRect sbox = item->desktopVisualBounds();

    if ( sbox && dbox.contains(*sbox) == false ) {
        Geom::Point const s_dt = sbox->midpoint();
        Geom::Point const s_w = desktop->d2w(s_dt);
        Geom::Point const d_dt = dbox.midpoint();
        Geom::Point const d_w = desktop->d2w(d_dt);
        Geom::Point const moved_w( d_w - s_w );
        desktop->scroll_relative(moved_w);
    }
}

void ObjectSet::clone(bool skip_undo)
{
    if (document() == nullptr) {
        return;
    }

    Inkscape::XML::Document *xml_doc = document()->getReprDoc();

    // check if something is selected
    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select an <b>object</b> to clone."));
        return;
    }

    // Assign IDs to selected objects that don't have an ID attribute
    enforceIds();

    auto reprs = xmlNodes_vector();

    clear();

    // sorting items from different parents sorts each parent's subset without possibly mixing them, just what we need
    sort(reprs.begin(),reprs.end(),sp_repr_compare_position_bool);

    std::vector<Inkscape::XML::Node*> newsel;

    for(auto sel_repr : reprs){
        Inkscape::XML::Node *parent = sel_repr->parent();

        Inkscape::XML::Node *clone = xml_doc->createElement("svg:use");
        clone->setAttribute("x", "0");
        clone->setAttribute("y", "0");
        gchar *href_str = g_strdup_printf("#%s", sel_repr->attribute("id"));
        clone->setAttribute("xlink:href", href_str);
        g_free(href_str);

        clone->setAttribute("inkscape:transform-center-x", sel_repr->attribute("inkscape:transform-center-x"));
        clone->setAttribute("inkscape:transform-center-y", sel_repr->attribute("inkscape:transform-center-y"));

        // add the new clone to the top of the original's parent
        parent->appendChild(clone);

        newsel.push_back(clone);
        Inkscape::GC::release(clone);
    }
    if (!skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "Clone"), INKSCAPE_ICON("edit-clone"));
    }

    setReprList(newsel);
}

void ObjectSet::relink()
{
    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>clones</b> to relink."));
        return;
    }

    Inkscape::UI::ClipboardManager *cm = Inkscape::UI::ClipboardManager::get();
    auto newid = cm->getFirstObjectID();
    if (newid.empty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Copy an <b>object</b> to clipboard to relink clones to."));
        return;
    }
    auto newrefAttribute = "#" + newid;

    // Get a copy of current selection.
    bool relinked = false;
    auto items_= items();
    for (auto item : items_){
        if (auto use = cast<SPUse>(item)) {
            // Get original referenced item, relink, then get new reference
            SPItem *ref = use->get_original();
            use->setAttribute(Inkscape::getHrefAttribute(*use->getRepr()).first, newrefAttribute);
            SPItem *newref = use->get_original();

            if (ref && newref) {
                // Compensate for position of new reference if requested.
                // Default behavior is to move according to transform, so not
                // handled explicitly.
                Inkscape::Preferences *prefs = Inkscape::Preferences::get();
                int compensation = prefs->getInt("/options/clonecompensation/value", SP_CLONE_COMPENSATION_UNMOVED);

                if (compensation == SP_CLONE_COMPENSATION_UNMOVED || compensation == SP_CLONE_COMPENSATION_PARALLEL) {
                    auto center = ref->getCenter();
                    auto newcenter = newref->getCenter();
                    Geom::Affine translation = Geom::Translate(newcenter - center);

                    // Transform of clone. Necessary to apply the offset
                    // translation from the reference prior to applying clone-
                    // specific transformations.
                    Geom::Affine t = item->transform;

                    // To make the clone appear unmoved, simply invert the
                    // translation. To make the clone move in parallel, add the
                    // translation back in, but make sure that the translation
                    // is applied to a shape that isn't transformed in any other way
                    Geom::Affine m = t.inverse() * translation.inverse() * t;
                    if (compensation == SP_CLONE_COMPENSATION_PARALLEL) {
                        m *= m.withoutTranslation().inverse() * translation * m.withoutTranslation();
                    }

                    // Compensation must be applied for each clone indivudally
                    // in case we are re-linking many clones that originally had
                    // different references.
                    auto s = ObjectSet(document());
                    s.add(item);
                    s.applyAffine(m);
                }
            }

            item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
            relinked = true;
        }
    }

    if (!relinked) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("<b>No clones to relink</b> in the selection."));
    } else {
        DocumentUndo::done(document(), RC_("Undo", "Relink clone"), INKSCAPE_ICON("edit-clone-unlink"));
    }
}


bool ObjectSet::unlink(const bool skip_undo, const bool silent)
{
    if (isEmpty()) {
        if(desktop() && !silent)
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>clones</b> to unlink."));
        return false;
    }

    // Get a copy of current selection.
    std::vector<SPItem*> new_select;
    bool unlinked = false;

    for (auto item : items_vector() | std::views::reverse) {
        ObjectSet tmp_set(document());
        tmp_set.set(item);
        auto *clip_obj = item->getClipObject();
        auto *mask_obj = item->getMaskObject();
        if (clip_obj) {
            // The following always-false check was added in 5bfbeb4a.
            // Cannot tell if necessary since neither commit/MR say what bug it fixes.
            // Keeping the if (false) explicit to minimize likelihood of regressions
            // if (is<SPUse>(clip_obj)) {
            if (false) {
                tmp_set.unsetMask(true, true, true);
                unlinked = tmp_set.unlink(true) || unlinked;
                tmp_set.setMask(true, false, true);
            }
            new_select.push_back(tmp_set.singleItem());
        } else if (mask_obj) {
            // if (is<SPUse>(mask_obj)) {
            if (false) {
                tmp_set.unsetMask(false, true, true);
                unlinked = tmp_set.unlink(true) || unlinked;
                tmp_set.setMask(false, false, true);
            }
            new_select.push_back(tmp_set.singleItem());
        } else {
            if (is<SPText>(item)) {
                SPObject *tspan = sp_tref_convert_to_tspan(item);

                if (tspan) {
                    item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
                }

                // Set unlink to true, and fall into the next if which
                // will include this text item in the new selection
                unlinked = true;
            }

            if (!(is<SPUse>(item) || is<SPTRef>(item))) {
                // keep the non-use item in the new selection
                new_select.push_back(item);
                continue;
            }

            SPItem *unlink = nullptr;
            auto use = cast<SPUse>(item);
            if (use) {
                unlink = use->unlink();
                // Unable to unlink use (external or invalid href?)
                if (!unlink) {
                    new_select.push_back(item);
                    continue;
                }
            } else /*if (is<SPTRef>(use))*/ {
                unlink = cast<SPItem>(sp_tref_convert_to_tspan(item));
                g_assert(unlink != nullptr);
            }

            unlinked = true;
            // Add ungrouped items to the new selection.
            new_select.push_back(unlink);
        }
    }

    if (!new_select.empty()) { // set new selection
        clear();
        setList(new_select);
    }
    if (!unlinked) {
        if(desktop() && !silent)
            desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("<b>No clones to unlink</b> in the selection."));
    }

    if (!skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "Unlink clone"), INKSCAPE_ICON("edit-clone-unlink"));
    }
    return unlinked;
}

bool ObjectSet::unlinkRecursive(const bool skip_undo, const bool force, const bool silent) {
    if (isEmpty()){
        if (desktop() && !silent)
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>clones</b> to unlink."));
        return false;
    }
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool pathoperationsunlink = prefs->getBool("/options/pathoperationsunlink/value", true);
    if (!force && !pathoperationsunlink) {
        if (desktop() && !pathoperationsunlink && !silent) {
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Unable to unlink. Check the setting for 'Unlinking Clones' in your preferences."));
        }
        return false;
    }
    bool unlinked = false;
    ObjectSet tmp_set(document());
    auto items_vec = items_vector();
    for (auto &item : items_vec) {
        tmp_set.set(item);
        unlinked |= tmp_set.unlink(true, silent);
        item = tmp_set.singleItem();
        if (is<SPGroup>(item)) {
            std::vector<SPObject*> c = item->childList(false);
            tmp_set.setList(c);
            unlinked |= tmp_set.unlinkRecursive(skip_undo, force, silent);
        }
    }
    if (!unlinked) {
        if(desktop() && !silent)
            desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("<b>No clones to unlink</b> in the selection."));
    }
    if (!skip_undo) {
        DocumentUndo::done(document(), RC_("Undo", "Unlink clone recursively"), INKSCAPE_ICON("edit-clone-unlink"));
    }
    setList(items_vec);
    return unlinked;
}

void ObjectSet::removeLPESRecursive(bool keep_paths) {
    if (isEmpty()){
        return;
    }

    ObjectSet tmp_set(document());
    std::vector<SPItem *> items_done;
    for (auto item : items_vector()) {
        if (auto spgroup = cast<SPGroup>(item)) {
            std::vector<SPObject*> c = spgroup->childList(false);
            tmp_set.setList(c);
            tmp_set.removeLPESRecursive(keep_paths);
        }
        if (auto splpeitem = cast<SPLPEItem>(item)) {
            // Maybe the item is changed from SPShape to SPPath invalidating selection
            // fix issue Inkscape#2321
            char const *id = splpeitem->getAttribute("id");
            SPDocument *document = splpeitem->document;
            splpeitem->removeAllPathEffects(keep_paths);
            auto upditem = cast<SPItem>(document->getObjectById(id));
            if (upditem) {
                items_done.push_back(upditem);
            }
        } else {
            items_done.push_back(item);
        }
    }
    setList(items_done);
}

void ObjectSet::cloneOriginal()
{
    SPItem *item = singleItem();

    gchar const *error = _("Select a <b>clone</b> to go to its original. Select a <b>linked offset</b> to go to its source. Select a <b>text on path</b> to go to the path. Select a <b>flowed text</b> to go to its frame.");

    // Check if other than two objects are selected

    if (!item) {
        if (desktop()) {
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, error);
        }
        return;
    }

    SPItem *original = nullptr;
    auto use = cast<SPUse>(item);
    if (use) {
        original = use->get_original();
    } else if (auto offset = cast<SPOffset>(item)) {
        original = sp_offset_get_source(offset);
    } else if (auto text = cast<SPText>(item)) {
        original = text->get_first_shape_dependency();
    } else if (auto flowtext = cast<SPFlowtext>(item)) {
        original = flowtext->get_frame(nullptr); // first frame only
    }

    if (original == nullptr) { // it's an object that we don't know what to do with
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, error);
        return;
    }

    if (!original) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("<b>Cannot find</b> the object to select (orphaned clone, offset, textpath, flowed text?)"));
        return;
    }

    for (SPObject *o = original; o && !is<SPRoot>(o); o = o->parent) {
        if (is<SPDefs>(o)) {
            if(desktop())
                desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("The object you're trying to select is <b>not visible</b> (it is in &lt;defs&gt;)"));
            return;
        }
    }

    if (original) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        bool highlight = prefs->getBool("/options/highlightoriginal/value");
        if (highlight) {
            Geom::OptRect a = item->desktopVisualBounds();
            Geom::OptRect b = original->desktopVisualBounds();
            if ( a && b && desktop()) {
                // draw a flashing line between the objects
                auto line = Geom::LineSegment{a->midpoint(), b->midpoint()};

                // We use a bpath as it supports dashes.
                auto canvas_item_bpath = new Inkscape::CanvasItemBpath(desktop()->getCanvasTemp(), path_from_curve(line));
                canvas_item_bpath->set_stroke(0x0000ddff);
                canvas_item_bpath->set_dashes({5.0, 3.0});
                canvas_item_bpath->set_visible(true);
                desktop()->add_temporary_canvasitem(canvas_item_bpath, 1000);
            }
        }

        clear();
        set(original);
        if (SP_CYCLING == SP_CYCLE_FOCUS && desktop()) {
            scroll_to_show_item(desktop(), original);
        }
    }
}

/**
* This applies the Fill Between Many LPE, and has it refer to the selection.
*/
void ObjectSet::cloneOriginalPathLPE(bool allow_transforms, bool sync, bool skip_undo)
{

    Inkscape::SVGOStringStream os;
    SPObject * firstItem = nullptr;
    auto items_= items();
    bool multiple = false;
    for (auto *item : items_) {
        if (is<SPShape>(item) || is<SPText>(item) || is<SPGroup>(item)) {
            if (firstItem) {
                os << "|";
                multiple = true;
            } else {
                firstItem = item;
            }
            os << '#' << item->getId() << ",0,1";
        }
    }
    if (firstItem) {
        Inkscape::XML::Document *xml_doc = document()->getReprDoc();
        SPObject *parent = firstItem->parent;
        // create the LPE
        Inkscape::XML::Node *lpe_repr = xml_doc->createElement("inkscape:path-effect");
        if (multiple) {
            lpe_repr->setAttribute("effect", "fill_between_many");
            lpe_repr->setAttributeOrRemoveIfEmpty("linkedpaths", os.str());
        } else {
            lpe_repr->setAttribute("effect", "clone_original");
            lpe_repr->setAttribute("css_properties", "");
            lpe_repr->setAttribute("attributes", "");
            lpe_repr->setAttribute("linkeditem", ((Glib::ustring)"#" + (Glib::ustring)firstItem->getId()));
        }
        lpe_repr->setAttribute("is_visible", "true");
        gchar const *method_str = allow_transforms ?  "d" : "bsplinespiro";
        lpe_repr->setAttribute("method", method_str);
        gchar const *allow_transforms_str = allow_transforms ? "true" : "false";
        lpe_repr->setAttribute("allow_transforms", allow_transforms_str);
        document()->getDefs()->getRepr()->addChild(lpe_repr, nullptr); // adds to <defs> and assigns the 'id' attribute
        std::string lpe_id_href = std::string("#") + lpe_repr->attribute("id");
        Inkscape::GC::release(lpe_repr);
        Inkscape::XML::Node* clone = nullptr;
        auto firstgroup = cast<SPGroup>(firstItem);
        auto shape = cast<SPShape>(firstItem);
        auto path = cast<SPPath>(firstItem);
        if (firstgroup) {
            if (!multiple) {
                clone = firstgroup->getRepr()->duplicate(xml_doc);
            }
        } else {
            // create the new path
            clone = xml_doc->createElement("svg:path");
            if (sync && !multiple && shape) {
                if (auto c = ptr_to_opt(shape->curveForEdit())) {
                    if (path) {
                        clone->setAttribute("original-d", sp_svg_write_path(*c));
                    }
                    clone->setAttribute("d", sp_svg_write_path(*c));
                } else {
                    clone->setAttribute("d", "M 0 0");
                }
            } else {
                clone->setAttribute("d", "M 0 0");
            }
        }
        if (clone) {
            // add the new clone to the top of the original's parent
            parent->appendChildRepr(clone);
            // select the new object:
            set(clone);
            Inkscape::GC::release(clone);
            SPObject *clone_obj = document()->getObjectById(clone->attribute("id"));
            auto clone_lpeitem = cast<SPLPEItem>(clone_obj);
            if (clone_lpeitem) {
                if (sync && !multiple) {
                    lpe_repr->setAttribute("attributes", "style,clip-path,mask");
                }
                lpe_repr->setAttribute("is_visible", "true");
                clone_lpeitem->addPathEffect(lpe_id_href, false);
            }
            if (!skip_undo) {
                if (multiple) {
                    DocumentUndo::done(document(), RC_("Undo", "Fill between many"), INKSCAPE_ICON("edit-clone-link-lpe"));
                } else {
                    DocumentUndo::done(document(), RC_("Undo", "Clone original"), INKSCAPE_ICON("edit-clone-link-lpe"));
                }
            }
        }
    } else {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select path(s) to fill."));
    }
}

void ObjectSet::toMarker(bool apply)
{
    // sp_selection_tile has similar code

    SPDocument *doc = document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    // check if something is selected
    if (isEmpty()) {
        if (desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                                             _("Select <b>object(s)</b> to convert to marker."));
        return;
    }

    doc->ensureUpToDate();
    Geom::OptRect r = visualBounds();
    if (!r) {
        return;
    }

    auto items_vec = items_vector();
    std::sort(items_vec.begin(), items_vec.end(), sp_item_repr_compare_position_bool);

    // bottommost object, after sorting
    SPObject *parent = items_vec.front()->parent;

    Geom::Affine parent_transform;
    {
        auto parentItem = cast<SPItem>(parent);
        if (parentItem) {
            parent_transform = parentItem->i2doc_affine();
        } else {
            g_assert_not_reached();
        }
    }

    // Create a list of duplicates, to be pasted inside marker element.
    std::vector<Inkscape::XML::Node*> repr_copies;
    for (auto *item : items_vec) {
        auto *dup = item->getRepr()->duplicate(xml_doc);
        repr_copies.push_back(dup);
    }

    Geom::Rect bbox(r->min() * doc->dt2doc(), r->max() * doc->dt2doc());

    // calculate the transform to be applied to objects to move them to 0,0
    // (alternative would be to define viewBox or set overflow:visible)
    Geom::Affine const move = Geom::Translate(-bbox.min());
    Geom::Point const center = bbox.dimensions() * 0.5;

    if (apply) {
        // Delete objects so that their clones don't get alerted;
        // the objects will be restored inside the marker element.
        for (auto item : items_vec) {
            item->deleteObject(false);
        }
    }

    // Hack: Temporarily set clone compensation to unmoved, so that we can move clone-originals
    // without disturbing clones.
    // See ActorAlign::on_button_click() in src/ui/dialog/align-and-distribute.cpp
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    int saved_compensation = prefs->getInt("/options/clonecompensation/value", SP_CLONE_COMPENSATION_UNMOVED);
    prefs->setInt("/options/clonecompensation/value", SP_CLONE_COMPENSATION_UNMOVED);

    gchar const *mark_id = generate_marker(repr_copies, bbox, doc, center, parent_transform * move);
    (void)mark_id;

    // restore compensation setting
    prefs->setInt("/options/clonecompensation/value", saved_compensation);

    DocumentUndo::done(doc, RC_("Undo", "Objects to marker"), "");
}

static void sp_selection_to_guides_recursive(SPItem *item, bool wholegroups) {
    auto group = cast<SPGroup>(item);
    if (group && !is<SPBox3D>(item) && !wholegroups) {
        std::vector<SPItem*> items=group->item_list();
        for (auto item : items){
            sp_selection_to_guides_recursive(item, wholegroups);
        }
    } else {
        item->convert_to_guides();
    }
}

void ObjectSet::toGuides()
{
    SPDocument *doc = document();
    // we need to copy the list because it gets reset when objects are deleted
    auto items_vec = items_vector();

    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to convert to guides."));
        return;
    }

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool deleteitems = !prefs->getBool("/tools/cvg_keep_objects", false);
    bool wholegroups = prefs->getBool("/tools/cvg_convert_whole_groups", false);

    // If an object is earlier in the selection list than its clone, and it is deleted, then the clone will have changed
    // and its entry in the selection list is invalid (crash).
    // Therefore: first convert all, then delete all.

    for (auto item : items_vec) {
        sp_selection_to_guides_recursive(item, wholegroups);
    }

    if (deleteitems) {
        clear();
        sp_selection_delete_impl(items_vec);
    }

    DocumentUndo::done(doc, RC_("Undo", "Objects to guides"), "");
}

/*
 * Convert objects to <symbol>. How that happens depends on what is selected:
 *
 * 1) A random selection of objects will be embedded into a single <symbol> element.
 *
 * 2) Except, a single <g> will have its content directly embedded into a <symbol>; the 'id' and
 *    'style' of the <g> are transferred to the <symbol>.
 *
 * 3) Except, a single <g> with a transform that isn't a translation will keep the group when
 *    embedded into a <symbol> (with 'id' and 'style' transferred to <symbol>). This is because a
 *    <symbol> cannot have a transform. (If the transform is a pure translation, the translation
 *    is moved to the referencing <use> element that is created.)
 *
 * Possible improvements:
 *
 *   Move objects inside symbol so bbox corner at 0,0 (see marker/pattern)
 *
 *   For SVG2, set 'refX' 'refY' to object center (with compensating shift in <use>
 *   transformation).
 */
void ObjectSet::toSymbol()
{

    SPDocument *doc = document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();
    // Check if something is selected.
    if (isEmpty()) {
      if (desktop())
          desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>objects</b> to convert to symbol."));
      return;
    }

    doc->ensureUpToDate();

    std::vector<SPObject*> items_(objects().begin(), objects().end());
    sort(items_.begin(),items_.end(),sp_object_compare_position_bool);

    // Keep track of parent, this is where <use> will be inserted.
    Inkscape::XML::Node *the_first_repr = items_[0]->getRepr();
    Inkscape::XML::Node *the_parent_repr = the_first_repr->parent();

    // Find out if we have a single group
    bool single_group = false;
    SPGroup *the_group = nullptr;
    Geom::Affine transform;
    if( items_.size() == 1 ) {
        SPObject *object = items_[0];
        the_group = cast<SPGroup>(object);
        if ( the_group ) {
            single_group = true;

            if( !sp_svg_transform_read( object->getAttribute("transform"), &transform ))
                transform = Geom::identity();

            if( transform.isTranslation() ) {

                // Create new list from group children.
                items_ = object->childList(false);

                // Hack: Temporarily set clone compensation to unmoved, so that we can move clone-originals
                // without disturbing clones.
                // See ActorAlign::on_button_click() in src/ui/dialog/align-and-distribute.cpp
                Inkscape::Preferences *prefs = Inkscape::Preferences::get();
                int saved_compensation = prefs->getInt("/options/clonecompensation/value", SP_CLONE_COMPENSATION_UNMOVED);
                prefs->setInt("/options/clonecompensation/value", SP_CLONE_COMPENSATION_UNMOVED);

                // Remove transform on group, updating clones.
                the_group->doWriteTransform(Geom::identity());

                // restore compensation setting
                prefs->setInt("/options/clonecompensation/value", saved_compensation);
            }
        }
    }

    // Create new <symbol>
    Inkscape::XML::Node *defsrepr = doc->getDefs()->getRepr();
    Inkscape::XML::Node *symbol_repr = xml_doc->createElement("svg:symbol");

    defsrepr->appendChild(symbol_repr);
    bool settitle = false;
    // For a single group, copy relevant attributes.
    if( single_group ) {
        Glib::ustring id = the_group->getAttribute("id");
        symbol_repr->setAttribute("style",  the_group->getAttribute("style"));
        symbol_repr->setAttribute("class",  the_group->getAttribute("class"));
        the_group->setAttribute("id", id + "_transform");
        symbol_repr->setAttribute("id", id);

        // This should eventually be replaced by 'refX' and 'refY' once SVG WG approves it.
        // It is done here for round-tripping
        symbol_repr->setAttribute("inkscape:transform-center-x",
                                  the_group->getAttribute("inkscape:transform-center-x"));
        symbol_repr->setAttribute("inkscape:transform-center-y",
                                  the_group->getAttribute("inkscape:transform-center-y"));

        the_group->removeAttribute("style");

    }

    // Move selected items to new <symbol>
    for (std::vector<SPObject*>::const_reverse_iterator i=items_.rbegin();i!=items_.rend();++i){
        gchar* title = (*i)->title();
        if (!single_group && !settitle && title) {
            Inkscape::XML::Node *title_repr = xml_doc->createElement("svg:title");
            symbol_repr->addChildAtPos(title_repr, 0);
            title_repr->appendChild(xml_doc->createTextNode(title));
            Inkscape::GC::release(title_repr);
            gchar * desc = (*i)->desc();
            if (desc) {
                Inkscape::XML::Node *desc_repr = xml_doc->createElement("svg:desc");
                desc_repr->appendChild(xml_doc->createTextNode(desc));
                symbol_repr->addChildAtPos(desc_repr, 1);
                Inkscape::GC::release(desc_repr);
            }
            g_free(desc);
            settitle = true;
        }
        g_free(title);
        Inkscape::XML::Node *repr = (*i)->getRepr();
        repr->parent()->removeChild(repr);
        symbol_repr->addChild(repr, nullptr);
    }

    if( single_group && transform.isTranslation() ) {
        the_group->deleteObject(true);
    }

    // Create <use> pointing to new symbol (to replace the moved objects).
    Inkscape::XML::Node *clone = xml_doc->createElement("svg:use");

    clone->setAttribute("xlink:href", Glib::ustring("#")+symbol_repr->attribute("id"));

    the_parent_repr->appendChild(clone);

    if( single_group && transform.isTranslation() ) {
        clone->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(transform));
    }

    // Change selection to new <use> element.
    set(clone);

    // Clean up
    Inkscape::GC::release(symbol_repr);

    DocumentUndo::done(doc, RC_("Undo", "Group to symbol"), "");
}

/*
 * Takes selected <use> that reference a symbol, and unSymbol those symbols
 */
void ObjectSet::unSymbol()
{
    for (const auto obj: items()) {
        auto use = cast<SPUse>(obj);
        if (use) {
            auto sym = cast<SPSymbol>(use->root());
            if (sym) {
                sym->unSymbol();
            }
        }
    }
    DocumentUndo::done(document(), RC_("Undo", "unSymbol all selected symbols"), "");
}

void ObjectSet::tile(bool apply)
{
    // toMarker has similar code

    SPDocument *doc = document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    // check if something is selected
    if (isEmpty()) {
        if (desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                                             _("Select <b>object(s)</b> to convert to pattern."));
        return;
    }

    doc->ensureUpToDate();
    Geom::OptRect r = visualBounds();
    if ( !r ) {
        return;
    }

    auto items_vec = items_vector();

    std::sort(items_vec.begin(), items_vec.end(), sp_object_compare_position_bool);

    // bottommost object, after sorting
    SPObject *parent = items_vec[0]->parent;

    Geom::Affine parent_transform;
    if (auto parentItem = cast<SPItem>(parent)) {
        parent_transform = parentItem->i2doc_affine();
    } else {
        g_assert_not_reached();
    }

    // remember the position of the first item
    int pos = items_vec[0]->getRepr()->position();

    // create a list of duplicates
    std::vector<Inkscape::XML::Node*> repr_copies;
    for (auto item : items_vec) {
        Inkscape::XML::Node *dup = item->getRepr()->duplicate(xml_doc);
        repr_copies.push_back(dup);
    }

    Geom::Rect bbox(r->min() * doc->dt2doc(), r->max() * doc->dt2doc());

    if (apply) {
        // delete objects so that their clones don't get alerted; this object will be restored shortly
        for (auto item : items_vec) {
            item->deleteObject(false);
        }
    }

    // Hack: Temporarily set clone compensation to unmoved, so that we can move clone-originals
    // without disturbing clones.
    // See ActorAlign::on_button_click() in src/ui/dialog/align-and-distribute.cpp
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    int saved_compensation = prefs->getInt("/options/clonecompensation/value", SP_CLONE_COMPENSATION_UNMOVED);
    prefs->setInt("/options/clonecompensation/value", SP_CLONE_COMPENSATION_UNMOVED);

    Geom::Affine move = Geom::Translate(- bbox.min());
    gchar const *pat_id = SPPattern::produce(repr_copies, bbox, doc,
                                       move.inverse() /* patternTransform */,
                                       parent_transform * move);

    // restore compensation setting
    prefs->setInt("/options/clonecompensation/value", saved_compensation);

    if (apply) {
        Inkscape::XML::Node *rect = xml_doc->createElement("svg:rect");
        gchar *style_str = g_strdup_printf("stroke:none;fill:url(#%s)", pat_id);
        rect->setAttribute("style", style_str);
        g_free(style_str);

        rect->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(parent_transform.inverse()));

        rect->setAttributeSvgDouble("width", bbox.width());
        rect->setAttributeSvgDouble("height", bbox.height());
        rect->setAttributeSvgDouble("x", bbox.left());
        rect->setAttributeSvgDouble("y", bbox.top());

        // restore parent and position
        parent->getRepr()->addChildAtPos(rect, pos);
        SPItem *rectangle = static_cast<SPItem *>(document()->getObjectByRepr(rect));

        Inkscape::GC::release(rect);

        clear();
        set(rectangle);
    }


    DocumentUndo::done(doc, RC_("Undo", "Objects to pattern"), "");
}

void ObjectSet::untile()
{

    SPDocument *doc = document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    // check if something is selected
    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select an <b>object with pattern fill</b> to extract objects from."));
        return;
    }

    std::vector<SPItem*> new_select;

    bool did = false;

    for (auto item : items_vector() | std::views::reverse) {
        SPStyle *style = item->style;

        if (!style || !style->fill.isPaintserver()) {
            continue;
        }

        SPPaintServer *server = item->style->getFillPaintServer();

        auto basePat = cast<SPPattern>(server);
        if (!basePat) {
            continue;
        }

        did = true;

        SPPattern *pattern = basePat->rootPattern();

        Geom::Affine pat_transform = basePat->getTransform();
        pat_transform *= item->transform;

        for (auto& child: pattern->children) {
            if (is<SPItem>(&child)) {
                Inkscape::XML::Node *copy = child.getRepr()->duplicate(xml_doc);
                auto i = cast<SPItem>(item->parent->appendChildRepr(copy));

               // FIXME: relink clones to the new canvas objects
               // use SPObject::setid when mental finishes it to steal ids of

                // this is needed to make sure the new item has curve (simply requestDisplayUpdate does not work)
                doc->ensureUpToDate();

                if (i) {
                    Geom::Affine transform( i->transform * pat_transform );
                    i->doWriteTransform(transform);

                    new_select.push_back(i);
                } else {
                    g_assert_not_reached();
                }
            }
        }

        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "fill", "none");
        sp_repr_css_change(item->getRepr(), css, "style");
    }

    if (!did) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("<b>No pattern fills</b> in the selection."));
    } else {
        DocumentUndo::done(document(), RC_("Undo", "Pattern to objects"), "");
        setList(new_select);
    }
}

void ObjectSet::createBitmapCopy()
{

    SPDocument *doc = document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    // check if something is selected
    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to make a bitmap copy."));
        return;
    }

    if (desktop()) {
        desktop()->messageStack()->flash(Inkscape::IMMEDIATE_MESSAGE, _("Rendering bitmap..."));
        // set "busy" cursor
        desktop()->setWaitingCursor();
    }

    // Get the bounding box of the selection
    doc->ensureUpToDate();
    Geom::OptRect bbox = documentBounds(SPItem::VISUAL_BBOX);
    if (!bbox) {
        if(desktop())
            desktop()->clearWaitingCursor();
        return; // exceptional situation, so not bother with a translatable error message, just quit quietly
    }

    // List of the items to show; all others will be hidden
    auto items_range = items();
    auto items_vec = std::vector<SPItem const *>(items_range.begin(), items_range.end());

    // Sort items so that the topmost comes last
    std::sort(items_vec.begin(), items_vec.end(), sp_item_repr_compare_position_bool);

    // Remember parent and z-order of the topmost one
    int pos = items_vec.back()->getRepr()->position();
    SPObject *parent_object = items_vec.back()->parent;
    Inkscape::XML::Node *parent = parent_object->getRepr();

    // Calculate resolution
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    double res;
    int const prefs_res = prefs->getInt("/options/createbitmap/resolution", 0);
    int const prefs_min = prefs->getInt("/options/createbitmap/minsize", 0);
    if (0 < prefs_res) {
        // If it's given explicitly in prefs, take it
        res = prefs_res;
    } else if (0 < prefs_min) {
        // If minsize is given, look up minimum bitmap size (default 250 pixels) and calculate resolution from it
        res = Inkscape::Util::Quantity::convert(prefs_min, "in", "px") / MIN(bbox->width(), bbox->height());
    } else {

        // Get export DPI from the first item available
        auto dpi = Geom::Point(0, 0);
        for (auto item : items_vec) {
            dpi = item->getExportDpi();
            if (dpi.x()) break;
        }
        if (!dpi.x()) {
            dpi = doc->getRoot()->getExportDpi();
        }
        if (dpi.x()) {
            res = dpi.x();
        } else {
            // if all else fails, take the default 96 dpi
            res = Inkscape::Util::Quantity::convert(1, "in", "px");
        }
    }

    if (res == Inkscape::Util::Quantity::convert(1, "in", "px")) { // for default 96 dpi, snap it to pixel grid
        bbox = bbox->roundOutwards();
    }

    // anti-aliasing override
    std::optional<Antialiasing> antialias;
    if (auto nv = doc->getNamedView()) {
        // if off, then disable antialiasing; if on, then let SVG dictate what it is
        if (!nv->antialias_rendering) {
            antialias = Antialiasing::None;
        }
    }

    Inkscape::Pixbuf *pb = sp_generate_internal_bitmap(doc, *bbox, res, items_vec, false, nullptr, 1, antialias);

    if (pb) {
        // Create the repr for the image
        Inkscape::XML::Node * repr = xml_doc->createElement("svg:image");
        sp_embed_image(repr, pb);
        repr->setAttributeSvgDouble("width", bbox->width());
        repr->setAttributeSvgDouble("height", bbox->height());

        // Calculate the matrix that will be applied to the image so that it exactly overlaps the source objects
        auto parentItem = cast<SPItem>(parent_object);
        Geom::Affine affine = Geom::Translate(bbox->left(), bbox->top()) * parentItem->i2doc_affine().inverse();

        // Write transform
        repr->setAttributeOrRemoveIfEmpty("transform", sp_svg_transform_write(affine));

        // add the new repr to the parent
        parent->addChildAtPos(repr, pos + 1);

        // Set selection to the new image
        clear();
        add(repr);

        // Clean up
        Inkscape::GC::release(repr);
        delete pb;

        // Complete undoable transaction
        DocumentUndo::done(doc, RC_("Undo", "Create bitmap"), INKSCAPE_ICON("selection-make-bitmap-copy"));
    }

    if(desktop()) {
        desktop()->clearWaitingCursor();
    }
}

/* Creates a mask or clipPath from selection.
 * What is a clip group?
 * A clip group is a tangled mess of XML that allows an object inside a group
 * to clip the entire group using a few <use>s and generally irritating me.
 */

void ObjectSet::setClipGroup()
{
    SPDocument* doc = document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to create clippath or mask from."));
        return;
    }

    auto p = xmlNodes_vector();

    sort(p.begin(),p.end(),sp_repr_compare_position_bool);

    clear();

    int topmost = (p.back())->position();
    Inkscape::XML::Node *topmost_parent = (p.back())->parent();

    Inkscape::XML::Node *inner = xml_doc->createElement("svg:g");
    inner->setAttribute("inkscape:label", "Clip");

    for(auto current : p){
        if (current->parent() == topmost_parent) {
            Inkscape::XML::Node *spnew = current->duplicate(xml_doc);
            sp_repr_unparent(current);
            inner->appendChild(spnew);
            Inkscape::GC::release(spnew);
            topmost --; // only reduce count for those items deleted from topmost_parent
        } else { // move it to topmost_parent first
            std::vector<Inkscape::XML::Node*> temp_clip;

            // At this point, current may already have no item, due to its being a clone whose original is already moved away
            // So we copy it artificially calculating the transform from its repr->attr("transform") and the parent transform
            gchar const *t_str = current->attribute("transform");
            Geom::Affine item_t(Geom::identity());
            if (t_str)
                sp_svg_transform_read(t_str, &item_t);
            item_t *= cast<SPItem>(doc->getObjectByRepr(current->parent()))->i2doc_affine();
            // FIXME: when moving both clone and original from a transformed group (either by
            // grouping into another parent, or by cut/paste) the transform from the original's
            // parent becomes embedded into original itself, and this affects its clones. Fix
            // this by remembering the transform diffs we write to each item into an array and
            // then, if this is clone, looking up its original in that array and pre-multiplying
            // it by the inverse of that original's transform diff.

            sp_selection_copy_one(current, item_t, temp_clip, xml_doc);
            sp_repr_unparent(current);

            // paste into topmost_parent (temporarily)
            std::vector<Inkscape::XML::Node*> copied = sp_selection_paste_impl(doc, doc->getObjectByRepr(topmost_parent), temp_clip);
            if (!copied.empty()) { // if success,
                // take pasted object (now in topmost_parent)
                Inkscape::XML::Node *in_topmost = copied.back();
                // make a copy
                Inkscape::XML::Node *spnew = in_topmost->duplicate(xml_doc);
                // remove pasted
                sp_repr_unparent(in_topmost);
                // put its copy into group
                inner->appendChild(spnew);
                Inkscape::GC::release(spnew);
            }
        }
    }

    Inkscape::XML::Node *outer = xml_doc->createElement("svg:g");
    outer->appendChild(inner);
    topmost_parent->addChildAtPos(outer, topmost + 1);

    Inkscape::XML::Node *clone = xml_doc->createElement("svg:use");
    clone->setAttribute("x", "0");
    clone->setAttribute("y", "0");
    clone->setAttribute("xlink:href", g_strdup_printf("#%s", inner->attribute("id")));

    clone->setAttribute("inkscape:transform-center-x", inner->attribute("inkscape:transform-center-x"));
    clone->setAttribute("inkscape:transform-center-y", inner->attribute("inkscape:transform-center-y"));

    std::vector<Inkscape::XML::Node*> templist;
    templist.push_back(clone);
    // add the new clone to the top of the original's parent
    gchar const *mask_id = SPClipPath::create(templist, doc);

    char* tmp = g_strdup_printf("url(#%s)", mask_id);
    outer->setAttribute("clip-path", tmp);
    g_free(tmp);

    Inkscape::GC::release(clone);

    set(outer);
    DocumentUndo::done(doc, RC_("Undo", "Create Clip Group"), "");
}

void ObjectSet::chameleonFill()
{
    auto doc = document();
    doc->ensureUpToDate();

    // Build a new drawing so our manipulations don't corupt to canvas
    Drawing drawing;
    unsigned dkey = SPItem::display_key_new(1);
    auto root = doc->getRoot()->invoke_show(drawing, dkey, SP_ITEM_SHOW_DISPLAY);
    drawing.setRoot(root);

    // Hide all *SELECTED* objects from the drawing
    for (auto&& item : items()) {
        item->invoke_hide(dkey);
    }

    drawing.update();

    // Now in a new loop, ask for a color to be set for each item.
    for (auto&& item : items()) {
        if (auto shape = cast<SPShape>(item)) {
            bool evenodd = shape->style->fill_rule.computed == SP_WIND_RULE_EVENODD;
            if (auto curve = shape->curve()) {
                auto color = drawing.averageColor(*curve * shape->i2dt_affine(), evenodd);
                auto style = sp_repr_css_attr_new();
                sp_repr_css_set_property_double(style, "fill-opacity", color.stealOpacity());
                sp_repr_css_set_property_string(style, "fill", color.toString(false));
                sp_desktop_apply_css_recursive(item, style, true);
                sp_repr_css_attr_unref(style);

            }
        }
    }

    doc->getRoot()->invoke_hide(dkey);

    DocumentUndo::done(doc, RC_("Undo", "Chameleon Fill"), "");
}

/**
 * Creates a mask or clipPath from selection.
 * Two different modes:
 *  if applyToLayer, all selection is moved to DEFS as mask/clippath
 *       and is applied to current layer
 *  otherwise, topmost object is used as mask for other objects
 * If \a apply_clip_path parameter is true, clipPath is created, otherwise mask
 *
 */
 void ObjectSet::setMask(bool apply_clip_path, bool apply_to_layer, bool remove_original)
{
    if(!desktop() && apply_to_layer)
        return;

    SPDocument *doc = document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    // check if something is selected
    bool is_empty = isEmpty();
    if ( apply_to_layer && is_empty) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to create clippath or mask from."));
        return;
    } else if (!apply_to_layer && (is_empty || std::ranges::distance(items()) == 1)) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select mask object and <b>object(s)</b> to apply clippath or mask to."));
        return;
    }

    // FIXME: temporary patch to prevent crash!
    // Remove this when bboxes are fixed to not blow up on an item clipped/masked with its own clone
    bool clone_with_original = object_set_contains_both_clone_and_original(this);
    if (clone_with_original) {
        g_warning("Unable to clip/mask an object with its own clone");
        return; // in this version, you cannot clip/mask an object with its own clone
    }
    // /END FIXME

    doc->ensureUpToDate();

    auto items_vec = items_vector();

    std::sort(items_vec.begin(), items_vec.end(), sp_object_compare_position_bool);

    // See lp bug #542004
    clear();

    // create a list of duplicates
    std::vector<std::pair<Inkscape::XML::Node*, Geom::Affine>> mask_items;
    std::vector<SPItem*> apply_to_items;
    std::vector<SPItem*> items_to_delete;
    std::vector<SPItem*> items_to_select;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool topmost = prefs->getBool("/options/maskobject/topmost", true);
    int grouping = prefs->getInt("/options/maskobject/grouping", PREFS_MASKOBJECT_GROUPING_NONE);

    if (apply_to_layer) {
        // all selected items are used for mask, which is applied to a layer
        apply_to_items.push_back(desktop()->layerManager().currentLayer());
    }

    for (auto i : items_vec) {
        if ((!topmost && !apply_to_layer && i == items_vec.front())
                || (topmost && !apply_to_layer && i == items_vec.back())
                || apply_to_layer)
        {
            Inkscape::XML::Node *dup = i->getRepr()->duplicate(xml_doc);
            mask_items.emplace_back(dup, i->i2doc_affine());

            if (remove_original) {
                items_to_delete.push_back(i);
            }
            else {
                items_to_select.push_back(i);
            }
            continue;
        }else{
            apply_to_items.push_back(i);
            items_to_select.push_back(i);
        }
    }

    items_vec.clear();

    if (grouping == PREFS_MASKOBJECT_GROUPING_ALL) {
        // group all those objects into one group
        // and apply mask to that
        auto set = ObjectSet{document()};
        set.add(apply_to_items.begin(), apply_to_items.end());

        items_to_select.clear();

        Inkscape::XML::Node *group = set.group();
        group->setAttribute("inkscape:groupmode", "maskhelper");

        // apply clip/mask only to newly created group
        apply_to_items.clear();
        apply_to_items.push_back(cast<SPItem>(doc->getObjectByRepr(group)));

        items_to_select.push_back(cast<SPItem>(doc->getObjectByRepr(group)));

        Inkscape::GC::release(group);
    }
    if (grouping == PREFS_MASKOBJECT_GROUPING_SEPARATE) {
        items_to_select.clear();
    }

    char const *attributeName = apply_clip_path ? "clip-path" : "mask";
    for (auto item : apply_to_items | std::views::reverse) {
        std::vector<Inkscape::XML::Node*> mask_items_dup;
        std::map<Inkscape::XML::Node*, Geom::Affine> dup_transf;
        for (auto const &mask_item : mask_items) {
            Inkscape::XML::Node *dup = mask_item.first->duplicate(xml_doc);
            mask_items_dup.push_back(dup);
            dup_transf[dup] = mask_item.second;
        }

        Inkscape::XML::Node *current = item->getRepr();
        // Node to apply mask to
        Inkscape::XML::Node *apply_mask_to = current;

        if (grouping == PREFS_MASKOBJECT_GROUPING_SEPARATE) {
            // enclose current node in group, and apply crop/mask on that
            Inkscape::XML::Node *group = xml_doc->createElement("svg:g");
            // make a note we should ungroup this when unsetting mask
            group->setAttribute("inkscape:groupmode", "maskhelper");

            Inkscape::XML::Node *spnew = current->duplicate(xml_doc);
            current->parent()->addChild(group, current);
            sp_repr_unparent(current);
            group->appendChild(spnew);

            // Apply clip/mask to group instead
            apply_mask_to = group;
            item = cast<SPItem>(doc->getObjectByRepr(group));

            items_to_select.push_back(item);
            Inkscape::GC::release(spnew);
            Inkscape::GC::release(group);
        }

        char const *mask_id = nullptr;
        if (apply_clip_path) {
            mask_id = SPClipPath::create(mask_items_dup, doc);
        } else {
            mask_id = SPMask::create(mask_items_dup, doc);
        }

        // inverted object transform should be applied to a mask object,
        // as mask is calculated in user space (after applying transform)
        for (auto const &it : mask_items_dup) {
            auto clip_item = cast<SPItem>(doc->getObjectByRepr(it));
            clip_item->doWriteTransform(dup_transf[it]);
            clip_item->doWriteTransform(clip_item->transform * item->i2doc_affine().inverse());
        }

        apply_mask_to->setAttribute(attributeName, Glib::ustring("url(#") + mask_id + ')');
    }

    for (auto item : items_to_delete) {
        item->deleteObject(false);
        items_to_select.erase(std::remove(items_to_select.begin(), items_to_select.end(), item), items_to_select.end());
    }

    addList(items_to_select);
}

void ObjectSet::unsetMask(const bool apply_clip_path,
                          const bool delete_helper_group,
                          const bool remove_original)
{
    SPDocument *doc = document();
    Inkscape::XML::Document *xml_doc = doc->getReprDoc();

    // check if something is selected
    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to remove clippath or mask from."));
        return;
    }

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool ungroup_masked = prefs->getBool("/options/maskobject/ungrouping", true);
    doc->ensureUpToDate();

    gchar const *attributeName = apply_clip_path ? "clip-path" : "mask";
    std::map<SPObject*,SPItem*> referenced_objects;

    auto items_vec = items_vector();
    clear();

    std::vector<SPGroup *> items_to_ungroup;
    auto items_to_select = items_vec;

    // SPObject* refers to a group containing the clipped path or mask itself,
    // whereas SPItem* refers to the item being clipped or masked
    for (auto i : items_vec) {
        if (remove_original) {
            // remember referenced mask/clippath, so orphaned masks can be moved back to document
            SPItem *item = i;
            SPObject *obj_ref = nullptr;

            if (apply_clip_path) {
                obj_ref = item->getClipObject();
            } else {
                obj_ref = item->getMaskObject();
            }

            // collect distinct mask object (and associate with item to apply transform)
            if (obj_ref) {
                referenced_objects[obj_ref] = item;
            }
        }

        i->setAttribute(attributeName, "none");

        auto group = cast<SPGroup>(i);
        if (ungroup_masked && group && delete_helper_group) {
            // if we had previously enclosed masked object in group,
            // add it to list so we can ungroup it later

            // ungroup only groups we created when setting clip/mask
            if (group->layerMode() == SPGroup::MASK_HELPER) {
                items_to_ungroup.push_back(group);
            }
        }
    }

    // restore mask objects into a document
    for (auto & referenced_object : referenced_objects) {
        SPObject *obj = referenced_object.first; // Group containing the clipped paths or masks
        std::vector<Inkscape::XML::Node *> items_to_move;
        for (auto& child: obj->children) {
            // Collect all clipped paths and masks within a single group
            Inkscape::XML::Node *copy = child.getRepr()->duplicate(xml_doc);
            if (copy->attribute("inkscape:original-d") && copy->attribute("inkscape:path-effect")) {
                copy->setAttribute("d", copy->attribute("inkscape:original-d"));
            } else if (copy->attribute("inkscape:original-d")) {
                copy->setAttribute("d", copy->attribute("inkscape:original-d"));
                copy->removeAttribute("inkscape:original-d");
            } else if (!copy->attribute("inkscape:path-effect") && !is<SPPath>(&child)) {
                copy->removeAttribute("d");
                copy->removeAttribute("inkscape:original-d");
            }
            items_to_move.push_back(copy);
        }

        if (!obj->isReferenced()) {
            // delete from defs if no other object references this mask
            obj->deleteObject(false);
        }

        // remember parent and position of the item to which the clippath/mask was applied
        Inkscape::XML::Node *parent = (referenced_object.second)->getRepr()->parent();
        Inkscape::XML::Node *ref_repr = referenced_object.second->getRepr();

        // Iterate through all clipped paths / masks
        for (auto const repr : items_to_move | std::views::reverse) {
            // insert into parent, restore pos
            parent->addChild(repr, ref_repr);

            auto mask_item = cast<SPItem>(document()->getObjectByRepr(repr));
            if (!mask_item) {
                continue;
            }
            items_to_select.push_back(mask_item);

            // transform mask, so it is moved the same spot where mask was applied
            mask_item->doWriteTransform(mask_item->transform * referenced_object.second->transform);
        }
    }

    // ungroup marked groups added when setting mask
    for (auto group : items_to_ungroup | std::views::reverse) {
        if (group) {
            items_to_select.erase(std::remove(items_to_select.begin(), items_to_select.end(), group), items_to_select.end());
            std::vector<SPItem*> children;
            sp_item_group_ungroup(group, children);
            items_to_select.insert(items_to_select.end(),children.rbegin(),children.rend());
        } else {
            g_assert_not_reached();
        }
    }

    // rebuild selection
    addList(items_to_select);
}

/**
 * \param with_margins margins defined in the xml under <sodipodi:namedview>
 *                     "fit-margin-..." attributes.  See SPDocument::fitToRect.
 * \return true if an undoable change should be recorded.
 */
bool ObjectSet::fitCanvas(bool with_margins, bool skip_undo)
{
    g_return_val_if_fail(document(), false);

    if (isEmpty()) {
        if(desktop())
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>object(s)</b> to fit canvas to."));
        return false;
    }
    Geom::OptRect const bbox = documentBounds(SPItem::VISUAL_BBOX);
    if (bbox) {
        document()->fitToRect(*bbox, with_margins);
        if(!skip_undo)
            DocumentUndo::done(document(), RC_("Undo", "Fit Page to Selection"), "");
        return true;
    } else {
        return false;
    }
}

void ObjectSet::swapFillStroke()
{
    SPIPaint *paint;
    SPPaintServer *server;
    Glib::ustring _paintserver_id;

    for (auto const item : items()) {
        SPCSSAttr *css = sp_repr_css_attr_new ();

        _paintserver_id.clear();
        paint = &(item->style->fill);
        if (paint->set && paint->isNone())
            sp_repr_css_set_property (css, "stroke", "none");
        else if (paint->set && paint->isColor()) {
            auto color = paint->getColor();
            color.addOpacity(item->style->fill_opacity.as_double());
            sp_repr_css_set_property_string(css, "stroke", color.toString());
        }
        else if (!paint->set)
            sp_repr_css_unset_property (css, "stroke");
        else if (paint->set && paint->isPaintserver()) {
            server = SP_STYLE_FILL_SERVER(item->style);
            if (server) {
                Inkscape::XML::Node *srepr = server->getRepr();
                _paintserver_id += "url(#";
                _paintserver_id += srepr->attribute("id");
                _paintserver_id += ")";
                sp_repr_css_set_property (css, "stroke", _paintserver_id.c_str());
            }
        }

        _paintserver_id.clear();
        paint = &(item->style->stroke);
        if (paint->set && paint->isNone())
            sp_repr_css_set_property (css, "fill", "none");
        else if (paint->set && paint->isColor()) {
            auto color = paint->getColor();
            color.addOpacity(item->style->stroke_opacity.as_double());
            sp_repr_css_set_property_string(css, "fill", color.toString());
        }
        else if (!paint->set)
            sp_repr_css_unset_property (css, "fill");
        else if (paint->set && paint->isPaintserver()) {
            server = SP_STYLE_STROKE_SERVER(item->style);
            if (server) {
                Inkscape::XML::Node *srepr = server->getRepr();
                _paintserver_id += "url(#";
                _paintserver_id += srepr->attribute("id");
                _paintserver_id += ")";
                sp_repr_css_set_property (css, "fill", _paintserver_id.c_str());
            }
        }

        if (desktop()) {
            Inkscape::ObjectSet set{};
            set.add(item);
            sp_desktop_set_style(&set, desktop(), css);
        } else {
            sp_desktop_apply_css_recursive(item, css, true);
        }

        sp_repr_css_attr_unref (css);
    }

    DocumentUndo::done(document(), RC_("Undo", "Swap fill and stroke of an object"), "");
}

/**
 * Creates a linked fill between all the objects in the current selection using
 * the "Fill Between Many" LPE. After this method completes, the linked fill
 * created becomes the new selection, so as to facilitate quick styling of the
 * fill.
 *
 * All objects referred to must have an ID. If an ID does not exist, it will be
 * created.
 *
 * As an additional timesaver, the fill object is created below the bottommost
 * object in the selection.
 */
void ObjectSet::fillBetweenMany()
{
    if (isEmpty()) {
        if (desktop()) {
            desktop()->messageStack()->flash(Inkscape::WARNING_MESSAGE, _("Select <b>path(s)</b> to create fill between."));
        }

        return;
    }

    SPDocument *doc  = document();
    SPObject *defs   = doc->getDefs();
    SPObject *effect = nullptr;

    Inkscape::XML::Node *effectRepr = doc->getReprDoc()->createElement("inkscape:path-effect");
    Inkscape::XML::Node *fillRepr   = doc->getReprDoc()->createElement("svg:path");

    Glib::ustring acc;
    Glib::ustring pathTarget;

    for (auto&& item : items()) {
        // Force-assign id if there is none present
        if (!item->getId()) {
            auto id = item->generate_unique_id();
            item->set(SPAttr::ID, id.c_str());
            item->updateRepr();
        }

        acc += "#";
        acc += item->getId();
        acc += ",0,1|";
    }

    effectRepr->setAttribute("effect", "fill_between_many");
    effectRepr->setAttribute("method", "originald");
    effectRepr->setAttribute("linkedpaths", acc.c_str());
    defs->appendChild(effectRepr);

    effect = doc->getObjectByRepr(effectRepr);
    pathTarget += "#";
    pathTarget += effect->getId();

    fillRepr->setAttribute("inkscape:original-d", "M 0,0");
    fillRepr->setAttribute("inkscape:path-effect", pathTarget.c_str());
    fillRepr->setAttribute("d", "M 0,0");

    // Get bottommost element in selection to create fill underneath
    auto items_vec = items_vector();
    SPObject *first = *std::min_element(items_vec.begin(), items_vec.end(), sp_object_compare_position_bool);
    SPObject *prev  = first->getPrev();

    first->parent->addChild(fillRepr, prev ? prev->getRepr() : nullptr);

    doc->ensureUpToDate();

    clear();
    add(fillRepr);

    DocumentUndo::done(doc, RC_("Undo", "Create linked fill object between paths"), "");
}

/**
 * Associates the given SPItem with a SiblingState enum
 * Needed for handling special cases while transforming objects
 * Inserts the [SPItem, SiblingState] pair to ObjectSet._sibling_state map
 * @param item
 * @return the SiblingState
 */
SiblingState
ObjectSet::getSiblingState(SPItem *item) {
    auto offset = cast<SPOffset>(item);
    auto flowtext = cast<SPFlowtext>(item);

    auto check_item = _sibling_state.find(item);
    if (check_item != _sibling_state.end() && check_item->second > SiblingState::SIBLING_NONE) {
        return check_item->second;
    }

    SiblingState ret = SiblingState::SIBLING_NONE;

	// moving both a clone and its original or any ancestor
    if (object_set_contains_original(item, this)) {
        ret = SiblingState::SIBLING_CLONE_ORIGINAL;

	// moving both a text-on-path and its path
    } else if (is<SPText>(item) && is<SPTextPath>(item->firstChild()) &&
               includes(sp_textpath_get_path_item(cast_unsafe<SPTextPath>(item->firstChild())))) {
        ret = SiblingState::SIBLING_TEXT_PATH;

	// moving both a flowtext and its frame
    } else if (flowtext && includes(flowtext->get_frame(nullptr))) {
        ret = SiblingState::SIBLING_TEXT_FLOW_FRAME;

	// moving both an offset and its source
    } else if (offset && offset->sourceHref && includes(sp_offset_get_source(offset))) {
        ret = SiblingState::SIBLING_OFFSET_SOURCE;

	// moving object containing sub object
    } else if (item->style && item->style->shape_inside.containsAnyShape(this)) {
        ret = SiblingState::SIBLING_TEXT_SHAPE_INSIDE;
    }

	_sibling_state[item] = ret;

    return ret;
}

void
ObjectSet::clearSiblingStates()
{
    _sibling_state.clear();
}

/**
 * \param with_margins margins defined in the xml under <sodipodi:namedview>
 *                     "fit-margin-..." attributes.  See SPDocument::fitToRect.
 *
 * WARNING: this is a page naive and it will break multi page documents.
 */
bool
fit_canvas_to_drawing(SPDocument *doc, bool with_margins)
{
    g_return_val_if_fail(doc, false);

    doc->ensureUpToDate();
    SPItem const *const root = doc->getRoot();
    Geom::OptRect bbox = root->documentVisualBounds();
    if (bbox) {
        doc->fitToRect(*bbox, with_margins);
        return true;
    } else {
        return false;
    }
}

void
fit_canvas_to_drawing(SPDesktop *desktop)
{
    if (fit_canvas_to_drawing(desktop->getDocument())) {
        DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Fit Page to Drawing"), "");
    }
}

static void itemtree_map(void (*f)(SPItem *, SPDesktop *), SPObject *root, SPDesktop *desktop) {
    // don't operate on layers
    if (auto item = cast<SPItem>(root)) {
        if (!desktop->layerManager().isLayer(item)) {
            f(item, desktop);
        }
    }
    for (auto& child: root->children) {
        //don't recurse into locked layers
        auto item = cast<SPItem>(&child);
        if (!(item && desktop->layerManager().isLayer(item) && item->isLocked())) {
            itemtree_map(f, &child, desktop);
        }
    }
}

static void unlock(SPItem *item, SPDesktop */*desktop*/) {
    if (item->isLocked()) {
        item->setLocked(false);
    }
}

static void unhide(SPItem *item, SPDesktop *desktop) {
    if (desktop->itemIsHidden(item)) {
        item->setExplicitlyHidden(false);
    }
}

static void process_all(void (*f)(SPItem *, SPDesktop *), SPDesktop *dt, bool layer_only) {
    if (!dt) return;

    SPObject *root;
    if (layer_only) {
        root = dt->layerManager().currentLayer();
    } else {
        root = dt->layerManager().currentRoot();
    }

    itemtree_map(f, root, dt);
}

void unlock_all(SPDesktop *dt) {
    process_all(&unlock, dt, true);
}

void unlock_all_in_all_layers(SPDesktop *dt) {
    process_all(&unlock, dt, false);
}

void unhide_all(SPDesktop *dt) {
    process_all(&unhide, dt, true);
}

void unhide_all_in_all_layers(SPDesktop *dt) {
    process_all(&unhide, dt, false);
}

namespace {

const char* get_action_key(double mh, double sh, double mv, double sv) {
    // do the action only if one of the scales/moves is greater than half the last significant
    // digit in the spinbox (currently spinboxes have 3 fractional digits, so that makes 0.0005). If
    // the value was changed by the user, the difference will be at least that much; otherwise it's
    // just rounding difference between the spinbox value and actual value, so no action is
    // performed
    double const threshold = 5e-4;
    const char* const action = mh > threshold ? "move:horizontal:" :
                               sh > threshold ? "scale:horizontal:" :
                               mv > threshold ? "move:vertical:" :
                               sv > threshold ? "scale:vertical:" : nullptr;
    // if (!action) {
        // return nullptr;
    // }
    // _action_key = _action_prefix + action;
    return action; //_action_key.c_str();
}

}

void sp_transform_selected_items(SPDesktop* desktop, const Geom::Rect& rect, const Inkscape::Util::Unit* unit,
        const std::string& action_prefix,
        bool transform_stroke, bool preserve_transform, bool use_visual_box) {

    if (!desktop || !unit) return;

    auto selection = desktop->getSelection();
    auto document = desktop->getDocument();
    auto& pm = document->getPageManager();
    auto page = pm.getSelectedPageRect();
    auto page_correction = document->get_origin_follows_page();

    document->ensureUpToDate();

    Geom::OptRect bbox_vis = selection->visualBounds();
    Geom::OptRect bbox_geom = selection->geometricBounds();
    Geom::OptRect bbox_user = selection->preferredBounds();

    if (!bbox_user) {
        return;
    }

    double old_w = bbox_user->width();
    double old_h = bbox_user->height();
    double new_w, new_h, new_x, new_y = 0;

    auto x = rect.min().x(); // _x_item.get_adjustment();
    auto y = rect.min().y(); //_y_item.get_adjustment();
    auto w = rect.width(); // _w_item.get_adjustment();
    auto h = rect.height(); // _h_item.get_adjustment();

    if (unit->type == Util::UNIT_TYPE_LINEAR) {
        new_w = Quantity::convert(w, unit, "px");
        new_h = Quantity::convert(h, unit, "px");
        new_x = Quantity::convert(x, unit, "px");
        new_y = Quantity::convert(y, unit, "px");

    } else {
        double old_x = bbox_user->min()[Geom::X] + old_w * selection->anchor.x();
        double old_y = bbox_user->min()[Geom::Y] + old_h * selection->anchor.y();

        // Adjust against selected page, so later correction isn't broken.
        if (page_correction) {
            old_x -= page.left();
            old_y -= page.top();
        }

        new_x = old_x * (x / 100 / unit->factor);
        new_y = old_y * (y / 100 / unit->factor);
        new_w = old_w * (w / 100 / unit->factor);
        new_h = old_h * (h / 100 / unit->factor);
    }

    // Adjust depending on the selected anchor.
    double x0 = new_x - old_w * selection->anchor.x() - (new_w - old_w) * selection->anchor.x();
    double y0 = new_y - old_h * selection->anchor.y() - (new_h - old_h) * selection->anchor.y();

    // Adjust according to the selected page, if needed
    if (page_correction) {
        x0 += page.left();
        y0 += page.top();
    }

    double x1 = x0 + new_w;
    // double xrel = new_w / old_w;
    double y1 = y0 + new_h;
    // double yrel = new_h / old_h;

    // Keep proportions if lock is on
    // if (_lock_btn.get_active()) {
    //     if (adj == _adj_h) {
    //         x1 = x0 + yrel * bbox_user->dimensions()[Geom::X];
    //     } else if (adj == _adj_w) {
    //         y1 = y0 + xrel * bbox_user->dimensions()[Geom::Y];
    //     }
    // }

    // scales and moves, in px
    double mh = fabs(x0 - bbox_user->min().x());
    double sh = fabs(x1 - bbox_user->max().x());
    double mv = fabs(y0 - bbox_user->min().y());
    double sv = fabs(y1 - bbox_user->max().y());

    // unless the unit is %, convert the scales and moves to the unit
    if (unit->type == Util::UNIT_TYPE_LINEAR) {
        mh = Quantity::convert(mh, "px", unit);
        sh = Quantity::convert(sh, "px", unit);
        mv = Quantity::convert(mv, "px", unit);
        sv = Quantity::convert(sv, "px", unit);
    }

    if (auto actionkey = get_action_key(mh, sh, mv, sv)) {
        Geom::Affine scaler;
        if (use_visual_box) { // SPItem::VISUAL_BBOX
            scaler = get_scale_transform_for_variable_stroke(*bbox_vis, *bbox_geom, transform_stroke, preserve_transform, x0, y0, x1, y1);
        } else {
            // 1) We could have used the newer get_scale_transform_for_variable_stroke() here, but to avoid regressions
            // we'll just use the old get_scale_transform_for_uniform_stroke() for now.
            // 2) get_scale_transform_for_uniform_stroke() is intended for visual bounding boxes, not geometrical ones!
            // we'll trick it into using a geometric bounding box though, by setting the stroke width to zero
            scaler = get_scale_transform_for_uniform_stroke(*bbox_geom, 0, 0, false, false, x0, y0, x1, y1);
        }

        selection->applyAffine(scaler);
        static std::string key = action_prefix + actionkey; // simple logger doesn't own strings
        DocumentUndo::maybeDone(document, key.c_str(), RC_("Undo", "Transform selected items"), INKSCAPE_ICON("tool-pointer"));
    }
}

Geom::Rect sp_selection_get_xywh(SPDesktop* desktop, const Inkscape::Util::Unit* unit, bool use_visual_box) {
    if (!desktop || !unit) return Geom::Rect();

    auto sel = desktop->getSelection();
    auto bbox = sel->bounds(use_visual_box ? SPItem::VISUAL_BBOX : SPItem::GEOMETRIC_BBOX);
    if (sel->isEmpty() || !bbox) return {};

    // auto const unit = _tracker->getActiveUnit();

    auto width = bbox->width();
    auto height = bbox->height();
    auto x = bbox->left() + width * sel->anchor.x();
    auto y = bbox->top() + height * sel->anchor.y();

    if (desktop->getDocument()->get_origin_follows_page()) {
        auto &pm = desktop->getDocument()->getPageManager();
        auto page = pm.getSelectedPageRect();
        x -= page.left();
        y -= page.top();
    }

    // auto _adj_x = _x_item.get_adjustment();
    // auto _adj_y = _y_item.get_adjustment();
    // auto _adj_w = _w_item.get_adjustment();
    // auto _adj_h = _h_item.get_adjustment();

    if (unit->type == Util::UNIT_TYPE_DIMENSIONLESS) {
        // todo if/when needed
        // double const val = unit->factor * 100;
        // _adj_x->set_value(val);
        // _adj_y->set_value(val);
        // _adj_w->set_value(val);
        // _adj_h->set_value(val);
        // _tracker->setFullVal(_adj_x->gobj(), x);
        // _tracker->setFullVal(_adj_y->gobj(), y);
        // _tracker->setFullVal(_adj_w->gobj(), width);
        // _tracker->setFullVal(_adj_h->gobj(), height);
        return {};
    }
    else {
        return Geom::Rect::from_xywh(
            Quantity::convert(x, "px", unit),
            Quantity::convert(y, "px", unit),
            Quantity::convert(width, "px", unit),
            Quantity::convert(height, "px", unit)
        );
    }

    return {};
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
