// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * System-wide clipboard management - implementation.
 *//*
 * Authors:
 * see git history
 *   Krzysztof Kosiński <tweenk@o2.pl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Incorporates some code from selection-chemistry.cpp, see that file for more credits.
 *   Abhishek Sharma
 *   Tavmjong Bah
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "clipboard.h"

#include <boost/bimap.hpp>
#include <chrono>
#include <giomm/application.h>
#include <glib/gi18n.h>
#include <glibmm/convert.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <2geom/path-sink.h>

#include "context-fns.h"
#include "desktop-style.h"
#include "path/path-curve.h"
#include "extension/db.h" // extension database
#include "extension/input.h"
#include "extension/output.h"
#include "file.h" // for file_import, used in _pasteImage
#include "filter-chemistry.h"
#include "gradient-drag.h"
#include "helper/png-write.h"
#include "id-clash.h"
#include "live_effects/lpe-bspline.h"
#include "live_effects/lpe-spiro.h"
#include "live_effects/lpeobject-reference.h"
#include "live_effects/parameter/path.h"
#include "object/box3d.h"
#include "object/persp3d.h"
#include "object/sp-clippath.h"
#include "object/sp-defs.h"
#include "object/sp-flowtext.h"
#include "object/sp-gradient-reference.h"
#include "object/sp-hatch.h"
#include "object/sp-linear-gradient.h"
#include "object/sp-marker.h"
#include "object/sp-mask.h"
#include "object/sp-mesh-gradient.h"
#include "object/sp-page.h"
#include "object/sp-path.h"
#include "object/sp-pattern.h"
#include "object/sp-radial-gradient.h"
#include "object/sp-root.h"
#include "object/sp-symbol.h"
#include "object/sp-textpath.h"
#include "object/sp-use.h"
#include "selection-chemistry.h"
#include "selection.h"
#include "svg/svg.h" // for sp_svg_transform_write, used in _copySelection
#include "text-chemistry.h"
#include "text-editing.h"
#include "ui/tool/control-point-selection.h"
#include "ui/tool/multi-path-manipulator.h"
#include "ui/tools/dropper-tool.h" // used in copy()
#include "ui/tools/node-tool.h"
#include "ui/tools/text-tool.h"
#include "util/value-utils.h"
#include "xml/sp-css-attr.h"

#ifdef _WIN32
#undef NOGDI
#include <windows.h>
#endif

using namespace Inkscape::Util;

namespace Inkscape::UI {
namespace {

constexpr bool DEBUG_CLIPBOARD = false;

/// Made up mimetype to represent Gdk::Pixbuf clipboard contents.
constexpr auto CLIPBOARD_GDK_PIXBUF_TARGET = "image/x-gdk-pixbuf";

constexpr auto CLIPBOARD_TEXT_TARGET = "text/plain";

/** List of supported clipboard targets, in order of preference.
 *
 * Clipboard Formats: http://msdn.microsoft.com/en-us/library/ms649013(VS.85).aspx
 * On Windows, most graphical applications can handle CF_DIB/CF_BITMAP and/or CF_ENHMETAFILE
 * GTK automatically presents an "image/bmp" target as CF_DIB/CF_BITMAP
 * Presenting "image/x-emf" as CF_ENHMETAFILE must be done by Inkscape
 */
constexpr auto preferred_targets = std::to_array({
    "image/x-inkscape-svg",
    "image/svg+xml",
    "image/svg+xml-compressed",
    "image/x-emf",
    "CF_ENHMETAFILE",
    "WCF_ENHMETAFILE", // seen on Wine
    "application/pdf",
    "image/x-adobe-illustrator"
});

#ifdef __APPLE__

template <typename L, typename R>
boost::bimap<L, R> make_bimap(std::initializer_list<typename boost::bimap<L, R>::value_type> list)
{
    return boost::bimap<L, R>(list.begin(), list.end());
}

// MIME type to Universal Type Identifiers
auto const mime_uti = make_bimap<std::string, std::string>({
    {"image/x-inkscape-svg", "org.inkscape.svg"},
    {"image/svg+xml",        "public.svg-image"},
    {"image/png",            "public.png"},
    {"image/webp",           "public.webp"},
    {"image/tiff",           "public.tiff"},
    {"image/jpeg",           "public.jpeg"},
    {"image/x-e-postscript", "com.adobe.encapsulated-postscript"},
    {"image/x-postscript",   "com.adobe.postscript"},
 // {"text/plain",           "public.plain-text"}, - GIMP color palette
    {"text/html",            "public.html"},
    {"application/pdf",      "com.adobe.pdf"},
    {"application/tar",      "public.tar-archive"},
    {"application/x-zip",    "public.zip-archive"},
});

#endif

/// Type used to represent the Inkscape clipboard on the GTK clipboard.
struct ClipboardSvg {};

/*
 * Fixme: Get rid of all event pumpers.
 * This will require big changes to the ClipboardManager API and its users.
 *
 * Note that it may be a good idea to wait for coroutine support first,
 * otherwise the code in this file will first be refactored to a mess of callbacks,
 * and then refactored back again when coroutine support finally lands.
 */
template <typename F>
void pump_until(F const &f)
{
    auto main_context = Glib::MainContext::get_default();
    while (!f()) {
        main_context->iteration(true);
    }
}

// Fixme: Get rid of temporary files hack.
/** Get a temporary file name.
 * 
 * @arg suffix file suffix. May only contain ASCII characters.
 * 
 * @returns Filename with absolute path.
 * Value is in platform-native encoding (see Glib::filename_to_utf8).
 */
std::string get_tmp_filename(char const *suffix)
{
    return Glib::build_filename(Glib::get_user_cache_dir(), suffix);
}

/**
 * Default implementation of the clipboard manager.
 */
class ClipboardManagerImpl : public ClipboardManager
{
public:
    void copy(ObjectSet *set) override;
    void copyPathParameter(Inkscape::LivePathEffect::PathParam *) override;
    bool copyString(Glib::ustring str) override;
    void copySymbol(Inkscape::XML::Node* symbol, gchar const* style, SPDocument *source, const char* symbol_set, Geom::Rect const &bbox, bool set_clipboard) override;
    void insertSymbol(SPDesktop *desktop, Geom::Point const &shift_dt, bool read_clipboard) override;
    bool paste(SPDesktop *desktop, bool in_place, bool on_page) override;
    bool pasteStyle(ObjectSet *set) override;
    bool pasteSize(ObjectSet *set, bool separately, bool apply_x, bool apply_y) override;
    bool pastePathEffect(ObjectSet *set) override;
    Glib::ustring getPathParameter(SPDesktop* desktop) override;
    Glib::ustring getShapeOrTextObjectId(SPDesktop *desktop) override;
    std::vector<Glib::ustring> getElementsOfType(SPDesktop *desktop, gchar const* type = "*", gint maxdepth = -1) override;
    Glib::ustring getFirstObjectID() override;

    ClipboardManagerImpl();

private:
    void _cleanStyle(SPCSSAttr *);
    void _copySelection(ObjectSet *);
    void _copyCompleteStyle(SPItem *item, Inkscape::XML::Node *target, bool child = false);
    void _copyUsedDefs(SPItem *);
    void _copyGradient(SPGradient *);
    void _copyPattern(SPPattern *);
    void _copyHatch(SPHatch *);
    void _copyTextPath(SPTextPath *);
    bool _copyNodes(SPDesktop *desktop, ObjectSet *set);
    Inkscape::XML::Node *_copyNode(Inkscape::XML::Node *, Inkscape::XML::Document *, Inkscape::XML::Node *);
    Inkscape::XML::Node *_copyIgnoreDup(Inkscape::XML::Node *, Inkscape::XML::Document *, Inkscape::XML::Node *);

    bool _pasteImage(SPDocument *doc);
    bool _pasteText(SPDesktop *desktop);
    bool _pasteNodes(SPDesktop *desktop, SPDocument *clipdoc, bool in_place, bool on_page);
    void _applyPathEffect(SPItem *, char const *);
    void _retrieveClipboard(Glib::ustring = "");

    bool _holdsOwnObjects();
    Glib::ustring _ownText();

    // clipboard callbacks
    void _onGet(char const *mime_type, Glib::RefPtr<Gio::OutputStream> const &output, GdkContentSerializer *serializer);

    // various helpers
    void _createInternalClipboard();
    void _discardInternalClipboard();
    Inkscape::XML::Node *_createClipNode();
    Geom::Scale _getScale(SPDesktop *desktop, Geom::Point const &min, Geom::Point const &max, Geom::Rect const &obj_rect, bool apply_x, bool apply_y);
    Glib::ustring _getBestTarget(SPDesktop *desktop = nullptr);
    void _registerSerializers();
    void _setClipboardTargets();
    void _setClipboardColor(Colors::Color const &color);
    void _userWarn(SPDesktop *, char const *);

    // private properties
    std::unique_ptr<SPDocument> _clipboardSPDoc; ///< Document that stores the clipboard until someone requests it
    Inkscape::XML::Node *_defs; ///< Reference to the clipboard document's defs node
    Inkscape::XML::Node *_root; ///< Reference to the clipboard's root node
    Inkscape::XML::Node *_clipnode; ///< The node that holds extra information
    Inkscape::XML::Document *_doc; ///< Reference to the clipboard's Inkscape::XML::Document
    std::set<SPItem*> cloned_elements;
    std::vector<SPCSSAttr*> te_selected_style;
    std::vector<unsigned> te_selected_style_positions;

    // we need a way to copy plain text AND remember its style;
    // the standard _clipnode is only available in an SVG tree, hence this special storage
    SPCSSAttr *_text_style; ///< Style copied along with plain text fragment

    Glib::RefPtr<Gdk::Clipboard> _clipboard; ///< Handle to the system wide clipboard - for convenience

    // For throttling rogue clipboard managers.
    std::optional<std::chrono::steady_clock::time_point> last_req;
};

ClipboardManagerImpl::ClipboardManagerImpl()
    : _defs(nullptr),
      _root(nullptr),
      _clipnode(nullptr),
      _doc(nullptr),
      _text_style(nullptr),
      _clipboard(Gdk::Display::get_default()->get_clipboard())
{
    // Clipboard requests on app termination can cause undesired extension
    // popup windows. Clearing the clipboard can prevent this.
    if (auto application = Gio::Application::get_default()) {
        application->signal_shutdown().connect([this] { _discardInternalClipboard(); });
    }

    _registerSerializers();
}

/**
 * Copy selection contents to the clipboard.
 */
void ClipboardManagerImpl::copy(ObjectSet *set)
{
    if (auto const desktop = set->desktop()) {
        // Special case for when the gradient dragger is active - copies gradient color
        if (auto const drag = desktop->getTool()->get_drag();
            drag && drag->hasSelection())
        {
            Color col = drag->getColor();

            // set the color as clipboard content (text in RRGGBBAA format)
            _setClipboardColor(col);

            // create a style with this color on fill and opacity in master opacity, so it can be
            // pasted on other stops or objects
            if (_text_style) {
                sp_repr_css_attr_unref(_text_style);
                _text_style = nullptr;
            }
            _text_style = sp_repr_css_attr_new();
            // print and set properties
            sp_repr_css_set_property_string(_text_style, "fill", col.toString(false));
            sp_repr_css_set_property_double(_text_style, "opacity", col.getOpacity());

            _discardInternalClipboard();
            return;
        }

        // Special case for when the color picker ("dropper") is active - copies color under cursor
        if (auto const dt = dynamic_cast<Tools::DropperTool const *>(desktop->getTool())) {
            _setClipboardColor(*dt->get_color(false, true));
            _discardInternalClipboard();
            return;
        }

        // Special case for when the text tool is active - if some text is selected, copy plain text,
        // not the object that holds it; also copy the style at cursor into
        if (auto const text_tool = dynamic_cast<Tools::TextTool*>(desktop->getTool())) {
            _discardInternalClipboard();
            _clipboard->set_text(get_selected_text(*text_tool));
            if (_text_style) {
                sp_repr_css_attr_unref(_text_style);
                _text_style = nullptr;
            }
            _text_style = get_style_at_cursor(*text_tool);
            return;
        }

        // Special case for copying part of a path instead of the whole selected object.
        if (_copyNodes(desktop, set)) {
            return;
        }

        if (set->isEmpty()) {  // check whether something is selected
            _userWarn(desktop, "Nothing was copied.");
            return;
        }
    }

    _createInternalClipboard();   // construct a new clipboard document
    _copySelection(set);   // copy all items in the selection to the internal clipboard

    _setClipboardTargets();
}

/**
 * Copy a Live Path Effect path parameter to the clipboard.
 * @param pp The path parameter to store in the clipboard.
 */
void ClipboardManagerImpl::copyPathParameter(Inkscape::LivePathEffect::PathParam *pp)
{
    if (!pp) {
        return;
    }
    SPItem * item = SP_ACTIVE_DESKTOP->getSelection()->singleItem();
    Geom::PathVector pv = pp->get_pathvector();
    if (item != nullptr) {
        pv *= item->i2doc_affine();
    }
    auto svgd = sp_svg_write_path(pv);

    if (svgd.empty()) {
        return;
    }

    _createInternalClipboard();

    Inkscape::XML::Node *pathnode = _doc->createElement("svg:path");
    pathnode->setAttribute("d", svgd);
    _root->appendChild(pathnode);
    Inkscape::GC::release(pathnode);

    fit_canvas_to_drawing(_clipboardSPDoc.get());
    _setClipboardTargets();
}

/**
 * @brief copies a string to the clipboard
 *
 * @param str string to copy
 */
bool ClipboardManagerImpl::copyString(Glib::ustring str) {
    if (!str.empty()) {
        _discardInternalClipboard();
        _clipboard->set_text(str);
        return true;
    }
    return false;
}

/**
 * Copy a symbol from the symbol dialog.
 *
 * @param symbol The Inkscape::XML::Node for the symbol.
 * @param style The style to be applied to the symbol.
 * @param source The source document of the symbol.
 * @param bbox The bounding box of the symbol, in desktop coordinates.
 */
void ClipboardManagerImpl::copySymbol(Inkscape::XML::Node* symbol, gchar const* style, SPDocument *source, const char* symbol_set,
                                      Geom::Rect const &bbox, bool set_clipboard)
{
    if (!symbol)
        return;

    _createInternalClipboard();

    // We add "_duplicate" to have a well defined symbol name that
    // bypasses the "prevent_id_classes" routine. We'll get rid of it
    // when we paste.
    auto original = cast<SPItem>(source->getObjectByRepr(symbol));
    _copyUsedDefs(original);
    Inkscape::XML::Node *repr = symbol->duplicate(_doc);
    Glib::ustring symbol_name;
    // disambiguate symbols from various symbol sets
    if (symbol_set && *symbol_set) {
        symbol_name = symbol_set;
        symbol_name += ":";
        symbol_name = sanitize_id(symbol_name);
    }
    symbol_name += repr->attribute("id");
    symbol_name += "_inkscape_duplicate";
    repr->setAttribute("id", symbol_name);
    _defs->appendChild(repr);
    auto nsymbol = cast<SPSymbol>(_clipboardSPDoc->getObjectById(symbol_name));
    if (nsymbol) {
        _copyCompleteStyle(original, repr, true);
        auto scale = _clipboardSPDoc->getDocumentScale();
        // Convert scale from source to clipboard user units
        nsymbol->scaleChildItemsRec(scale, Geom::Point(0, 0), false);
        if (!nsymbol->title()) {
            nsymbol->setTitle(nsymbol->label() ? nsymbol->label() : nsymbol->getId());
        }
        auto href = Glib::ustring("#") + symbol_name;
        size_t pos = href.find( "_inkscape_duplicate" );
        // while ffix rename id we do this hack
        href.erase( pos );
        Inkscape::XML::Node *use_repr = _doc->createElement("svg:use");
        use_repr->setAttribute("xlink:href", href);
   
        /**
        * If the symbol has a viewBox but no width or height, then take width and
        * height from the viewBox and set them on the use element. Otherwise, the
        * use element will have 100% document width and height!
        */
        {
            auto widthAttr = symbol->attribute("width");
            auto heightAttr = symbol->attribute("height");
            auto viewBoxAttr = symbol->attribute("viewBox");
            if (viewBoxAttr && !(heightAttr || widthAttr)) {
                SPViewBox vb;
                vb.set_viewBox(viewBoxAttr);
                if (vb.viewBox_set) {
                    use_repr->setAttributeSvgDouble("width", vb.viewBox.width());
                    use_repr->setAttributeSvgDouble("height", vb.viewBox.height());
                }
            }
        }
        // Set a default style in <use> rather than <symbol> so it can be changed.
        use_repr->setAttribute("style", style);
        _root->appendChild(use_repr);
        // because a extrange reason on append use getObjectsByElement("symbol") return 2 elements, 
        // it not give errrost by the moment;
        if (auto use = cast<SPUse>(_clipboardSPDoc->getObjectByRepr(use_repr))) {
            Geom::Affine affine = source->getDocumentScale();
            use->doWriteTransform(affine, &affine, false);
        }
        // Set min and max offsets based on the bounding rectangle.
        _clipnode->setAttributePoint("min", bbox.min());
        _clipnode->setAttributePoint("max", bbox.max());
        fit_canvas_to_drawing(_clipboardSPDoc.get());
    }
    if (set_clipboard) {
        _setClipboardTargets();
    }
}

/**
 * Insert a symbol into the document at the prescribed position (at the end of a drag).
 *
 * @param desktop The desktop onto which the symbol has been dropped.
 * @param shift_dt The vector by which the symbol position should be shifted, in desktop coordinates.
 */
void ClipboardManagerImpl::insertSymbol(SPDesktop *desktop, Geom::Point const &shift_dt, bool read_clipboard)
{
    if (!desktop || !Inkscape::have_viable_layer(desktop, desktop->messageStack())) {
        return;
    }
    if (read_clipboard) {
        _retrieveClipboard("text/plain;charset=utf-8");
    }
    auto &symbol = _clipboardSPDoc;
    if (!symbol) {
        return;
    }

    auto *root = symbol->getRoot();

    // Synthesize a clipboard position in order to paste the symbol where it got dropped.
    if (auto *clipnode = sp_repr_lookup_name(root->getRepr(), "inkscape:clipboard", 1)) {
        clipnode->setAttributePoint("min", clipnode->getAttributePoint("min") + shift_dt);
        clipnode->setAttributePoint("max", clipnode->getAttributePoint("max") + shift_dt);
    }

    sp_import_document(desktop, symbol.get(), true);
}

/**
 * Paste from the system clipboard into the active desktop.
 * @param in_place Whether to put the contents where they were when copied.
 */
bool ClipboardManagerImpl::paste(SPDesktop *desktop, bool in_place, bool on_page)
{
    // do any checking whether we really are able to paste before requesting the contents
    if (!desktop) {
        return false;
    }
    if (!Inkscape::have_viable_layer(desktop, desktop->messageStack())) {
        return false;
    }

    // Objects copied in this PROOF, pasted in the Text tool: never read them back as text through
    // GTK. That wrote the whole copy as SVG source to ourselves on this one thread; a small copy
    // pasted SVG markup into the text, a larger one hung PROOF for good (owner, 2026-10-26). A copied
    // text gives its characters at the cursor, as in Illustrator; anything else pastes as objects.
    bool const own_objects = _holdsOwnObjects();
    if (own_objects && !on_page) {
        if (auto text_tool = dynamic_cast<Tools::TextTool *>(desktop->getTool())) {
            if (auto characters = _ownText(); !characters.empty() && text_tool->pasteInline(characters)) {
                return true;
            }
        }
    }

    Glib::ustring target = own_objects ? Glib::ustring(preferred_targets.front()) : _getBestTarget(desktop);
    if constexpr (DEBUG_CLIPBOARD) {
        std::cout << "paste(): Best target: " << target << std::endl;
    }

    // Special cases of clipboard content handling go here
    // Note that target priority is determined in _getBestTarget.
    // TODO: Handle x-special/gnome-copied-files and text/uri-list to support pasting files

    // if there is an image on the clipboard, paste it
    if (!on_page && target == CLIPBOARD_GDK_PIXBUF_TARGET) {
        return _pasteImage(desktop->doc());
    }
    if (!on_page && target == CLIPBOARD_TEXT_TARGET) {
        // It was text, and we did paste it. If not, continue on.
        if (_pasteText(desktop)) {
            return true;
        }
        // If the clipboard contains text/plain, but is an svg document
        // then we'll try and detect it and then paste it if possible.
    }

    _retrieveClipboard(target);
    auto &tempdoc = _clipboardSPDoc;
    if (!tempdoc) {
        if (target == CLIPBOARD_TEXT_TARGET) {
            _userWarn(desktop, _("Can't paste text outside of the text tool."));
            return false;
        } else {
            _userWarn(desktop, _("Nothing on the clipboard."));
            return false;
        }
    }

    if (_pasteNodes(desktop, tempdoc.get(), in_place, on_page)) {
        return true;
    }

    // copy definitions
    sp_import_document(desktop, tempdoc.get(), in_place, on_page);

    // _copySelection() has put all items in groups, now ungroup them (preserves transform
    // relationships of clones, text-on-path, etc.)
    if (target == "image/x-inkscape-svg") {
        SPDocument *doc = nullptr;
        desktop->getSelection()->ungroup(true);
        auto vec2 = desktop->getSelection()->items_vector();
        for (auto item : vec2) {
            // just a bit beauty on paste hidden items unselect
            doc = item->document;
            if (vec2.size() > 1 && item->isHidden()) {
                desktop->getSelection()->remove(item);
            }
            if (auto pasted_lpe_item = cast<SPLPEItem>(item)) {
                remove_hidder_filter(pasted_lpe_item);
            }
        }
        if (doc) {
            doc->update_lpobjs();
        }
    }

    return true;
}

/**
 * Copy any selected nodes and return true if there were nodes.
 */
bool ClipboardManagerImpl::_copyNodes(SPDesktop *desktop, ObjectSet *set)
{
    auto const node_tool = dynamic_cast<Inkscape::UI::Tools::NodeTool *>(desktop->getTool());
    if (!node_tool || !node_tool->_selected_nodes)
        return false;

    SPPath *first_path = nullptr;
    for (auto obj : set->items()) {
        if ((first_path = cast<SPPath>(obj))) {
            break;
        }
    }

    auto builder = new Geom::PathBuilder();
    node_tool->_multipath->copySelectedPath(builder);
    Geom::PathVector pathv = builder->peek();

    // _createInternalClipboard done after copy, as deleting clipboard
    // document may trigger tool switch (as in PathParam::~PathParam)
    _createInternalClipboard();

    // Copy document height so that desktopVisualBounds() is equivalent in the
    // source document and the clipboard.
    _clipboardSPDoc->setWidthAndHeight(desktop->doc()->getWidth(), desktop->doc()->getHeight());

    // Were any nodes actually copied?
    if (pathv.empty() || !first_path)
        return false;

    Inkscape::XML::Node *pathRepr = _doc->createElement("svg:path");

    // pathv is in desktop coordinates
    auto source_scale = first_path->i2dt_affine();
    pathRepr->setAttribute("d", sp_svg_write_path(pathv * source_scale.inverse()));
    pathRepr->setAttributeOrRemoveIfEmpty("transform", first_path->getAttribute("transform"));

    // Group the path to make it consistant with other copy processes
    auto group = _doc->createElement("svg:g");
    _root->appendChild(group);
    Inkscape::GC::release(group);

    // Store the style for paste-as-object operations. Ignored if pasting into an other path.
    pathRepr->setAttribute("style", first_path->style->write(SP_STYLE_FLAG_IFSET) );
    group->appendChild(pathRepr);
    Inkscape::GC::release(pathRepr);

    // Store the parent transformation, and scaling factor of the copied object
    if (auto parent = cast<SPItem>(first_path->parent)) {
          auto transform_str = sp_svg_transform_write(parent->i2doc_affine());
          group->setAttributeOrRemoveIfEmpty("transform", transform_str);
    }

    // Set the translation for paste-in-place operation, must be done after repr appends
    if (auto path_obj = cast<SPPath>(_clipboardSPDoc->getObjectByRepr(pathRepr))) {
        // we could use pathv.boundsFast here, but that box doesn't include stroke width
        // so we must take the value from the visualBox of the new shape instead.
        assert(Geom::are_near(path_obj->document->getDimensions(), first_path->document->getDimensions()));
        auto bbox = *(path_obj->desktopVisualBounds());
        _clipnode->setAttributePoint("min", bbox.min());
        _clipnode->setAttributePoint("max", bbox.max());
    }
    _setClipboardTargets();
    return true;
}

/**
 * Paste nodes into a selected path and return true if it's possible.
 *   if the node tool selected
 *   and one path selected in target
 *   and one path in source
 */
bool ClipboardManagerImpl::_pasteNodes(SPDesktop *desktop, SPDocument *clipdoc, bool in_place, bool on_page)
{
    auto const node_tool = dynamic_cast<Inkscape::UI::Tools::NodeTool *>(desktop->getTool());
    if (!node_tool || desktop->getSelection()->objects().size() != 1)
        return false;

    SPObject *obj = desktop->getSelection()->objects().back();
    auto target_path = cast<SPPath>(obj);
    if (!target_path)
        return false;

    auto const dt_to_target = target_path->dt2i_affine();
    // Select all nodes prior to pasting in, for later inversion.
    node_tool->_selected_nodes->selectAll();

    for (auto node = clipdoc->getReprRoot()->firstChild(); node; node = node->next()) {
        auto source_obj = clipdoc->getObjectByRepr(node);

        // Unpack group that may have a transformation inside it.
        if (auto source_group = cast<SPGroup>(source_obj)) {
            if (source_group->children.size() == 1) {
                source_obj = source_group->firstChild();
            }
        }

        if (auto source_path = cast<SPPath>(source_obj)) {
            auto source_to_target = source_path->i2dt_affine();
            auto source_curve = *source_path->curveForEdit();
            auto target_curve = *target_path->curveForEdit();

            auto bbox = *(source_path->desktopVisualBounds());
            if (!in_place) {
                // Move the source curve to the mouse pointer (desktop coordinates)
                source_to_target *= Geom::Translate((desktop->point() - bbox.midpoint()).round());
            } else if (auto clipnode = sp_repr_lookup_name(clipdoc->getReprRoot(), "inkscape:clipboard", 1)) {
                // Force translation so a foreign path will end up in the right place.
                source_to_target *= Geom::Translate(clipnode->getAttributePoint("min") - bbox.min());
            }

            source_to_target *= dt_to_target;

            // Finally convert the curve into path item's coordinate system
            source_curve *= source_to_target;

            // Add the source curve to the target copy
            pathvector_append(target_curve, std::move(source_curve));

            // Set the attribute to keep the document up to date (fixes undo)
            auto str = sp_svg_write_path(target_curve);
            target_path->setAttribute("d", str);

            if (on_page) {
                g_warning("Node paste on page not Implemented");
            }
        }
    }

    // Finally we invert the selection, this selects all newly added nodes.
    node_tool->_selected_nodes->invertSelection();

    return true;
}

/**
 * Returns the id of the first visible copied object.
 */
Glib::ustring ClipboardManagerImpl::getFirstObjectID()
{
    _retrieveClipboard("image/x-inkscape-svg");
    auto tempdoc = _clipboardSPDoc.get();
    if (!tempdoc) {
        return {};
    }

    Inkscape::XML::Node *root = tempdoc->getReprRoot();

    if (!root) {
        return {};
    }

    Inkscape::XML::Node *ch = root->firstChild();
    Inkscape::XML::Node *child = nullptr;
    // now clipboard is wrapped on copy since 202d57ea fix
    while (ch != nullptr &&
           g_strcmp0(ch->name(), "svg:g") &&
           g_strcmp0(child?child->name():nullptr, "svg:g") &&
           g_strcmp0(child?child->name():nullptr, "svg:path") &&
           g_strcmp0(child?child->name():nullptr, "svg:use") &&
           g_strcmp0(child?child->name():nullptr, "svg:text") &&
           g_strcmp0(child?child->name():nullptr, "svg:image") &&
           g_strcmp0(child?child->name():nullptr, "svg:rect") &&
           g_strcmp0(child?child->name():nullptr, "svg:ellipse") &&
           g_strcmp0(child?child->name():nullptr, "svg:circle")
        ) {
        ch = ch->next();
        child = ch ? ch->firstChild(): nullptr;
    }

    if (child) {
        char const *id = child->attribute("id");
        if (id) {
            return id;
        }
    }

    return {};
}

/**
 * Remove certain css elements which are not useful for pasteStyle
 */
void ClipboardManagerImpl::_cleanStyle(SPCSSAttr *style)
{
    if (style) {
        /* Clean text 'position' properties */
        sp_repr_css_unset_property(style, "text-anchor");
        sp_repr_css_unset_property(style, "shape-inside");
        sp_repr_css_unset_property(style, "shape-subtract");
        sp_repr_css_unset_property(style, "shape-padding");
        sp_repr_css_unset_property(style, "shape-margin");
        sp_repr_css_unset_property(style, "inline-size");
    }
}

/**
 * Implements the Paste Style action.
 */
bool ClipboardManagerImpl::pasteStyle(ObjectSet *set)
{
    auto dt = set->desktop();
    if (!dt) {
        return false;
    }

    // check whether something is selected
    if (set->isEmpty()) {
        _userWarn(set->desktop(), _("Select <b>object(s)</b> to paste style to."));
        return false;
    }

    _retrieveClipboard("image/x-inkscape-svg");
    auto &tempdoc = _clipboardSPDoc;
    if (!tempdoc) {
        // no document, but we can try _text_style
        if (_text_style) {
            _cleanStyle(_text_style);
            sp_desktop_set_style(set, set->desktop(), _text_style);
            return true;
        } else {
            _userWarn(set->desktop(), _("No style on the clipboard."));
            return false;
        }
    }

    auto prefs = Inkscape::Preferences::get();
    auto const copy_computed = prefs->getBool("/options/copycomputedstyle/value", true);

    Inkscape::XML::Node *root = tempdoc->getReprRoot();
    Inkscape::XML::Node *clipnode = sp_repr_lookup_name(root, "inkscape:clipboard", 1);

    if (!clipnode) {
        _userWarn(set->desktop(), _("No style on the clipboard."));
        return false;
    }

    bool pasted = false;

    if (copy_computed) {
        SPCSSAttr *style = sp_repr_css_attr(clipnode, "style");
        sp_desktop_set_style(set, set->desktop(), style);
        pasted = true;
    } else {
        for (auto node : set->xmlNodes()) {
            pasted = node->copyAttribute("class", clipnode, true) || pasted;
            pasted = node->copyAttribute("style", clipnode, true) || pasted;
        }
    }

    if (pasted) {
        // pasted style might depend on defs from the source
        set->document()->importDefs(tempdoc.get());
    }

    return pasted;
}

/**
 * Resize the selection or each object in the selection to match the clipboard's size.
 * @param separately Whether to scale each object in the selection separately
 * @param apply_x Whether to scale the width of objects / selection
 * @param apply_y Whether to scale the height of objects / selection
 */
bool ClipboardManagerImpl::pasteSize(ObjectSet *set, bool separately, bool apply_x, bool apply_y)
{
    if (!apply_x && !apply_y) {
        return false; // pointless parameters
    }

    if (set->isEmpty()) {
        if(set->desktop())
            _userWarn(set->desktop(), _("Select <b>object(s)</b> to paste size to."));
        return false;
    }

    // FIXME: actually, this should accept arbitrary documents
    _retrieveClipboard("image/x-inkscape-svg");
    auto tempdoc = _clipboardSPDoc.get();
    if (!tempdoc) {
        if (set->desktop()) {
            _userWarn(set->desktop(), _("No size on the clipboard."));
        }
        return false;
    }

    // retrieve size information from the clipboard
    Inkscape::XML::Node *root = tempdoc->getReprRoot();
    Inkscape::XML::Node *clipnode = sp_repr_lookup_name(root, "inkscape:clipboard", 1);
    if (!clipnode) return false;

    Geom::Point min, max;
    bool visual_bbox = !Inkscape::Preferences::get()->getInt("/tools/bounding_box");
    min = clipnode->getAttributePoint((visual_bbox ? "min" : "geom-min"), min);
    max = clipnode->getAttributePoint((visual_bbox ? "max" : "geom-max"), max);

    if (separately) {
        // resize each object in the selection
        auto itemlist = set->items();
        for (auto item : itemlist) {
            if (item) {
                Geom::OptRect obj_size = item->desktopPreferredBounds();
                if ( obj_size ) {
                    item->scale_rel(_getScale(set->desktop(), min, max, *obj_size, apply_x, apply_y));
                }
            } else {
                g_assert_not_reached();
            }
        }
    } else {
        // resize the selection as a whole
        Geom::OptRect sel_size = set->preferredBounds();
        if (sel_size) {
            set->scaleRelative(sel_size->midpoint(),
                                         _getScale(set->desktop(), min, max, *sel_size, apply_x, apply_y));
        }
    }

    return true;
}

/**
 * Applies a path effect from the clipboard to the selected path.
 */
bool ClipboardManagerImpl::pastePathEffect(ObjectSet *set)
{
    /** @todo FIXME: pastePathEffect crashes when moving the path with the applied effect,
        segfaulting in fork_private_if_necessary(). */

    if (!set->desktop()) {
        return false;
    }

    if (!set || set->isEmpty()) {
        _userWarn(set->desktop(), _("Select <b>object(s)</b> to paste live path effect to."));
        return false;
    }

    _retrieveClipboard("image/x-inkscape-svg");
    auto &tempdoc = _clipboardSPDoc;
    if (tempdoc) {
        Inkscape::XML::Node *root = tempdoc->getReprRoot();
        Inkscape::XML::Node *clipnode = sp_repr_lookup_name(root, "inkscape:clipboard", 1);
        if ( clipnode ) {
            char const *effectstack = clipnode->attribute("inkscape:path-effect");
            if ( effectstack ) {
                set->document()->importDefs(tempdoc.get());

                // importing defs can adjust node names, so grab the current names again
                effectstack = clipnode->attribute("inkscape:path-effect");

                // make sure all selected items are converted to paths first (i.e. rectangles)
                set->toLPEItems();
                auto itemlist= set->items();
                for(auto item : itemlist){
                    _applyPathEffect(item, effectstack);
                    item->doWriteTransform(item->transform);
                }

                return true;
            }
        }
    }

    // no_effect:
    _userWarn(set->desktop(), _("No effect on the clipboard."));
    return false;
}

/**
 * Get LPE path data from the clipboard.
 * @return The retrieved path data (contents of the d attribute), or "" if no path was found
 */
Glib::ustring ClipboardManagerImpl::getPathParameter(SPDesktop* desktop)
{
    _retrieveClipboard(); // any target will do here
    auto doc = _clipboardSPDoc.get();
    if (!doc) {
        _userWarn(desktop, _("Nothing on the clipboard."));
        return "";
    }

    // unlimited search depth
    auto repr = sp_repr_lookup_name(doc->getReprRoot(), "svg:path", -1);
    auto item = cast<SPItem>(doc->getObjectByRepr(repr));

    if (!item) {
        _userWarn(desktop, _("Clipboard does not contain a path."));
        return "";
    }

    // Adjust any copied path into the target document transform.
    auto tr_p = item->i2doc_affine();
    auto tr_s = doc->getDocumentScale().inverse();
    auto pathv = sp_svg_read_pathv(repr->attribute("d"));
    return sp_svg_write_path(pathv * tr_s * tr_p);
}

/**
 * Get object id of a shape or text item from the clipboard.
 * @return The retrieved id string (contents of the id attribute), or "" if no shape or text item was found.
 */
Glib::ustring ClipboardManagerImpl::getShapeOrTextObjectId(SPDesktop *desktop)
{
    // https://bugs.launchpad.net/inkscape/+bug/1293979
    // basically, when we do a depth-first search, we're stopping
    // at the first object to be <svg:path> or <svg:text>.
    // but that could then return the id of the object's
    // clip path or mask, not the original path!

    _retrieveClipboard(); // any target will do here
    auto tempdoc = _clipboardSPDoc.get();
    if (!tempdoc) {
        _userWarn(desktop, _("Nothing on the clipboard."));
        return "";
    }
    Inkscape::XML::Node *root = tempdoc->getReprRoot();

    // 1293979: strip out the defs of the document
    root->removeChild(tempdoc->getDefs()->getRepr());

    Inkscape::XML::Node *repr = sp_repr_lookup_name(root, "svg:path", -1); // unlimited search depth
    if (!repr) {
        repr = sp_repr_lookup_name(root, "svg:text", -1);
    }
    if (!repr) {
        repr = sp_repr_lookup_name(root, "svg:ellipse", -1);
    }
    if (!repr) {
        repr = sp_repr_lookup_name(root, "svg:rect", -1);
    }
    if (!repr) {
        repr = sp_repr_lookup_name(root, "svg:circle", -1);
    }

    if (!repr) {
        _userWarn(desktop, _("Clipboard does not contain a path."));
        return "";
    }

    auto svgd = repr->attribute("id");
    return svgd ? svgd : "";
}

/**
 * Get all objects id  from the clipboard.
 * @return A vector containing all IDs or empty if no shape or text item was found.
 * type. Set to "*" to retrieve all elements of the types vector inside, feel free to populate more
 */
std::vector<Glib::ustring> ClipboardManagerImpl::getElementsOfType(SPDesktop *desktop, gchar const* type, gint maxdepth)
{
    _retrieveClipboard(); // any target will do here
    auto tempdoc = _clipboardSPDoc.get();
    if (!tempdoc) {
        _userWarn(desktop, _("Nothing on the clipboard."));
        return {};
    }
    Inkscape::XML::Node *root = tempdoc->getReprRoot();

    // 1293979: strip out the defs of the document
    if (auto repr = tempdoc->getDefs()->getRepr()) {
        root->removeChild(repr);
    }
    std::vector<Inkscape::XML::Node const *> reprs;
    if (strcmp(type, "*") == 0){
        //TODO:Fill vector with all possible elements
        std::vector<Glib::ustring> types;
        types.push_back((Glib::ustring)"svg:path");
        types.push_back((Glib::ustring)"svg:circle");
        types.push_back((Glib::ustring)"svg:rect");
        types.push_back((Glib::ustring)"svg:ellipse");
        types.push_back((Glib::ustring)"svg:text");
        types.push_back((Glib::ustring)"svg:use");
        types.push_back((Glib::ustring)"svg:g");
        types.push_back((Glib::ustring)"svg:image");
        for (auto type_elem : types) {
            std::vector<Inkscape::XML::Node const *> reprs_found = sp_repr_lookup_name_many(root, type_elem.c_str(), maxdepth); // unlimited search depth
            reprs.insert(reprs.end(), reprs_found.begin(), reprs_found.end());
        }
    } else {
        reprs = sp_repr_lookup_name_many(root, type, maxdepth);
    }

    std::vector<Glib::ustring> result;
    for (auto node : reprs) {
        result.emplace_back(node->attribute("id"));
    }

    if (result.empty()) {
        _userWarn(desktop, (Glib::ustring::compose(_("Clipboard does not contain any objects of type \"%1\"."), type)).c_str());
        return {};
    }

    return result;
}

/**
 * Iterate over a list of items and copy them to the clipboard.
 */
void ClipboardManagerImpl::_copySelection(ObjectSet *selection)
{
    auto prefs = Preferences::get();
    auto const copy_computed = prefs->getBool("/options/copycomputedstyle/value", true);
    SPPage *page = nullptr;

    // copy the defs used by all items
    auto itemlist = selection->items();
    cloned_elements.clear();
    std::vector<SPItem *> items(itemlist.begin(), itemlist.end());
    for (auto item : itemlist) {
        if (!page) {
            page = item->document->getPageManager().getPageFor(item, false);
        }
        auto lpeitem = cast<SPLPEItem>(item);
        if (lpeitem) {
            for (auto satellite : lpeitem->get_satellites(false, true)) {
                if (satellite) {
                    auto item2 = cast<SPItem>(satellite);
                    if (item2 && std::find(items.begin(), items.end(), item2) == items.end()) {
                        items.push_back(item2);
                    }
                }
            }
        }
    }
    cloned_elements.clear();
    for (auto item : items) {
        if (item) {
            _copyUsedDefs(item);
        } else {
            g_assert_not_reached();
        }
    }

    // copy the representation of the items
    std::vector<SPObject *> sorted_items(items.begin(), items.end());
    {
        // Get external text references and add them to sorted_items
        auto ext_refs = text_categorize_refs(selection->document(),
                sorted_items.begin(), sorted_items.end(),
                TEXT_REF_EXTERNAL);
        for (auto const &ext_ref : ext_refs) {
            sorted_items.push_back(selection->document()->getObjectById(ext_ref.first));
        }
    }
    sort(sorted_items.begin(), sorted_items.end(), sp_object_compare_position_bool);

    //remove already copied elements from cloned_elements
    std::vector<SPItem*>tr;
    for(auto cloned_element : cloned_elements){
        if(std::find(sorted_items.begin(),sorted_items.end(),cloned_element)!=sorted_items.end())
            tr.push_back(cloned_element);
    }
    for(auto & it : tr){
        cloned_elements.erase(it);
    }

    // One group per shared parent
    std::map<SPObject const *, Inkscape::XML::Node *> groups;

    sorted_items.insert(sorted_items.end(),cloned_elements.begin(),cloned_elements.end());
    for(auto sorted_item : sorted_items){
        auto item = cast<SPItem>(sorted_item);
        if (item) {
            // Create a group with the parent transform. This group will be ungrouped when pasting
            // und takes care of transform relationships of clones, text-on-path, etc.
            auto &group = groups[item->parent];
            if (!group) {
                group = _doc->createElement("svg:g");
                group->setAttribute("id", item->parent->getId()); // avoid getting a clashing id
                _root->appendChild(group);
                Inkscape::GC::release(group);

                if (auto parent = cast<SPItem>(item->parent)) {
                    auto transform_str = sp_svg_transform_write(parent->i2doc_affine());
                    group->setAttributeOrRemoveIfEmpty("transform", transform_str);
                }
            }

            Inkscape::XML::Node *obj = item->getRepr();
            Inkscape::XML::Node *obj_copy;
            if(cloned_elements.find(item)==cloned_elements.end())
                obj_copy = _copyNode(obj, _doc, group);
            else
                obj_copy = _copyNode(obj, _doc, _clipnode);

            if (copy_computed) {
                // copy complete inherited style
                _copyCompleteStyle(item, obj_copy);
            }
        }
    }
    // copy style for Paste Style action
    if (auto item = selection->singleItem()) {
        if (copy_computed) {
            SPCSSAttr *style = take_style_from_item(item);
            _cleanStyle(style);
            sp_repr_css_set(_clipnode, style, "style");
            sp_repr_css_attr_unref(style);
        } else {
            _clipnode->copyAttribute("class", item->getRepr(), true);
            _clipnode->copyAttribute("style", item->getRepr(), true);
        }

        // copy path effect from the first path
        if (gchar const *effect = item->getRepr()->attribute("inkscape:path-effect")) {
            _clipnode->setAttribute("inkscape:path-effect", effect);
        }
    }

    if (Geom::OptRect size = selection->visualBounds()) {
        _clipnode->setAttributePoint("min", size->min());
        _clipnode->setAttributePoint("max", size->max());
    }
    if (Geom::OptRect geom_size = selection->geometricBounds()) {
        _clipnode->setAttributePoint("geom-min", geom_size->min());
        _clipnode->setAttributePoint("geom-max", geom_size->max());
    }
    if (page) {
        auto page_rect = page->getDesktopRect();
        _clipnode->setAttributePoint("page-min", page_rect.min());
        _clipnode->setAttributePoint("page-max", page_rect.max());
    }
    // Preferably set bounds based on original doc.
    // Some of the objects like <use> referring to objects which are not part of selection don't have proper bounds
    // at this stage.
    if (Geom::OptRect bounds = selection->documentBounds(SPItem::VISUAL_BBOX)) {
        _clipboardSPDoc->fitToRect(bounds.value());
    } else {
        fit_canvas_to_drawing(_clipboardSPDoc.get());
    }
}

/**
 * Copies the style from the stylesheet to preserve it.
 *
 * @param item - The source item (connected to it's document)
 * @param target - The target xml node to store the style in.
 * @param child - Flag to indicate a recursive call, do not use.
 */
void ClipboardManagerImpl::_copyCompleteStyle(SPItem *item, Inkscape::XML::Node *target, bool child)
{
    auto source = item->getRepr();
    SPCSSAttr *css;
    if (child) {
        // Child styles shouldn't copy their parent's existing cascaded style.
        css = sp_repr_css_attr(source, "style");
    } else {
        css = sp_repr_css_attr_inherited(source, "style");
    }
    for (auto iter : item->style->properties()) {
        if (iter->style_src == SPStyleSrc::STYLE_SHEET) {
            css->setAttributeOrRemoveIfEmpty(iter->name(), iter->get_value());
        }
    }
    sp_repr_css_set(target, css, "style");
    sp_repr_css_attr_unref(css);

    if (is<SPGroup>(item)) {
        // Recursively go through chldren too
        auto source_child = source->firstChild();
        auto target_child = target->firstChild();
        while (source_child && target_child) {
            if (auto child_item = cast<SPItem>(item->document->getObjectByRepr(source_child))) {
                _copyCompleteStyle(child_item, target_child, true);
            }
            source_child = source_child->next();
            target_child = target_child->next();
        }
    }
}

/**
 * Recursively copy all the definitions used by a given item to the clipboard defs.
 */
void ClipboardManagerImpl::_copyUsedDefs(SPItem *item)
{
    bool recurse = true;

    if (auto use = cast<SPUse>(item)) {
        if (auto original = use->get_original()) {
            if (original->document != use->document) {
                recurse = false;
            } else {
                cloned_elements.insert(original);
            }
        }
    }

    // copy fill and stroke styles (patterns and gradients)
    SPStyle *style = item->style;

    if (style && (style->fill.isPaintserver())) {
        SPPaintServer *server = item->style->getFillPaintServer();
        if (is<SPLinearGradient>(server) || is<SPRadialGradient>(server) || is<SPMeshGradient>(server) ) {
            _copyGradient(cast<SPGradient>(server));
        }
        auto pattern = cast<SPPattern>(server);
        if (pattern) {
            _copyPattern(pattern);
        }
        auto hatch = cast<SPHatch>(server);
        if (hatch) {
            _copyHatch(hatch);
        }
    }
    if (style && (style->stroke.isPaintserver())) {
        SPPaintServer *server = item->style->getStrokePaintServer();
        if (is<SPLinearGradient>(server) || is<SPRadialGradient>(server) || is<SPMeshGradient>(server) ) {
            _copyGradient(cast<SPGradient>(server));
        }
        auto pattern = cast<SPPattern>(server);
        if (pattern) {
            _copyPattern(pattern);
        }
        auto hatch = cast<SPHatch>(server);
        if (hatch) {
            _copyHatch(hatch);
        }
    }

    // For shapes, copy all of the shape's markers
    auto shape = cast<SPShape>(item);
    if (shape) {
        for (auto & i : shape->_marker) {
            if (i) {
                _copyNode(i->getRepr(), _doc, _defs);
            }
        }
    }

    // For 3D boxes, copy perspectives
    if (auto box = cast<SPBox3D>(item)) {
        if (auto perspective = box->get_perspective()) {
            _copyNode(perspective->getRepr(), _doc, _defs);
        }
    }

    // Copy text paths
    {
        auto text = cast<SPText>(item);
        SPTextPath *textpath = text ? cast<SPTextPath>(text->firstChild()) : nullptr;
        if (textpath) {
            _copyTextPath(textpath);
        }
        if (text) {
            for (auto &&shape_prop_ptr : {
                    reinterpret_cast<SPIShapes SPStyle::*>(&SPStyle::shape_inside),
                    reinterpret_cast<SPIShapes SPStyle::*>(&SPStyle::shape_subtract) }) {
                for (auto *href : (text->style->*shape_prop_ptr).hrefs) {
                    auto shape_obj = href->getObject();
                    if (!shape_obj)
                        continue;
                    auto shape_repr = shape_obj->getRepr();
                    if (sp_repr_is_def(shape_repr)) {
                        _copyIgnoreDup(shape_repr, _doc, _defs);
                    }
                }
            }
        }
    }

    // Copy clipping objects
    if (SPObject *clip = item->getClipObject()) {
        _copyNode(clip->getRepr(), _doc, _defs);
        // recurse
        for (auto &o : clip->children) {
            if (auto childItem = cast<SPItem>(&o)) {
                _copyUsedDefs(childItem);
            }
        }
    }
    // Copy mask objects
    if (SPObject *mask = item->getMaskObject()) {
            _copyNode(mask->getRepr(), _doc, _defs);
            // recurse into the mask for its gradients etc.
            for(auto& o: mask->children) {
                auto childItem = cast<SPItem>(&o);
                if (childItem) {
                    _copyUsedDefs(childItem);
                }
            }
    }

    // Copy filters
    if (style->getFilter()) {
        SPObject *filter = style->getFilter();
        if (is<SPFilter>(filter)) {
            _copyNode(filter->getRepr(), _doc, _defs);
        }
    }

    // For lpe items, copy lpe stack if applicable
    auto lpeitem = cast<SPLPEItem>(item);
    if (lpeitem) {
        if (lpeitem->hasPathEffect()) {
            PathEffectList path_effect_list( *lpeitem->path_effect_list);
            for (auto &lperef : path_effect_list) {
                LivePathEffectObject *lpeobj = lperef->lpeobject;
                if (lpeobj) {
                  _copyNode(lpeobj->getRepr(), _doc, _defs);
                }
            }
        }
    }

    if (!recurse) {
        return;
    }

    // recurse
    for(auto& o: item->children) {
        auto childItem = cast<SPItem>(&o);
        if (childItem) {
            _copyUsedDefs(childItem);
        }
    }
}

/**
 * Copy a single gradient to the clipboard's defs element.
 */
void ClipboardManagerImpl::_copyGradient(SPGradient *gradient)
{
    while (gradient) {
        // Climb up the refs, copying each one in the chain.
        // Ignore dups here as gradients can point at each other and we want to preserve that structure and to also not
        // break links from shape styles into these gradients.
        _copyIgnoreDup(gradient->getRepr(), _doc, _defs);
        if (gradient->ref) {
            gradient = gradient->ref->getObject();
        }
        else {
            gradient = nullptr;
        }
    }
}

/**
 * Copy a single pattern to the clipboard document's defs element.
 */
void ClipboardManagerImpl::_copyPattern(SPPattern *pattern)
{
    // climb up the references, copying each one in the chain
    while (pattern) {
        _copyNode(pattern->getRepr(), _doc, _defs);

        // items in the pattern may also use gradients and other patterns, so recurse
        for (auto& child: pattern->children) {
            auto childItem = cast<SPItem>(&child);
            if (childItem) {
                _copyUsedDefs(childItem);
            }
        }
        pattern = pattern->ref.getObject();
    }
}

/**
 * Copy a single hatch to the clipboard document's defs element.
 */
void ClipboardManagerImpl::_copyHatch(SPHatch *hatch)
{
    // climb up the references, copying each one in the chain
    while (hatch) {
        _copyNode(hatch->getRepr(), _doc, _defs);

        for (auto &child : hatch->children) {
            auto childItem = cast<SPItem>(&child);
            if (childItem) {
                _copyUsedDefs(childItem);
            }
        }
        hatch = hatch->ref.getObject();
    }
}

/**
 * Copy a text path to the clipboard's defs element.
 */
void ClipboardManagerImpl::_copyTextPath(SPTextPath *tp)
{
    SPItem *path = sp_textpath_get_path_item(tp);
    if (!path) {
        return;
    }
    // textpaths that aren't in defs (on the canvas) shouldn't be copied because if
    // both objects are being copied already, this ends up stealing the refs id.
    if(path->parent && is<SPDefs>(path->parent)) {
        _copyIgnoreDup(path->getRepr(), _doc, _defs);
    }
}

/**
 * Copy a single XML node from one document to another.
 * @param node The node to be copied
 * @param target_doc The document to which the node is to be copied
 * @param parent The node in the target document which will become the parent of the copied node
 * @return Pointer to the copied node
 */
Inkscape::XML::Node *ClipboardManagerImpl::_copyNode(Inkscape::XML::Node *node, Inkscape::XML::Document *target_doc, Inkscape::XML::Node *parent)
{
    Inkscape::XML::Node *dup = node->duplicate(target_doc);
    parent->appendChild(dup);
    Inkscape::GC::release(dup);
    return dup;
}

Inkscape::XML::Node *ClipboardManagerImpl::_copyIgnoreDup(Inkscape::XML::Node *node, Inkscape::XML::Document *target_doc, Inkscape::XML::Node *parent)
{
    if (sp_repr_lookup_descendant(_root, "id", node->attribute("id"))) {
        // node already copied
        return nullptr;
    }
    return _copyNode(node, target_doc, parent);
}

/**
 * Retrieve a bitmap image from the clipboard and paste it into the active document.
 */
bool ClipboardManagerImpl::_pasteImage(SPDocument *doc)
{
    if (!doc) {
        return false;
    }

    // retrieve image data
    Glib::RefPtr<Gio::AsyncResult> result;
    _clipboard->read_texture_async([&] (auto &res) { result = res; });
    pump_until([&] { return result; });

    Glib::RefPtr<Gdk::Texture> img;
    try {
        img = _clipboard->read_texture_finish(result);
    } catch (Glib::Error const &err) {
        std::cout << "Pasting image failed: " << err.what() << std::endl;
        return false;
    }

    if (!img || !doc) {
        return false;
    }

    auto const filename = get_tmp_filename("inkscape-clipboard-import");
    img->save_to_png(filename);

    auto prefs = Preferences::get();
    auto attr_saved = prefs->getString("/dialogs/import/link");
    bool ask_saved = prefs->getBool("/dialogs/import/ask");
    auto mode_saved = prefs->getString("/dialogs/import/import_mode_svg");
    prefs->setString("/dialogs/import/link", "embed");
    prefs->setBool("/dialogs/import/ask", false);
    prefs->setString("/dialogs/import/import_mode_svg", "embed");

    auto png = Extension::Input::find_by_mime("image/png");
    png->set_gui(false);
    file_import(doc, filename, png);

    prefs->setString("/dialogs/import/link", attr_saved);
    prefs->setBool("/dialogs/import/ask", ask_saved);
    prefs->setString("/dialogs/import/import_mode_svg", mode_saved);
    png->set_gui(true);

    unlink(filename.c_str());

    return true;
}

/**
 * Paste text into the selected text object or create a new one to hold it.
 */
bool ClipboardManagerImpl::_pasteText(SPDesktop *desktop)
{
    if (!desktop) {
        return false;
    }

    Glib::RefPtr<Gio::AsyncResult> result;
    _clipboard->read_text_async([&] (auto &res) { result = res; });
    pump_until([&] { return result; });

    // Parse the clipboard text as if it was a color string.
    Glib::ustring clip_text;
    try {
        clip_text = _clipboard->read_text_finish(result);
    } catch (Glib::Error const &err) {
        std::cout << "Pasting text failed: " << err.what() << std::endl;
        return false;
    }

    // retrieve text data
    // if the text editing tool is active, paste the text into the active text object
    if (auto text_tool = dynamic_cast<Tools::TextTool *>(desktop->getTool())) {
        return text_tool->pasteInline(clip_text);
    }

    if (clip_text.length() < 30) {
        // Zero makes it impossible to paste a 100% transparent black, but it's useful.
        if (auto color = Colors::Color::parse(clip_text)) {
            auto color_css = sp_repr_css_attr_new();
            sp_repr_css_set_property_string(color_css, "fill", color->toString(false));
            sp_repr_css_set_property_double(color_css, "fill-opacity", color->getOpacity());
            sp_desktop_set_style(desktop, color_css);
            sp_repr_css_attr_unref(color_css);
            return true;
        }
    }

    return false;
}

/**
 * Applies a pasted path effect to a given item.
 */
void ClipboardManagerImpl::_applyPathEffect(SPItem *item, char const *effectstack)
{
    if (!item) {
        return;
    }

    auto lpeitem = cast<SPLPEItem>(item);
    if (lpeitem && effectstack) {
        std::istringstream iss(effectstack);
        std::string href;
        while (std::getline(iss, href, ';'))
        {
            SPObject *obj = sp_uri_reference_resolve(item->document, href.c_str());
            if (!obj) {
                return;
            }
            auto lpeobj = cast<LivePathEffectObject>(obj);
            if (lpeobj) {
                Inkscape::LivePathEffect::LPESpiro *spiroto = dynamic_cast<Inkscape::LivePathEffect::LPESpiro *>(lpeobj->get_lpe());
                bool has_spiro = lpeitem->hasPathEffectOfType(Inkscape::LivePathEffect::SPIRO);
                Inkscape::LivePathEffect::LPEBSpline *bsplineto = dynamic_cast<Inkscape::LivePathEffect::LPEBSpline *>(lpeobj->get_lpe());
                bool has_bspline = lpeitem->hasPathEffectOfType(Inkscape::LivePathEffect::BSPLINE);
                if ((!spiroto || !has_spiro) && (!bsplineto || !has_bspline)) {
                    lpeitem->addPathEffect(lpeobj);
                }
            }
        }
        // for each effect in the stack, check if we need to fork it before adding it to the item
        lpeitem->forkPathEffectsIfNecessary(1);
    }
}

/**
 * Retrieve the clipboard contents as a document.
 */
/**
 * Whether the clipboard holds objects copied in this PROOF (its internal clipboard document).
 */
bool ClipboardManagerImpl::_holdsOwnObjects()
{
    if (!_clipboard->is_local() || !_clipboardSPDoc) {
        return false;
    }
    auto const content = _clipboard->get_content();
    return content && GlibValue::from_content_provider<ClipboardSvg>(*content);
}

/**
 * The characters of the texts in the internal clipboard document, one text a line.
 */
Glib::ustring ClipboardManagerImpl::_ownText()
{
    Glib::ustring result;
    auto gather = [&](auto &self, SPObject &object) -> void {
        for (auto &child : object.children) {
            if (is<SPText>(&child) || is<SPFlowtext>(&child)) {
                if (!result.empty()) {
                    result += '\n';
                }
                result += sp_te_get_string_multiline(cast<SPItem>(&child));
            } else if (is<SPItem>(&child)) {
                self(self, child); // groups; definitions aren't items
            }
        }
    };
    gather(gather, *_clipboardSPDoc->getRoot());
    return result;
}

void ClipboardManagerImpl::_retrieveClipboard(Glib::ustring best_target)
{
    if (_clipboard->is_local()) {
        auto const content = _clipboard->get_content();
        if (!content) {
            _discardInternalClipboard();
        }

        if (!GlibValue::from_content_provider<ClipboardSvg>(*content)) {
            _discardInternalClipboard();
        }

        // Nothing needs to be done, just use existing clipboard document.
        return;
    }

    _discardInternalClipboard();

    Glib::RefPtr<Gio::AsyncResult> result;
    _clipboard->read_async({best_target}, 0, [&] (auto &res) { result = res; });
    pump_until([&] { return result; });

    if (best_target.empty()) {
        best_target = _getBestTarget();
    }

    if (best_target.empty()) {
        return;
    }

    // FIXME: Temporary hack until we add memory input.
    // Save the clipboard contents to some file, then read it
    auto const filename = get_tmp_filename("inkscape-clipboard-import");

    bool file_saved = false;
    Glib::ustring target = best_target;

#ifdef _WIN32
    if (best_target == "CF_ENHMETAFILE" || best_target == "WCF_ENHMETAFILE") {
        // Try to save clipboard data as en emf file (using win32 API).
        // Fixme: Untested with GTK4.
        if (OpenClipboard(NULL)) {
            HGLOBAL hglb = GetClipboardData(CF_ENHMETAFILE);
            if (hglb) {
                HENHMETAFILE hemf = CopyEnhMetaFile((HENHMETAFILE) hglb, filename.c_str());
                if (hemf) {
                    file_saved = true;
                    target = "image/x-emf";
                    DeleteEnhMetaFile(hemf);
                }
            }
            CloseClipboard();
        }
    }
#endif

    if (!file_saved) {
        Glib::RefPtr<Gio::InputStream> data;
        try {
            data = _clipboard->read_finish(result, best_target);
        } catch (Glib::Error const &err) {
            std::cout << "Pasting failed: " << best_target << ' ' << err.what() << std::endl;
            return;
        }

        auto file = Gio::File::create_for_path(filename);
        auto out = file->replace();

        bool done = false;
        out->splice_async(data, [&] (auto &result) {
            out->splice_finish(result);
            done = true;
        });
        pump_until([&] { return done; });
    }

    auto delete_file = scope_exit([&] { unlink(filename.c_str()); });

    // there is no specific plain SVG input extension, so if we can paste the Inkscape SVG format,
    // we use the image/svg+xml mimetype to look up the input extension
    if (target == "image/x-inkscape-svg" || target == "text/plain") {
        target = "image/svg+xml";
    }
    // Use the EMF extension to import metafiles
    if (target == "CF_ENHMETAFILE" || target == "WCF_ENHMETAFILE") {
        target = "image/x-emf";
    }

    Extension::DB::InputList inlist;
    Extension::db.get_input_list(inlist);
    auto in = inlist.begin();
    for (; in != inlist.end() && target != (*in)->get_mimetype(); ++in) {
    };
    if (in == inlist.end()) {
        return;
    }

    try {
        _clipboardSPDoc = (*in)->open(filename.c_str());
    } catch (...) {
    }
}

/**
 * Callback called when some other application requests data from Inkscape.
 *
 * Finds a suitable output extension to save the internal clipboard document,
 * then saves it to memory and sets the clipboard contents.
 */
void ClipboardManagerImpl::_onGet(char const *mime_type, Glib::RefPtr<Gio::OutputStream> const &output, GdkContentSerializer *serializer)
{
    // Every path answers GTK exactly once: here, or when the copy into its stream has finished.
    bool answered_later = false;
    auto answer = scope_exit([&] { if (!answered_later) gdk_content_serializer_return_success(serializer); });

    if (!_clipboardSPDoc) {
        return;
    }

    Glib::ustring target = mime_type;
    g_info("Clipboard _onGet target: %s", target.c_str());

    if (target == "") {
        return; // this shouldn't happen
    }

    if (target == CLIPBOARD_TEXT_TARGET) {
        target = "image/x-inkscape-svg";
    }

#ifdef __APPLE__
    // translate UTI back to MIME
    if (auto mime = mime_uti.right.find(target); mime != mime_uti.right.end()) {
        target = mime->get_left();
    }
#endif

    // Refuse to return anything other than svg/text/png if being inundated with requests from a rogue clipboard manager.
    if (last_req) {
        constexpr auto magic_timeout = std::chrono::milliseconds{100};
        if (std::chrono::steady_clock::now() - *last_req < magic_timeout) {
            // Unbroken chain of rapid clipboard requests since _setClipboardTargets().
            last_req = std::chrono::steady_clock::now();
            if (target != "image/svg+xml" && target != "image/x-inkscape-svg" && target != "image/png") {
                std::cerr << "Denied clipboard request: " << mime_type << std::endl;
                return;
            }
        } else {
            // Chain has ended.
            last_req.reset();
        }
    }

    // FIXME: Temporary hack until we add support for memory output.
    // Save to a temporary file, read it back and then set the clipboard contents
    auto const filename = get_tmp_filename("inkscape-clipboard-export");

    // XXX This is a crude fix for clipboards accessing extensions
    // Remove when gui is extracted from extension execute and uses exceptions.
    bool previous_gui = INKSCAPE.use_gui();
    INKSCAPE.use_gui(false);

    try {
        Extension::DB::OutputList outlist;
        Extension::db.get_output_list(outlist);
        auto out = outlist.begin();
        for ( ; out != outlist.end() && target != (*out)->get_mimetype(); ++out) {
        }
        if (!(*out)->loaded()) {
            // Need to load the extension.
            (*out)->set_state(Inkscape::Extension::Extension::STATE_LOADED);
        }

        if ((*out)->is_raster()) {
            double dpi = Inkscape::Util::Quantity::convert(1, "in", "px");
            Inkscape::Colors::Color bgcolor{0x00000000};

            auto origin = Geom::Point(_clipboardSPDoc->getRoot()->x.computed, _clipboardSPDoc->getRoot()->y.computed);
            auto area = Geom::Rect(origin, origin + _clipboardSPDoc->getDimensions());

            auto width  = static_cast<unsigned long>(area.width() + 0.5);
            auto height = static_cast<unsigned long>(area.height() + 0.5);

            // read from namedview
            auto const raster_file = Glib::filename_to_utf8(get_tmp_filename("inkscape-clipboard-export-raster"));
            sp_export_png_file(_clipboardSPDoc.get(), raster_file.c_str(), area, width, height, dpi, dpi, bgcolor, nullptr, nullptr, true, {});
            (*out)->export_raster(_clipboardSPDoc.get(), raster_file.c_str(), filename.c_str(), true);
            unlink(raster_file.c_str());
        } else {
            (*out)->save(_clipboardSPDoc.get(), filename.c_str(), true);
        }

        auto file = Gio::File::create_for_path(filename);
        auto in = file->read();

        // No waiting here in a loop of our own: when PROOF reads its own clipboard, the reader runs
        // on this thread, and a copy larger than GTK's pipe never finished (the owner's PROOF hung
        // in a paste, 2026-10-26). GTK takes the answer when the copy is done.
        output->splice_async(in, [output, file, filename, serializer] (auto &result) {
            try {
                output->splice_finish(result);
                gdk_content_serializer_return_success(serializer);
            } catch (Glib::Error const &error) {
                gdk_content_serializer_return_error(serializer, g_error_copy(error.gobj()));
            }
            try { file->remove(); } catch (...) {}
            unlink(filename.c_str());
        }, Gio::OutputStream::SpliceFlags::CLOSE_SOURCE);
        answered_later = true;
    } catch (...) {
    }

    INKSCAPE.use_gui(previous_gui);
    if (!answered_later) {
        unlink(filename.c_str()); // delete the temporary file
    }

    if (last_req) {
        last_req = std::chrono::steady_clock::now();
    }
}

/**
 * Creates an internal clipboard document from scratch.
 */
void ClipboardManagerImpl::_createInternalClipboard()
{
    _clipboardSPDoc = SPDocument::createNewDoc(nullptr, true);
    assert(_clipboardSPDoc);
    _defs = _clipboardSPDoc->getDefs()->getRepr();
    _doc = _clipboardSPDoc->getReprDoc();
    _root = _clipboardSPDoc->getReprRoot();

    // Preserve ANY copied text kerning
    _root->setAttribute("xml:space", "preserve");

    if (SP_ACTIVE_DOCUMENT) {
        _clipboardSPDoc->setDocumentBase(SP_ACTIVE_DOCUMENT->getDocumentBase());
    }

    _clipnode = _doc->createElement("inkscape:clipboard");
    _root->appendChild(_clipnode);
    Inkscape::GC::release(_clipnode);

    // once we create a SVG document, style will be stored in it, so flush _text_style
    if (_text_style) {
        sp_repr_css_attr_unref(_text_style);
        _text_style = nullptr;
    }
}

/**
 * Deletes the internal clipboard document.
 */
void ClipboardManagerImpl::_discardInternalClipboard()
{
    if (_clipboardSPDoc) {
        _clipboardSPDoc.reset();
        _defs = nullptr;
        _doc = nullptr;
        _root = nullptr;
        _clipnode = nullptr;
    }
}

/**
 * Get the scale to resize an item, based on the command and desktop state.
 */
Geom::Scale ClipboardManagerImpl::_getScale(SPDesktop *desktop, Geom::Point const &min, Geom::Point const &max, Geom::Rect const &obj_rect, bool apply_x, bool apply_y)
{
    double scale_x = 1.0;
    double scale_y = 1.0;

    if (apply_x) {
        scale_x = (max[Geom::X] - min[Geom::X]) / obj_rect[Geom::X].extent();
    }
    if (apply_y) {
        scale_y = (max[Geom::Y] - min[Geom::Y]) / obj_rect[Geom::Y].extent();
    }
    // If the "lock aspect ratio" button is pressed and we paste only a single coordinate,
    // resize the second one by the same ratio too
    if (desktop && Inkscape::Preferences::get()->getBool("/tools/select/lock_aspect_ratio", false)) {
        if (apply_x && !apply_y) {
            scale_y = scale_x;
        }
        if (apply_y && !apply_x) {
            scale_x = scale_y;
        }
    }

    return Geom::Scale(scale_x, scale_y);
}

/**
 * Find the most suitable clipboard target.
 */
Glib::ustring ClipboardManagerImpl::_getBestTarget(SPDesktop *desktop)
{
    auto formats = _clipboard->get_formats();

    if constexpr (DEBUG_CLIPBOARD) {
        std::cout << "_getBestTarget(): Clipboard formats: " << formats->to_string() << std::endl;
    }

    // Prioritise text when the text tool is active
    if (desktop && dynamic_cast<Inkscape::UI::Tools::TextTool *>(desktop->getTool())) {
        if (formats->contain_mime_type("text/plain") || formats->contain_mime_type("text/plain;charset=utf-8")) {
            return CLIPBOARD_TEXT_TARGET;
        }
    }

    for (auto tgt : preferred_targets) {
        if (formats->contain_mime_type(tgt)) {
            return tgt;
        }
    }
#ifdef _WIN32
    if (OpenClipboard(NULL))
    {   // If both bitmap and metafile are present, pick the one that was exported first.
        UINT format = EnumClipboardFormats(0);
        while (format) {
            if (format == CF_ENHMETAFILE || format == CF_DIB || format == CF_BITMAP) {
                break;
            }
            format = EnumClipboardFormats(format);
        }
        CloseClipboard();

        if (format == CF_ENHMETAFILE) {
            return "CF_ENHMETAFILE";
        }
        if (format == CF_DIB || format == CF_BITMAP) {
            return CLIPBOARD_GDK_PIXBUF_TARGET;
        }
    }

    if (IsClipboardFormatAvailable(CF_ENHMETAFILE)) {
        return "CF_ENHMETAFILE";
    }
#endif
    if (formats->contain_gtype(GDK_TYPE_TEXTURE)) {
        return CLIPBOARD_GDK_PIXBUF_TARGET;
    }
    if (formats->contain_mime_type("text/plain")) {
        return CLIPBOARD_TEXT_TARGET;
    }

    return "";
}

/**
 * Register the serializers for the ClipboardSvg type.
 *
 * Fixme: This only happens once on first use, so doesn't adapt to extensions being loaded/unloaded.
 * GTK4 makes this hard to support, because it is not designed to unregister serialisers.
 */
void ClipboardManagerImpl::_registerSerializers()
{
    Extension::DB::OutputList outlist;
    Extension::db.get_output_list(outlist);
    std::vector<std::string> target_list;

    bool plaintextSet = false;
    for (auto out : outlist) {
        if (!out->deactivated()) {
            Glib::ustring mime = out->get_mimetype();
#ifdef __APPLE__
            auto uti = mime_uti.left.find(mime);
            if (uti != mime_uti.left.end()) {
                target_list.emplace_back(uti->get_right());
            }
#endif
            if (mime != CLIPBOARD_TEXT_TARGET) {
                if (!plaintextSet && mime.find("svg") == Glib::ustring::npos) {
                    target_list.emplace_back(CLIPBOARD_TEXT_TARGET);
                    plaintextSet = true;
                }
                target_list.emplace_back(mime);
            }
        }
    }

    // Add PNG export explicitly since there is no extension for this...
    // On Windows, GTK will also present this as a CF_DIB/CF_BITMAP
    target_list.emplace_back("image/png");

    // TODO: register support for the CF_ENHMETAFILE file format. We accept it, but don't write it
    //  out right now, due to a regression it was causing with GTK4. See this link for details:
    //  https://gitlab.com/inkscape/inkscape/-/work_items/6097

    for (auto const &tgt : target_list) {
        gdk_content_register_serializer(GlibValue::type<ClipboardSvg>(), tgt.c_str(), +[] (GdkContentSerializer *serializer) {
            auto mime = gdk_content_serializer_get_mime_type(serializer);
            auto out = Glib::wrap(gdk_content_serializer_get_output_stream(serializer), true);
            auto self = reinterpret_cast<decltype(this)>(gdk_content_serializer_get_user_data(serializer));
            self->_onGet(mime, out, serializer); // returns success or an error, maybe later
        }, this, nullptr);
    }
}

/**
 * Set the clipboard targets to reflect the mimetypes Inkscape can output.
 */
void ClipboardManagerImpl::_setClipboardTargets()
{
    _clipboard->set_content(Gdk::ContentProvider::create(GlibValue::create<ClipboardSvg>()));
    last_req = std::chrono::steady_clock::now();
}

/**
 * Set the string representation of a 32-bit RGBA color as the clipboard contents.
 */
void ClipboardManagerImpl::_setClipboardColor(Colors::Color const &color)
{
    _clipboard->set_text(color.toString());
}

/**
 * Put a notification on the message stack.
 */
void ClipboardManagerImpl::_userWarn(SPDesktop *desktop, char const *msg)
{
    if (desktop) {
        desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE, msg);
    }
}

} // namespace

ClipboardManager *ClipboardManager::get()
{
    static ClipboardManagerImpl instance;
    return &instance;
}

} // namespace Inkscape::UI

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
