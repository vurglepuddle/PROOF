// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Inkscape::PageManager - Multi-Page management.
 *
 * Copyright 2021 Martin Owens <doctormo@geek-2.com>
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "page-manager.h"

#include <glibmm/i18n.h>
#include "attributes.h"
#include "colors/manager.h"
#include "desktop.h"
#include "display/control/canvas-page.h"
#include "document.h"
#include "extension/template.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-defs.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection-chemistry.h"
#include "util/parse-int-range.h"
#include "util/numeric/converters.h"

namespace Inkscape {

bool PageManager::move_objects()
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    return prefs->getBool("/tools/pages/move_objects", true);
}

PageManager::PageManager(SPDocument *document)
    : border_show(true)
    , border_on_top(true)
    , shadow_show(true)
    , checkerboard(false)
    , background_color{CanvasPage::DEFAULT_BACKGROUND_COLOR}
    , margin_color{CanvasPage::DEFAULT_MARGIN_COLOR}
    , bleed_color{CanvasPage::DEFAULT_BLEED_COLOR}
    , border_color{CanvasPage::DEFAULT_BORDER_COLOR}
{
    _document = document;

    // We use resource signals to avoid adding pages which are not built yet (have no id)
    _resources_changed = _document->connectResourcesChanged("page", [this]() {
        if (_document) {
            auto res = _document->getResourceList("page");
            for (auto obj : res) { // Additions
                if (auto page = cast<SPPage>(obj)) {
                    if (std::find(_pages.begin(), _pages.end(), page) == _pages.end()) {
                        addPage(page);
                    }
                }
            }
            for (auto page : _pages) {
                if (std::find(res.begin(), res.end(), cast<SPObject>(page)) == res.end()) {
                    removePage(page->getRepr());
                }
            }
        }
    });
}

PageManager::~PageManager()
{
    deactivate();
}

void PageManager::deactivate()
{
    _pages.clear();
    _selected_page = nullptr;
    _document = nullptr;
}

/**
 * Add a page to this manager, called from namedview parent.
 */
void PageManager::addPage(SPPage *page)
{
    g_assert(page->document == _document);
    if (std::find(_pages.begin(), _pages.end(), page) != _pages.end()) {
        // Refuse to double add pages to list.
        return;
    }
    page->_updateTotalHRefCount(1);
    _pages.push_back(page);
    reorderPages();
    selectPage(page); // Since this is called from namedview, select last page added.
    pagesChanged(page);
}

/**
 * Remove a page from this manager
 */
void PageManager::removePage(Inkscape::XML::Node *child)
{
    for (unsigned i = 0; i < _pages.size(); i++) {
        SPPage *page = _pages[i];
        if (page->getRepr() == child) {
            _pages.erase(_pages.begin() + i);

            // Document is being destroyed
            if (!_document) {
                return;
            }

            if (hasPages() && page->isViewportPage()) {
                _document->fitToRect(getFirstPage()->getDesktopRect(), {});
            }

            // Reselect because this page is gone.
            if (_selected_page == page) {
                if (i < _pages.size()) {
                    selectPage(_pages[i]);
                } else if (!_pages.empty()) {
                    selectPage(_pages[i - 1]);
                } else {
                    selectPage(nullptr);
                }
            }

            pagesChanged(nullptr);
            break;
        }
    }
}

/**
 * Reorder pages within the internal list to keep it up to date by tracking
 * their order in the object tree.
 */
void PageManager::reorderPages()
{
    bool changes = false;
    std::sort(_pages.begin(), _pages.end(), [this, &changes](SPPage *first, SPPage *second) {
        auto ret = sp_repr_compare_position_bool(first->getRepr(), second->getRepr());
        changes |= ret;
        return ret;
    });
    if (changes) {
        pagesChanged(nullptr);
    }
}

/**
 * Enables multi page support by turning the document viewBox into
 * the first page. Ensures that there is a selected page.
 */
void PageManager::enablePages()
{
    if (!hasPages()) {
        _selected_page = newDocumentPage(*_document->preferredBounds(), true);
    } else if (!_selected_page) {
        _selected_page = _pages.back();
    }
}

/**
 * Add a new page of the default size, this will be either
 * the size of the viewBox if no pages exist, or the size
 * of the selected page.
 */
SPPage *PageManager::newPage()
{
    enablePages();
    auto rect = _selected_page->getRect();
    auto new_page = newPage(rect.width(), rect.height());
    new_page->copyFrom(_selected_page);
    return new_page;
}

/**
 * Add a new page of the given width and height.
 */
SPPage *PageManager::newPage(double width, double height)
{
    auto loc = nextPageLocation();
    return newPage(Geom::Rect::from_xywh(loc, Geom::Point(width, height)));
}

/**
 * Return the location of the next created page.
 */
Geom::Point PageManager::nextPageLocation() const
{
    // Get a new location for the page.
    double top = 0.0;
    double left = 0.0;
    for (auto &page : _pages) {
        auto rect = page->getRect();
        if (rect.right() > left) {
            left = rect.right() + 10;
        }
    }
    return Geom::Point(left, top);
}

/**
 * Add a new page with the given rectangle.
 */
SPPage *PageManager::newPage(Geom::Rect rect, bool first_page)
{
    // This turns on pages support, which will make two pages if none exist yet.
    // The first is the ViewBox page, and the second is made below as the "second"
    if (!hasPages() && !first_page) {
        enablePages();
    }

    auto xml_doc = _document->getReprDoc();
    auto repr = xml_doc->createElement("svg:view");
    repr->setAttributeRect("viewBox", rect);

    if (auto defs = _document->getDefs()) {
        if (auto page = cast<SPPage>(defs->appendChildRepr(repr))) {
            Inkscape::GC::release(repr);
            return page;
        }
    }
    return nullptr;
}

/**
 * Create a new page, resizing the rectangle from desktop coordinates.
 */
SPPage *PageManager::newDesktopPage(Geom::Rect rect, bool first_page)
{
    rect *= _document->dt2doc();
    return newDocumentPage(rect, first_page);
}

/**
 * Create a new page, using document coordinates.
 */
SPPage *PageManager::newDocumentPage(Geom::Rect rect, bool first_page)
{
    return newPage(rect * _document->getDocumentScale().inverse(), first_page);
}

/**
 * Delete the given page.
 *
 * @param page - The page to be deleted.
 * @param content - Also remove the svg objects that are inside the page.
 */
void PageManager::deletePage(SPPage *page, bool content)
{
    if (page) {
        if (content) {
            auto const exclusive_items = page->getExclusiveItems();

            ObjectSet items_to_delete{_document};
            for (auto *item : exclusive_items) {
                // Fully inside this page does not mean exclusive to it: artboards can
                // overlap. Keep artwork that also belongs to a retained page.
                if (getPagesFor(item, false).size() == 1) {
                    items_to_delete.add(item);
                }
            }

            for (auto *item : page->getOverlappingItems()) {
                // Only delete objects when they rest on one page.
                if (getPagesFor(item, false).size() == 1) {
                    items_to_delete.add(item);
                }
            }
            items_to_delete.deleteItems(true);
        }
        // Only adjust if there will be a page after viewport page is deleted
        bool fit_viewport = page->isViewportPage() && getPageCount() >= 2;

        // Removal from pages is done automatically via signals.
        page->deleteObject();

        if (fit_viewport) {
            _document->fitToRect(getFirstPage()->getDocumentRect(), false);
        }
    }

    // As above with the viewbox shadowing, we need go back to a single page
    // (which is zero pages) when needed.
    if (auto page = getFirstPage()) {
        if (getPageCount() == 1) {
            auto rect = page->getDesktopRect();
            // We delete the page, only if it's bare (no margins etc)
            if (page->isBarePage())
                deletePage(page, false);
            _document->fitToRect(rect, false);
         }
    }
}

/**
 * Creates a new page of the size of the currently selected page,
 * and makes a deep copy of its overlapping elements.
 */
SPPage *PageManager::duplicatePage()
{
    enablePages();

    auto contents = ObjectSet(_document);
    contents.setList(_selected_page->getOverlappingItems());

    auto rect = _selected_page->getRect();

    auto new_page = newPage(rect.width(), rect.height());
    new_page->copyFrom(_selected_page);

    auto new_rect = new_page->getRect();
    auto move_tr = (new_rect.midpoint() - rect.midpoint()) * _document->getDocumentScale();

    contents.duplicate(true, false, true);
    contents.applyAffine(Geom::Affine(Geom::Translate(move_tr)));

    return new_page;
}

/**
 * Delete the selected page.
 *
 * @param content - Also remove the svg objects that are inside the page.
 */
void PageManager::deletePage(bool content)
{
    deletePage(_selected_page, content);
}

/**
 * Disables multi page supply by removing all the page objects.
 */
void PageManager::disablePages()
{
    while (hasPages()) {
        deletePage(getLastPage(), false);
    }
}


/**
 * Get page index, returns -1 if the page is not found in this document.
 */
int PageManager::getPageIndex(const SPPage *page) const
{
    if (page) {
        auto it = std::find(_pages.begin(), _pages.end(), page);
        if (it != _pages.end()) {
            return it - _pages.begin();
        }
        g_warning("Can't get page index for %s", page->getId());
    }
    return -1;
}

/**
 * Return the index of the page in the index
 */
int PageManager::getSelectedPageIndex() const
{
    return getPageIndex(_selected_page);
}

/**
 * Returns the selected page rect, OR the viewbox rect.
 */
Geom::Rect PageManager::getSelectedPageRect() const
{
    return _selected_page ? _selected_page->getDesktopRect() : *(_document->preferredBounds());
}

Geom::Affine PageManager::getSelectedPageAffine() const
{
    return _selected_page ? _selected_page->getDesktopAffine() : Geom::identity();
}

/**
 * Called when the pages vector is updated, either page
 * deleted or page created (but not if the page is modified)
 */
void PageManager::pagesChanged(SPPage *new_page)
{
    if (_pages.empty() || getSelectedPageIndex() == -1) {
        selectPage(nullptr);
    }

    if (!_selected_page) {
        selectPage(new_page);
    }

    _pages_changed_signal.emit(new_page);
}

/**
 * Set the given page as the selected page.
 *
 * @param page - The page to set as the selected page.
 */
bool PageManager::selectPage(SPPage *page)
{
    if (!page || getPageIndex(page) >= 0) {
        if (_selected_page != page) {
            _selected_page = page;
            _page_selected_signal.emit(_selected_page);

            // Modified signal for when the attributes themselves are modified.
            _page_modified_connection.disconnect();
            if (page) {
                _page_modified_connection = page->connectModified([this](SPObject *, unsigned int) {
                    _page_modified_signal.emit(_selected_page);
                });
            }

            return true;
        }
    }
    return false;
}

/**
 * Select the first page the given sp-item object is within.
 *
 * If the item is between two pages and one of them is already selected
 * then don't change the selection.
 */
bool PageManager::selectPage(SPItem *item, bool contains)
{
    if (_selected_page && _selected_page->itemOnPage(item, contains)) {
        return true;
    }
    for (auto &page : getPagesFor(item, contains)) {
        return selectPage(page);
    }
    return false;
}

/**
 * Get the page at the given position or return nullptr if out of range.
 *
 * @param index - The page index (from 0) of the page.
 */
SPPage *PageManager::getPage(int index) const
{
    if (index < 0 || index >= _pages.size()) {
        return nullptr;
    }
    return _pages[index];
}

/**
 * Get the pages from a set of pages in the given set.
 *
 * @param pages - A set of page positions in the format "1,2-3...etc"
 * @param inverse - Reverse the selection, selecting pages not in page_pos.
 *
 * @returns A vector of SPPage objects found. Not found pages are ignored.
 */
std::vector<SPPage *> PageManager::getPages(const std::string &pages, bool inverse) const
{
    return getPages(parseIntRange(pages, 1, getPageCount()), inverse);
}

/**
 * Get the pages from a set of pages in the given set.
 *
 * @param page_pos - A set of page positions indexed from 1.
 * @param inverse - Reverse the selection, selecting pages not in page_pos.
 *
 * @returns A vector of SPPage objects found. Not found pages are ignored.
 */
std::vector<SPPage *> PageManager::getPages(std::set<unsigned int> page_pos, bool inverse) const
{
    std::vector<SPPage *> ret;
    for (auto page : _pages) {
        bool contains = page_pos.find(page->getPagePosition()) != page_pos.end();
        if (contains != inverse) {
            ret.push_back(page);
        }
    }
    return ret;
}

/**
 * Return a list of pages this item is on.
 */
std::vector<SPPage *> PageManager::getPagesFor(SPItem *item, bool contains) const
{
    std::vector<SPPage *> ret;
    for (auto &page : _pages) {
        if (page->itemOnPage(item, contains)) {
            ret.push_back(page);
        }
    }
    return ret;
}

/**
 * Return the first page that contains the given item
 */
SPPage *PageManager::getPageFor(SPItem *item, bool contains) const
{
    for (auto &page : _pages) {
        if (page->itemOnPage(item, contains))
            return page;
    }
    return nullptr;
}

/**
 * Get a page at a specific starting location.
 */
SPPage *PageManager::getPageAt(Geom::Point pos) const
{
    for (auto &page : _pages) {
        if (page->getDesktopRect().corner(0) == pos) {
            return page;
        }
    }
    return nullptr;
}

/**
 * This provides a simple way of selecting a page based on their layering
 * Pages which are entirely contained within another are selected before
 * their larger parents.
*/
SPPage *PageManager::findPageAt(Geom::Point pos) const
{
    SPPage* ret = nullptr;
    for (auto &page : getPages()) {
        auto rect = page->getSensitiveRect();
        // If the point is inside the page boundry
        if (rect.contains(pos)) {
            // If we don't have a page yet, or the new page is inside the old one.
            if (!ret || ret->getSensitiveRect().contains(rect)) {
                ret = page;
            }
        }
    }
    return ret;
}

/**
 * Returns the page attached to the viewport, or nullptr if no pages
 * or none of the pages are the viewport page.
 */
SPPage *PageManager::getViewportPage() const
{
    for (auto &page : _pages) {
        if (page->isViewportPage()) {
            return page;
        }
    }
    return nullptr;
}

/**
 * Returns the total area of all the pages in desktop units.
 */
Geom::OptRect PageManager::getDesktopRect() const
{
    Geom::OptRect total_area;
    for (auto &page : _pages) {
        if (total_area) {
            total_area->unionWith(page->getDesktopRect());
        } else {
            total_area = page->getDesktopRect();
        }
    }
    return total_area;
}

/**
 * Center/zoom on the given page.
 */
void PageManager::zoomToPage(SPDesktop *desktop, SPPage *page, bool width_only)
{
    Geom::Rect rect = page ? page->getDesktopRect() : *(_document->preferredBounds());
    if (rect.minExtent() < 1.0)
        return;
    if (width_only) {
        desktop->set_display_width(rect, 10);
    } else {
        desktop->set_display_area(rect, 10);
    }
}

/**
 * Center without zooming on the given page
 */
void PageManager::centerToPage(SPDesktop *desktop, SPPage *page)
{
    Geom::Rect rect = page ? page->getDesktopRect() : *(_document->preferredBounds());
    desktop->set_display_center(rect);
}

/**
 * Change page size, margins and bleeds by a set amount
 */
void PageManager::scalePages(Geom::Scale const &scale)
{
    for (auto &page : _pages) {
        page->setRect(page->getRect() * scale);
        for (int side = 0; side < 4; side++) {
            page->setMarginSide(side, page->getMarginSide(side) * scale.vector()[0]);
            page->setBleedSide(side, page->getBleedSide(side) * scale.vector()[0]);
        }
    }
}

void PageManager::resizePage(double width, double height)
{
    resizePage(_selected_page, width, height);
}

void PageManager::resizePage(SPPage *page, double width, double height)
{
    if (_pages.empty() || page) {
        // Resizing the Viewport, means the page gets updated automatically
        if (_pages.empty() || (page && page->isViewportPage())) {
            auto rect = Geom::Rect(Geom::Point(0, 0), Geom::Point(width, height));
            _document->fitToRect(rect, false);
        } else if (page) {
            page->setSize(width, height);
        }
    }
}

/**
 * Rotate the selected page by the given number of 90 degree rotations.
 */
void PageManager::rotatePage(int turns)
{
    Geom::Rect page_size = getSelectedPageRect();
    Geom::Translate const center(page_size.midpoint());
    Geom::Rotate const rotate(Geom::Rotate::from_degrees(turns * 90));
    Geom::Affine const tr(center.inverse() * rotate * center);

    auto contents = ObjectSet();
    if (_selected_page) {
        contents.setList(_selected_page->getOverlappingItems());
    } else {
        contents.setList(_document->getRoot()->item_list());
    }
    contents.applyAffine(tr);

    auto new_box = Geom::Rect(page_size.min() * tr, page_size.max() * tr);
    fitToRect(new_box, _selected_page);
}

/**
 * Change page orientation, landscape to portrait and back.
 */
void PageManager::changeOrientation()
{
    auto rect = getSelectedPageRect();
    resizePage(rect.height(), rect.width());
}

/**
 * Resize the page to the given selection. If nothing is selected,
 * Resize to all the items on the selected page.
 */
void PageManager::fitToSelection(ObjectSet *selection, bool add_margins)
{
    auto desktop = selection->desktop();

    if (!selection || selection->isEmpty()) {
        // This means there aren't any pages, so revert to the default assumption
        // that the viewport is resized around ALL objects.
        if (!_selected_page) {
            fitToRect(_document->getRoot()->documentPreferredBounds(), _selected_page, add_margins);
        } else {
            // This allows the pages to be resized around the items related to the page only.
            auto contents = ObjectSet();
            contents.setList(getOverlappingItems(desktop, _selected_page));
            if (contents.isEmpty()) {
                fitToRect(_document->getRoot()->documentPreferredBounds(), _selected_page, add_margins);
            } else {
                fitToSelection(&contents, add_margins);
            }
        }
    } else if (auto rect = selection->documentPreferredBounds()) {
        fitToRect(rect, _selected_page, add_margins);
    }
}

/**
 * Fit the selected page to the given rectangle.
 */
void PageManager::fitToRect(Geom::OptRect rect, SPPage *page, bool add_margins)
{
    if (!rect) return;
    bool viewport = true;
    if (page) {
        viewport = page->isViewportPage();
        page->setDocumentRect(*rect, add_margins);
        rect = page->getDocumentRect();
    }
    if (viewport) {
        _document->fitToRect(*rect);
        if (page && !page->isViewportPage()) {
            // The document's fitToRect has slightly mangled the page rect, fix it.
            page->setDocumentRect(Geom::Rect(Geom::Point(0, 0), rect->dimensions()));
        }
    }
}


/**
 * Return a list of objects touching this page, or viewbox (of single page document)
 */
std::vector<SPItem *> PageManager::getOverlappingItems(SPDesktop *desktop, SPPage *page, bool hidden, bool in_bleed, bool in_layers)
{
    if (page) {
        return page->getOverlappingItems(hidden, in_bleed, in_layers);
    }
    auto doc_rect = _document->preferredBounds();
    return _document->getItemsPartiallyInBox(desktop->dkey, *doc_rect, true, true, true, false, in_layers);
}

/**
 * Manage the page subset of attributes from sp-namedview and store them.
 */
bool PageManager::subset(SPAttr key, const gchar *value)
{
    switch (key) {
        case SPAttr::SHOWBORDER:
            this->border_show.readOrUnset(value);
            break;
        case SPAttr::BORDERLAYER:
            this->border_on_top.readOrUnset(value);
            break;
        case SPAttr::BORDERCOLOR: {
            auto const old_opacity = border_color.getOpacity();
            border_color = Colors::Color::parse(value).value_or(CanvasPage::DEFAULT_BORDER_COLOR);
            border_color.setOpacity(old_opacity);
            break;
        }
        case SPAttr::BORDEROPACITY:
            border_color.setOpacity(value ? g_ascii_strtod(value, nullptr) : 1.0);
            break;
        case SPAttr::PAGECOLOR:
            background_color = Colors::Color::parse(value).value_or(CanvasPage::DEFAULT_BACKGROUND_COLOR);
            break;
        case SPAttr::SHOWPAGESHADOW: // Deprecated
            this->shadow_show.readOrUnset(value);
            break;
        case SPAttr::INKSCAPE_DESK_CHECKERBOARD:
            checkerboard.readOrUnset(value);
            return false; // propagate further
        case SPAttr::PAGELABELSTYLE:
            label_style = value ? value : "default";

            // Update user action button
            if (auto action = _document->getActionGroup()->lookup_action("page-label-style")) {
                action->change_state(label_style == "below");
            }
            break;
        default:
            return false;
    }
    return true;
}

/**
 * Update the canvas item with the default display attributes.
 */
bool PageManager::setDefaultAttributes(Inkscape::CanvasPage *item)
{
    auto bdcolor = getBorderColor();

    if (!border_show) {
        bdcolor.setOpacity(0.0);
    }

    auto bgcolor = getBackgroundColor();

    // note: page background color doesn't have configurable transparency; it is considered to be opaque;
    // here alpha gets manipulated to reveal checkerboard pattern, if needed
    if (checkerboard) {
        bgcolor.setOpacity(0.0);
    } else {
        bgcolor.setOpacity(1.0);
    }

    auto dkcolor = _document->getNamedView()->getDeskColor();

    bool ret = item->setOnTop(border_on_top);
    // fixed shadow size, not configurable; shadow changes size with zoom
    ret |= item->setShadow(border_show && shadow_show ? 2 : 0);
    ret |= item->setPageColor(bdcolor, bgcolor, dkcolor, getMarginColor(), getBleedColor(), getShadowColor());
    ret |= item->setLabelStyle(label_style);
    return ret;
}

/**
 * Return a page's size label, or match via width and height.
 */
std::string PageManager::getSizeLabel(SPPage *page)
{
    auto box = *_document->preferredBounds();
    if (page) {
        box = page->getDesktopRect();
        auto label = page->getSizeLabel();
        if (!label.empty())
            return _(label.c_str());
    }
    return getSizeLabel(box.width(), box.height());
}

/**
 * Loop through all page sizes to find a matching one for this width and height.
 *
 * @param width - The X axis size in pixels
 * @param height - The Y axis size in pixels
 */
std::string PageManager::getSizeLabel(double width, double height)
{
    using namespace Inkscape::Util;

    if (auto preset = Inkscape::Extension::Template::get_any_preset(width, height)) {
        return _(preset->get_name().c_str());
    }

    static auto px = Inkscape::Util::UnitTable::get().getUnit("px");
    auto unit = _document->getDisplayUnit();
    return format_number(Quantity::convert(width, px, unit), 2)
             + " × " +
           format_number(Quantity::convert(height, px, unit), 2)
             + " " + unit->abbr;
}

Colors::Color const &PageManager::getShadowColor() const
{
    if (_document) {
        auto desk = _document->getNamedView()->getDeskColor();
        if (auto desk_hsl = desk.converted(Colors::Space::Type::HSL); desk_hsl && (*desk_hsl)[2] < 0.05) {
            return CanvasPage::DEFAULT_SHADOW_COLOR_LIGHT;
        }
    }
    return CanvasPage::DEFAULT_SHADOW_COLOR;
}

/**
 * Called when the viewbox is resized.
 */
void PageManager::movePages(Geom::Affine tr)
{
    // Adjust each page against the change in position of the viewbox.
    for (auto &page : _pages) {
        page->movePage(tr, false);
    }
}

}; // namespace Inkscape

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
