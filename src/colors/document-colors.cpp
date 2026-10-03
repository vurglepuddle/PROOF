// SPDX-License-Identifier: GPL-2.0-or-later
#include "document-colors.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstring>
#include <functional>
#include <string_view>

#include "cms/profile.h"
#include "cms/system.h"
#include "document-cms.h"
#include "manager.h"
#include "spaces/cms.h"
#include "utils.h"
#include "document.h"
#include "object/color-profile.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "style.h"
#include "xml/repr.h"
#include "xml/sp-css-attr.h"

namespace Inkscape::Colors::DocumentColors {
namespace {
constexpr auto MODE = "proof:color-mode";
constexpr auto PROFILE = "proof:color-profile";
char const *pref(Space::Type type) {
    return type == Space::Type::CMYK ? "/options/workingcolors/cmyk" : "/options/workingcolors/rgb";
}
bool compatible(CMS::Profile const &profile, Space::Type type) {
    auto cls = profile.getProfileClass();
    return (cls == cmsSigInputClass || cls == cmsSigDisplayClass || cls == cmsSigOutputClass ||
            cls == cmsSigColorSpaceClass) &&
           profile.getColorSpace() == (type == Space::Type::CMYK ? cmsSigCmykData : cmsSigRgbData);
}
bool rgb_like(Space::Type type) {
    return type == Space::Type::RGB || type == Space::Type::CSSNAME ||
           type == Space::Type::HSL || type == Space::Type::HSV || type == Space::Type::HWB;
}
void visit(SPObject *object, std::function<void(SPObject *)> const &fn) {
    if (!object) return;
    // A spot's alternate and tint are its ink definition, not process artwork.
    if (object->getRepr()->attribute("proof:ink")) return;
    fn(object);
    for (auto &child : object->children) visit(&child, fn);
}
}

std::vector<std::shared_ptr<CMS::Profile>> profiles(Space::Type type) {
    auto &system = CMS::System::get();
    if (system.getProfiles().empty()) system.refreshProfiles();
    std::vector<std::shared_ptr<CMS::Profile>> result;
    if (type == Space::Type::RGB) result.push_back(CMS::Profile::create_srgb());
    for (auto const &profile : system.getProfiles()) {
        if (compatible(*profile, type)) result.push_back(profile);
    }
    return result;
}

std::shared_ptr<CMS::Profile> workingProfile(Space::Type type) {
    auto selected = Preferences::get()->getString(pref(type));
    auto available = profiles(type);
    for (auto const &profile : available) {
        if (profile->getPath() == selected && !selected.empty()) return profile;
        if (selected == "builtin-srgb" && profile->getPath().empty()) return profile;
    }
    // Never silently substitute a different press profile for a missing choice.
    if (!selected.empty()) return {};
    if (type == Space::Type::RGB) return CMS::Profile::create_srgb();
    for (auto const &profile : available) {
        auto name = profile->getName();
        if (name.find("GRACoL") != std::string::npos && name.find("2006") != std::string::npos) return profile;
    }
    return {};
}

void setWorkingProfile(Space::Type type, std::shared_ptr<CMS::Profile> const &profile) {
    if (!profile || !compatible(*profile, type)) return;
    Preferences::get()->setString(pref(type), profile->getPath().empty() ? "builtin-srgb" : profile->getPath());
}

RenderingIntent workingIntent() {
    auto prefs = Preferences::get();
    auto intent = static_cast<RenderingIntent>(prefs->getIntLimited("/options/workingcolors/intent", 3, 2, 5));
    if (intent == RenderingIntent::RELATIVE_COLORIMETRIC && !prefs->getBool("/options/workingcolors/bpc", true))
        intent = RenderingIntent::RELATIVE_COLORIMETRIC_NOBPC;
    return intent;
}

Space::Type mode(SPDocument *document) {
    auto root = document ? document->getReprRoot() : nullptr;
    auto value = root ? root->attribute(MODE) : nullptr;
    return value && std::string_view(value) == "CMYK" ? Space::Type::CMYK : Space::Type::RGB;
}

std::shared_ptr<Space::CMS> assignedSpace(SPDocument *document) {
    auto root = document ? document->getReprRoot() : nullptr;
    auto value = root ? root->attribute(PROFILE) : nullptr;
    auto space = value ? document->getDocumentCMS().getSpace(value) : nullptr;
    return space && space->hasValidCmsProfile() && space->getComponentType() == mode(document) ? space : nullptr;
}

Color interpret(SPDocument *document, Color const &color) {
    auto target = assignedSpace(document);
    if (!target || color.getSpace()->getType() == Space::Type::CMS) return color;
    auto type = color.getSpace()->getType();
    if (type == target->getComponentType()) return Color(target, color.getValues());
    if (rgb_like(type)) {
        auto rgb = color.converted(Space::Type::RGB);
        if (rgb && target->getComponentType() == Space::Type::RGB) return Color(target, rgb->getValues());
        if (rgb) return rgb->converted(target).value_or(color);
    }
    return color;
}

void refresh(SPDocument *document) {
    if (!document || !document->getRoot()) return;
    visit(document->getRoot(), [](SPObject *object) {
        if (object->style) object->style->readFromObject(object);
        object->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
    });
}

std::string assign(SPDocument *document, Space::Type target_mode,
                   std::shared_ptr<CMS::Profile> const &profile, RenderingIntent intent) {
    if (!document || !document->getRoot()) return "No active document.";
    if (target_mode != Space::Type::RGB && target_mode != Space::Type::CMYK) return "Choose RGB or CMYK.";
    if (profile && !compatible(*profile, target_mode)) return "The profile does not match the document color mode.";

    auto &cms = document->getDocumentCMS();
    std::string name;
    std::shared_ptr<Space::AnySpace> target;
    if (profile) {
        name = cms.attachProfileToDoc(*profile, ColorProfileStorage::HREF_DATA, intent);
        target = cms.getSpace(name);
        if (!target) return "The selected profile could not be embedded.";
    } else {
        target = Manager::get().find(target_mode);
    }
    bool convert = mode(document) != target_mode;
    struct Change { XML::Node *node; boost::intrusive_ptr<SPCSSAttr> css; };
    std::vector<Change> changes;
    visit(document->getRoot(), [&](SPObject *object) {
        auto style = object->style;
        if (!style) return;
        auto css = boost::intrusive_ptr(sp_repr_css_attr_new(), false);
        bool changed = false;
        auto paint = [&](char const *property, Color const &source, char const *opacity, double alpha) {
            auto color = source;
            auto type = color.getSpace()->getComponentType();
            if (rgb_like(type)) {
                if (color.getSpace()->getType() != Space::Type::CMS) color.convert(Space::Type::RGB);
                type = Space::Type::RGB;
            }
            std::optional<Color> result;
            if (type == target_mode) result = Color(target, color.getValues()); // assignment: preserve numbers
            else if (convert) result = color.converted(target);
            if (!result) return;
            sp_repr_css_set_property_string(css.get(), property, result->toString(false));
            if (opacity && source.hasOpacity()) sp_repr_css_set_property_double(css.get(), opacity, alpha * source.getOpacity());
            changed = true;
        };
        if (!style->fill.isDerived() && style->fill.isColor())
            paint("fill", style->fill.getColor(), "fill-opacity", style->fill_opacity.as_double());
        if (!style->stroke.isDerived() && style->stroke.isColor())
            paint("stroke", style->stroke.getColor(), "stroke-opacity", style->stroke_opacity.as_double());
        if (style->stop_color.set && !style->stop_color.currentcolor)
            paint("stop-color", style->stop_color.getColor(), "stop-opacity", style->stop_opacity.as_double());
        if (style->color.set) paint("color", style->color.getColor(), nullptr, 1.0);
        if (changed) changes.push_back({object->getRepr(), std::move(css)});
    });
    auto root = document->getReprRoot();
    root->setAttribute(MODE, target_mode == Space::Type::CMYK ? "CMYK" : "RGB");
    root->setAttribute(PROFILE, name.empty() ? nullptr : name.c_str());
    for (auto const &change : changes) sp_repr_css_change(change.node, change.css.get(), "style");
    refresh(document);
    document->ensureUpToDate();
    return {};
}
}

namespace Inkscape::Colors::DocumentColors {
namespace {
constexpr auto NEW_MODE = "/options/workingcolors/newmode";
char const *mode_name(Space::Type type) { return type == Space::Type::CMYK ? "CMYK" : "RGB"; }
}

Space::Type newDocumentMode() {
    auto value = Preferences::get()->getString(NEW_MODE);
    if (value == "CMYK") return Space::Type::CMYK;
    if (value == "RGB") return Space::Type::RGB;
    return workingProfile(Space::Type::CMYK) ? Space::Type::CMYK : Space::Type::RGB;
}

void setNewDocumentMode(Space::Type type) {
    if (type == Space::Type::RGB || type == Space::Type::CMYK) Preferences::get()->setString(NEW_MODE, mode_name(type));
}

Space::Type inferMode(SPDocument *document) {
    bool cmyk = false;
    auto check = [&](Color const &color) { cmyk = cmyk || color.getSpace()->getComponentType() == Space::Type::CMYK; };
    std::function<void(SPObject *)> walk = [&](SPObject *object) {
        if (auto style = object->style; style && !cmyk) {
            if (style->fill.isColor()) check(style->fill.getColor());
            if (style->stroke.isColor()) check(style->stroke.getColor());
            if (style->stop_color.set && !style->stop_color.currentcolor) check(style->stop_color.getColor());
        }
        for (auto &child : object->children) {
            if (cmyk) return;
            walk(&child);
        }
    };
    if (document && document->getRoot()) walk(document->getRoot());
    return cmyk ? Space::Type::CMYK : Space::Type::RGB;
}

bool adopt(SPDocument *document, Space::Type target_mode) {
    if (!document || !document->getRoot() || document->getReprRoot()->attribute(MODE)) return false;
    auto &cms = document->getDocumentCMS();
    std::string name;
    for (auto cp : cms.getObjects()) {
        auto space = cms.getSpace(cp->getName());
        if (space && space->hasValidCmsProfile() && space->getComponentType() == target_mode) {
            name = space->getName();
            break;
        }
    }
    if (name.empty()) {
        if (auto profile = workingProfile(target_mode)) {
            name = cms.attachProfileToDoc(*profile, ColorProfileStorage::HREF_DATA, workingIntent());
        }
    }
    auto root = document->getReprRoot();
    root->setAttribute(PROFILE, name.empty() ? nullptr : name.c_str());
    root->setAttribute(MODE, mode_name(target_mode));
    return true;
}

std::string switchMode(SPDocument *document, Space::Type target_mode) {
    if (!document || !document->getRoot()) return "No active document.";
    if (document->getReprRoot()->attribute(MODE) && mode(document) == target_mode && assignedSpace(document)) {
        setNewDocumentMode(target_mode);
        return {};
    }
    auto profile = workingProfile(target_mode);
    if (!profile) {
        return target_mode == Space::Type::CMYK
                   ? "No CMYK working profile is available. Choose one in Document Properties > Color."
                   : "No RGB working profile is available. Choose one in Document Properties > Color.";
    }
    auto error = assign(document, target_mode, profile, workingIntent());
    if (error.empty()) setNewDocumentMode(target_mode);
    return error;
}

void assignmentChanged(SPDocument *document) {
    if (!document) return;
    refresh(document);
    document->getDocumentCMS().emitAssignmentChanged();
}
}

namespace Inkscape::Colors::DocumentColors {
namespace {
constexpr char const *FALLBACK_PROPERTIES[] = {"fill", "stroke", "stop-color", "flood-color", "lighting-color", "color"};
bool non_css(char const *value) {
    return value && (std::strstr(value, "icc-color") || std::strstr(value, "icc-named-color") ||
                     std::strstr(value, "device-cmyk"));
}
// Visit elements whose inline style has an ICC or CMYK paint.
template <typename F>
void visit_style_paints(XML::Node *root, F const &fn) {
    sp_repr_visit_descendants(root, [&](XML::Node *node) {
        if (non_css(node->attribute("style"))) {
            auto css = sp_repr_css_attr(node, "style");
            for (auto property : FALLBACK_PROPERTIES) {
                if (auto value = css->attribute(property); non_css(value)) fn(node, property, value);
            }
            sp_repr_css_attr_unref(css);
        }
        return true;
    });
}
}

void insertFallbacks(SPDocument *document) {
    if (!document || !document->getReprRoot()) return;
    auto &cms = document->getDocumentCMS();
    visit_style_paints(document->getReprRoot(), [&](XML::Node *node, char const *property, char const *value) {
        if (auto color = cms.parse(value)) node->setAttribute(property, rgba_to_hex(color->toRGBA(), false));
    });
}

void stripFallbacks(XML::Node *root) {
    if (!root) return;
    visit_style_paints(root, [](XML::Node *node, char const *property, char const *) {
        node->removeAttribute(property);
    });
}
}
