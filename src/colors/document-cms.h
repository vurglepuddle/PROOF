// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Colors::DocumentCMS - Look after a document's icc profiles and keep
 *  track of all the colors in use and their color spaces.
 *
 * Copyright 2023 Martin Owens <doctormo@geek-2.com>
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_COLORS_DOCUMENTCMS_H
#define SEEN_COLORS_DOCUMENTCMS_H

#include <map>
#include <memory>
#include <sigc++/signal.h>
#include <string>
#include <vector>

#include "color.h"
#include <sigc++/scoped_connection.h>

class SPDocument;

namespace Inkscape {
class ColorProfile;
enum class ColorProfileStorage;

namespace Colors {
namespace CMS {
class Profile;
}
namespace Space {
enum class Type;
class AnySpace;
class CMS;
} // namespace Space
class ColorProfileLink;
enum class RenderingIntent;

class DocumentCMS
{
public:
    DocumentCMS() = delete;
    ~DocumentCMS();

    DocumentCMS(DocumentCMS const &) = delete;
    void operator=(DocumentCMS const &) = delete;

    DocumentCMS(SPDocument *document);

    std::optional<Color> parse(char const *value) const;
    std::optional<Color> parse(std::string const &value) const;

    std::shared_ptr<Space::CMS> addProfileURI(std::string uri, std::string name, RenderingIntent intent);
    std::shared_ptr<Space::CMS> addProfile(std::shared_ptr<CMS::Profile> profile, std::string name,
                                           RenderingIntent intent);
    void removeProfile(std::shared_ptr<Space::CMS> space);

    sigc::connection connectChanged(const sigc::slot<void()> &slot) { return _changed_signal.connect(slot); }
    sigc::connection connectModified(const sigc::slot<void(std::shared_ptr<Space::AnySpace>)> &slot)
    {
        return _modified_signal.connect(slot);
    }

    std::pair<std::string, bool> checkProfileName(Colors::CMS::Profile const &profile, RenderingIntent intent, std::optional<std::string> name = {}) const;
    std::optional<std::string> attachProfileToDoc(std::string const &lookup, ColorProfileStorage storage,
                                                  RenderingIntent intent);
    std::string attachProfileToDoc(Colors::CMS::Profile const &profile, ColorProfileStorage storage,
                                   RenderingIntent intent);
    void setRenderingIntent(std::string const &name, RenderingIntent intent);

    std::shared_ptr<Space::CMS> getSpace(std::string const &name) const;

    ColorProfile *getColorProfileForSpace(std::string const &name) const;
    ColorProfile *getColorProfileForSpace(std::shared_ptr<Space::CMS> space) const;

    std::vector<std::shared_ptr<Space::CMS>> getSpaces() const;
    std::vector<ColorProfile *> getObjects() const;

    std::shared_ptr<Space::AnySpace> findSvgColorSpace(std::string const &input) const;
private:
    void refreshResources();

    SPDocument *_document = nullptr;
    std::vector<std::shared_ptr<ColorProfileLink>> _links;

    sigc::scoped_connection _resource_connection;
    sigc::scoped_connection _reconstruction_connection;
    sigc::signal<void()> _changed_signal;

    mutable std::map<std::string, std::shared_ptr<Space::CMS>> _spaces;

protected:
    friend class ColorProfileLink;

    sigc::signal<void(std::shared_ptr<Space::AnySpace>)> _modified_signal;
};

} // namespace Colors
} // namespace Inkscape

#endif // SEEN_COLORS_DOCUMENTCMS_H

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
