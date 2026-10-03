// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Singleton class to access the preferences file - implementation.
 */
/* Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2008,2009 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "preferences.h"

#include <cstring>
#include <ctime>
#include <glib/gstdio.h>
#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <glibmm/stringutils.h>
#include <iomanip>
#include <utility>

#include "attribute-rel-util.h"
#include "colors/color.h"
#include "colors/manager.h"
#include "io/resource.h"
#include "preferences-skeleton.h"
#include "ui/error-reporter.h"
#include "util-string/ustring-format.h"
#include "util/units.h"
#include "xml/attribute-record.h"
#include "xml/node-iterators.h"
#include "xml/node-observer.h"

#define PREFERENCES_FILE_NAME "preferences.xml"

namespace Inkscape {

static Inkscape::XML::Document *loadImpl( std::string const& prefsFilename, Glib::ustring & errMsg );
static void migrateDetails( Inkscape::XML::Document *from, Inkscape::XML::Document *to );

static Inkscape::XML::Document *migrateFromDoc = nullptr;

// private inner class definition

/**
 * XML - prefs observer bridge.
 *
 * This is an XML node observer that watches for changes in the XML document storing the preferences.
 * It is used to implement preference observers.
 */
class Preferences::PrefNodeObserver : public XML::NodeObserver {
public:
    PrefNodeObserver(Observer &o, Glib::ustring filter) :
        _observer(o),
        _filter(std::move(filter))
    {}
    ~PrefNodeObserver() override = default;
    void notifyAttributeChanged(XML::Node &node, GQuark name, Util::ptr_shared, Util::ptr_shared) override;
private:
    Observer &_observer;
    Glib::ustring const _filter;
};

/**
 * PROOF: apply a changed default once to profiles saved before the change, keeping any choice
 * the user makes afterwards. Each change is recorded under /proof/upgrades.
 */
static void upgrade_proof_defaults(Preferences &prefs)
{
    // Measure and transform by geometric bounds, without stroke, as Illustrator does by default
    // ("Use Preview Bounds" off). The Inkscape default was the visual bounding box.
    if (!prefs.getBool("/proof/upgrades/geometric-bounds")) {
        prefs.setInt("/tools/bounding_box", 1);
        prefs.setBool("/proof/upgrades/geometric-bounds", true);
    }
}

Preferences::Preferences()
{
    _prefs_filename = Inkscape::IO::Resource::profile_path(PREFERENCES_FILE_NAME);

    _loadDefaults();
    _load();
    upgrade_proof_defaults(*this);

    _initialized = true;
}


Preferences::~Preferences()
{
    // unref XML document
    Inkscape::GC::release(_prefs_doc);
}

/**
 * Load internal defaults.
 *
 * In the future this will try to load the system-wide file before falling
 * back to the internal defaults.
 */
void Preferences::_loadDefaults()
{
    _prefs_doc = sp_repr_read_mem(preferences_skeleton, PREFERENCES_SKELETON_SIZE, nullptr);
#ifdef _WIN32
    setBool("/options/desktopintegration/value", 1);
#endif
#if defined(__APPLE__)
    // No maximise for macOS, see lp:1302627
    setInt("/options/defaultwindowsize/value", -1);
#endif
}

/**
 * Load the user's customized preferences.
 *
 * Tries to load the user's preferences.xml file. If there is none, creates it.
 */
void Preferences::_load()
{
    Glib::ustring const not_saved = _("Inkscape will run with default settings, "
                                      "and new settings will not be saved. ");

    // NOTE: After we upgrade to Glib 2.16, use Glib::ustring::compose

    // 1. Does the file exist?
    if (!g_file_test(_prefs_filename.c_str(), G_FILE_TEST_EXISTS)) {
        auto _prefs_dir = Inkscape::IO::Resource::profile_path();
        // No - we need to create one.
        // Does the profile directory exist?
        if (!g_file_test(_prefs_dir.c_str(), G_FILE_TEST_EXISTS)) {
            // No - create the profile directory
            if (g_mkdir_with_parents(_prefs_dir.c_str(), 0755)) {
                // the creation failed
                //_reportError(Glib::ustring::compose(_("Cannot create profile directory %1."),
                //    Glib::filename_to_utf8(_prefs_dir)), not_saved);
                gchar *msg = g_strdup_printf(_("Cannot create profile directory %s."), _prefs_dir.c_str());
                _reportError(msg, not_saved);
                g_free(msg);
                return;
            }
        } else if (!g_file_test(_prefs_dir.c_str(), G_FILE_TEST_IS_DIR)) {
            // The profile dir is not actually a directory
            //_reportError(Glib::ustring::compose(_("%1 is not a valid directory."),
            //    Glib::filename_to_utf8(_prefs_dir)), not_saved);
            gchar *msg = g_strdup_printf(_("%s is not a valid directory."), _prefs_dir.c_str());
            _reportError(msg, not_saved);
            g_free(msg);
            return;
        }
        // create some subdirectories for user stuff
        char const *user_dirs[] = {"extensions", "fonts", "icons", "keys", "palettes", "templates", nullptr};
        for (int i=0; user_dirs[i]; ++i) {
            // XXX Why are we doing this here? shouldn't this be an IO load item?
            auto dir = Inkscape::IO::Resource::profile_path(user_dirs[i]);
            if (!g_file_test(dir.c_str(), G_FILE_TEST_EXISTS))
                g_mkdir(dir.c_str(), 0755);
        }
        // The profile dir exists and is valid.
        if (!g_file_set_contents(_prefs_filename.c_str(), preferences_skeleton, PREFERENCES_SKELETON_SIZE, nullptr)) {
            // The write failed.
            //_reportError(Glib::ustring::compose(_("Failed to create the preferences file %1."),
            //    Glib::filename_to_utf8(_prefs_filename)), not_saved);
            gchar *msg = g_strdup_printf(_("Failed to create the preferences file %s."),
                Glib::filename_to_utf8(_prefs_filename).c_str());
            _reportError(msg, not_saved);
            g_free(msg);
            return;
        }

        if ( migrateFromDoc ) {
            migrateDetails( migrateFromDoc, _prefs_doc );
        }

        // The prefs file was just created.
        // We can return now and skip the rest of the load process.
        _writable = true;
        return;
    }

    // Yes, the pref file exists.
    Glib::ustring errMsg;
    Inkscape::XML::Document *prefs_read = loadImpl( _prefs_filename, errMsg );

    if ( prefs_read ) {
        // Merge the loaded prefs with defaults.
        _prefs_doc->root()->mergeFrom(prefs_read->root(), "id");
        Inkscape::GC::release(prefs_read);
        _writable = true;
    } else {
        _reportError(errMsg, not_saved);
    }
}

//_reportError(msg, not_saved);
static Inkscape::XML::Document *loadImpl( std::string const& prefsFilename, Glib::ustring & errMsg )
{
    // 2. Is it a regular file?
    if (!g_file_test(prefsFilename.c_str(), G_FILE_TEST_IS_REGULAR)) {
        gchar *msg = g_strdup_printf(_("The preferences file %s is not a regular file."),
            Glib::filename_to_utf8(prefsFilename).c_str());
        errMsg = msg;
        g_free(msg);
        return nullptr;
    }

    // 3. Is the file readable?
    gchar *prefs_xml = nullptr; gsize len = 0;
    if (!g_file_get_contents(prefsFilename.c_str(), &prefs_xml, &len, nullptr)) {
        gchar *msg = g_strdup_printf(_("The preferences file %s could not be read."),
            Glib::filename_to_utf8(prefsFilename).c_str());
        errMsg = msg;
        g_free(msg);
        return nullptr;
    }

    // 4. Is it valid XML?
    Inkscape::XML::Document *prefs_read = sp_repr_read_mem(prefs_xml, len, nullptr);
    g_free(prefs_xml);
    if (!prefs_read) {
        gchar *msg = g_strdup_printf(_("The preferences file %s is not a valid XML document."),
            Glib::filename_to_utf8(prefsFilename).c_str());
        errMsg = msg;
        g_free(msg);
        return nullptr;
    }

    // 5. Basic sanity check: does the root element have a correct name?
    if (strcmp(prefs_read->root()->name(), "inkscape")) {
        gchar *msg = g_strdup_printf(_("The file %s is not a valid Inkscape preferences file."),
            Glib::filename_to_utf8(prefsFilename).c_str());
        errMsg = msg;
        g_free(msg);
        Inkscape::GC::release(prefs_read);
        return nullptr;
    }

    return prefs_read;
}

static void migrateDetails( Inkscape::XML::Document *from, Inkscape::XML::Document *to )
{
    // TODO pull in additional prefs with more granularity
    to->root()->mergeFrom(from->root(), "id");
}

/**
 * Flush all pref changes to the XML file.
 */
void Preferences::save()
{
    // no-op if the prefs file is not writable
    if (_writable) {
        // sp_repr_save_file uses utf-8 instead of the glib filename encoding.
        // I don't know why filenames are kept in utf-8 in Inkscape and then
        // converted to filename encoding when necessary through special functions
        // - wouldn't it be easier to keep things in the encoding they are supposed
        // to be in?

        // No, it would not. There are many reasons, one key reason being that the
        // rest of GTK+ is explicitly UTF-8. From an engineering standpoint, keeping
        // the filesystem encoding would change things from a one-to-many problem to
        // instead be a many-to-many problem. Also filesystem encoding can change
        // from one run of the program to the next, so can not be stored.
        // There are many other factors, so ask if you would like to learn them. - JAC
        Glib::ustring utf8name = Glib::filename_to_utf8(_prefs_filename);
        if (!utf8name.empty()) {
            sp_repr_save_file(_prefs_doc, utf8name.c_str());
        }
    }
}

/**
 * Deletes the preferences.xml file
 */
void Preferences::reset()
{
    time_t sptime = time (nullptr);
    struct tm *sptm = localtime (&sptime);
    gchar sptstr[256];
    strftime(sptstr, 256, "%Y_%m_%d_%H_%M_%S", sptm);

    char *new_name = g_strdup_printf("%s_%s.xml", _prefs_filename.c_str(), sptstr);


    if (g_file_test(_prefs_filename.c_str(), G_FILE_TEST_EXISTS)) {
        //int retcode = g_unlink (_prefs_filename.c_str());
        int retcode = g_rename (_prefs_filename.c_str(), new_name );
        if (retcode == 0) g_warning("%s %s.", _("Preferences file was backed up to"), new_name);
        else g_warning("%s", _("There was an error trying to reset the preferences file."));
    }

    g_free(new_name);
    _observer_map.clear();
    Inkscape::GC::release(_prefs_doc);
    _prefs_doc = nullptr;
    _loadDefaults();
    _load();
    save();
}

bool Preferences::getLastError( Glib::ustring& primary, Glib::ustring& secondary )
{
    bool result = _hasError;
    if ( _hasError ) {
        primary = _lastErrPrimary;
        secondary = _lastErrSecondary;
        _hasError = false;
        _lastErrPrimary.clear();
        _lastErrSecondary.clear();
    } else {
        primary.clear();
        secondary.clear();
    }
    return result;
}

// Now for the meat.

/**
 * Get names of all entries in the specified path.
 *
 * @param path Preference path to query.
 * @return A vector containing all entries in the given directory.
 */
std::vector<Preferences::Entry> Preferences::getAllEntries(Glib::ustring const &path)
{
    std::vector<Entry> temp;
    Inkscape::XML::Node *node = _getNode(path, false);
    if (node) {
        for (const auto & iter : node->attributeList()) {
            temp.emplace_back(path + '/' + g_quark_to_string(iter.key), iter.value.pointer());
        }
    }
    return temp;
}

/**
 * Get the paths to all subdirectories of the specified path.
 *
 * @param path Preference path to query.
 * @return A vector containing absolute paths to all subdirectories in the given path.
 */
std::vector<Glib::ustring> Preferences::getAllDirs(Glib::ustring const &path)
{
    std::vector<Glib::ustring> temp;
    Inkscape::XML::Node *node = _getNode(path, false);
    if (node) {
        for (Inkscape::XML::NodeSiblingIterator i = node->firstChild(); i; ++i) {
            if (i->attribute("id") == nullptr) {
                continue;
            }
            temp.push_back(path + '/' + i->attribute("id"));
        }
    }
    return temp;
}

/**
 * Is the given path a directory. If false it may be the entry
 * doesn't exist or is a value instead.
 */
bool Preferences::isDir(Glib::ustring const &path)
{
    Inkscape::XML::Node *node = _getNode(path, false);
    return node && node->attribute("id") != nullptr;
}

// getter methods

Preferences::Entry const Preferences::getEntry(Glib::ustring const &pref_path)
{
    // This function uses caching because it is called very often.
    // We implement caching also for the negative case, where no preference exists.
    // For standard GUI preferences, the negative case is usually not needed
    // because they should have their default value in preferences_skeleton.h.
    // However, for preferences of extensions (i.e., the options that the user can select in the
    // Extension dialog), this case is quite common (ca. 1000 times on Inkscape startup).

    // Negative caching is done "implicitly" here by storing std::nullopt in the cache
    // and passing it to Entry().

    if (_initialized) {
        // get cached value, if it exists
        auto it = cachedEntry.find(pref_path.raw());
        if (it != cachedEntry.end()) {
            auto const &cacheResult = it->second;
            return cacheResult;
        }
    }

    auto const entry = Entry(pref_path, _getRawValue(pref_path));

    if (_initialized) {
        // write to cache
        // Note: Several other functions in this class also write to `cachedEntry`.
        cachedEntry[pref_path.raw()] = entry;
    }
    return entry;
}

// setter methods

/**
 * Set a boolean attribute of a preference.
 *
 * @param pref_path Path of the preference to modify.
 * @param value The new value of the pref attribute.
 */
void Preferences::setBool(Glib::ustring const &pref_path, bool value)
{
    /// @todo Boolean values should be stored as "true" and "false",
    /// but this is not possible due to an interaction with event contexts.
    /// Investigate this in depth.
    _setRawValue(pref_path, ( value ? "1" : "0" ));
}

/**
 * Set an point attribute of a preference.
 *
 * @param pref_path Path of the preference to modify.
 * @param value The new value of the pref attribute.
 */
void Preferences::setPoint(Glib::ustring const &pref_path, Geom::Point value)
{
    setDouble(pref_path + "/x", value[Geom::X]);
    setDouble(pref_path + "/y", value[Geom::Y]);
}

/**
 * Set an integer attribute of a preference.
 *
 * @param pref_path Path of the preference to modify.
 * @param value The new value of the pref attribute.
 */
void Preferences::setInt(Glib::ustring const &pref_path, int value)
{
    _setRawValue(pref_path, Inkscape::ustring::format_classic(value));
}

/**
 * Set an unsigned integer attribute of a preference.
 *
 * @param pref_path Path of the preference to modify.
 * @param value The new value of the pref attribute.
 */
void Preferences::setUInt(Glib::ustring const &pref_path, unsigned int value)
{
    _setRawValue(pref_path, Inkscape::ustring::format_classic(value));
}

/**
 * Set a floating point attribute of a preference.
 *
 * @param pref_path Path of the preference to modify.
 * @param value The new value of the pref attribute.
 */
void Preferences::setDouble(Glib::ustring const &pref_path, double value)
{
    static constexpr auto digits10 = std::numeric_limits<double>::digits10; // number of decimal digits that are ensured to be precise
    _setRawValue(pref_path, Inkscape::ustring::format_classic(std::setprecision(digits10), value));
}

/**
 * Set a floating point attribute of a preference.
 *
 * @param pref_path Path of the preference to modify.
 * @param value The new value of the pref attribute.
 * @param unit_abbr The string of the unit (abbreviated).
 */
void Preferences::setDoubleUnit(Glib::ustring const &pref_path, double value, Glib::ustring const &unit_abbr)
{
    static constexpr auto digits10 = std::numeric_limits<double>::digits10; // number of decimal digits that are ensured to be precise
    Glib::ustring str = Glib::ustring::compose("%1%2", Inkscape::ustring::format_classic(std::setprecision(digits10), value), unit_abbr);
    _setRawValue(pref_path, str);
}

void Preferences::setColor(Glib::ustring const &pref_path, Colors::Color const &color)
{
    _setRawValue(pref_path, color.toString());
}

void Preferences::setColor(Glib::ustring const &pref_path, Glib::ustring const &value)
{
    if (auto color = Colors::Color::parse(value)) {
        setColor(pref_path, color.value());
    }
}

/**
 * Set a string attribute of a preference.
 *
 * @param pref_path Path of the preference to modify.
 * @param value The new value of the pref attribute.
 */
void Preferences::setString(Glib::ustring const &pref_path, Glib::ustring const &value)
{
    _setRawValue(pref_path, value);
}

void Preferences::setStyle(Glib::ustring const &pref_path, SPCSSAttr *style)
{
    Glib::ustring css_str;
    sp_repr_css_write_string(style, css_str);
    _setRawValue(pref_path, css_str);
}

void Preferences::mergeStyle(Glib::ustring const &pref_path, SPCSSAttr *style)
{
    SPCSSAttr *current = getStyle(pref_path);
    sp_repr_css_merge(current, style);
    sp_attribute_purge_default_style(current, SP_ATTRCLEAN_DEFAULT_REMOVE);
    Glib::ustring css_str;
    sp_repr_css_write_string(current, css_str);
    _setRawValue(pref_path, css_str);
    sp_repr_css_attr_unref(current);
}

/**
 *  Remove an entry
 *  Make sure observers have been removed before calling
 */
void Preferences::remove(Glib::ustring const &pref_path)
{
    cachedEntry.erase(pref_path);

    Inkscape::XML::Node *node = _getNode(pref_path, false);
    if (node && node->parent()) {
        node->parent()->removeChild(node);
    } else { //Handle to remove also attributes in path not only the container node
        // verify path
        g_assert( pref_path.at(0) == '/' );
        if (_prefs_doc == nullptr){
            return;
        }
        node = _prefs_doc->root();
        Inkscape::XML::Node *child = nullptr;
        gchar **splits = g_strsplit(pref_path.c_str(), "/", 0);
        if ( splits ) {
            for (int part_i = 0; splits[part_i]; ++part_i) {
                // skip empty path segments
                if (!splits[part_i][0]) {
                    continue;
                }
                if (!node->firstChild()) {
                    node->removeAttribute(splits[part_i]);
                    g_strfreev(splits);
                    return;
                }
                for (child = node->firstChild(); child; child = child->next()) {
                    if (!strcmp(splits[part_i], child->attribute("id"))) {
                        break;
                    }
                }
                node = child;
            }
        }
        g_strfreev(splits);
    }
}

/**
 * Class that holds additional information for registered Observers.
 */
class Preferences::_ObserverData
{
public:
    _ObserverData(Inkscape::XML::Node *node, bool isAttr) : _node(node), _is_attr(isAttr) {}

    Inkscape::XML::Node *_node; ///< Node at which the wrapping PrefNodeObserver is registered
    bool _is_attr; ///< Whether this Observer watches a single attribute
};

Preferences::Observer::Observer(Glib::ustring path) :
    observed_path(std::move(path))
{
}

Preferences::Observer::~Observer()
{
    // on destruction remove observer to prevent invalid references
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    prefs->removeObserver(*this);
}

void Preferences::PrefNodeObserver::notifyAttributeChanged(XML::Node &node, GQuark name, Util::ptr_shared, Util::ptr_shared new_value)
{
    // filter out attributes we don't watch
    gchar const *attr_name = g_quark_to_string(name);
    if ( _filter.empty() || (_filter == attr_name) ) {
        _ObserverData *d = Preferences::_get_pref_observer_data(_observer);
        Glib::ustring notify_path = _observer.observed_path;

        if (!d->_is_attr) {
            std::vector<gchar const *> path_fragments;
            notify_path.reserve(256); // this will make appending operations faster

            // walk the XML tree, saving each of the id attributes in a vector
            // we terminate when we hit the observer's attachment node, because the path to this node
            // is already stored in notify_path
            for (XML::NodeParentIterator n = &node; static_cast<XML::Node*>(n) != d->_node; ++n) {
                path_fragments.push_back(n->attribute("id"));
            }
            // assemble the elements into a path
            for (std::vector<gchar const *>::reverse_iterator i = path_fragments.rbegin(); i != path_fragments.rend(); ++i) {
                notify_path.push_back('/');
                notify_path.append(*i);
            }

            // append attribute name
            notify_path.push_back('/');
            notify_path.append(attr_name);
        }
        std::optional<Glib::ustring> new_value_converted = std::nullopt;
        if (new_value.pointer() != nullptr) {
            new_value_converted = Glib::ustring(new_value.pointer());
        }
        _observer.notify(Preferences::Entry(notify_path, new_value_converted));
    }
}

/**
 * Find the XML node to observe.
 */
XML::Node *Preferences::_findObserverNode(Glib::ustring const &pref_path, Glib::ustring &node_key, Glib::ustring &attr_key, bool create)
{
    // first assume that the last path element is an entry.
    _keySplit(pref_path, node_key, attr_key);

    // find the node corresponding to the "directory".
    Inkscape::XML::Node *node = _getNode(node_key, create), *child;
    if (!node) {
        return nullptr;
    }

    for (child = node->firstChild(); child; child = child->next()) {
        // If there is a node with id corresponding to the attr key,
        // this means that the last part of the path is actually a key (folder).
        // Change values accordingly.
        if (attr_key == child->attribute("id")) {
            node = child;
            attr_key = "";
            node_key = pref_path;
            break;
        }
    }
    return node;
}

void Preferences::addObserver(Observer &o)
{
    // prevent adding the same observer twice
    if ( _observer_map.find(&o) == _observer_map.end() ) {
        Glib::ustring node_key, attr_key;
        Inkscape::XML::Node *node;
        node = _findObserverNode(o.observed_path, node_key, attr_key, true);
        if (node) {
            // set additional data
            o._data.reset(new _ObserverData(node, !attr_key.empty()));

            _observer_map[&o].reset(new PrefNodeObserver(o, attr_key));

            // if we watch a single pref, we want to receive notifications only for a single node
            if (o._data->_is_attr) {
                node->addObserver( *(_observer_map[&o]) );
            } else {
                node->addSubtreeObserver( *(_observer_map[&o]) );
            }
        } else {
            g_warning("Failed to add a preference observer because the key does not exist: %s",
                      o.observed_path.c_str());
        }
    }
}

void Preferences::removeObserver(Observer &o)
{
    // prevent removing an observer which was not added
    auto it = _observer_map.find(&o);
    if (it != _observer_map.end()) {
        Inkscape::XML::Node *node = o._data->_node;
        _ObserverData *priv_data = o._data.get();

        if (priv_data->_is_attr) {
            node->removeObserver(*it->second);
        } else {
            node->removeSubtreeObserver(*it->second);
        }

        _observer_map.erase(it);
    }
}


/**
 * Get the XML node corresponding to the given pref key.
 *
 * @param pref_key Preference key (path) to get.
 * @param create Whether to create the corresponding node if it doesn't exist.
 * @param separator The character used to separate parts of the pref key.
 * @return XML node corresponding to the specified key.
 *
 * Derived from former inkscape_get_repr(). Private because it assumes that the backend is
 * a flat XML file, which may not be the case e.g. if we are using GConf (in future).
 */
Inkscape::XML::Node *Preferences::_getNode(Glib::ustring const &pref_key, bool create)
{
    // verify path
    g_assert( pref_key.empty() || pref_key.at(0) == '/' ); // empty corresponds to root node
    // No longer necessary, can cause problems with input devices which have a dot in the name
    // g_assert( pref_key.find('.') == Glib::ustring::npos );

    if (_prefs_doc == nullptr){
        return nullptr;
    }
    Inkscape::XML::Node *node = _prefs_doc->root();
    Inkscape::XML::Node *child = nullptr;
    gchar **splits = g_strsplit(pref_key.c_str(), "/", 0);

    if ( splits ) {
        for (int part_i = 0; splits[part_i]; ++part_i) {
            // skip empty path segments
            if (!splits[part_i][0]) {
                continue;
            }

            for (child = node->firstChild(); child; child = child->next()) {
                if (child->attribute("id") == nullptr) {
                    continue;
                }
                if (!strcmp(splits[part_i], child->attribute("id"))) {
                    break;
                }
            }

            // If the previous loop found a matching key, child now contains the node
            // matching the processed key part. If no node was found then it is NULL.
            if (!child) {
                if (create) {
                    // create the rest of the key
                    while(splits[part_i]) {
                        child = node->document()->createElement("group");
                        child->setAttribute("id", splits[part_i]);
                        node->appendChild(child);

                        ++part_i;
                        node = child;
                    }
                    g_strfreev(splits);
                    splits = nullptr;
                    return node;
                } else {
                    g_strfreev(splits);
                    splits = nullptr;
                    return nullptr;
                }
            }

            node = child;
        }
        g_strfreev(splits);
    }
    return node;
}

/** Get raw value for preference path, without any caching.
 * std::nullopt is returned when the requested entry does not exist
 */
std::optional<Glib::ustring> Preferences::_getRawValue(Glib::ustring const &path)
{
    // create node and attribute keys
    Glib::ustring node_key, attr_key;
    _keySplit(path, node_key, attr_key);

    // retrieve the attribute
    Inkscape::XML::Node *node = _getNode(node_key, false);
    if ( node == nullptr ) {
        return std::nullopt;
    }
    gchar const *attr = node->attribute(attr_key.c_str());
    if ( attr == nullptr ) {
        return std::nullopt;
    }
    return attr;
}

void Preferences::_setRawValue(Glib::ustring const &path, Glib::ustring const &value)
{
    // create node and attribute keys
    Glib::ustring node_key, attr_key;
    _keySplit(path, node_key, attr_key);

    // update cache first, so by the time notification change fires and observers are called,
    // they have access to current settings even if they watch a group
    if (_initialized) {
        cachedEntry[path.raw()] = Entry(path, value);
    }

    // set the attribute
    Inkscape::XML::Node *node = _getNode(node_key, true);
    node->setAttribute(attr_key, value);
}

// The Entry::isValid* methods check if the preference exists, and then verify if the data would be
// correctly converted to the requested type.

bool Preferences::Entry::isValidBool() const
{
    if (!isSet()) {
        return false;
    }
    auto const &s = _value.value().raw();
    // format is currently "0"/"1", may change to "true"/"false" in the future
    // see Preferences::setBool()
    return (s == "1" || s == "0" || s == "true" || s == "false");
}

bool Preferences::Entry::isValidInt() const
{
    if (!isSet()) {
        return false;
    }

    auto const &s = _value.value().raw();

    // true, false are treated as 1, 0 by getInt(), even though it's not entirely appropriate
    // we're gonna treat them as valid integers here
    if (s == "true" || s == "false") {
        // warn that we're treating "true" and "false" as integers
        g_warning("Integer preference value are set as boolean: '%s', treating it as %d: %s", s.c_str(),
                  s == "true" ? 1 : 0, _pref_path.c_str());
        return true;
    }

    errno = 0;
    
    const char* cstr = s.c_str();
    char* endPtr = nullptr;
    long value = strtol(cstr, &endPtr, 0);
    if (endPtr == cstr) {
        // no valid number found
        return false;
    }
    // checking for overflow _is_ necessary because while int is 32-bit on all
    // modern platforms, long is 64-bit on 64-bit Linux and macOS (LP64 model).
    // For other platforms, the check is optimized out.
    if (errno == ERANGE || value < INT_MIN || value > INT_MAX) {
        return false; // overflow
    }

    // getInt() will also happily retrieve unsigned integers as overflow them
    // However we have a getUInt() method for that, we're gonna therefore
    // treat them as invalid.

    return true;
}

bool Preferences::Entry::isValidUInt() const
{
    if (!isSet()) {
        return false;
    }

    auto const &s = _value.value().raw();

    errno = 0;
    const char* cstr = s.c_str();
    char* end_ptr = nullptr;
    // Negative value wraps around. We rely on ull being larger than uint to
    // check for this. Mind that ulong can be the same size as uint on some
    // platforms.
    unsigned long long value = strtoull(cstr, &end_ptr, 0);
    if (end_ptr == cstr) {
        return false;
    }
    if (errno == ERANGE || value > UINT_MAX) {
        return false; // overflow
    }

    return true;
}

bool Preferences::Entry::isValidDouble() const
{
    if (!isSet()) {
        return false;
    }

    auto const &value_str = _value.value().raw();
    std::string::size_type end_index = 0;

    try {
        Glib::Ascii::strtod(value_str, end_index, 0);
    } catch (std::runtime_error const &e) {
        return false;
    }

    if(end_index == 0) {
        return false; // failed to read anything numeric
    }

    // extract the unit if any, and check if it's a valid unit
    auto unit = value_str.substr(end_index);
    if(!unit.empty()) {
        return Util::UnitTable::get().hasUnit(unit);
    }

    return true;
}

bool Preferences::Entry::isConvertibleTo(Glib::ustring const &type) const
{
    auto from = getUnit();
    if (!from.empty()) {
        auto to = Util::UnitTable::get().getUnit(type);
        return to->compatibleWith(from);
    }

    // if the unit is empty 
    return false;
}

bool Preferences::Entry::isValidColor() const
{
    if (!isSet()) {
        return false;
    }

    return Colors::Color::parse(_value.value().raw()).has_value() || isValidUInt();
}

// The Entry::get* methods convert the preference string from the XML file back to the original value.
// The conversions here are the inverse of Preferences::set*.

bool Preferences::Entry::getBool(bool def) const
{
    if (!isSet()) {
        return def;
    }
    if (cached_bool) {
        return value_bool;
    }
    cached_bool = true;
    // .raw() is only for performance reasons (std::string comparison is faster than Glib::ustr)
    auto const &s = _value.value().raw();
    // format is currently "0"/"1", may change to "true"/"false" in the future, see Preferences::setBool()
    if (s == "1" || s == "true") {
        value_bool = true;
    } else if (s == "0" || s == "false") {
        value_bool = false;
    } else {
        [[unlikely]];
        value_bool = false;
        g_warning("Bool preference value has invalid format. '%s' (raw value: %s)", _pref_path.c_str(), s.c_str());
    }
    return value_bool;
}

Colors::Color Preferences::Entry::getColor(std::string const &def) const
{
    if (isSet()) {
        // Note: we don't cache the resulting Color object
        // because this function is called rarely and therefore not performance-relevant
        // (exemplary Inkscape startup: 40 calls to getColor vs. 10k calls to getBool())
        if (auto res = Colors::Color::parse(_value.value().raw())) {
            return *res;
        } else if (isValidUInt()) {
            // Support historical uint values that are now read as colors (some icon theme colors
            // at least have transitioned from rgba uints to color strings)
            return Colors::Color(getUInt());
        }
    }
    if (auto res = Colors::Color::parse(def)) {
        return *res;
    }
    // Transparent black is the default's default
    return Colors::Color(0x00000000);
}

int Preferences::Entry::getInt(int def) const
{
    if (!isSet()) {
        return def;
    }
    if (cached_int) {
        return value_int;
    }
    cached_int = true;
    // .raw() is only for performance reasons (std::string comparison is faster than Glib::ustr)
    auto const &s = _value.value().raw();
    if (s == "true") {
        g_warning("Integer preference value is set as true, treating it as 1: %s", _pref_path.c_str());
        value_int = 1;
    } else if (s == "false") {
        g_warning("Integer preference value is set as false, treating it as 0: %s", _pref_path.c_str());
        value_int = 0;
    } else {
        int val = 0;

        // TODO: We happily save unsigned integers (notably RGBA values) as signed integers and overflow as needed.
        //       We should consider adding an unsigned integer type to preferences or use HTML colors where appropriate
        //       (the latter would breaks backwards compatibility, though)
        errno = 0;
        val = (int)strtol(s.c_str(), nullptr, 0);
        if (errno == ERANGE) {
            errno = 0;
            val = (int)strtoul(s.c_str(), nullptr, 0);
            if (errno == ERANGE) {
                g_warning("Integer preference out of range: '%s' (raw value: %s)", _pref_path.c_str(), s.c_str());
                val = 0;
            }
        }
        value_int = val;
    }
    return value_int;
}

int Preferences::Entry::getIntLimited(int def, int min, int max) const
{
    int val = getInt(def);
    return (val >= min && val <= max ? val : def);
}

unsigned int Preferences::Entry::getUInt(unsigned int def) const
{
    if (!isSet()) {
        return def;
    }
    if (cached_uint) {
        return value_uint;
    }
    cached_uint = true;

    // Note: 'strtoul' can also read overflowed (i.e. negative) signed int values that we used to save before we
    //       had the unsigned type, so this is fully backwards compatible and can be replaced seamlessly
    unsigned int val = 0;
    auto const &s = _value.value();
    errno = 0;
    val = (unsigned int)strtoul(s.c_str(), nullptr, 0);
    if (errno == ERANGE) {
        g_warning("Unsigned integer preference out of range: '%s' (raw value: %s)", _pref_path.c_str(), s.c_str());
        val = 0;
    }

    value_uint = val;
    return value_uint;
}

/// get double value, assert that the value is set
double Preferences::Entry::_getDoubleAssumeExisting() const
{
    g_assert(_value.has_value());
    if (cached_double) {
        return value_double;
    }
    cached_double = true;
    try {
        value_double = Glib::Ascii::strtod(_value.value().raw());
    } catch (const std::runtime_error &e) {
        value_double = 0;
        g_warning("Double preference out of range: '%s' (raw value: %s)", _pref_path.c_str(), _value.value().c_str());
    }
    return value_double;
}

double Preferences::Entry::getDouble(double def, Glib::ustring const &requested_unit) const
{
    if (!isSet()) {
        return def;
    }

    double val = _getDoubleAssumeExisting();
    if (requested_unit.length() == 0) {
        // no unit specified, don't do conversion
        return val;
    }
    return Util::Quantity::convert(val, getUnit(), requested_unit);
}

double Preferences::Entry::getDoubleLimited(double def, double min, double max, Glib::ustring const &unit) const
{
    double val = getDouble(def, unit);
    return (val >= min && val <= max ? val : def);
}

Glib::ustring Preferences::Entry::getString(Glib::ustring const &def) const
{
    return _value.value_or(def);
}

Glib::ustring Preferences::Entry::getUnit() const
{
    if (!isSet()) {
        return "";
    }
    if (cached_unit) {
        return value_unit;
    }

    // Determine unit from preference value,
    // e.g., pref_value_str = "123.45px" --> value_unit = "px".
    auto const &pref_value_str = _value.value().raw();
    std::string::size_type end_index = 0;
    try {
        Glib::Ascii::strtod(pref_value_str, end_index, 0);
    } catch (const std::runtime_error &e) {
        end_index = 0;
    }
    if (end_index == 0) {
        [[unlikely]];
        // isSet() == true but the string is:
        // - is empty
        // - or does not start with a numeric value
        // - or the number is out of range (double over/underflow)
        // --> cannot determine unit
        g_warning("Double preference value has invalid format. Failed to extract unit for '%s' (raw value: %s)",
                  _pref_path.c_str(), pref_value_str.c_str());
        value_unit = "";
    } else {
        value_unit = pref_value_str.substr(end_index, pref_value_str.size());
    }

    cached_unit = true;
    return value_unit;
}

SPCSSAttr *Preferences::Entry::getStyle() const
{
    if (!isSet()) {
        return sp_repr_css_attr_new();
    }
    // Note: the resulting style object is not cached.
    // This does not hurt performance because getStyle() is called rarely.
    // (For a typical Inkscape startup, 20 calls to getStyle vs 10k calls to getBool.)
    SPCSSAttr *style = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(style, _value.value().c_str());
    return style;
}

SPCSSAttr *Preferences::Entry::getInheritedStyle() const
{
    // This method is quite "dirty". We ignore whatever is stored this Entry
    // and just get the style from Preferences.
    // A more beautiful solution would need major refactoring of Entry and Preferences.
    if (!isSet()) {
        return sp_repr_css_attr_new();
    }

    return Inkscape::Preferences::get()->_getInheritedStyleForPath(_pref_path);
}

Glib::ustring Preferences::Entry::getEntryName() const
{
    Glib::ustring path_base = _pref_path;
    path_base.erase(0, path_base.rfind('/') + 1);
    return path_base;
}

SPCSSAttr *Preferences::_getInheritedStyleForPath(Glib::ustring const &prefPath)
{
    Glib::ustring node_key, attr_key;
    _keySplit(prefPath, node_key, attr_key);

    Inkscape::XML::Node *node = _getNode(node_key, false);
    return sp_repr_css_attr_inherited(node, attr_key.c_str());
}

// XML backend helper: Split the path into a node key and an attribute key.
void Preferences::_keySplit(Glib::ustring const &pref_path, Glib::ustring &node_key, Glib::ustring &attr_key)
{
    // everything after the last slash
    attr_key = pref_path.substr(pref_path.rfind('/') + 1, Glib::ustring::npos);
    // everything before the last slash
    node_key = pref_path.substr(0, pref_path.rfind('/'));
}

void Preferences::_reportError(Glib::ustring const &msg, Glib::ustring const &secondary)
{
    _hasError = true;
    _lastErrPrimary = msg;
    _lastErrSecondary = secondary;
    if (_errorHandler) {
        _errorHandler->handleError(msg, secondary);
    }
}
void Preferences::setErrorHandler(ErrorReporter* handler)
{
    _errorHandler = handler;
}

void Preferences::unload()
{
    if (_instance) {
        delete _instance;
        _instance = nullptr;
    }
}

Glib::ustring Preferences::getPrefsFilename() const
{ //
    return Glib::filename_to_utf8(_prefs_filename);
}

Preferences *Preferences::_instance = nullptr;


PrefObserver Preferences::PreferencesObserver::create(
    Glib::ustring path, std::function<void (const Preferences::Entry& new_value)> callback) {
    assert(callback);

    return PrefObserver(new Preferences::PreferencesObserver(std::move(path), std::move(callback)));
}

Preferences::PreferencesObserver::PreferencesObserver(Glib::ustring path, std::function<void (const Preferences::Entry&)> callback) :
    Observer(std::move(path)), _callback(std::move(callback)) {

    auto prefs = Inkscape::Preferences::get();
    prefs->addObserver(*this);
}

void Preferences::PreferencesObserver::notify(Preferences::Entry const& new_val) {
    _callback(new_val);
}

void Preferences::PreferencesObserver::call_recursive(Glib::ustring const &path) {
    auto prefs = Inkscape::Preferences::get();

    for (auto &e : prefs->getAllEntries(path)) {
        _callback(e);
    }
    for (auto &d : prefs->getAllDirs(path)) {
        call_recursive(d);
    }
}

void Preferences::PreferencesObserver::call() {
    auto prefs = Inkscape::Preferences::get();

    if (!prefs->isDir(observed_path)) {
        _callback(prefs->getEntry(observed_path));
    } else {
        call_recursive(observed_path);
    }
}

PrefObserver Preferences::createObserver(Glib::ustring path, std::function<void (const Preferences::Entry&)> callback) {
    return Preferences::PreferencesObserver::create(path, std::move(callback));
}

PrefObserver Preferences::createObserver(Glib::ustring path, std::function<void ()> callback) {
    return createObserver(std::move(path), [=](const Entry&) { callback(); });
}

Colors::Color Preferences::getColor(Glib::ustring const &pref_path, std::string const &def)
{
    return getEntry(pref_path).getColor(def);
}


} // namespace Inkscape

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
