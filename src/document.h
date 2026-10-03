// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_DOCUMENT_H
#define SEEN_SP_DOCUMENT_H

/** \file
 * SPDocument: Typed SVG document implementation
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   MenTaLguY <mental@rydia.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2004-2005 MenTaLguY
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <chrono>
#include <cstddef>                             // for size_t
#include <deque>                               // for deque
#include <map>                                 // for map
#include <memory>                              // for unique_ptr, default_de...
#include <queue>                               // for queue
#include <span>
#include <string>                              // for string
#include <vector>                              // for vector

#include <boost/ptr_container/ptr_list.hpp>    // for ptr_list

#include <giomm/simpleactiongroup.h>           // for SimpleActionGroup
#include <glib.h>                              // for GQuark, gboolean, gchar
#include <glibmm/refptr.h>                     // for RefPtr
#include <glibmm/ustring.h>                    // for ustring
#include <sigc++/connection.h>                 // for connection
#include <sigc++/signal.h>                     // for signal

#include <2geom/affine.h>                      // for Affine
#include <2geom/rect.h>                        // for Rect, OptRect
#include <2geom/transforms.h>                  // for Scale

#include "3rdparty/libcroco/src/cr-cascade.h"  // for CRCascade

#include "composite-undo-stack-observer.h"

// This variable is introduced with 0.92.1
// with the introduction of automatic fix 
// for files detected to have been created 
// with previous versions to have a similar
// look in 0.92+.
extern bool sp_no_convert_text_baseline_spacing;


// This variable is introduced with 0.92.1
// with the introduction of automatic fix 
// for files detected to have been created 
// with previous versions to have a similar
// look in 0.92+.
extern bool sp_do_not_fix_pre_92;


namespace Avoid {
class Router;
}

class Persp3D;
class Persp3DImpl;
class SPDefs;
class SPGroup;
class SPItem;
class SPItemCtx;
class SPNamedView;
class SPObject;
class SPRoot;

namespace Inkscape {
    class DocumentUndo;
    class Event;
    class EventLog;
    class PageManager;
    namespace Colors {
        class DocumentCMS;
    }
    class Selection;
    class UndoStackObserver;
    namespace XML {
        struct Document;
        class Event;
        class Node;
    } // namespace XML
    namespace Util {
        class Unit;
        class Quantity;
    } // namespace Util
} // namespace Inkscape

/// Typed SVG document implementation.
class SPDocument
{
public:
    /// For sanity check in SPObject::requestDisplayUpdate
    unsigned update_in_progress = 0;

    SPDocument();
    ~SPDocument();
    SPDocument(SPDocument const &) = delete;
    SPDocument &operator=(SPDocument const &) = delete;

    static int get_new_doc_number();

    // Document creation ------------------
    static std::unique_ptr<SPDocument> createDoc(Inkscape::XML::Document *rdoc, char const *filename,
            char const *base, char const *name, SPDocument *parent = nullptr);
    static std::unique_ptr<SPDocument> createNewDoc(char const *filename,
            bool make_new = false, SPDocument *parent = nullptr);
    static std::unique_ptr<SPDocument> createNewDocFromMem(std::span<char const> buffer,
            std::string const &filename = "");
    SPDocument *createChildDoc(std::string const &filename);

    void setPages(bool enabled);
    void prunePages(const std::string &page_nums, bool invert = false);

    // Make a deep copy.
    std::unique_ptr<SPDocument> copy() const;
    enum class ImportLayersMode
    {
        None,
        ToGroup, // convert layers to groups
        // Merge //TODO: implement
    };
    enum class ImportRoot
    {
        None,
        AlwaysGroup,   // always create new group at root
        WhenNeeded,    // Create group if needed for applying properties
        Single,        // Keep single object at root, group when needed
        UngroupSingle, // Single+ungroup when possible
    };
    void import(SPDocument &input_doc, Inkscape::XML::Node *parent, Inkscape::XML::Node *after_node,
                Geom::Affine transform, std::vector<Inkscape::XML::Node *> *pasted_objects_result = nullptr,
                ImportRoot rootMode = ImportRoot::None, ImportLayersMode layerMode = ImportLayersMode::None);
    // Substitute doc root
    void rebase(Inkscape::XML::Document * new_xmldoc, bool keep_namedview = true);
    // Substitute doc root with a file
    void rebase(const gchar * file, bool keep_namedview = true);
    // Substitute doc root with file in disk
    void rebase(bool keep_namedview = true);
    // Document status --------------------
    void setVirgin(bool Virgin) { virgin = Virgin; }
    bool getVirgin() { return virgin; }
    const SPDocument *getOriginalDocument() const { return _original_document; }

    bool isModifiedSinceSave() const { return modified_since_save; }
    bool isModifiedSinceAutoSave() const { return modified_since_autosave; }
    void setModifiedSinceSave(bool const modified = true);
    void setModifiedSinceAutoSaveFalse() { modified_since_autosave = false; };

    bool idle_handler();
    bool rerouting_handler();

    void requestModified();
    /// PROOF: run pending idle updates now. While GTK paints back-to-back frames on Windows,
    /// idle callbacks are starved; the canvas calls this before each frame.
    void flushPendingUpdates();
    bool _updateDocument(int flags, unsigned int object_modified_tag = 0); // Used by stand-alone sp_document_idle_handler
    int ensureUpToDate(unsigned int object_modified_tag = 0);

    bool addResource(char const *key, SPObject *object);
    bool removeResource(char const *key, SPObject *object);
    const std::vector<SPObject*>& getResourceList(char const *key);
    void process_pending_resource_changes();

    void do_change_filename(char const *const filename, bool const rebase);
    void changeFilenameAndHrefs(char const *filename);
    void setXMLDialogSelectedObject(SPObject *activexmltree) { _activexmltree = activexmltree; }
    SPObject *getXMLDialogSelectedObject() { return _activexmltree; }

    Inkscape::EventLog *get_event_log() { return _event_log.get(); }

    Inkscape::PageManager& getPageManager() { return *_page_manager; }
    const Inkscape::PageManager& getPageManager() const { return *_page_manager; }

    Inkscape::Colors::DocumentCMS &getDocumentCMS() { return *_cms_manager; }
    const Inkscape::Colors::DocumentCMS &getDocumentCMS() const { return *_cms_manager; }

private:
    void _importDefsNode(SPDocument *source, Inkscape::XML::Node *defs, Inkscape::XML::Node *target_defs);
    SPObject *_activexmltree;

    std::unique_ptr<Inkscape::PageManager> _page_manager;
    std::unique_ptr<Inkscape::Colors::DocumentCMS> _cms_manager;

    std::queue<GQuark> pending_resource_changes;

    // Find items by geometry --------------------
    std::deque<SPItem*> const &get_flat_item_list(unsigned int dkey, bool into_groups, bool active_only) const;

    SPDocument *_searchForChild(std::string const &filename, SPDocument const *avoid = nullptr);
    /** Detect Y-axis orientation change.
     * \return true if change has been detected */
    bool has_yaxis_orientation_changed();
    /** Update desktop transform after Y-axis orientation change.
     * \return shift to apply to display to keep content from scrolling */
    double update_desktop_affine();

    void runMigrationsForOlderVersions();

public:
    void importDefs(SPDocument *source);

    unsigned int vacuumDocument();

    /******** Getters and Setters **********/

    // Document structure -----------------
    Avoid::Router* getRouter() const { return _router.get(); }
    
    /** Returns our SPRoot */
    SPRoot *getRoot() { return root; }
    SPRoot const *getRoot() const { return root; }

    /** Return the main defs object for the document. */
    SPDefs *getDefs();

    Inkscape::XML::Node *getReprRoot() { return rroot; }
    Inkscape::XML::Node *getReprNamedView();
    SPNamedView *getNamedView();

    /** Our Inkscape::XML::Document. */
    Inkscape::XML::Document *getReprDoc() { return rdoc; }
    Inkscape::XML::Document const *getReprDoc() const { return rdoc; }


    std::vector<Glib::ustring> getLanguages() const;

    SPDocument *getParent() { return _parent_document; }
    SPDocument const *getParent() const { return _parent_document; }

    Inkscape::Selection *getSelection() { return _selection.get(); }

    // Styling
    CRCascade    *getStyleCascade() { return style_cascade; }

    // File information --------------------

    /** A filename, or NULL */
    void setDocumentFilename(char const *filename);
    char const *getDocumentFilename() const { return document_filename; }

    /** To be used for resolving relative hrefs. */
    void setDocumentBase( char const* document_base );
    char const *getDocumentBase() const { return document_base; };

    /** basename or other human-readable label for the document. */
    char const* getDocumentName() const { return document_name; }

    bool isTemplate();

    // Document geometry ------------------------
    Inkscape::Util::Unit const* getDisplayUnit();

    void setDocumentScale( const double scaleX, const double scaleY );
    void setDocumentScale( const double scale );
    Geom::Scale getDocumentScale(bool computed = true) const;
    void scaleContentBy(Geom::Scale const &delta);

    void setWidthAndHeight(const Inkscape::Util::Quantity &width, const Inkscape::Util::Quantity &height, bool changeSize=true);
    Geom::Point getDimensions() const;

    void setWidth(const Inkscape::Util::Quantity &width, bool changeSize=true);
    void setHeight(const Inkscape::Util::Quantity &height, bool changeSize=true);
    Inkscape::Util::Quantity getWidth() const;
    Inkscape::Util::Quantity getHeight() const;

    void setViewBox();
    void setViewBox(const Geom::Rect &viewBox);
    Geom::Rect getViewBox() const;

    Geom::OptRect preferredBounds() const;
    Geom::OptRect pageBounds();
    void fitToRect(Geom::Rect const &rect, bool with_margins = false);
    void setupViewport(SPItemCtx *ctx);

    // Desktop geometry ------------------------
    /// Document to desktop coordinate transformation.
    const Geom::Affine &doc2dt() const;
    /// Desktop to document coordinate transformation.
    const Geom::Affine &dt2doc() const
    {
        // Note: doc2dt().inverse() happens to be identical to doc2dt()
        return doc2dt();
    }
    /// True if the desktop Y-axis points down, false if it points up.
    bool yaxisdown() const { return yaxisdir() > 0; }
    /// "1" if the desktop Y-axis points down, "-1" if it points up.
    double yaxisdir() const { return _doc2dt[3]; }
    // return true if coordinate system origin needs to move to current page
    bool get_origin_follows_page();
    void set_origin_follows_page(bool on);
    // signal emitted when Y-axis orientation gets flipped
    sigc::signal<void (double)> get_y_axis_flipped() { return _y_axis_flipped; }

    // Find items -----------------------------
    void bindObjectToId(char const *id, SPObject *object);
    SPObject *getObjectById(std::string const &id) const;
    SPObject *getObjectById(char const *id) const;
    SPObject *getObjectByHref(std::string const &href) const;
    SPObject *getObjectByHref(char const *href) const;

    void bindObjectToRepr(Inkscape::XML::Node *repr, SPObject *object);
    SPObject *getObjectByRepr(Inkscape::XML::Node *repr) const;

    std::vector<SPObject *> getObjectsByClass(Glib::ustring const &klass) const;
    std::vector<SPObject *> getObjectsByElement(Glib::ustring const &element, bool custom = false) const;
    std::vector<SPObject *> getObjectsBySelector(Glib::ustring const &selector) const;

    /**
     * @brief Generate a document-wide unique id.
     *
     * Generates an id string not in use by any object in the document.
     * The generated string is based on the given prefix by appending a number.
     */
    std::string generate_unique_id(char const *prefix);

    /**
     * @brief Set the reference document object.
     * Use this function to extend functionality of getObjectById() - it will search in reference document.
     * This is useful when rendering objects that have been copied from this document into a sandbox document.
     * Setting reference will allow sandbox document to find gradients, or linked objects that may have been
     * referenced by copied object.
     * @param document 
     */
    void set_reference_document(SPDocument* document);
    SPDocument* get_reference_document();

    /**
     * @brief Object used to temporarily set and then automatically clear reference document.
     */
    struct install_reference_document {
        install_reference_document(SPDocument* inject_into, SPDocument* reference);
        ~install_reference_document();
    private:
        SPDocument* _parent;
    };

    std::vector<SPItem*> getItemsInBox         (unsigned int dkey, Geom::Rect const &box, bool take_hidden = false, bool take_insensitive = false, bool take_groups = true, bool enter_groups = false, bool enter_layers = true) const;
    std::vector<SPItem*> getItemsPartiallyInBox(unsigned int dkey, Geom::Rect const &box, bool take_hidden = false, bool take_insensitive = false, bool take_groups = true, bool enter_groups = false, bool enter_layers = true) const;

protected:
    // These functions should be moved to desktop, or canvas. They do not belong in a document API
    friend class SPDesktop;

    SPItem *getItemAtPoint(unsigned int key, Geom::Point const &p, bool into_groups, SPItem *upto = nullptr, bool outline = false) const;

    std::vector<SPItem*> getItemsAtPoints(unsigned const key, std::vector<Geom::Point> points, bool all_layers = true, bool topmost_only = true, size_t limit = 0, bool active_only = true, bool outline = false, double distance = 0.25) const;

    SPItem *getGroupAtPoint(unsigned int key,  Geom::Point const &p, bool outline = false) const;

public:
    // Box tool -------------------------------
    void setCurrentPersp3D(Persp3D * const persp);
    /*
     * getCurrentPersp3D returns current_persp3d (if non-NULL) or the first
     * perspective in the defs. If no perspective exists, returns NULL.
     */
    Persp3D * getCurrentPersp3D();
    void update_lpobjs();
    void setCurrentPersp3DImpl(Persp3DImpl * const persp_impl) { current_persp3d_impl = persp_impl; }
    Persp3DImpl * getCurrentPersp3DImpl() { return current_persp3d_impl; }

    void getPerspectivesInDefs(std::vector<Persp3D*> &list) const;
    unsigned int numPerspectivesInDefs() const {
        std::vector<Persp3D*> list;
        getPerspectivesInDefs(list);
        return list.size();
    }


    // Document undo/redo ----------------------
    unsigned long serial() const { return _serial; }  // Returns document's unique number.
    bool isSeeking() const {return seeking;} // In a transition between two "good" states of document?
    bool isPartial() const {return partial != nullptr;} // In partianl undo/redo transaction
    void resetKey() { actionkey.clear(); }
    Glib::ustring const &action_key() const { return actionkey; }
    bool isSensitive() const { return sensitive; }

    // Garbage collecting ----------------------
    void queueForOrphanCollection(SPObject *object);
    void collectOrphans();


    // Actions ---------------------------------
    Glib::RefPtr<Gio::SimpleActionGroup> getActionGroup() { return action_group; }

    /************* Data ***************/
private:

    // Document ------------------------------
    std::unique_ptr<Avoid::Router> _router; // Instance of the connector router
    std::unique_ptr<Inkscape::Selection> _selection;

    // Document status -----------------------

    bool virgin ;   ///< Has the document never been touched?
    bool modified_since_save = false;
    bool modified_since_autosave = false;
    sigc::connection modified_connection;
    sigc::connection rerouting_connection;

    // Document structure --------------------
    Inkscape::XML::Document *rdoc; ///< Our Inkscape::XML::Document
    Inkscape::XML::Node *rroot; ///< Root element of Inkscape::XML::Document

    SPRoot *root;             ///< Our SPRoot

    // A list of svg documents being used or shown within this document
    std::vector<std::unique_ptr<SPDocument>> _child_documents;
    // Conversely this is a parent document because this is a child.
    SPDocument *_parent_document = nullptr;
    // When copying documents, this can refer to its original
    SPDocument const *_original_document = nullptr;
    // Reference document to fall back to when getObjectById cannot find element in '*this' document
    SPDocument *_ref_document = nullptr;

    // Styling
    CRCascade *style_cascade;

    // Desktop geometry
    mutable Geom::Affine _doc2dt;

    // File information ----------------------
    char *document_filename;   ///< A filename, or NULL
    char *document_base;  ///< To be used for resolving relative hrefs.
    char *document_name;  ///< basename or other human-readable label for the document.

    // Find items ----------------------------
    std::map<std::string, SPObject *> iddef;
    std::map<Inkscape::XML::Node *, SPObject *> reprdef;

    // Box tool ----------------------------
    Persp3D *current_persp3d; /**< Currently 'active' perspective (to which, e.g., newly created boxes are attached) */
    Persp3DImpl *current_persp3d_impl;

    // Document undo/redo ----------------------
    friend Inkscape::DocumentUndo;
    std::unique_ptr<Inkscape::EventLog> _event_log;

    /* Undo/Redo state */
    bool sensitive; /* If we save actions to undo stack */
    Inkscape::XML::Event * partial; /* partial undo log when interrupted */
    std::deque<Inkscape::Event *> undo; /* Undo stack of reprs */
    std::deque<Inkscape::Event *> redo; /* Redo stack of reprs */
    /* Undo listener */
    Inkscape::CompositeUndoStackObserver undoStackObservers;

    // XXX only for testing!
    std::unique_ptr<Inkscape::UndoStackObserver> console_output_undo_observer;

    bool seeking; // Related to undo/redo/unique id
    unsigned long _serial; // Unique document number (used by undo/redo).
    Glib::ustring actionkey; // Last action key, used to combine actions in undo.
    double action_expires; // Expire time for last action key
    std::optional<std::chrono::steady_clock::time_point> undo_timer; // Timer for last action key
    unsigned long object_id_counter; // Steadily-incrementing counter used to assign unique ids to objects.

    // Garbage collecting ----------------------

    std::vector<SPObject *> _collection_queue; ///< Orphans

    // Actions ---------------------------------
    Glib::RefPtr<Gio::SimpleActionGroup> action_group;

    /*********** Signals **************/

    using IDChangedSignal = sigc::signal<void (SPObject *)>;
    using ResourcesChangedSignal = sigc::signal<void ()>;
    using ModifiedSignal = sigc::signal<void (unsigned)>;
    using ObjectBoundSignal = sigc::signal<void ()>;
    using FilenameSetSignal = sigc::signal<void (char const *)> ;
    using ResizedSignal = sigc::signal<void (double, double)>;
    using ReconstructionStart = sigc::signal<void ()>;
    using ReconstructionFinish = sigc::signal<void ()>;
    using CommitSignal = sigc::signal<void ()>;
    using BeforeCommitSignal = sigc::signal<void ()>; // allow to add actions berfore commit to include in undo

    using IDChangedSignalMap = std::map<GQuark, SPDocument::IDChangedSignal>;
    using ResourcesChangedSignalMap = std::map<GQuark, SPDocument::ResourcesChangedSignal>;

    /** Dictionary of signals for id changes */
    IDChangedSignalMap id_changed_signals;

    SPDocument::ModifiedSignal modified_signal;
    SPDocument::ObjectBoundSignal object_bound_signal;
    SPDocument::FilenameSetSignal filename_set_signal;
    SPDocument::ReconstructionStart _reconstruction_start_signal;
    SPDocument::ReconstructionFinish  _reconstruction_finish_signal;
    SPDocument::CommitSignal commit_signal; // Used by friend Inkscape::DocumentUndo
    SPDocument::BeforeCommitSignal before_commit_signal; // Used by friend Inkscape::DocumentUndo

    sigc::signal<void ()> destroySignal;
    sigc::signal<void ()> _saved_or_modified_signal;
    sigc::signal<void (double)> _y_axis_flipped;

public:
    /**
     * @brief Add the observer to the document's undo listener
     * The caller is in charge of freeing any memory allocated to the observer
     * @param observer
     */
    void addUndoObserver(Inkscape::UndoStackObserver& observer);
    void removeUndoObserver(Inkscape::UndoStackObserver& observer);

    sigc::connection connectDestroy(sigc::signal<void ()>::slot_type slot);
    sigc::connection connectModified(ModifiedSignal::slot_type slot);
    sigc::connection connectObjectBound(ObjectBoundSignal::slot_type slot);
    sigc::connection connectFilenameSet(FilenameSetSignal::slot_type slot);
    sigc::connection connectCommit(CommitSignal::slot_type slot);
    sigc::connection connectBeforeCommit(BeforeCommitSignal::slot_type slot);
    sigc::connection connectIdChanged(const char *id, IDChangedSignal::slot_type slot);
    sigc::connection connectResourcesChanged(char const *key, SPDocument::ResourcesChangedSignal::slot_type slot);
    sigc::connection connectReconstructionStart(ReconstructionStart::slot_type slot, bool first = false);
    sigc::connection connectReconstructionFinish(ReconstructionFinish::slot_type slot);
    sigc::connection connectSavedOrModified(sigc::slot<void ()> &&slot);

    /* Resources */
    std::map<std::string, std::vector<SPObject *> > resources;
    ResourcesChangedSignalMap resources_changed_signals; // Used by Extension::Internal::Filter

    void _emitModified(unsigned int object_modified_tag = 0);  // Used by SPItem
    void emitReconstructionStart();
    void emitReconstructionFinish();
};

/*
 * Ideas: How to overcome style invalidation nightmare
 *
 * 1. There is reference request dictionary, that contains
 * objects (styles) needing certain id. Object::build checks
 * final id against it, and invokes necessary methods
 *
 * 2. Removing referenced object is simply prohibited -
 * needs analyse, how we can deal with situations, where
 * we simply want to ungroup etc. - probably we need
 * Repr::reparent method :( [Or was it ;)]
 *
 */

#endif // SEEN_SP_DOCUMENT_H

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
