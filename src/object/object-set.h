// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Multiindex container for selection
 *
 * Authors:
 *   Adrian Boguszewski
 *   Marc Jeanmougin
 *
 * Copyright (C) 2016 Adrian Boguszewski
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_PROTOTYPE_OBJECTSET_H
#define INKSCAPE_PROTOTYPE_OBJECTSET_H

#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/random_access_index.hpp>

#include <sigc++/connection.h>

#include "sp-object.h"
#include "sp-item.h"
#include "sp-item-group.h"
#include "livarot/LivarotDefs.h"
#include "util-string/context-string.h"

/**
 * SiblingState enums are used to associate the current state
 * while grabbing objects. 
 * Specifically used by ObjectSet.applyAffine() to manage transforms
 * while dragging objects
 */
enum class SiblingState {
    SIBLING_NONE,		// no relation to item
    SIBLING_CLONE_ORIGINAL,	// moving both a clone and its original or any ancestor
    SIBLING_OFFSET_SOURCE,	// moving both an offset and its source
    SIBLING_TEXT_PATH,		// moving both a text-on-path and its path
    SIBLING_TEXT_FLOW_FRAME,	// moving both a flowtext and its frame
    SIBLING_TEXT_SHAPE_INSIDE,	// moving object containing sub object
};

class SPDocument;
class SPDesktop;

class SPBox3D;
class Persp3D;

namespace Inkscape {

namespace XML {
class Node;
}

struct hashed{};
struct random_access{};

inline constexpr auto object_to_node = [] (SPObject *obj) {
    return obj->getRepr();
};

using MultiIndexContainer = boost::multi_index_container<
        SPObject*,
        boost::multi_index::indexed_by<
                boost::multi_index::sequenced<>,
                boost::multi_index::random_access<
                        boost::multi_index::tag<random_access>>,
                boost::multi_index::hashed_unique<
                        boost::multi_index::tag<hashed>,
                        boost::multi_index::identity<SPObject*>>
        >>;

class ObjectSet
{
public:
    enum CompareSize {HORIZONTAL, VERTICAL, AREA};

    ObjectSet() = default;
    explicit ObjectSet(SPDesktop *desktop);
    explicit ObjectSet(SPDocument *document) : _document{document} {}
    virtual ~ObjectSet();

    ObjectSet(ObjectSet const &) = delete;
    ObjectSet &operator=(ObjectSet const &) = delete;

    void setDocument(SPDocument *document) {
        _document = document;
    }

    /**
     * Add an SPObject to the set of selected objects.
     *
     * @param obj the SPObject to add
     * @param nosignal true if no signals should be sent
     */
    bool add(SPObject *object, bool nosignal = false);

    /**
     * Add an XML node's SPObject to the set of selected objects.
     *
     * @param the xml node of the item to add
     */
    void add(XML::Node *repr);

    /**  Add items from an STL iterator range to the selection.
     *  \param from the begin iterator
     *  \param to the end iterator
     */
    template <typename InputIterator>
    void add(InputIterator from, InputIterator to) {
        if (from == to) {
            return;
        }
        for(auto it = from; it != to; ++it) {
            _add(*it);
        }
        _emitChanged();
    }

    /**
     * Removes an item from the set of selected objects.
     *
     * It is ok to call this method for an unselected item.
     *
     * @param item the item to unselect
     *
     * @return is success
     */
    bool remove(SPObject* object);

    /**  Removes items from an STL iterator range from the selection.
     *  \param from the begin iterator
     *  \param to the end iterator
     */
    template <typename InputIterator>
    void remove(InputIterator from, InputIterator to) {
        if (from == to) {
            return;
        }
        for(auto it = from; it != to; ++it) {
            _remove(*it);
        }
        _emitChanged();
   }

    /**
     * Returns true if the given object is selected.
     */
    bool includes(SPObject *object, bool anyAncestor = false);
    bool includes(Inkscape::XML::Node *node, bool anyAncestor = false);

    /**
     * Returns ancestor if the given object has ancestor selected.
     */
    SPObject * includesAncestor(SPObject *object);

    /**
     * Set the selection to a single specific object.
     *
     * @param obj the object to select
     */
    void set(SPObject *object, bool persist_selection_context = false);
    void set(XML::Node *repr);
    /**
     * Unselects all selected objects.
     */
    void clear();

    /**
     * Returns size of the selection.
     */
    int size();

    /**
     * Returns true if no items are selected.
     */
    bool isEmpty();

    /**
     * Removes an item if selected, adds otherwise.
     *
     * @param item the item to unselect
     */
    void toggle(SPObject *obj);

    /**
     * Returns a single selected object.
     *
     * @return NULL unless exactly one object is selected
     */
    SPObject *single();

    /**
     * Returns a single selected item.
     *
     * @return NULL unless exactly one object is selected
     */
    SPItem *singleItem();

    /**
     * Returns the first selected item, returns nullptr if no items selected.
     */
    SPItem *firstItem() const;

    /**
     * Returns the last selected item, returns nullptr if no items selected.
     */
    SPItem *lastItem() const;

    /**
     * Returns the smallest item from this selection.
     */
    SPItem *smallestItem(CompareSize compare);

    /**
     * Returns the largest item from this selection.
     */
    SPItem *largestItem(CompareSize compare);

    /** Returns the list of selected objects. */
    auto &objects() {
        return _container.get<random_access>();
    }

    template <typename T>
    requires std::is_base_of_v<SPObject, T>
    auto objects_of_type() {
        return objects()
            | std::views::filter(is<T>)
            | std::views::transform(cast_unsafe<T>);
    }

    /** Returns a range of selected SPItems. */
    auto items() {
        return objects_of_type<SPItem>();
    }

    std::vector<SPItem *> items_vector() {
        auto i = items();
        return {i.begin(), i.end()};
    }

    /** Returns a range of selected groups. */
    auto groups() {
        return objects_of_type<SPGroup>();
    }

    /** Returns a range of the xml nodes of all selected items. */
    auto xmlNodes() {
        return objects()
            | std::views::filter(is<SPItem>)
            | std::views::transform(object_to_node);
    }

    std::vector<XML::Node *> xmlNodes_vector() {
        auto i = xmlNodes();
        return {i.begin(), i.end()};
    }

    /**
     * Returns a single selected object's xml node.
     *
     * @return NULL unless exactly one object is selected
     */
    XML::Node *singleRepr();

    /**
     * The top-most item, or NULL if the selection is empty.
     */
    XML::Node *topRepr() const;

    /**
     * Selects exactly the specified objects.
     *
     * @param objs the objects to select
     */
    template <class T>
    requires std::is_base_of_v<SPObject, T>
    void setList(std::vector<T *> const &objs) {
        _clear();
        addList(objs);
    }

    /**
     * Selects the objects with the same IDs as those in `list`.
     *
     * @todo How about adding `setIdList(std::vector<Glib::ustring> const &list)`
     * 
     * @param list the repr list to add
     */
    void setReprList(std::vector<XML::Node*> const &list);

    /**
     * Assign IDs to selected objects that don't have an ID attribute
     * Checks if the object's id attribute is NULL. If it is, assign it a unique ID
     */
    void enforceIds();

    /**
     * Adds the specified objects to selection, without deselecting first.
     *
     * @param objs the objects to select
     */
    template <class T>
    requires std::is_base_of_v<SPObject, T>
    void addList(std::vector<T *> const &objs) {
        for (auto obj: objs) {
            if (!includes(obj)) {
                add(obj, true);
            }
        }
        _emitChanged();
    }

    /**
     * Remove the specified objects from selection.
     *
     * @param objs the objects to select
     */
    template <class T>
    requires std::is_base_of_v<SPObject, T>
    void removeList(std::vector<T *> const &objs) {
        for (auto obj: objs) {
            remove(obj);
        }
        _emitChanged();
    }

    /** Returns the bounding rectangle of the selection. */
    Geom::OptRect bounds(SPItem::BBoxType type) const;
    Geom::OptRect visualBounds() const;
    Geom::OptRect geometricBounds() const;
    Geom::OptRect strokedBounds() const;

    /**
     * Returns either the visual or geometric bounding rectangle of the selection, based on the
     * preferences specified for the selector tool
     */
    Geom::OptRect preferredBounds() const;

    /* Returns the bounding rectangle of the selectionin document coordinates.*/
    Geom::OptRect documentBounds(SPItem::BBoxType type) const;

    /**
     * Returns either the visual or geometric bounding rectangle of selection in document
     * coordinates based on preferences specified for the selector tool
     */
    Geom::OptRect documentPreferredBounds() const;

    /**
     * Returns the rotation/skew center of the selection.
     */
    std::optional<Geom::Point> center() const;

    /** Returns a list of all perspectives which have a 3D box in the current selection.
       (these may also be nested in groups) */
    std::list<Persp3D *> const perspList();

    /**
     * Returns a list of all 3D boxes in the current selection which are associated to @c
     * persp. If @c pers is @c NULL, return all selected boxes.
     */
    std::list<SPBox3D *> const box3DList(Persp3D *persp = nullptr);

    /**
     * Returns the desktop the selection is bound to
     *
     * @return the desktop the selection is bound to, or NULL if in console mode
     */
    SPDesktop *desktop() { return _desktop; }

    /**
     * Returns the document the selection is bound to
     *
     * @return the document the selection is bound to, or NULL if in console mode
     */
    SPDocument *document() { return _document; }

    //item groups operations
    //in selection-chemistry.cpp
    void deleteItems(bool skip_undo = false);
    void duplicate(bool suppressDone = false, bool duplicateLayer = false, bool includeHidden = false);
    void clone(bool skip_undo = false);

    /**
     * @brief Unlink all directly selected clones.
     * @param skip_undo If this is set to true the call to DocumentUndo::done is omitted.
     * @return True if anything was unlinked, otherwise false.
     */
    bool unlink(const bool skip_undo = false, const bool silent = false);
    /**
     * @brief Recursively unlink any clones present in the current selection,
     * including clones which are used to clip other objects, groups of clones etc.
     * @return true if anything was unlinked, otherwise false.
     */
    bool unlinkRecursive(const bool skip_undo = false, const bool force = false, const bool silent = false);
    void removeLPESRecursive(bool keep_paths);
    void relink();
    void cloneOriginal();
    void cloneOriginalPathLPE(bool allow_transforms = false, bool sync = false, bool skip_undo = false);
    Inkscape::XML::Node* group(bool is_anchor = false);
    void popFromGroup();
    void ungroup(bool skip_undo = false);
    void ungroup_all(bool skip_undo = false);

    //z-order management
    //in selection-chemistry.cpp
    void stackUp(bool skip_undo = false);
    void raise(bool skip_undo = false);
    void raiseToTop(bool skip_undo = false);
    void stackDown(bool skip_undo = false);
    void lower(bool skip_undo = false);
    void lowerToBottom(bool skip_undo = false);
    void toNextLayer(bool skip_undo = false);
    void toPrevLayer(bool skip_undo = false);
    void toLayer(SPObject *layer);
    void toLayer(SPObject *layer, Inkscape::XML::Node *after);

    //clipboard management
    //in selection-chemistry.cpp
    void copy();
    void cut();
    void pasteStyle();
    void pasteSize(bool apply_x, bool apply_y);
    void pasteSizeSeparately(bool apply_x, bool apply_y);
    void pastePathEffect();

    //path operations
    //in path-chemistry.cpp
    void combine(bool skip_undo = false, bool silent = false);
    void breakApart(bool skip_undo = false, bool overlapping = true, bool silent = false);
    void toCurves(bool skip_undo = false, bool clonesjustunlink = false);
    void toLPEItems();
    void pathReverse();

    // path operations
    // in path/path-object-set.cpp
    bool strokesToPaths(bool legacy = false, bool skip_undo = false);
    bool simplifyPaths(bool skip_undo = false);

    // Boolean operations
    void pathUnion    (bool skip_undo = false, bool silent = false);
    void pathIntersect(bool skip_undo = false, bool silent = false);
    void pathDiff     (bool skip_undo = false, bool silent = false);
    void pathSymDiff  (bool skip_undo = false, bool silent = false);
    void pathCut      (bool skip_undo = false, bool silent = false);
    void pathSlice    (bool skip_undo = false, bool silent = false);

    // Other path operations
    // in selection-chemistry.cpp
    void toMarker(bool apply = true);
    void toGuides();
    void toSymbol();
    void unSymbol();
    void tile(bool apply = true); //"Object to Pattern"
    void untile();
    void createBitmapCopy();
    void setMask(bool apply_clip_path, bool apply_to_layer, bool remove_original);
    void editMask(bool clip);
    void unsetMask(const bool apply_clip_path, const bool delete_helper_group, bool remove_original);
    void setClipGroup();
    void chameleonFill();

    // moves
    // in selection-chemistry.cpp
    void removeLPE();
    void removeFilter();
    void reapplyAffine();
    void clearLastAffine();
    void applyAffine(Geom::Affine const &affine, bool set_i2d=true,bool compensate=true, bool adjust_transf_center=true);
    void setTransformPreview(Geom::Affine const &affine, unsigned key);
    void removePathTransforms();
    void removeTransform();
    void setScaleAbsolute(double, double, double, double);
    void scaleRelative(const Geom::Point&, const Geom::Scale&);
    void rotateRelative(const Geom::Point&, double);
    void skewRelative(const Geom::Point&, double, double);
    void moveRelative(const Geom::Point &move, bool compensate = true);
    void moveRelative(double dx, double dy);
    void move(double dx, double dy);
    void moveScreen(double dx, double dy);
    void move(double dx, double dy, bool rotated);
    void move(double dx, double dy, bool rotated, bool screen);
    void moveScreen(double dx, double dy, bool rotated);

    // various
    bool fitCanvas(bool with_margins, bool skip_undo = false);
    void swapFillStroke();
    void fillBetweenMany();

    SiblingState getSiblingState(SPItem *item);
    void insertSiblingState(SPObject *object, SiblingState state);
    void clearSiblingStates();

protected:
    virtual void _connectSignals(SPObject* object) {};
    virtual void _releaseSignals(SPObject* object) {};
    virtual void _emitChanged(bool persist_selection_context = false);
    void _add(SPObject* object);
    void _clear();
    void _remove(SPObject* object);
    bool _anyAncestorIsInSet(SPObject *object);
    void _removeDescendantsFromSet(SPObject *object);
    void _removeAncestorsFromSet(SPObject *object);
    SPItem *_sizeistItem(bool sml, CompareSize compare);
    SPObject *_getMutualAncestor(SPObject *object);
    virtual void _add3DBoxesRecursively(SPObject *obj);
    virtual void _remove3DBoxesRecursively(SPObject *obj);

    MultiIndexContainer _container;
    SPDesktop *_desktop = nullptr;
    SPDocument *_document = nullptr;
    std::list<SPBox3D *> _3dboxes;
    std::unordered_map<SPObject*, sigc::connection> _releaseConnections;

private:
    void _pathBoolOp(BooleanOp bop, char const *icon_name, Inkscape::Util::Internal::ContextString description, bool skip_undo, bool silent);
    void _pathBoolOp(BooleanOp bop);

    void _disconnect(SPObject* object);
    std::map<SPObject *, SiblingState> _sibling_state;

    Geom::Affine _last_affine;
};

} // namespace Inkscape

#endif // INKSCAPE_PROTOTYPE_OBJECTSET_H

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
