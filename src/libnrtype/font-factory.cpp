// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * TODO: insert short description here
 *//*
 * Authors:
 *     fred
 *     bulia byak <buliabyak@users.sf.net>
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <iomanip>  // Debugging
#include <memory>
#include <pango/pango-font.h>
#include <pango/pango-fontmap.h>
#include <pangomm/fontdescription.h>
#include <pangomm/fontfamily.h>
#include <pangomm/fontmap.h>
#include <pangomm/wrap_init.h>
#include <vector>

#ifndef PANGO_ENABLE_ENGINE
#define PANGO_ENABLE_ENGINE
#endif

#include <unordered_map>

#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>

#include <fontconfig/fontconfig.h>

#include <pango/pangofc-fontmap.h>
#include <pango/pangoft2.h>
#include <pango/pango-ot.h>

#include "io/sys.h"
#include "io/resource.h"

#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "libnrtype/font-utils.h"
#include "libnrtype/OpenTypeUtil.h"


#ifdef _WIN32
#undef NOGDI
#include <glibmm.h>
#include <windows.h>
#endif

/////////////////// helper functions

static void noop(...) {}
//#define PANGO_DEBUG g_print
#define PANGO_DEBUG noop

///////////////////// FontFactory
// the substitute function to tell fontconfig to enforce outline fonts
static void FactorySubstituteFunc(FcPattern *pattern, gpointer /*data*/)
{
    FcPatternAddBool(pattern, "FC_OUTLINE", FcTrue);
    //char *fam = NULL;
    //FcPatternGetString(pattern, "FC_FAMILY",0, &fam);
    //printf("subst_f on %s\n",fam);
}

FontFactory::FontFactory()
    : fontServer(pango_ft2_font_map_new())
{
    auto font_dir = Glib::getenv("INKSCAPE_FONTCONFIG");
    if (!font_dir.empty()) {
        // TODO: This object leaks and should be destroyed with FcConfigDestroy, but it doesn't work
        fontConfig = FcConfigCreate();
        AddFontConfig(font_dir.c_str());
        pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(fontServer), fontConfig);
    }

    fontContext = pango_font_map_create_context(fontServer);
    fontConfig = pango_fc_font_map_get_config(PANGO_FC_FONT_MAP(fontServer));

    Pango::wrap_init();
    // Prevent system language from over-riding the font language
    pango_context_set_language(fontContext, pango_language_from_string("und"));
    _font_map = Glib::wrap(fontServer);
    pango_ft2_font_map_set_resolution(PANGO_FT2_FONT_MAP(fontServer), 72, 72);
#if PANGO_VERSION_CHECK(1,48,0)
    pango_fc_font_map_set_default_substitute(PANGO_FC_FONT_MAP(fontServer), FactorySubstituteFunc, this, nullptr);
#else
    pango_ft2_font_map_set_default_substitute(PANGO_FT2_FONT_MAP(fontServer), FactorySubstituteFunc, this, nullptr);
#endif

}

FontFactory::~FontFactory()
{
    loaded.clear();
    // Shutdown sequence must start with pango, then font-config strictly!
    pango_fc_font_map_shutdown(PANGO_FC_FONT_MAP(fontServer));
    g_object_unref(fontContext);
    fontServer = 0; // freed by _font_map
    FcFini();
}

void FontFactory::refreshConfig()
{
    pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(fontServer));
}

/*
 * Wrap calls to pango_font_description_get_family
 * and replace some of the pango font names with generic css names
 * http://www.w3.org/TR/2008/REC-CSS2-20080411/fonts.html#generic-font-families
 *
 * This function should be called in place of pango_font_description_get_family()
 */
char const *sp_font_description_get_family(PangoFontDescription const *fontDescr)
{
    static auto const fontNameMap = std::map<std::string, std::string>{
        { "Sans", "sans-serif" },
        { "Serif", "serif" },
        { "Monospace", "monospace" }
    };

    char const *pangoFamily = pango_font_description_get_family(fontDescr);

    if (pangoFamily) {
        if (auto it = fontNameMap.find(pangoFamily); it != fontNameMap.end()) {
            return it->second.c_str();
        }
    }

    return pangoFamily;
}

std::string getSubstituteFontName(std::string const &font)
{
    auto descr = pango_font_description_new();
    pango_font_description_set_family(descr, font.c_str());
    auto fontinstance = FontFactory::get().Face(descr);
    auto descr2 = pango_font_describe(fontinstance->get_font());
    auto name = std::string(sp_font_description_get_family(descr2));
    pango_font_description_free(descr);
    return name;
}

Glib::ustring FontFactory::GetUIFamilyString(PangoFontDescription const *fontDescr)
{
    Glib::ustring family;

    g_assert(fontDescr);

    if (fontDescr) {
        // For now, keep it as family name taken from pango
        char const *pangoFamily = sp_font_description_get_family(fontDescr);

        if (pangoFamily) {
            family = pangoFamily;
        }
    }

    return family;
}

Glib::ustring FontFactory::GetUIStyleString(PangoFontDescription const *fontDescr)
{
    Glib::ustring style;

    g_assert(fontDescr);

    if (fontDescr) {
        PangoFontDescription *fontDescrCopy = pango_font_description_copy(fontDescr);

        pango_font_description_unset_fields(fontDescrCopy, PANGO_FONT_MASK_FAMILY);
        pango_font_description_unset_fields(fontDescrCopy, PANGO_FONT_MASK_SIZE);

        // For now, keep it as style name taken from pango
        char *fontDescrAsString = pango_font_description_to_string(fontDescrCopy);

        style = fontDescrAsString;
        g_free(fontDescrAsString);

        // Unsetting family causes "Normal" to be returned if all other values are default.
        if (style == "Normal") {
            style == "";
        }

        pango_font_description_free(fontDescrCopy);
    }

    return style;
}

// Calculate a Style "value" based on CSS values for ordering styles.
static int StyleNameValue(Glib::ustring const &style)
{
    PangoFontDescription *pfd = pango_font_description_from_string (style.c_str());
    int value =
        pango_font_description_get_weight (pfd) * 1000000 +
        pango_font_description_get_style  (pfd) *   10000 +
        pango_font_description_get_stretch(pfd) *     100 +
        pango_font_description_get_variant(pfd);
    pango_font_description_free (pfd);
    return value;
}

/**
 * Returns a list of all font names available in this font config
 */
std::vector<std::string> FontFactory::GetAllFontNames()
{
    std::vector<std::string> ret;
    PangoFontFamily **families = nullptr;
    int numFamilies = 0;
    pango_font_map_list_families(fontServer, &families, &numFamilies);
    // When pango version is newer, this can become a c++11 loop
    for (int currentFamily = 0; currentFamily < numFamilies; ++currentFamily) {
        ret.emplace_back(pango_font_family_get_name(families[currentFamily]));
    }
    return ret;
}

/*
 * Returns true if the font family is in the local font server map.
 */
bool FontFactory::hasFontFamily(const std::string &family)
{
    return getSubstituteFontName(family) == family;
}

std::map<std::string, PangoFontFamily *> FontFactory::GetUIFamilies()
{
    std::map<std::string, PangoFontFamily *> result;

    // Gather the family names as listed by Pango
    PangoFontFamily **families = nullptr;
    int numFamilies = 0;
    pango_font_map_list_families(fontServer, &families, &numFamilies);

    for (int currentFamily = 0; currentFamily < numFamilies; ++currentFamily) {
        char const *displayName = pango_font_family_get_name(families[currentFamily]);

        if (!displayName || *displayName == '\0') {
            std::cerr << "FontFactory::GetUIFamilies: Missing displayName! " << std::endl;
            continue;
        }
        if (!g_utf8_validate(displayName, -1, nullptr)) {
            // TODO: can can do anything about this or does it always indicate broken fonts that should not be used?
            std::cerr << "FontFactory::GetUIFamilies: Illegal characters in displayName. ";
            std::cerr << "Ignoring font '" << displayName << "'" << std::endl;
            continue;
        }
        result.emplace(displayName, families[currentFamily]);
    }

    g_free(families);
    return result;
}

std::vector<Glib::RefPtr<Pango::FontFamily>> FontFactory::get_font_families() {
    auto list = _font_map->list_families();
    std::vector<Glib::RefPtr<Pango::FontFamily>> sorted;
    sorted.reserve(list.size());

    for (auto&& family : list) {
        auto name = family->get_name();
        if (name.empty()) {
            std::cerr << "FontFactory::get_font_families - Missing font family name! " << std::endl;
            continue;
        }
        if (!g_utf8_validate(name.c_str(), -1, nullptr)) {
            std::cerr << "FontFactory::get_font_families - Illegal characters in font family name ";
            std::cerr << "Ignoring font '" << name << "'" << std::endl;
            continue;
        }

        sorted.emplace_back(family);
    }

    std::sort(sorted.begin(), sorted.end(), [](const Glib::RefPtr<Pango::FontFamily>& a, const Glib::RefPtr<Pango::FontFamily>& b){
        return a->get_name() < b->get_name();
    });
    
    return sorted;
}

std::vector<StyleNames> FontFactory::GetUIStyles(PangoFontFamily *in)
{
    if (!in) {
        std::cerr << "FontFactory::GetUIStyles(): PangoFontFamily is NULL" << std::endl;
        return {};
    }

    // Gather the styles for this family
    PangoFontFace **faces = nullptr;
    int numFaces = 0;
    pango_font_family_list_faces(in, &faces, &numFaces);

    std::vector<StyleNames> result;

    for (int currentFace = 0; currentFace < numFaces; currentFace++) {

        // If the face has a name, describe it, and then use the
        // description to get the UI family and face strings
        char const *displayName = pango_font_face_get_face_name(faces[currentFace]);
        if (!displayName || *displayName == '\0') {
            std::cerr << "FontFactory::GetUIStyles: Missing displayName! " << std::endl;
            continue;
        }

        PangoFontDescription *faceDescr = pango_font_face_describe(faces[currentFace]);

        if (faceDescr) {

            // pango_font_face_describe() does not include font variations. We need to add them
            // ourselves!!  pango_font_family_is_variable() uses the FontConfig FC_VARIABLE bool
            // but this only seems to be set true if the fonts 'fvar' table has variable weight,
            // width (stretch), or optical size.  Thus Decovar is NOT marked as a variable font!!!
            //
            // The following is very wasteful but is necessary. We can't just load the font once since
            // the faces may not be in one file. For example, Amestelvar has two different files, one
            // for roman and another for italic variations.
            auto pango_font = pango_font_map_load_font(fontServer, fontContext, faceDescr);
            if (pango_font) {
                auto hb_font = pango_font_get_hb_font(pango_font); // Pango owns hb_font
                if (hb_font) {
                    std::map<Glib::ustring, Glib::ustring> openTypeVarNames;
                    readOpenTypeFvarNamedInstances(hb_font, openTypeVarNames);
                    if (openTypeVarNames.find(displayName) != openTypeVarNames.end()) {
                        pango_font_description_set_variations(faceDescr, openTypeVarNames[displayName].c_str());
                    }
                } else {
                    std::cerr << "FontFactory::GetUIStyles: failed to load hb_font!" << std::endl;
                }
                g_object_unref(pango_font);
            } else {
                std::cerr << "FontFactory::GetUIStyles: failed to load pango_font!" << std::endl;
            }

            Glib::ustring familyUIName = GetUIFamilyString(faceDescr);
            Glib::ustring styleUIName = GetUIStyleString(faceDescr);

            // Disable synthesized (faux) font faces except for CSS generic faces
            if (pango_font_face_is_synthesized(faces[currentFace]) ) {
                if (familyUIName.compare("sans-serif") != 0 &&
                    familyUIName.compare("serif"     ) != 0 &&
                    familyUIName.compare("monospace" ) != 0 &&
                    familyUIName.compare("fantasy"   ) != 0 &&
                    familyUIName.compare("cursive"   ) != 0 ) {
                    pango_font_description_free(faceDescr);
                    continue;
                }
            }

            styleUIName = Inkscape::canonize_fontspec(styleUIName);

            // std::cout << "  Display Name: "   << std::setw(20) << displayName
            //           << "  familyUIName: " << std::setw(20) << familyUIName
            //           << "  styleUIName: "  << std::setw(30) << styleUIName
            //           << "  (" << pango_font_description_to_string(faceDescr) << ")"
            //           << "  Variable: " << std::boolalpha << (bool)pango_font_family_is_variable(in)
            //           << std::endl;

            // NOTE: CSS no longer limits weights to multiple of 100.
            // As of Pango 1.23.0, "weight=450" is valid font description syntax.

            // Additional notes, helpful for debugging:
            //   Pango's FC backend:
            //     Weights defined in fontconfig/src/fcweight.c (was fontconfig/fontconfig.h)
            //     String equivalents in fontconfig/src/fcfreetype.c
            //     Weight set from os2->usWeightClass
            //   Use Fontforge: Element->Font Info...->OS/2->Misc->Weight Class to check font weight

            bool exists = false;
            for (auto const &tmp : result) {
                if (tmp.css_name.compare(styleUIName) == 0) {
                    exists = true;
                    std::cerr << "Warning: Font face with same CSS values already added: "
                              << familyUIName.raw() << " " << styleUIName.raw()
                              << " (" << tmp.display_name.raw()
                              << ", " << displayName << ")"
                              << " CSS (Pango): " << tmp.css_name << std::endl;
                    std::cerr << "  This can happen if a variable font file and the corresponding fixed font files are installed together." << std::endl;
                    break;
                }
            }

            if (!exists && !familyUIName.empty() && !styleUIName.empty()) {
                // Add the style information
                result.emplace_back(styleUIName, displayName);
            }
            pango_font_description_free(faceDescr);
        }
    }
    g_free(faces);

    // Sort the style list
    std::sort(result.begin(), result.end(), [] (auto &a, auto &b) {
        return StyleNameValue(a.css_name) < StyleNameValue(b.css_name);
    });

    // std::cout << "FontFactory::GetUIStyles: " << std::endl;
    // for (auto r : result) {
    //     std::cout << "  " << std::setw(20) << std::left << r.display_name << ": " << r.css_name << std::endl;
    // }
    // std::cout << "FontFactory::GetUIStyles: Exit" << std::endl;

    return result;
}

std::shared_ptr<FontInstance> FontFactory::FaceFromDescr(char const *family, char const *style)
{
    PangoFontDescription *temp_descr = pango_font_description_from_string(style);
    pango_font_description_set_family(temp_descr,family);
    auto res = Face(temp_descr);
    pango_font_description_free(temp_descr);
    return res;
}

std::shared_ptr<FontInstance> FontFactory::FaceFromPangoString(char const *pangoString)
{
    std::shared_ptr<FontInstance> fontInstance;

    g_assert(pangoString);

    if (pangoString) {

        // Create a font description from the string - this may fail or
        // produce unexpected results if the string does not have a good format
        PangoFontDescription *descr = pango_font_description_from_string(pangoString);

        if (descr) {
            if (sp_font_description_get_family(descr)) {
                fontInstance = Face(descr);
            }
            pango_font_description_free(descr);
        }
    }

    return fontInstance;
}

std::shared_ptr<FontInstance> FontFactory::FaceFromFontSpecification(char const *fontSpecification)
{
    std::shared_ptr<FontInstance> font;

    g_assert(fontSpecification);

    if (fontSpecification) {
        // How the string is used to reconstruct a font depends on how it
        // was constructed in ConstructFontSpecification.  As it stands,
        // the font specification is a pango-created string
        font = FaceFromPangoString(fontSpecification);
    }

    return font;
}

std::unique_ptr<FontInstance> FontFactory::create_face(PangoFontDescription* descr) {
    // Mandatory huge size (hinting workaround).
    pango_font_description_set_size(descr, fontSize * PANGO_SCALE);

    if (!sp_font_description_get_family(descr)) {
        return {};
    }

    auto descr_copy = pango_font_description_copy(descr);
    return std::make_unique<FontInstance>(pango_font_map_load_font(fontServer, fontContext, descr), descr_copy);
}

std::shared_ptr<FontInstance> FontFactory::Face(PangoFontDescription *descr, bool canFail)
{
    // Mandatory huge size (hinting workaround).
    pango_font_description_set_size(descr, fontSize * PANGO_SCALE);

    // Check if already loaded.
    if (auto res = loaded.lookup(descr)) {
        return res;
    }

    // Handle failures by falling back to sans-serif. If even that fails, throw.
    auto fallback = [&] {
        if (canFail) {
            auto const tc = pango_font_description_to_string(descr);
            PANGO_DEBUG("Falling back from %s to 'sans-serif' because InstallFace failed\n", tc);
            g_free(tc);
            pango_font_description_set_family(descr, "sans-serif");
            return Face(descr, false);
        } else {
            throw std::runtime_error(std::string("Could not load any face for font ") + pango_font_description_to_string(descr));
        }
    };

    // Workaround for bug #1025565: fonts without families blow up Pango.
    if (!sp_font_description_get_family(descr)) {
        g_warning("%s", _("Ignoring font without family that will crash Pango"));
        return fallback();
    }

    // Create the face.
    // Note: The descr of the returned pangofont may differ from what was asked. We use the original as the map key.
    try {
        auto descr_copy = pango_font_description_copy(descr);
        return loaded.add(
                   descr_copy,
                   std::make_unique<FontInstance>(
                       pango_font_map_load_font(fontServer, fontContext, descr),
                       descr_copy
                   )
               );
    } catch (FontInstance::CtorException const &) {
        return fallback();
    }
}

// Not used, need to add variations if ever used.
// std::shared_ptr<FontInstance> FontFactory::Face(char const *family, int variant, int style, int weight, int stretch, int /*size*/, int /*spacing*/)
// {
//     // std::cout << "FontFactory::Face(family, variant, style, weight, stretch)" << std::endl;
//     PangoFontDescription *temp_descr = pango_font_description_new();
//     pango_font_description_set_family(temp_descr,family);
//     pango_font_description_set_weight(temp_descr,(PangoWeight)weight);
//     pango_font_description_set_stretch(temp_descr,(PangoStretch)stretch);
//     pango_font_description_set_style(temp_descr,(PangoStyle)style);
//     pango_font_description_set_variant(temp_descr,(PangoVariant)variant);
//     auto res = Face(temp_descr);
//     pango_font_description_free(temp_descr);
//     return res;
// }

# ifdef _WIN32
void FontFactory::AddFontFilesWin32(char const *directory_path)
{
    static std::vector<char const *> const allowed_ext = {"ttf", "otf"};
    std::vector<std::string> files;
    Inkscape::IO::Resource::get_filenames_from_path(files, directory_path, allowed_ext, {});
    for (auto const &file : files) {
        wchar_t *file_utf16 = (wchar_t*) g_utf8_to_utf16 (file.c_str(), -1, NULL, NULL, NULL);
        if (!file_utf16) {
            g_warning("Cannot convert path %s to UTF16", file.c_str());
            continue;
        }
        int result = AddFontResourceExW(file_utf16, FR_PRIVATE, 0);
        if (result != 0) {
            g_info("Font File: %s added sucessfully.", file.c_str());
        } else {
            g_warning("Font File: %s wasn't added sucessfully", file.c_str());
        }
        g_free(file_utf16);
    }
}
# endif

void FontFactory::AddFontsDir(char const *utf8dir)
{
    if (!Inkscape::IO::file_test(utf8dir, G_FILE_TEST_IS_DIR)) {
        g_info("Fonts dir '%s' does not exist and will be ignored.", utf8dir);
        return;
    }

    gchar *dir;
# ifdef _WIN32
    AddFontFilesWin32(utf8dir);
    dir = g_win32_locale_filename_from_utf8(utf8dir);
# else
    dir = g_filename_from_utf8(utf8dir, -1, nullptr, nullptr, nullptr);
# endif
    if (!dir) {
# ifdef _WIN32
        g_warning ("Could not retrieve ACP path for '%s'", utf8dir);
# else
        g_warning ("Could not retrieve FS-encoded path for '%s'", utf8dir);
# endif
    }

    FcBool res = FcConfigAppFontAddDir(fontConfig, (FcChar8 const *)dir);
    if (res == FcTrue) {
        g_info("Fonts dir '%s' added successfully.", utf8dir);
        pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(fontServer));
    } else {
        g_warning("Could not add fonts dir '%s'.", utf8dir);
    }

    g_free(dir);
}

void FontFactory::AddFontFile(char const *utf8file)
{
    if (!Inkscape::IO::file_test(utf8file, G_FILE_TEST_IS_REGULAR)) {
        g_warning("Font file '%s' does not exist and will be ignored.", utf8file);
        return;
    }

    gchar *file;
# ifdef _WIN32
    file = g_win32_locale_filename_from_utf8(utf8file);
# else
    file = g_filename_from_utf8(utf8file, -1, nullptr, nullptr, nullptr);
# endif

    FcBool res = FcConfigAppFontAddFile(fontConfig, (FcChar8 const *)file);
    if (res == FcTrue) {
        g_info("Font file '%s' added successfully.", utf8file);
        pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(fontServer));
    } else {
        g_warning("Could not add font file '%s'.", utf8file);
    }

    g_free(file);
}

void FontFactory::AddFontConfig(char const *utf8file)
{
    if (!Inkscape::IO::file_test(utf8file, G_FILE_TEST_IS_REGULAR)) {
        g_warning("Font config '%s' does not exist and will be ignored.", utf8file);
        return;
    }

# ifdef _WIN32
    gchar *lfile = g_win32_locale_filename_from_utf8(utf8file);
# else
    gchar *lfile = g_filename_from_utf8(utf8file, -1, nullptr, nullptr, nullptr);
# endif
    gchar *file = g_canonicalize_filename(lfile, nullptr);
    g_free(lfile);

    if (FcConfigParseAndLoad(fontConfig, (FcChar8 const *)file, FcTrue)) {
        FcConfigBuildFonts(fontConfig);
    } else {
        g_warning("Failed to add font config: %s", file);
    }
    g_free(file);
}

bool FontFactory::Compare::operator()(PangoFontDescription const *a, PangoFontDescription const *b) const
{
    // return pango_font_description_equal(a, b);
    auto const fa = sp_font_description_get_family(a);
    auto const fb = sp_font_description_get_family(b);
    if ((bool)fa != (bool)fb) return false;
    if (fa && fb && std::strcmp(fa, fb) != 0) return false;
    if (pango_font_description_get_style(a)   != pango_font_description_get_style(b)  ) return false;
    if (pango_font_description_get_variant(a) != pango_font_description_get_variant(b)) return false;
    if (pango_font_description_get_weight(a)  != pango_font_description_get_weight(b) ) return false;
    if (pango_font_description_get_stretch(a) != pango_font_description_get_stretch(b)) return false;
    if (g_strcmp0(pango_font_description_get_variations(a),
                  pango_font_description_get_variations(b) ) != 0) return false;
    return true;
}

size_t FontFactory::Hash::operator()(PangoFontDescription const *x) const
{
    // Need to avoid using the size field.
    size_t hash = 0;
    auto const family = sp_font_description_get_family(x);
    hash += family ? g_str_hash(family) : 0;
    hash *= 1128467;
    hash += (size_t)pango_font_description_get_style(x);
    hash *= 1128467;
    hash += (size_t)pango_font_description_get_variant(x);
    hash *= 1128467;
    hash += (size_t)pango_font_description_get_weight(x);
    hash *= 1128467;
    hash += (size_t)pango_font_description_get_stretch(x);
    hash *= 1128467;
    auto const variations = pango_font_description_get_variations(x);
    hash += variations ? g_str_hash(variations) : 0;
    return hash;
}

/**
 * Use font config to parse the postscript name found in pdf/ps files and return
 * font config family and style information.
 */
PangoFontDescription *FontFactory::parsePostscriptName(std::string const &name, bool substitute)
{
    PangoFontDescription *ret = nullptr;

    // Use our local inkscape font-config setup, to include custom font dirs
    FcConfig *conf = pango_fc_font_map_get_config(PANGO_FC_FONT_MAP(fontServer));
    FcPattern *pat = FcNameParse(reinterpret_cast<const unsigned char *>((std::string(":postscriptname=") + name).c_str()));

    // These must be called before FcFontMatch, see FontConfig docs.
    FcConfigSubstitute(conf, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    // We match the pattern and return the results
    FcResult result;
    FcPattern *match = FcFontMatch(conf, pat, &result);
    if (match) {
        // To block mis-matching we check the postscript name matches itself
        FcChar8 *output = nullptr;
        FcPatternGetString(match, FC_POSTSCRIPT_NAME, 0, &output);
        if (substitute || (output && name == (char *)output)) {
            ret = pango_fc_font_description_from_pattern(match, false);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return ret;
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
