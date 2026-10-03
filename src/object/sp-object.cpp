// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPObject implementation.
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Stephen Silver <sasilver@users.sourceforge.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Adrian Boguszewski
 *
 * Copyright (C) 1999-2016 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <cstring>
#include <ranges>
#include <string>
#include <vector>
#include <limits>
#include <glibmm.h>

#include "attributes.h"
#include "attribute-rel-util.h"
#include "color-profile.h"
#include "document.h"
#include "colors/document-colors.h"
#include "io/fix-broken-links.h"
#include "preferences.h"
#include "style.h"
#include "live_effects/lpeobject.h"
#include "sp-factory.h"
#include "sp-font.h"
#include "sp-glyph.h"
#include "sp-paint-server.h"
#include "sp-page.h"
#include "sp-root.h"
#include "sp-use.h"
#include "sp-use-reference.h"
#include "sp-style-elem.h"
#include "sp-script.h"
#include "streq.h"
#include "strneq.h"
#include "xml/node-fns.h"
#include "xml/href-attribute-helper.h"
#include "debug/event-tracker.h"
#include "debug/simple-event.h"
#include "debug/demangle.h"
#include "svg/css-ostringstream.h"
#include "util/format.h"
#include "util/longest-common-suffix.h"

#define noSP_OBJECT_DEBUG_CASCADE

#define noSP_OBJECT_DEBUG

#ifdef SP_OBJECT_DEBUG
# define debug(f, a...) { g_print("%s(%d) %s:", \
                                  __FILE__,__LINE__,__FUNCTION__); \
                          g_print(f, ## a); \
                          g_print("\n"); \
                        }
#else
# define debug(f, a...) /* */
#endif

// Define to enable indented tracing of SPObject.
//#define OBJECT_TRACE
static unsigned indent_level = 0;

/**
 * A friend class used to set internal members on SPObject so as to not expose settors in SPObject's public API
 */
class SPObjectImpl
{
public:

/**
 * Null's the id member of an SPObject without attempting to free prior contents.
 *
 * @param[inout] obj Pointer to the object which's id shall be nulled.
 */
    static void setIdNull( SPObject* obj ) {
        if (obj) {
            obj->id = nullptr;
        }
    }

/**
 * Sets the id member of an object, freeing any prior content.
 *
 * @param[inout] obj Pointer to the object which's id shall be set.
 * @param[in] id New id
 */
    static void setId( SPObject* obj, gchar const* id ) {
        if (obj && (id != obj->id) ) {
            if (obj->id) {
                g_free(obj->id);
                obj->id = nullptr;
            }
            if (id) {
                obj->id = g_strdup(id);
            }
        }
    }
};

/**
 * Constructor, sets all attributes to default values.
 */
SPObject::SPObject()
    : cloned{0}
    , uflags{0}
    , mflags{0}
{
    debug("id=%p, typename=%s", this, g_type_name_from_instance((GTypeInstance *)this));

    SPObjectImpl::setIdNull(this);

    // FIXME: now we create style for all objects, but per SVG, only the following can have style attribute:
    // vg, g, defs, desc, title, symbol, use, image, switch, path, rect, circle, ellipse, line, polyline,
    // polygon, text, tspan, tref, textPath, altGlyph, glyphRef, marker, linearGradient, radialGradient,
    // stop, pattern, clipPath, mask, filter, feImage, a, font, glyph, missing-glyph, foreignObject
    style = new SPStyle(nullptr, this);
    context_style = nullptr;
}

/**
 * Destructor, frees the used memory and unreferences a potential successor of the object.
 */
SPObject::~SPObject()
{
    g_free(this->_label);
    g_free(this->_default_label);

    if (this->_successor) {
        sp_object_unref(this->_successor, nullptr);
        this->_successor = nullptr;
    }
    if (this->_tmpsuccessor) {
        sp_object_unref(this->_tmpsuccessor, nullptr);
        this->_tmpsuccessor = nullptr;
    }
    if (parent) {
        parent->children.erase(parent->children.iterator_to(*this));
    }

    delete style;
    this->document = nullptr;
    this->repr = nullptr;
}

// CPPIFY: make pure virtual
void SPObject::read_content() {
    //throw;
}

void SPObject::update(SPCtx* /*ctx*/, unsigned int /*flags*/) {
    //throw;
}

void SPObject::modified(unsigned int /*flags*/) {
#ifdef OBJECT_TRACE
    objectTrace( "SPObject::modified  (default) (empty function)" );
    objectTrace( "SPObject::modified  (default)", false );
#endif
    //throw;
}

namespace {

namespace Debug = Inkscape::Debug;
namespace Util = Inkscape::Util;

typedef Debug::SimpleEvent<Debug::Event::REFCOUNT> BaseRefCountEvent;

class RefCountEvent : public BaseRefCountEvent {
public:
    RefCountEvent(SPObject *object, int bias, char const *name)
    : BaseRefCountEvent(name)
    {
        _addProperty("object", Util::format("%p", object).pointer());
        _addProperty("class", Debug::demangle(typeid(*object).name()));
        _addProperty("new-refcount", Util::format("%d", object->refCount + bias).pointer());
    }
};

class RefEvent : public RefCountEvent {
public:
    RefEvent(SPObject *object)
    : RefCountEvent(object, 1, "sp-object-ref")
    {}
};

class UnrefEvent : public RefCountEvent {
public:
    UnrefEvent(SPObject *object)
    : RefCountEvent(object, -1, "sp-object-unref")
    {}
};

} // namespace

gchar const* SPObject::getId() const {
    return id;
}

/**
 * Accumulate this id and all it's descendants ids
 */
void SPObject::getIds(std::set<std::string> &ret) const {
    if (id) {
        ret.insert(std::string(id));
    }
    for (auto &child : children) {
        child.getIds(ret);
    }
}

/**
 * Returns the id as a url param, in the form 'url(#{id})'
 */
std::string SPObject::getUrl() const {
    if (id) {
        return std::string("url(#") + id + ")";
    }
    return "";
}

char const *SPObject::getLanguage() const
{
    if (_lang_attribute) {
        return _lang_attribute->c_str();
    }
    if (parent) {
        return parent->getLanguage();
    }
    return "";
}

Inkscape::XML::Node * SPObject::getRepr() {
    return repr;
}

Inkscape::XML::Node const* SPObject::getRepr() const{
    return repr;
}


SPObject *sp_object_ref(SPObject *object, SPObject *owner)
{
    g_return_val_if_fail(object != nullptr, NULL);

    Inkscape::Debug::EventTracker<RefEvent> tracker(object);

    object->refCount++;

    return object;
}

SPObject *sp_object_unref(SPObject *object, SPObject *owner)
{
    g_return_val_if_fail(object != nullptr, NULL);

    Inkscape::Debug::EventTracker<UnrefEvent> tracker(object);

    object->refCount--;

    if (object->refCount <= 0) {
        delete object;
    }

    return nullptr;
}

void SPObject::hrefObject(SPObject* owner)
{
    // if (owner) std::cout << "  owner: " << *owner << std::endl;

    // If owner is a clone, do not increase hrefcount, it's already href'ed by original.
    if (!owner || !owner->cloned) {
        hrefcount++;
        _updateTotalHRefCount(1);
    }

    if(owner)
        hrefList.push_front(owner);
}

void SPObject::unhrefObject(SPObject* owner)
{
    if (!owner || !owner->cloned) {
        g_return_if_fail(hrefcount > 0);

        hrefcount--;
        _updateTotalHRefCount(-1);
    }

    if(owner)
        hrefList.remove(owner);
}

void SPObject::_updateTotalHRefCount(int increment) {
    SPObject *topmost_collectable = nullptr;
    for ( SPObject *iter = this ; iter ; iter = iter->parent ) {
        iter->_total_hrefcount += increment;
        if ( iter->_total_hrefcount < iter->hrefcount ) {
            g_critical("HRefs overcounted");
        }
        if ( iter->_total_hrefcount == 0 &&
             iter->_collection_policy != COLLECT_WITH_PARENT )
        {
            topmost_collectable = iter;
        }
    }
    if (topmost_collectable) {
        topmost_collectable->requestOrphanCollection();
    }
}

void SPObject::getLinked(std::vector<SPObject *> &objects, LinkedObjectNature direction) const
{
    if (direction == LinkedObjectNature::ANY || direction == LinkedObjectNature::DEPENDENT) {
        // href list is all back links
        for (auto linked : hrefList) {
            objects.push_back(linked);
        }
    }
}

bool SPObject::isAncestorOf(SPObject const *object) const
{
    g_return_val_if_fail(object != nullptr, false);
    object = object->parent;
    while (object) {
        if ( object == this ) {
            return true;
        }
        object = object->parent;
    }
    return false;
}

SPObject const *SPObject::nearestCommonAncestor(SPObject const *object) const {
    g_return_val_if_fail(object != nullptr, NULL);

    using Inkscape::Algorithms::nearest_common_ancestor;
    return nearest_common_ancestor<SPObject::ConstParentIterator>(this, object, nullptr);
}

static SPObject const *AncestorSon(SPObject const *obj, SPObject const *ancestor) {
    SPObject const *result = nullptr;
    if ( obj && ancestor ) {
        if (obj->parent == ancestor) {
            result = obj;
        } else {
            result = AncestorSon(obj->parent, ancestor);
        }
    }
    return result;
}

int sp_object_compare_position(SPObject const *first, SPObject const *second)
{
    int result = 0;
    if (first != second) {
        SPObject const *ancestor = first->nearestCommonAncestor(second);
        // Need a common ancestor to be able to compare
        if ( ancestor ) {
            // we have an object and its ancestor (should not happen when sorting selection)
            if (ancestor == first) {
                result = -1;
            } else if (ancestor == second) {
                result = 1;
            } else {
                SPObject const *to_first = AncestorSon(first, ancestor);
                SPObject const *to_second = AncestorSon(second, ancestor);

                g_assert(to_second->parent == to_first->parent);

                result = sp_repr_compare_position(to_first->getRepr(), to_second->getRepr());
            }
        }
    }
    return result;
}

bool sp_object_compare_position_bool(SPObject const *first, SPObject const *second){
    return sp_object_compare_position(first,second)<0;
}

SPObject *SPObject::appendChildRepr(Inkscape::XML::Node *repr) {
    if ( !cloned ) {
        getRepr()->appendChild(repr);
        return document->getObjectByRepr(repr);
    } else {
        g_critical("Attempt to append repr as child of cloned object");
        return nullptr;
    }
}

void SPObject::setCSS(SPCSSAttr *css, gchar const *attr)
{
    g_assert(this->getRepr() != nullptr);
    sp_repr_css_set(this->getRepr(), css, attr);
}

void SPObject::changeCSS(SPCSSAttr *css, gchar const *attr)
{
    g_assert(this->getRepr() != nullptr);
    sp_repr_css_change(this->getRepr(), css, attr);
}

std::vector<SPObject*> SPObject::childList(bool add_ref, Action) {
    std::vector<SPObject*> l;
    for (auto& child: children) {
        if (add_ref) {
            sp_object_ref(&child);
        }
        l.push_back(&child);
    }
    return l;
}

std::vector<SPObject*> SPObject::ancestorList(bool root_to_tip)
{
    std::vector<SPObject *> ancestors;
    for (SPObject::ParentIterator iter=parent ; iter ; ++iter) {
        ancestors.push_back(iter);
    }
    if (root_to_tip) {
        std::reverse(ancestors.begin(), ancestors.end());
    }
    return ancestors;
}

gchar const *SPObject::label() const {
    return _label;
}

gchar const *SPObject::defaultLabel() const {
    if (_label) {
        return _label;
    } else {
        if (!_default_label) {
            if (getId()) {
                _default_label = g_strdup_printf("#%s", getId());
            } else if (getRepr()) {
                _default_label = g_strdup_printf("<%s>", getRepr()->name());
            } else {
                _default_label = g_strdup("Default label");
            }
        }
        return _default_label;
    }
}

void SPObject::setLabel(gchar const *label)
{
    getRepr()->setAttribute("inkscape:label", label);
    // Update anything that's watching the object's label
    _modified_signal.emit(this, SP_OBJECT_MODIFIED_FLAG);
}


void SPObject::requestOrphanCollection() {
    g_return_if_fail(document != nullptr);
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    // do not remove style or script elements (Bug #276244)
    if (is<SPStyleElem>(this)) {
        // leave it
    } else if (is<SPScript>(this)) {
        // leave it
    } else if (is<SPFont>(this)) {
        // leave it
    } else if (!prefs->getBool("/options/cleanupswatches/value", false) && is<SPPaintServer>(this) && static_cast<SPPaintServer*>(this)->isSwatch()) {
        // leave it
    } else if (is<Inkscape::ColorProfile>(this)) {
        // leave it
    } else if (is<SPPage>(this)) {
        // leave it
    } else if (is<LivePathEffectObject>(this)) {
        document->queueForOrphanCollection(this);
    } else {
        document->queueForOrphanCollection(this);

        /** \todo
         * This is a temporary hack added to make fill&stroke rebuild its
         * gradient list when the defs are vacuumed.  gradient-vector.cpp
         * listens to the modified signal on defs, and now we give it that
         * signal.  Mental says that this should be made automatic by
         * merging SPObjectGroup with SPObject; SPObjectGroup would issue
         * this signal automatically. Or maybe just derive SPDefs from
         * SPObjectGroup?
         */

        this->requestModified(SP_OBJECT_CHILD_MODIFIED_FLAG);
    }
}

void SPObject::_sendDeleteSignalRecursive() {
    for (auto& child: children) {
        child._delete_signal.emit(&child);
        child._sendDeleteSignalRecursive();
    }
}

void SPObject::deleteObject(bool propagate, bool propagate_descendants)
{
    sp_object_ref(this, nullptr);
    if (is<SPLPEItem>(this)) {
        cast<SPLPEItem>(this)->removeAllPathEffects(false, propagate_descendants);
    }
    if (propagate) {
        _delete_signal.emit(this);
    }
    if (propagate_descendants) {
        this->_sendDeleteSignalRecursive();
    }
    
    Inkscape::XML::Node *repr = getRepr();
    if (repr && repr->parent()) {
        sp_repr_unparent(repr);
    }

    if (_successor) {
        _successor->deleteObject(propagate, propagate_descendants);
    }
    sp_object_unref(this, nullptr);
}

void SPObject::cropToObject(SPObject *except)
{
    std::vector<SPObject *> toDelete;
    for (auto &child : children) {
        if (is<SPItem>(&child)) {
            if (child.isAncestorOf(except)) {
                child.cropToObject(except);
            } else if (&child != except) {
                sp_object_ref(&child, nullptr);
                toDelete.push_back(&child);
            }
        }
    }
    for (auto &i : toDelete) {
        i->deleteObject(true, true);
        sp_object_unref(i, nullptr);
    }
}

/**
 * Removes objects which are not related to given list of objects.
 *
 * @arg except_objects - A list of objects to crop around
 *
 * Use Case: Group[MyRect1 , MyRect2] , MyRect3
 * List Provided: MyRect1, MyRect3
 * Output doc: Group[MyRect1], MyRect3
 * List Provided: MyRect1, Group
 * Output doc: Group[MyRect1, MyRect2] (notice MyRect2 is not deleted as it is related to Group)
 */
void SPObject::cropToObjects(std::vector<SPObject *> except_objects)
{
    if (except_objects.empty()) {
        return;
    }
    std::vector<SPObject *> links;
    std::vector<SPObject *> toDelete;

    // Get a list of all forward links so we don't delete them
    for (auto item : except_objects) {
        item->getLinkedRecursive(links, LinkedObjectNature::DEPENDENCY);
    }

    except_objects.insert(except_objects.end(), links.begin(), links.end());

    // Collect a list of objects we expect to delete.
    getObjectsExcept(toDelete, except_objects);

    for (auto &i : toDelete) {
        // Don't propergate the delete signal as we may delete clones later
        i->deleteObject(false, false);
    }
}

void SPObject::getObjectsExcept(std::vector<SPObject *> &objects, const std::vector<SPObject *> &excepts)
{
    for (auto &child : children) {
        if (is<SPItem>(&child)) {
            int child_flag = 1;
            for (auto except : excepts) {
                if (&child == except) {
                    child_flag = 0;
                    break;
                }
                if (child.isAncestorOf(except)) {
                    child_flag = 2;
                }
            }
            if (child_flag == 1) {
                objects.push_back(&child);
            } else if (child_flag == 2) {
                child.getObjectsExcept(objects, excepts);
            }
        }
    }
}

void SPObject::getLinkedRecursive(std::vector<SPObject *> &objects, LinkedObjectNature direction) const
{
    // Recurse through multiple links
    for (auto link : getLinked(direction)) {
        // Make sure we never recurse objects multiple times.
        if (std::find(objects.begin(), objects.end(), link) == objects.end()) {
            objects.push_back(link);
            link->getLinkedRecursive(objects, direction);
        }
    }

    for (auto &child : children) {
        if (is<SPItem>(&child)) {
            // Recurse through all ancestors
            child.getLinkedRecursive(objects, direction);
        }
    }
}

void SPObject::attach(SPObject *object, SPObject *prev)
{
    g_return_if_fail(object != nullptr);
    g_return_if_fail(!prev || prev->parent == this);
    g_return_if_fail(!object->parent);

    sp_object_ref(object, this);
    object->parent = this;
    this->_updateTotalHRefCount(object->_total_hrefcount);

    auto it = children.begin();
    if (prev != nullptr) {
        it = ++children.iterator_to(*prev);
    }
    children.insert(it, *object);

    if (!object->xml_space.set)
        object->xml_space.value = this->xml_space.value;
}

void SPObject::reorder(SPObject* obj, SPObject* prev) {
    g_return_if_fail(obj != nullptr);
    g_return_if_fail(obj->parent);
    g_return_if_fail(obj->parent == this);
    g_return_if_fail(obj != prev);
    g_return_if_fail(!prev || prev->parent == obj->parent);

    auto it = children.begin();
    if (prev != nullptr) {
        it = ++children.iterator_to(*prev);
    }

    children.splice(it, children, children.iterator_to(*obj));
}

void SPObject::detach(SPObject *object)
{
    g_return_if_fail(object != nullptr);
    g_return_if_fail(object->parent == this);

    children.erase(children.iterator_to(*object));
    object->releaseReferences();

    object->parent = nullptr;

    this->_updateTotalHRefCount(-object->_total_hrefcount);
    sp_object_unref(object, this);
}

SPObject *SPObject::get_child_by_repr(Inkscape::XML::Node *repr)
{
    g_return_val_if_fail(repr != nullptr, NULL);
    SPObject *result = nullptr;

    if (children.size() > 0 && children.back().getRepr() == repr) {
        result = &children.back();   // optimization for common scenario
    } else {
        for (auto& child: children) {
            if (child.getRepr() == repr) {
                result = &child;
                break;
            }
        }
    }
    return result;
}

/**
 * Get closest child to a reference representation. May traverse backwards
 * until it finds a child SPObject node.
 *
 * @param obj Parent object
 * @param ref Reference node, may be NULL
 * @return Child, or NULL if not found
 */
static SPObject *get_closest_child_by_repr(SPObject &obj, Inkscape::XML::Node *ref)
{
    for (; ref; ref = ref->prev()) {
        // The most likely situation is that `ref` is indeed a child of `obj`,
        // so try that first, before checking getObjectByRepr.
        if (auto result = obj.get_child_by_repr(ref)) {
            return result;
        }

        // Only continue if `ref` is not an SPObject, but e.g. an XML comment
        if (obj.document->getObjectByRepr(ref)) {
            break;
        }
    }

    return nullptr;
}

void SPObject::child_added(Inkscape::XML::Node *child, Inkscape::XML::Node *ref) {
    SPObject* object = this;

    const std::string type_string = NodeTraits::get_type_string(*child);

    SPObject* ochild = SPFactory::createObject(type_string);
    if (ochild == nullptr) {
        // Currently, there are many node types that do not have
        // corresponding classes in the SPObject tree.
        // (rdf:RDF, inkscape:clipboard, ...)
        // Thus, simply ignore this case for now.
        return;
    }

    SPObject *prev = get_closest_child_by_repr(*object, ref);
    object->attach(ochild, prev);
    sp_object_unref(ochild, nullptr);

    ochild->invoke_build(object->document, child, object->cloned);
}

void SPObject::release() {
    SPObject* object = this;
    debug("id=%p, typename=%s", object, g_type_name_from_instance((GTypeInstance*)object));

    style->filter.clear();
    style->fill.href.reset();
    style->stroke.href.reset();
    style->shape_inside.clear();
    style->shape_subtract.clear();

    auto tmp = children | std::views::transform([] (SPObject &obj) { return &obj; });
    auto toRelease = std::vector<SPObject *>(tmp.begin(), tmp.end());

    for (auto& p: toRelease) {
        object->detach(p);
    }
}

void SPObject::remove_child(Inkscape::XML::Node* child) {
    debug("id=%p, typename=%s", this, g_type_name_from_instance((GTypeInstance*)this));

    SPObject *ochild = this->get_child_by_repr(child);

    // If the xml node has got a corresponding child in the object tree
    if (ochild) {
        this->detach(ochild);
    }
}

void SPObject::order_changed(Inkscape::XML::Node *child, Inkscape::XML::Node * /*old_ref*/, Inkscape::XML::Node *new_ref) {
    SPObject* object = this;

    SPObject *ochild = object->get_child_by_repr(child);
    g_return_if_fail(ochild != nullptr);
    SPObject *prev = get_closest_child_by_repr(*object, new_ref);
    object->reorder(ochild, prev);
    ochild->position_changed(prev);
}

/**
 * The order of this object has changed
 */
void SPObject::position_changed(SPObject *prev)
{
    _position_changed_signal.emit(this);
}

void SPObject::tag_name_changed(gchar const* oldname, gchar const* newname) {
    g_warning("XML Element renamed from %s to %s!", oldname, newname);
}

void SPObject::build(SPDocument *document, Inkscape::XML::Node *repr) {

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::build" );
#endif
    SPObject* object = this;

    /* Nothing specific here */
    debug("id=%p, typename=%s", object, g_type_name_from_instance((GTypeInstance*)object));

    object->readAttr(SPAttr::XML_SPACE);
    object->readAttr(SPAttr::LANG);
    object->readAttr(SPAttr::XML_LANG);   // "xml:lang" overrides "lang" per spec, read it last.
    object->readAttr(SPAttr::INKSCAPE_LABEL);
    object->readAttr(SPAttr::INKSCAPE_COLLECT);

    if(object->cloned && (repr->attribute("id")) ) // The cases where this happens are when the "original" has no id. This happens
                                                   // if it is a SPString (a TextNode, e.g. in a <title>), or when importing
                                                   // stuff externally modified to have no id. 
        object->clone_original = document->getObjectById(repr->attribute("id"));

    for (Inkscape::XML::Node *rchild = repr->firstChild() ; rchild != nullptr; rchild = rchild->next()) {
        const std::string typeString = NodeTraits::get_type_string(*rchild);

        SPObject* child = SPFactory::createObject(typeString);
        if (child == nullptr) {
            // Currently, there are many node types that do not have
            // corresponding classes in the SPObject tree.
            // (rdf:RDF, inkscape:clipboard, ...)
            // Thus, simply ignore this case for now.
            continue;
        }

        object->attach(child, object->lastChild());
        sp_object_unref(child, nullptr);
        child->invoke_build(document, rchild, object->cloned);
    }

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::build", false );
#endif
}

void SPObject::invoke_build(SPDocument *document, Inkscape::XML::Node *repr, unsigned int cloned)
{
#ifdef OBJECT_TRACE
    objectTrace( "SPObject::invoke_build" );
#endif
    debug("id=%p, typename=%s", this, g_type_name_from_instance((GTypeInstance*)this));

    g_assert(document != nullptr);
    g_assert(repr != nullptr);

    g_assert(this->document == nullptr);
    g_assert(this->repr == nullptr);
    g_assert(this->getId() == nullptr);

    /* Bookkeeping */

    this->document = document;
    this->repr = repr;
    if (!cloned) {
        Inkscape::GC::anchor(repr);
    }
    this->cloned = cloned;

    /* Invoke derived methods, if any */
    this->build(document, repr);

    if ( !cloned ) {
        this->document->bindObjectToRepr(this->repr, this);

        if (Inkscape::XML::id_permitted(this->repr)) {
            /* If we are not cloned, and not seeking, force unique id */
            gchar const *id = this->repr->attribute("id");
            if (!document->isSeeking()) {
                auto realid = generate_unique_id(id);
                this->document->bindObjectToId(realid.c_str(), this);
                SPObjectImpl::setId(this, realid.c_str());

                /* Redefine ID, if required */
                if (!id || std::strcmp(id, getId()) != 0) {
                    this->repr->setAttribute("id", getId());
                }
            } else if (id) {
                // bind if id, but no conflict -- otherwise, we can expect
                // a subsequent setting of the id attribute
                if (!this->document->getObjectById(id)) {
                    this->document->bindObjectToId(id, this);
                    SPObjectImpl::setId(this, id);
                }
            }
        }
    } else {
        g_assert(this->getId() == nullptr);
    }

    this->document->process_pending_resource_changes();

    /* Signalling (should be connected AFTER processing derived methods */
    repr->addObserver(*this);

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::invoke_build", false );
#endif
}

int SPObject::getIntAttribute(char const *key, int def)
{
    return getRepr()->getAttributeInt(key, def);
}

unsigned SPObject::getPosition(){
    g_assert(this->repr);

    return repr->position();
}

void SPObject::appendChild(Inkscape::XML::Node *child) {
    g_assert(this->repr);

    repr->appendChild(child);
}

SPObject* SPObject::nthChild(unsigned index) {
    g_assert(this->repr);
    if (hasChildren()) {
        std::vector<SPObject*> l;
        unsigned counter = 0;
        for (auto& child: children) {
            if (counter == index) {
                return &child;
            }
            counter++;
        }
    }
    return nullptr;
}

void SPObject::addChild(Inkscape::XML::Node *child, Inkscape::XML::Node * prev)
{
    g_assert(this->repr);

    repr->addChild(child,prev);
}

void SPObject::releaseReferences() {
    g_assert(this->document);
    g_assert(this->repr);
    g_assert(cloned || repr->_anchored_refcount() > 0);

    repr->removeObserver(*this);

    this->_release_signal.emit(this);

    this->release();

    /* all hrefs should be released by the "release" handlers */
    g_assert(this->hrefcount == 0);

    if (!cloned) {
        if (this->id) {
            this->document->bindObjectToId(this->id, nullptr);
        }
        g_free(this->id);
        this->id = nullptr;

        g_free(this->_default_label);
        this->_default_label = nullptr;

        this->document->bindObjectToRepr(this->repr, nullptr);

        Inkscape::GC::release(this->repr);
    } else {
        g_assert(!this->id);
    }

    this->document = nullptr;
    this->repr = nullptr;
}

SPObject *SPObject::getPrev()
{
    SPObject *prev = nullptr;
    if (parent && !parent->children.empty() && &parent->children.front() != this) {
        prev = &*(--parent->children.iterator_to(*this));
    }
    return prev;
}

SPObject* SPObject::getNext()
{
    SPObject *next = nullptr;
    if (parent && !parent->children.empty() && &parent->children.back() != this) {
        next = &*(++parent->children.iterator_to(*this));
    }
    return next;
}

void SPObject::notifyChildAdded(Inkscape::XML::Node &node, Inkscape::XML::Node &child, Inkscape::XML::Node *ref)
{
    child_added(&child, ref);
}

void SPObject::notifyChildRemoved(Inkscape::XML::Node &, Inkscape::XML::Node &child, Inkscape::XML::Node *)
{
    remove_child(&child);
}

void SPObject::notifyChildOrderChanged(Inkscape::XML::Node &, Inkscape::XML::Node &child, Inkscape::XML::Node *old_prev,
                                       Inkscape::XML::Node *new_prev)
{
    order_changed(&child, old_prev, new_prev);
}

void SPObject::notifyElementNameChanged(Inkscape::XML::Node &node, GQuark old_name, GQuark new_name)
{
    auto const oldname = g_quark_to_string(old_name);
    auto const newname = g_quark_to_string(new_name);

    tag_name_changed(oldname, newname);
}

void SPObject::set(SPAttr key, gchar const* value) {
#ifdef OBJECT_TRACE
    std::stringstream temp;
    temp << "SPObject::set: " << sp_attribute_name(key)  << " " << (value?value:"null");
    objectTrace( temp.str() );
#endif

    g_assert(key != SPAttr::INVALID);

    SPObject* object = this;

    switch (key) {

        case SPAttr::ID:

            //XML Tree being used here.
            if ( !object->cloned && object->getRepr()->type() == Inkscape::XML::NodeType::ELEMENT_NODE ) {
                SPDocument *document=object->document;
                SPObject *conflict=nullptr;

                gchar const *new_id = value;

                if (new_id) {
                    conflict = document->getObjectById((char const *)new_id);
                }

                if ( conflict && conflict != object ) {
                    if (!document->isSeeking()) {
                        sp_object_ref(conflict, nullptr);
                        // give the conflicting object a new ID
                        auto new_conflict_id = conflict->generate_unique_id();
                        conflict->setAttribute("id", new_conflict_id);
                        sp_object_unref(conflict, nullptr);
                    } else {
                        new_id = nullptr;
                    }
                }

                if (object->getId()) {
                    document->bindObjectToId(object->getId(), nullptr);
                    SPObjectImpl::setId(object, nullptr);
                }

                if (new_id) {
                    SPObjectImpl::setId(object, new_id);
                    document->bindObjectToId(object->getId(), object);
                }

                g_free(object->_default_label);
                object->_default_label = nullptr;
            }
            break;

        case SPAttr::INKSCAPE_LABEL:
            g_free(object->_label);
            if (value) {
                object->_label = g_strdup(value);
            } else {
                object->_label = nullptr;
            }
            g_free(object->_default_label);
            object->_default_label = nullptr;
            break;

        case SPAttr::INKSCAPE_COLLECT:
            if ( value && !std::strcmp(value, "always") ) {
                object->setCollectionPolicy(SPObject::ALWAYS_COLLECT);
            } else {
                object->setCollectionPolicy(SPObject::COLLECT_WITH_PARENT);
            }
            break;

        case SPAttr::XML_SPACE:
            if (value && !std::strcmp(value, "preserve")) {
                object->xml_space.value = SP_XML_SPACE_PRESERVE;
                object->xml_space.set = TRUE;
            } else if (value && !std::strcmp(value, "default")) {
                object->xml_space.value = SP_XML_SPACE_DEFAULT;
                object->xml_space.set = TRUE;
            } else if (object->parent) {
                SPObject *parent;
                parent = object->parent;
                object->xml_space.value = parent->xml_space.value;
            }
            object->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
            break;

        case SPAttr::LANG:
        case SPAttr::XML_LANG: {
            // Note: An empty string _is_ valid for `xml:lang` and `lang`
            // `xml:lang` takes precedence
            if (key == SPAttr::LANG && getAttribute("xml:lang")) {
                break;
            }
            gchar const *lang_attr_value = value;
            if (key == SPAttr::XML_LANG && !lang_attr_value && !is<SPGlyph>(this)) {
                // Use `lang` only if `xml:lang` is not set.
                // Also SVG 1.1 <glyph> has a different spec for the `lang` attribute that should not be used.
                lang_attr_value = getAttribute("lang");
            }
            _lang_attribute = lang_attr_value ? std::make_optional(lang_attr_value) : std::nullopt;
            // To do: sanity check
            requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
        } break;

        case SPAttr::STYLE:
            object->style->readFromObject( object );
            object->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
            break;

        default:
            break;
    }

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::set", false );
#endif
}

void SPObject::setKeyValue(SPAttr key, gchar const *value)
{
    this->set(key, value);
}

void SPObject::readAttr(SPAttr keyid)
{
    if (keyid == SPAttr::XLINK_HREF) {
        auto value = Inkscape::getHrefAttribute(*getRepr()).second;
        setKeyValue(keyid, value);
        return;
    }

    char const *key = sp_attribute_name(keyid);

    assert(key != nullptr);
    assert(getRepr() != nullptr);

    char const *value = getRepr()->attribute(key);

    setKeyValue(keyid, value);
}

void SPObject::readAttr(gchar const *key)
{
    g_assert(key != nullptr);

    //XML Tree being used here.
    g_assert(this->getRepr() != nullptr);

    auto keyid = sp_attribute_lookup(key);
    if (keyid != SPAttr::INVALID) {
        /* Retrieve the 'key' attribute from the object's XML representation */
        gchar const *value = getRepr()->attribute(key);

        setKeyValue(keyid, value);
    }
}

void SPObject::notifyAttributeChanged(Inkscape::XML::Node &, GQuark key_, Util::ptr_shared, Util::ptr_shared)
{
    auto const key = g_quark_to_string(key_);
    readAttr(key);
    if (document && document->getReprRoot() == getRepr() &&
        (!std::strcmp(key, "proof:color-mode") || !std::strcmp(key, "proof:color-profile"))) {
        Inkscape::Colors::DocumentColors::assignmentChanged(document);
    }
}

void SPObject::notifyContentChanged(Inkscape::XML::Node &, Util::ptr_shared, Util::ptr_shared)
{
    read_content();
}

/**
 * Return string representation of space value.
 */
static gchar const *sp_xml_get_space_string(unsigned int space)
{
    switch (space) {
        case SP_XML_SPACE_DEFAULT:
            return "default";
        case SP_XML_SPACE_PRESERVE:
            return "preserve";
        default:
            return nullptr;
    }
}

Inkscape::XML::Node* SPObject::write(Inkscape::XML::Document *doc, Inkscape::XML::Node *repr, guint flags) {
#ifdef OBJECT_TRACE
    objectTrace( "SPObject::write" );
#endif

    if (!repr && (flags & SP_OBJECT_WRITE_BUILD)) {
        repr = this->getRepr()->duplicate(doc);
        if (!( flags & SP_OBJECT_WRITE_EXT )) {
            repr->removeAttribute("inkscape:collect");
        }
    } else if (repr) {
        repr->setAttribute("id", this->getId());

        if (this->xml_space.set) {
            char const *xml_space;
            xml_space = sp_xml_get_space_string(this->xml_space.value);
            repr->setAttribute("xml:space", xml_space);
        }

        if (_lang_attribute) {
            repr->setAttribute("xml:lang", *_lang_attribute);
        } else {
            repr->removeAttribute("xml:lang");
            // SVG 1.1 <glyph> has a different spec for the `lang` attribute, so don't touch that.
            if (!is<SPGlyph>(this)) {
                // Only remove `lang` if removing `xml:lang`.
                // We can leave `lang` otherwise. It does not matter since `xml:lang` takes precedence.
                repr->removeAttribute("lang");
            }
        }

        if ( flags & SP_OBJECT_WRITE_EXT &&
             this->collectionPolicy() == SPObject::ALWAYS_COLLECT )
        {
            repr->setAttribute("inkscape:collect", "always");
        } else {
            repr->removeAttribute("inkscape:collect");
        }

        if (style) {
            // Write if property set by style attribute in this object
            Glib::ustring style_prop = style->write(SPStyleSrc::STYLE_PROP);

            // Write style attributes (SPStyleSrc::ATTRIBUTE) back to xml object
            bool any_written = false;
            auto properties = style->properties();
            for (auto * prop : properties) {
                if (prop->shall_write(SP_STYLE_FLAG_IFSET | SP_STYLE_FLAG_IFSRC, SPStyleSrc::ATTRIBUTE)) {
                    // WARNING: We don't know for sure if the css names are the same as the attribute names
                    auto val = repr->attribute(prop->name().c_str());
                    auto new_val = prop->get_value();
                    if (new_val.empty() && !val || new_val != val) {
                        repr->setAttributeOrRemoveIfEmpty(prop->name(), new_val);
                        any_written = true;
                    }
                }
            }
            if(any_written) {
                // We need to ask the object to update the style and keep things in sync
                // see `case SPAttr::STYLE` above for how the style attr itself does this.
                style->readFromObject(this);
                requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
            }

            // Check for valid attributes. This may be time consuming.
            // It is useful, though, for debugging Inkscape code.
            Inkscape::Preferences *prefs = Inkscape::Preferences::get();
            if( prefs->getBool("/options/svgoutput/check_on_editing") ) {
                // Check styles only if it is an element node
                if (repr->type() == Inkscape::XML::NodeType::ELEMENT_NODE) {
                    unsigned int flags = sp_attribute_clean_get_prefs();
                    style_prop = sp_attribute_clean_style(repr, style_prop.c_str(), flags);
                }
            }

            repr->setAttributeOrRemoveIfEmpty("style", style_prop);
        } else {
            /** \todo I'm not sure what to do in this case.  Bug #1165868
             * suggests that it can arise, but the submitter doesn't know
             * how to do so reliably.  The main two options are either
             * leave repr's style attribute unchanged, or explicitly clear it.
             * Must also consider what to do with property attributes for
             * the element; see below.
             */
            char const *style_str = repr->attribute("style");
            if (!style_str) {
                style_str = "NULL";
            }
            g_warning("Item's style is NULL; repr style attribute is %s", style_str);
        }
    }

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::write", false );
#endif
    return repr;
}

/**
* Indicates that another object supercedes this one.
* Used by duple and stamp to keep references of LPE
*/
void 
SPObject::setTmpSuccessor(SPObject *tmpsuccessor) {
    assert(tmpsuccessor != NULL);
    assert(_tmpsuccessor == NULL);
    assert(tmpsuccessor->_tmpsuccessor == NULL);
    sp_object_ref(tmpsuccessor, nullptr);
    _tmpsuccessor = tmpsuccessor;
    if (repr) {
        char const *linked_fill_id = getAttribute("inkscape:linked-fill");
        if (linked_fill_id && document) {
            SPObject *lfill = document->getObjectById(linked_fill_id);
            if (lfill && lfill->_tmpsuccessor) {
                lfill->_tmpsuccessor->setAttribute("inkscape:linked-fill",lfill->_tmpsuccessor->getId());
            }
        }

        if (children.size() == _tmpsuccessor->children.size()) {
            for (auto &obj : children) {
                auto tmpsuccessorchild = _tmpsuccessor->nthChild(obj.getPosition());
                if (tmpsuccessorchild && !obj._tmpsuccessor) {
                    obj.setTmpSuccessor(tmpsuccessorchild);
                }
            }
        }
    }
}

/**
* Fix temporary successors in duple stamp.
*/
void 
SPObject::fixTmpSuccessors() {
    for (auto &obj : children) {
        obj.fixTmpSuccessors();
    }
    if (_tmpsuccessor) {
        char const *linked_fill_id = getAttribute("inkscape:linked-fill");
        if (linked_fill_id && document) {
            SPObject *lfill = document->getObjectById(linked_fill_id);
            if (lfill && lfill->_tmpsuccessor) {
                _tmpsuccessor->setAttribute("inkscape:linked-fill", lfill->_tmpsuccessor->getId());
            }
        }
    }
}

void 
SPObject::unsetTmpSuccessor() {
    for (auto &object : children) {
        object.unsetTmpSuccessor();
    }
    if (_tmpsuccessor) {
        sp_object_unref(_tmpsuccessor, nullptr);
        _tmpsuccessor = nullptr;
    }
}

/**
* Returns ancestor non layer.
*/
SPObject const * SPObject::getTopAncestorNonLayer() const {
    auto group = cast<SPGroup>(parent);
    if (group && group->layerMode() != SPGroup::LAYER) {
        return group->getTopAncestorNonLayer();
    } else {
        return this;
    }
};


Inkscape::XML::Node * SPObject::updateRepr(unsigned int flags)
{
#ifdef OBJECT_TRACE
    objectTrace( "SPObject::updateRepr 1" );
#endif

    if ( !cloned ) {
        Inkscape::XML::Node *repr = getRepr();
        if (repr) {
#ifdef OBJECT_TRACE
            objectTrace( "SPObject::updateRepr 1", false );
#endif
            return updateRepr(repr->document(), repr, flags);
        } else {
            g_critical("Attempt to update non-existent repr");
#ifdef OBJECT_TRACE
            objectTrace( "SPObject::updateRepr 1", false );
#endif
            return nullptr;
        }
    } else {
        /* cloned objects have no repr */
#ifdef OBJECT_TRACE
        objectTrace( "SPObject::updateRepr 1", false );
#endif
        return nullptr;
    }
}

Inkscape::XML::Node * SPObject::updateRepr(Inkscape::XML::Document *doc, Inkscape::XML::Node *repr, unsigned int flags)
{
#ifdef OBJECT_TRACE
    objectTrace( "SPObject::updateRepr 2" );
#endif

    g_assert(doc != nullptr);

    if (cloned) {
        /* cloned objects have no repr */
#ifdef OBJECT_TRACE
        objectTrace( "SPObject::updateRepr 2", false );
#endif
        return nullptr;
    }

    if (!(flags & SP_OBJECT_WRITE_BUILD) && !repr) {
        repr = getRepr();
    }

#ifdef OBJECT_TRACE
    Inkscape::XML::Node *node = write(doc, repr, flags);
    objectTrace( "SPObject::updateRepr 2", false );
    return node;
#else
    return this->write(doc, repr, flags);
#endif

}

/* Modification */

void SPObject::requestDisplayUpdate(unsigned int flags)
{
    g_return_if_fail( this->document != nullptr );

#ifndef NDEBUG
    // expect no nested update calls
    if (document->update_in_progress) {
        // observed with LPE on <rect>
        g_warning("WARNING: Requested update while update in progress, counter = %d", document->update_in_progress);
    }
#endif

    /* requestModified must be used only to set one of SP_OBJECT_MODIFIED_FLAG or
     * SP_OBJECT_CHILD_MODIFIED_FLAG */
    g_return_if_fail(!(flags & SP_OBJECT_PARENT_MODIFIED_FLAG));
    g_return_if_fail((flags & SP_OBJECT_MODIFIED_FLAG) || (flags & SP_OBJECT_CHILD_MODIFIED_FLAG));
    g_return_if_fail(!((flags & SP_OBJECT_MODIFIED_FLAG) && (flags & SP_OBJECT_CHILD_MODIFIED_FLAG)));

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::requestDisplayUpdate" );
#endif

    bool already_propagated = (!(this->uflags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG)));
    //https://stackoverflow.com/a/7841333
    if ((this->uflags & flags) !=  flags ) {
        this->uflags |= flags;
    }
    /* If requestModified has already been called on this object or one of its children, then we
     * don't need to set CHILD_MODIFIED on our ancestors because it's already been done.
     */
    if (already_propagated) {
        if(this->document) {
            if (parent) {
                parent->requestDisplayUpdate(SP_OBJECT_CHILD_MODIFIED_FLAG);
            } else {
                this->document->requestModified();
            }
        }
    }

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::requestDisplayUpdate", false );
#endif

}

void SPObject::updateDisplay(SPCtx *ctx, unsigned int flags)
{
    g_return_if_fail(!(flags & ~SP_OBJECT_MODIFIED_CASCADE));

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::updateDisplay" );
#endif

    assert(++(document->update_in_progress));

#ifdef SP_OBJECT_DEBUG_CASCADE
    g_print("Update %s:%s %x %x %x\n", g_type_name_from_instance((GTypeInstance *) this), getId(), flags, this->uflags, this->mflags);
#endif

    /* Get this flags */
    flags |= this->uflags;
    /* Copy flags to modified cascade for later processing */
    this->mflags |= this->uflags;
    /* We have to clear flags here to allow rescheduling update */
    this->uflags = 0;

    // Merge style if we have good reasons to think that parent style is changed */
    /** \todo
     * I am not sure whether we should check only propagated
     * flag. We are currently assuming that style parsing is
     * done immediately. I think this is correct (Lauris).
     */
    if (style) {
        style->block_filter_bbox_updates = true;
        if ((flags & SP_OBJECT_STYLESHEET_MODIFIED_FLAG)) {
            style->readFromObject(this);
        } else if (parent && (flags & SP_OBJECT_STYLE_MODIFIED_FLAG) && (flags & SP_OBJECT_PARENT_MODIFIED_FLAG)) {
            style->cascade( this->parent->style );
        }
        style->block_filter_bbox_updates = false;
    }

    try {
        update(ctx, flags);
    } catch (...) {
        /** \todo
        * in case of catching an exception we need to inform the user somehow that the document is corrupted
        * maybe by implementing an document flag documentOk
        * or by a modal error dialog
        */
        g_warning("SPObject::updateDisplay: uncaught exception");
    }

    assert((document->update_in_progress)--);

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::updateDisplay", false );
#endif
}

void SPObject::requestModified(unsigned int flags)
{
    g_return_if_fail( this->document != nullptr );

    /* requestModified must be used only to set one of SP_OBJECT_MODIFIED_FLAG or
     * SP_OBJECT_CHILD_MODIFIED_FLAG */
    g_return_if_fail(!(flags & SP_OBJECT_PARENT_MODIFIED_FLAG));
    g_return_if_fail((flags & SP_OBJECT_MODIFIED_FLAG) || (flags & SP_OBJECT_CHILD_MODIFIED_FLAG));
    g_return_if_fail(!((flags & SP_OBJECT_MODIFIED_FLAG) && (flags & SP_OBJECT_CHILD_MODIFIED_FLAG)));

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::requestModified" );
#endif

    bool already_propagated = (!(this->mflags & (SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG)));

    this->mflags |= flags;

    /* If requestModified has already been called on this object or one of its children, then we
     * don't need to set CHILD_MODIFIED on our ancestors because it's already been done.
     */
    if (already_propagated) {
        if (parent) {
            parent->requestModified(SP_OBJECT_CHILD_MODIFIED_FLAG);
        } else {
            document->requestModified();
        }
    }
#ifdef OBJECT_TRACE
    objectTrace( "SPObject::requestModified", false );
#endif
}

void SPObject::emitModified(unsigned int flags)
{
    /* only the MODIFIED_CASCADE flag is legal here */
    g_return_if_fail(!(flags & ~SP_OBJECT_MODIFIED_CASCADE));

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::emitModified", true, flags );
#endif

#ifdef SP_OBJECT_DEBUG_CASCADE
    g_print("Modified %s:%s %x %x %x\n", g_type_name_from_instance((GTypeInstance *) this), getId(), flags, this->uflags, this->mflags);
#endif

    flags |= this->mflags;
    /* We have to clear mflags beforehand, as signal handlers may
     * make changes and therefore queue new modification notifications
     * themselves. */
    this->mflags = 0;

    sp_object_ref(this);

    this->modified(flags);

    _modified_signal.emit(this, flags);
    sp_object_unref(this);

#ifdef OBJECT_TRACE
    objectTrace( "SPObject::emitModified", false );
#endif
}

gchar const *SPObject::getTagName() const
{
    g_assert(repr != nullptr);

    /// \todo fixme: Exception if object is NULL? */
    //XML Tree being used here.
    return getRepr()->name();
}

gchar const *SPObject::getAttribute(gchar const *key) const
{
    g_assert(this->repr != nullptr);

    /// \todo fixme: Exception if object is NULL? */
    //XML Tree being used here.
    return (gchar const *) getRepr()->attribute(key);
}

void SPObject::setAttribute(Inkscape::Util::const_char_ptr key,
                            Inkscape::Util::const_char_ptr value)
{
    g_assert(this->repr != nullptr);

    /// \todo fixme: Exception if object is NULL? */
    //XML Tree being used here.
    getRepr()->setAttribute(key, value);
}

void SPObject::setAttributeDouble(Inkscape::Util::const_char_ptr key, double value) {
    Inkscape::CSSOStringStream os;
    os << value;
    setAttribute(key, os.str());
}
double SPObject::getAttributeDouble(Inkscape::Util::const_char_ptr key, double def)
{
    return getRepr()->getAttributeDouble(key, def);
}

void SPObject::removeAttribute(gchar const *key)
{
    /// \todo fixme: Exception if object is NULL? */
    //XML Tree being used here.
    getRepr()->removeAttribute(key);
}

bool SPObject::storeAsDouble( gchar const *key, double *val ) const
{
    g_assert(this->getRepr()!= nullptr);
    double nan = std::numeric_limits<double>::quiet_NaN();
    double temp_val = ((Inkscape::XML::Node *)(this->getRepr()))->getAttributeDouble(key, nan);
    if (std::isnan(temp_val)) {
        return false;
    }
    *val = temp_val;
    return true;
}

std::string SPObject::generate_unique_id(char const *default_id) const
{
    if (default_id && !document->getObjectById(default_id)) {
        return default_id;
    }

    //XML Tree being used here.
    auto name = repr->name();
    g_assert(name);

    if (auto local = std::strchr(name, ':')) {
        name = local + 1;
    }

    return document->generate_unique_id(name);
}

void SPObject::_requireSVGVersion(Inkscape::Version version) {
    for (SPObject::ParentIterator iter = this; iter; ++iter) {
        if (auto root = cast<SPRoot>(static_cast<SPObject *>(iter))) {
            if (root->svg_version < version) {
                root->svg_version = version;
            }
        }
    }
}

// Titles and descriptions

/* Note:
   Titles and descriptions are stored in 'title' and 'desc' child elements
   (see section 5.4 of the SVG 1.0 and 1.1 specifications).  The spec allows
   an element to have more than one 'title' child element, but strongly
   recommends against this and requires using the first one if a choice must
   be made.  The same applies to 'desc' elements.  Therefore, these functions
   ignore all but the first 'title' child element and first 'desc' child
   element, except when deleting a title or description.

   This will change in SVG 2, where multiple 'title' and 'desc' elements will
   be allowed with different localized strings.
*/

gchar * SPObject::title() const
{
    return getTitleOrDesc("svg:title");
}

bool SPObject::setTitle(gchar const *title, bool verbatim)
{
    return setTitleOrDesc(title, "svg:title", verbatim);
}

gchar * SPObject::desc() const
{
    return getTitleOrDesc("svg:desc");
}

bool SPObject::setDesc(gchar const *desc, bool verbatim)
{
    return setTitleOrDesc(desc, "svg:desc", verbatim);
}

char * SPObject::getTitleOrDesc(gchar const *svg_tagname) const
{
    char *result = nullptr;
    SPObject *elem = findFirstChild(svg_tagname);
    if ( elem ) {
        //This string copy could be avoided by changing 
        //the return type of SPObject::getTitleOrDesc 
        //to std::unique_ptr<Glib::ustring>
        result = g_strdup(elem->textualContent().c_str());
    }
    return result;
}

bool SPObject::setTitleOrDesc(gchar const *value, gchar const *svg_tagname, bool verbatim)
{
    if (!verbatim) {
        // If the new title/description is just whitespace,
        // treat it as though it were NULL.
        if (value) {
            bool just_whitespace = true;
            for (const gchar *cp = value; *cp; ++cp) {
                if (!std::strchr("\r\n \t", *cp)) {
                    just_whitespace = false;
                    break;
                }
            }
            if (just_whitespace) {
                value = nullptr;
            }
        }
        // Don't stomp on mark-up if there is no real change.
        if (value) {
            gchar *current_value = getTitleOrDesc(svg_tagname);
            if (current_value) {
                bool different = std::strcmp(current_value, value);
                g_free(current_value);
                if (!different) {
                    return false;
                }
            }
        }
    }

    SPObject *elem = findFirstChild(svg_tagname);

    if (value == nullptr) {
        if (elem == nullptr) {
            return false;
        }
        // delete the title/description(s)
        while (elem) {
            elem->deleteObject();
            elem = findFirstChild(svg_tagname);
        }
        return true;
    }

    Inkscape::XML::Document *xml_doc = document->getReprDoc();

    if (!elem) {
        // create a new 'title' or 'desc' element, putting it at the
        // beginning (in accordance with the spec's recommendations)
        Inkscape::XML::Node *xml_elem = xml_doc->createElement(svg_tagname);
        repr->addChild(xml_elem, nullptr);
        elem = document->getObjectByRepr(xml_elem);
        Inkscape::GC::release(xml_elem);
    }
    else {
        // remove the current content of the 'text' or 'desc' element
        auto tmp = elem->children | std::views::transform([] (SPObject &obj) { return &obj; });
        std::vector<SPObject*> vec(tmp.begin(), tmp.end());
        for (auto &child: vec) {
            child->deleteObject();
        }
    }

    // add the new content
    elem->appendChildRepr(xml_doc->createTextNode(value));
    return true;
}

SPObject* SPObject::findFirstChild(gchar const *tagname) const
{
    for (auto& child: const_cast<SPObject*>(this)->children)
    {
        if (child.repr->type() == Inkscape::XML::NodeType::ELEMENT_NODE &&
            !std::strcmp(child.repr->name(), tagname)) {
            return &child;
        }
    }
    return nullptr;
}

Glib::ustring SPObject::textualContent() const
{
    Glib::ustring text;

    for (auto& child: children)
    {
        Inkscape::XML::NodeType child_type = child.repr->type();

        if (child_type == Inkscape::XML::NodeType::ELEMENT_NODE) {
            text += child.textualContent();
        }
        else if (child_type == Inkscape::XML::NodeType::TEXT_NODE) {
            text += child.repr->content();
        }
    }
    return text;
}

Glib::ustring SPObject::getExportFilename() const
{
    if (auto filename = repr->attribute("inkscape:export-filename")) {
        return Glib::ustring(filename);
    }
    return "";
}

void SPObject::setExportFilename(Glib::ustring filename)
{
    // Is this svg has been saved before.
    const char *doc_filename = document->getDocumentFilename();
    std::string base = Glib::path_get_dirname(doc_filename ? doc_filename : filename.c_str());

    filename = Inkscape::optimizePath(filename, base);
    repr->setAttributeOrRemoveIfEmpty("inkscape:export-filename", filename.c_str());
}

Geom::Point SPObject::getExportDpi() const
{
    return Geom::Point(
        repr->getAttributeDouble("inkscape:export-xdpi", 0.0),
        repr->getAttributeDouble("inkscape:export-ydpi", 0.0));
}

void SPObject::setExportDpi(Geom::Point dpi)
{
    if (!dpi.x() || !dpi.y()) {
        repr->removeAttribute("inkscape:export-xdpi");
        repr->removeAttribute("inkscape:export-ydpi");
    } else {
        repr->setAttributeSvgDouble("inkscape:export-xdpi", dpi.x());
        repr->setAttributeSvgDouble("inkscape:export-ydpi", dpi.y());
    }
}

// For debugging: Print SP tree structure.
void SPObject::recursivePrintTree( unsigned level )
{
    if (level == 0) {
        std::cout << "SP Object Tree" << std::endl;
    }
    std::cout << "SP: ";
    for (unsigned i = 0; i < level; ++i) {
        std::cout << "  ";
    }
    std::cout << (getId()?getId():"No object id")
              << " clone: " << std::boolalpha << (bool)cloned
              << " hrefcount: " << hrefcount << std::endl;
    for (auto& child: children) {
        child.recursivePrintTree(level + 1);
    }
}

// Function to allow tracing of program flow through SPObject and derived classes.
// To trace function, add at entrance ('in' = true) and exit of function ('in' = false).
void SPObject::objectTrace( std::string const &text, bool in, unsigned flags ) {
    if( in ) {
        for (unsigned i = 0; i < indent_level; ++i) {
            std::cout << "  ";
        }
        std::cout << text << ":"
                  << " entrance: "
                  << (id?id:"null")
                  // << " uflags: " << uflags
                  // << " mflags: " << mflags
                  // << " flags: " << flags
                  << std::endl;
        ++indent_level;
    } else {
        --indent_level;
        for (unsigned i = 0; i < indent_level; ++i) {
            std::cout << "  ";
        }
        std::cout << text << ":"
                  << " exit:     "
                  << (id?id:"null")
                  // << " uflags: " << uflags
                  // << " mflags: " << mflags
                  // << " flags: " << flags
                  << std::endl;
    }
}

std::ostream &operator<<(std::ostream &out, const SPObject &o)
{
    out << (o.getId()?o.getId():"No ID")
        << " cloned: " << std::boolalpha << (bool)o.cloned
        << " ref: " << o.refCount
        << " href: " << o.hrefcount
        << " total href: " << o._total_hrefcount;
    return out;
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
