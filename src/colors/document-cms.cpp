// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DocumentCMS - Look after all a document's icc profiles and lists of used colors.
 *
 * Copyright 2023 Martin Owens <doctormo@geek-2.com>
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "document-cms.h"
#include "document-colors.h"

#include "cms/profile.h"
#include "cms/system.h"
#include "colors/color.h"
#include "colors/manager.h"
#include "colors/spaces/cms.h"
#include "document.h"
#include "io/sys.h"
#include "object/color-profile.h"
#include "object/sp-defs.h"
#include "object/sp-root.h"

namespace Inkscape::Colors {

class ColorProfileLink
{
public:
    ColorProfileLink(DocumentCMS *man, ColorProfile *elem);
    ~ColorProfileLink();

    DocumentCMS *tracker = nullptr;
    ColorProfile *cp = nullptr;
    std::shared_ptr<Space::CMS> space;

private:
    sigc::scoped_connection _modified_connection;

    bool generateSpace();
    bool updateSpace();
};

/**
 * A local private class for tracking the signals between the color-profile xml in an SPDocument
 * and the Space::CMS object which is the functional end of the color system.
 */
ColorProfileLink::ColorProfileLink(DocumentCMS *man, ColorProfile *elem)
    : tracker(man)
    , cp(elem)
{
    _modified_connection = cp->connectModified([this](SPObject *obj, guint flags) {
        if (space ? updateSpace() : generateSpace()) {
            tracker->_modified_signal.emit(space);
        }
    });
    generateSpace();
}

ColorProfileLink::~ColorProfileLink()
{
    _modified_connection.disconnect();
    if (space)
        tracker->removeProfile(space);
}

/**
 * Attempt to turn the data in the ColorProfile into a Space::CMS object
 *
 * @returns true if a space was generated
 */
bool ColorProfileLink::generateSpace()
{
    if (space)
        throw ColorError("Unexpected generation of CMS profile space");

    std::shared_ptr<Inkscape::Colors::CMS::Profile> profile;

    auto data = cp->getProfileData();
    auto local_id = cp->getLocalProfileId();
    if (!data.empty()) {
        profile = CMS::Profile::create_from_data(data);
    } else if (!local_id.empty()) {
        profile = CMS::System::get().getProfile(local_id);
    }

    if (profile) {
        space = tracker->addProfile(profile, cp->getName(), cp->getRenderingIntent());
    } else {
        g_warning("Incomplete CMS profile, no color space created for '%s'", cp->getName().c_str());
    }
    return (bool)space;
}

/**
 * Update the space, this typically means the intent has changed.
 */
bool ColorProfileLink::updateSpace()
{
    if (space->getName() != cp->getName()) {
        return generateSpace();
    }
    if (space->getIntent() != cp->getRenderingIntent()) {
        space->setIntent(cp->getRenderingIntent());
        return true;
    }
    return false;
}

DocumentCMS::DocumentCMS(SPDocument *document)
    : _document(document)
{
    if (document) {
        _resource_connection =
            _document->connectResourcesChanged("iccprofile", sigc::mem_fun(*this, &DocumentCMS::refreshResources));
        // During redo a resource can be constructed before its id is replayed.
        _reconstruction_connection =
            _document->connectReconstructionFinish(sigc::mem_fun(*this, &DocumentCMS::refreshResources));
    }
}

DocumentCMS::~DocumentCMS()
{
    _document = nullptr;
}

/**
 * Create an optional color, like Color::parse but with the document's cms spaces.
 */
std::optional<Color> DocumentCMS::parse(char const *value) const
{
    if (!value)
        return {};
    return parse(std::string(value));
}

/**
 * Create an optional color, like Color::parse, but match to document cms profiles where needed.
 */
std::optional<Color> DocumentCMS::parse(std::string const &value) const
{
    Space::Type space_type;
    std::string cms_name;
    std::vector<double> values;
    std::vector<double> fallback;
    if (Parsers::get().parse(value, space_type, cms_name, values, fallback)) {
        if (cms_name.empty()) {
            auto color = Color::ifValid(space_type, std::move(values));
            return color ? std::optional(DocumentColors::interpret(_document, *color)) : std::nullopt;
        }

        // Find a space or construct an anonymous one so we don't lose data.
        if (!_spaces.contains(cms_name)) {
            _spaces[cms_name] = std::make_shared<Space::CMS>(cms_name, values.size());
        }
        auto space = _spaces.find(cms_name)->second;

        if (!space->hasValidCmsProfile()) {
            for (int i = 2; i >= 0; i--) {
                // Assume RGB fallback data if three doubles. Else black.
                values.insert(values.begin(), fallback.size() == 3 ? fallback[i] : 0.0);
            }
        }
        return Color(std::move(space), std::move(values));
    }
    // Couldn't be parsed as a color at all
    return {};
}

/**
 * Make sure the icc-profile resource list is linked and up to date
 * with the color manager's list of available color spaces.
 */
void DocumentCMS::refreshResources()
{
    bool changed = false;

    // 1. Look for color profile which have been created
    std::vector<ColorProfile *> objs;
    for (auto obj : _document->getResourceList("iccprofile")) {
        if (!obj->getId())
            continue;
        if (auto cp = cast<ColorProfile>(obj)) {
            objs.push_back(cp);
            bool found = false;
            for (auto &link : _links) {
                found = found || (link->cp == cp);
            }
            if (!found) {
                _links.emplace_back(new ColorProfileLink(this, cp));
                changed = true;
            }
        }
    }
    // 2. Look for color profiles which have been deleted
    for (auto iter = _links.begin(); iter != _links.end();) {
        if (std::find(objs.begin(), objs.end(), (*iter)->cp) == objs.end()) {
            iter = _links.erase(iter);
            changed = true;
        } else
            ++iter;
    }

    // 3. Tell the rest of inkscape if something is added or removed
    if (changed) {
        // Undo/redo can restore a paint before its profile. Once the resource
        // exists, re-read document paints so no unresolved fallback survives.
        if (auto root = _document->getRoot(); root && root->getRepr()->attribute("proof:color-profile") &&
            _document->getObjectByRepr(root->getRepr()) == root) {
            DocumentColors::refresh(_document);
        }
        _changed_signal.emit();
    }
}

/**
 * Add the icc profile via a URI as a color space with the attending settings.
 */
std::shared_ptr<Space::CMS> DocumentCMS::addProfileURI(std::string uri, std::string name, RenderingIntent intent)
{
    return addProfile(Inkscape::Colors::CMS::Profile::create_from_uri(std::move(uri)), std::move(name), intent);
}

/**
 * Add the icc profile as a color space with the attending settings.
 */
std::shared_ptr<Space::CMS> DocumentCMS::addProfile(std::shared_ptr<CMS::Profile> profile, std::string name,
                                                    RenderingIntent intent)
{
    // The name from the color-profile xml element overrides any internal name
    auto space = std::make_shared<Space::CMS>(profile, name);

    name = space->getName();
    // A color can precede its embedded profile in the SVG tree. Replace its
    // unresolved placeholder when the profile resource becomes available.
    if (_spaces.contains(name) && _spaces.at(name)->hasValidCmsProfile())
        throw ColorError("Color profile with that name already exists.");

    space->setIntent(intent != RenderingIntent::UNKNOWN ? intent : RenderingIntent::PERCEPTUAL);
    _spaces[name] = space;
    return space;
}

/**
 * Remove the icc profile as a color space
 */
void DocumentCMS::removeProfile(std::shared_ptr<Space::CMS> space)
{
    auto result = std::find_if(_spaces.begin(), _spaces.end(), [space](const auto &it) { return it.second == space; });

    if (result != _spaces.end())
        _spaces.erase(result);
}

/**
 * Generate a valid profile name based on the profile name, the name of the intent is
 * added on if the document already contains this profile using a different intent.
 *
 * @arg profile - The profile who's name will be used if name isn't specified.
 * @arg intent - The rendering intent which will be used in the case of a collision.
 * @arg name - The requested name, which will be compared to make sure it's not being used already.
 *             DO NOT pass in an unsanitized profile name here.
 *
 * @returns The sanitized name checked against the document's existing profiles, and true if the profile exists.
 */
std::pair<std::string, bool> DocumentCMS::checkProfileName(Colors::CMS::Profile const &profile, RenderingIntent intent, std::optional<std::string> name) const
{
    if (name && !name->empty()) {
        if (auto space = getSpace(*name)) {
            if (*space->getProfile() == profile && space->getIntent() == intent) {
                // This document already contains this exact profile and intent
                return std::make_pair(*name, true);
            }
        }
    }
    // Get a sanitized name from the profile
    auto new_name = profile.getName(true);
    if (auto space = getSpace(new_name)) {
        if (space->getIntent() == intent) {
            // Profile is already in use
            return std::make_pair(std::move(new_name), true);
        } else {
            // Append intent when it's used but not the same intent
            new_name += "-" + Colors::intentIds[intent];
        }
    }
    return std::make_pair(std::move(new_name), (bool)getSpace(new_name));
}

/**
 * Attach the named profile to the document. The name is used as a look-up in
 * the CMS::System database then attached to the manager's document using the
 * given storage mechanism.
 *
 * @args lookup - The string name, Id or path to look up in the systems database.
 * @args storage - The mechanism to use when storing the profile in the document.
 * @args intent - The rendering intent to use when transforming colors in this profile.
 *
 * @returns The new sanitized name for the color space in the document.
 */
std::optional<std::string> DocumentCMS::attachProfileToDoc(std::string const &lookup, ColorProfileStorage storage,
                                                           RenderingIntent intent)
{
    auto &cms = Inkscape::Colors::CMS::System::get();
    if (auto profile = cms.getProfile(lookup)) {
        return attachProfileToDoc(*profile, storage, intent);
    }
    g_error("Couldn't get the icc profile '%s'", lookup.c_str());
    return {};
}

/**
 * Attach the given profile to the document using the storage mechanism.
 *
 * @args lookup - The string name, Id or path to look up in the systems database.
 * @args storage - The mechanism to use when storing the profile in the document.
 * @args intent - The rendering intent to use when transforming colors in this profile.
 *
 * @returns The new sanitized name for the color space in the document.
 */
std::string DocumentCMS::attachProfileToDoc(Colors::CMS::Profile const &profile, ColorProfileStorage storage,
                                            RenderingIntent intent)
{
    auto [new_name, exists] = checkProfileName(profile, intent);
    if (!exists) {
        Inkscape::ColorProfile::createFromProfile(_document, profile, std::move(new_name), storage, intent);
    }
    return new_name;
}

/**
 * Get the specific color space from the list of available spaces.
 *
 * @arg name - The name of the color space
 */
std::shared_ptr<Space::CMS> DocumentCMS::getSpace(std::string const &name) const
{
    auto it = _spaces.find(name);
    if (it != _spaces.end())
        return it->second;
    return nullptr;
}

/**
 * Get the document color-profile SPObject for the named cms profile. Returns nullptr
 * if the name is not found, if the link has not yet been made or the space is
 * not a CMS color space (i.e. sRGB).
 */
ColorProfile *DocumentCMS::getColorProfileForSpace(std::string const &name) const
{
    return getColorProfileForSpace(getSpace(name));
}

/**
 * Get the document color-profile SPObject for the given space.
 * Returns nullptr if the space is not a CMS color space.
 */
ColorProfile *DocumentCMS::getColorProfileForSpace(std::shared_ptr<Space::CMS> space) const
{
    for (auto &link : _links) {
        if (space && link->space && link->space->getName() == space->getName()) {
            return link->cp;
        }
    }
    return nullptr;
}

/**
 * Sets the rendering intent for the given color space in an agnostic way.
 *
 * If the space is a CMS space then the intent is updated in the SPObject.
 */
void DocumentCMS::setRenderingIntent(std::string const &name, RenderingIntent intent)
{
    if (auto cp = getColorProfileForSpace(name)) {
        cp->setRenderingIntent(intent);
        _document->ensureUpToDate();
    }
}

/**
 * Generate a list of CMS spaces linked in this tracker
 */
std::vector<std::shared_ptr<Space::CMS>> DocumentCMS::getSpaces() const
{
    std::vector<std::shared_ptr<Space::CMS>> ret;
    for (auto &link : _links) {
        if (link->space) {
            ret.push_back(link->space);
        }
    }
    return ret;
}

/**
 * Find the color space indicated by a stand alone svg attribute, e.g. interpolation.
 *
 * Will return a CMS color space if the input matches the name for in use icc profiles.
 */
std::shared_ptr<Space::AnySpace> DocumentCMS::findSvgColorSpace(std::string const &input) const
{
    if (auto space = getSpace(input)) {
        return space;
    }
    return Manager::get().findSvgColorSpace(input);
}

/**
 * Generate a list of SP-objects linked in this tracker
 */
std::vector<ColorProfile *> DocumentCMS::getObjects() const
{
    std::vector<ColorProfile *> ret;
    for (auto &link : _links) {
        if (link->cp) {
            ret.push_back(link->cp);
        }
    }
    return ret;
}

} // namespace Inkscape::Colors

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
