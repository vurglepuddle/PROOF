// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   miklos erdelyi
 *
 * Copyright (C) 2007 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_EXTENSION_INTERNAL_PDFINPUT_H
#define SEEN_EXTENSION_INTERNAL_PDFINPUT_H

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#include <glibmm/refptr.h>
#include <gtkmm/dialog.h>
#include <gtkmm/notebook.h>
#include <unordered_map>

#include "extension/implementation/implementation.h"
#include "async/channel.h"
#include "poppler-utils.h"
#include "svg-builder.h"

// clang-format off
#define PDF_COMMON_INPUT_PARAMS \
            "<param name=\"embedImages\" gui-text=\"" N_("Embed Images") "\" type=\"bool\">true</param>\n" \
            "<param name=\"convertColors\" gui-text=\"" N_("Convert Colors to sRGB") "\" type=\"bool\">true</param>\n" \
            "<param name=\"importPages\" gui-text=\"" N_("Import Pages") "\" type=\"bool\">true</param>\n" \
            "<param name=\"approximationPrecision\" gui-text=\"" N_("Approximation Precision:") "\" type=\"float\" min=\"1\" max=\"100\">2.0</param>\n" \
            "<param name=\"fontRendering\" gui-text=\"" N_("Font Rendering:") "\" type=\"optiongroup\">\n" \
                "<option value=\"render-missing\">" N_("Render Missing") "</option>\n" \
                "<option value=\"substitute\">" N_("Substitute missing fonts") "</option>\n" \
                "<option value=\"keep-missing\">" N_("Keep missing fonts' names") "</option>\n" \
                "<option value=\"delete-missing\">" N_("Delete missing font text") "</option>\n" \
                "<option value=\"render-all\">" N_("Draw all text") "</option>\n" \
                "<option value=\"delete-all\">" N_("Delete all text") "</option>\n" \
            "</param>\n" \
            "<param name=\"clipTo\" gui-text=\"" N_("Text output options:") "\" type=\"optiongroup\">\n" \
                "<option value=\"none\">" N_("None") "</option>\n" \
                "<option value=\"media-box\">" N_("Media Box") "</option>\n" \
                "<option value=\"crop-box\">" N_("Crop Box") "</option>\n" \
                "<option value=\"trim-box\">" N_("Trim Box") "</option>\n" \
                "<option value=\"bleed-box\">" N_("Bleed Box") "</option>\n" \
                "<option value=\"art-box\">" N_("Art Box") "</option>\n" \
            "</param>\n" \
            "<param name=\"groupBy\" gui-text=\"" N_("Group by:") "\" type=\"optiongroup\">\n" \
                "<option value=\"by-xobject\">" N_("PDF XObject") "</option>\n" \
                "<option value=\"by-layer\">" N_("PDF Layer") "</option>\n" \
            "</param>\n"
// clang-format on

namespace Gtk {
class Builder;
class Button;
class CheckButton;
class DrawingArea;
class Entry;
class Label;
class ListStore;
class Scale;
} // namespace Gtk

#ifdef HAVE_POPPLER_CAIRO
struct _PopplerDocument;
typedef struct _PopplerDocument            PopplerDocument;
#endif

class Page;
class PDFDoc;

namespace Gtk {
class Button;
class CheckButton;
class ComboBox;
class ComboBoxText;
class DrawingArea;
class Frame;
class Scale;
class Box;
class Label;
class Entry;
} // namespace Gtk

namespace Inkscape {

namespace UI::Widget {
class SpinButton;
class Frame;
} // namespace UI::Widget

enum class PdfImportType : unsigned char
{
    PDF_IMPORT_INTERNAL,
    PDF_IMPORT_CAIRO,
};

namespace Extension::Internal {

class FontModelColumns;
struct AiArtDiffering;

/**
 * PDF import using libpoppler.
 */
class PdfImportDialog : public Gtk::Dialog
{
public:
    PdfImportDialog(std::shared_ptr<PDFDoc> doc, const gchar *uri, Input *mod);
    ~PdfImportDialog() override;

    bool showDialog();
    bool getImportPages();
    std::string getSelectedPages();
    PdfImportType getImportMethod();
    FontStrategies getFontStrategies();
    void setFontStrategies(const FontStrategies &fs);
    /// PROOF: for an .ai whose own art reads in full but draws differently from its page,
    /// says how far, and offers to open that art instead of importing the page.
    void offerEditable(AiArtDiffering const &differing);
    /// Whether that art was chosen.
    bool getOpenEditable() const { return _open_editable; }

private:
    void _setPreviewPage(int page);
    void _setFonts(const FontList &fonts);

    // Signal handlers
    void _drawFunc(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);
    void _onPageNumberChanged();
    void _onNotebookPageChanged(Gtk::Widget *page, guint pageNum);

    Input *_mod; // The input module being used, stores prefs

    Glib::RefPtr<Gtk::Builder> _builder;

    Gtk::Notebook &_notebook;
    Gtk::Entry &_page_numbers;
    Gtk::DrawingArea &_preview_area;
    Gtk::ComboBox &_clip_to;
    Gtk::ComboBox &_group_by;
    Gtk::CheckButton &_embed_images;
    Gtk::CheckButton &_convert_colors;
    Gtk::CheckButton &_import_pages;
    Gtk::Scale &_mesh_slider;
    Gtk::Label &_mesh_label;
    Gtk::Button &_next_page;
    Gtk::Button &_prev_page;
    Gtk::Label &_current_page;
    Glib::RefPtr<Gtk::ListStore> _font_model;
    FontModelColumns *_font_col;
    Gtk::Button &_ok_button;
    bool _open_editable = false;

    std::shared_ptr<PDFDoc> _pdf_doc;   // Document to be imported
    std::string _current_pages;  // Current selected pages
    FontList _font_list;         // List of fonts and the pages they appear on
    int _total_pages = 0;
    int _preview_page = 1;
    Page *_previewed_page;    // Currently previewed page
    unsigned char *_thumb_data; // Thumbnail image data
    int _thumb_width, _thumb_height;    // Thumbnail size
    int _thumb_rowstride;
    int _preview_width, _preview_height;    // Size of the preview area
    bool _render_thumb;     // Whether we can/shall render thumbnails
#ifdef HAVE_POPPLER_CAIRO
    bool _preview_rendering_in_progress = false;
    std::unordered_map<int, std::shared_ptr<cairo_surface_t>> _cairo_surfaces;
    std::vector<Async::Channel::Dest> _channels;
    PopplerDocument *_poppler_doc = nullptr;
#endif
};

class PdfInput : public Inkscape::Extension::Implementation::Implementation
{
public:
    std::unique_ptr<SPDocument> open(Inkscape::Extension::Input *mod, char const *uri, bool is_importing) override;
    static void init();

    bool custom_gui() const override { return true; }

private:
    void add_builder_page(
        std::shared_ptr<PDFDoc> pdf_doc,
        SvgBuilder *builder, SPDocument *doc,
        int page_num,
        std::string const &crop_to,
        double color_delta);
};

} // namespace Inkscape::Extension::Internal

} // namespace Inkscape

#endif // SEEN_EXTENSION_INTERNAL_PDFINPUT_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
