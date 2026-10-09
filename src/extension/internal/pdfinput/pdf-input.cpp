// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Native PDF import using libpoppler.
 *
 * Authors:
 *   miklos erdelyi
 *   Abhishek Sharma
 *
 * Copyright (C) 2007 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#include "pdf-input.h"

#include <Catalog.h>
#include <ErrorCodes.h>
#include <FontInfo.h>
#include <GfxFont.h>
#include <GlobalParams.h>
#include <OptionalContent.h>
#include <PDFDoc.h>
#include <Page.h>
#include <goo/GooString.h>

#ifdef HAVE_POPPLER_CAIRO
#include <glib/poppler.h>
#include <glib/poppler-document.h>
#include <glib/poppler-page.h>
#include <goo/gmem.h>
#endif

#include <gtk/gtk.h>
#include <gdkmm/general.h>
#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <gtkmm/accessible.h>
#include <gtkmm/builder.h>
#include <gtkmm/button.h>
#include <gtkmm/cellrenderercombo.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/entry.h>
#include <gtkmm/frame.h>
#include <gtkmm/label.h>
#include <gtkmm/liststore.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scale.h>
#include <string_view>
#include <utility>

#include "ai-native-open.h"
#include "async/async.h"
#include "document-undo.h"
#include "document.h"
#include "extension/input.h"
#include "extension/system.h"
#include "inkscape.h"
#include "object/sp-root.h"
#include "pdf-parser.h"
#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/dialog-events.h"
#include "ui/dialog-run.h"
#include "ui/pack.h"
#include "util/gobjectptr.h"
#include "util/parse-int-range.h"
#include "util/units.h"

using namespace Inkscape::UI;

namespace {

void sanitize_page_number(int &page_num, const int num_pages) {
    if (page_num < 1 || page_num > num_pages) {
        std::cerr << "Inkscape::Extension::Internal::PdfInput::open: Bad page number "
                  << page_num
                  << ". Import first page instead."
                  << std::endl;
        page_num = 1;
    }
}

}

namespace Inkscape {
namespace Extension {
namespace Internal {

class FontModelColumns : public Gtk::TreeModel::ColumnRecord
{
public:
    FontModelColumns()
    {
        add(id);
        add(family);
        add(style);
        add(weight);
        add(stretch);
        add(proc_label);
        add(proc_id);
        add(icon);
        add(em);
    }
    ~FontModelColumns() override = default;
    Gtk::TreeModelColumn<int> id;
    Gtk::TreeModelColumn<Glib::ustring> family;
    Gtk::TreeModelColumn<Glib::ustring> style;
    Gtk::TreeModelColumn<Glib::ustring> weight;
    Gtk::TreeModelColumn<Glib::ustring> stretch;
    Gtk::TreeModelColumn<Glib::ustring> proc_label;
    Gtk::TreeModelColumn<int> proc_id;
    Gtk::TreeModelColumn<Glib::ustring> icon;
    Gtk::TreeModelColumn<bool> em;
};

static FontStrategy pref_to_font_strategy(std::string const &value)
{
    if (value == "render-missing") {
        return FontStrategy::RENDER_MISSING;
    } else if (value == "substitute") {
        return FontStrategy::SUBSTITUTE_MISSING;
    } else if (value == "keep-missing") {
        return FontStrategy::KEEP_MISSING;
    } else if (value == "delete-missing") {
        return FontStrategy::DELETE_MISSING;
    } else if (value == "render-all") {
        return FontStrategy::RENDER_ALL;
    } else if (value == "delete-all") {
        return FontStrategy::DELETE_ALL;
    }
    g_warning("Unknown fontRendering option '%s'", value.c_str());
    return FontStrategy::SUBSTITUTE_MISSING;
}

/**
 * \brief The PDF import dialog
 * FIXME: Probably this should be placed into src/ui/dialog
 */

PdfImportDialog::PdfImportDialog(std::shared_ptr<PDFDoc> doc, const gchar * /*uri*/, Input *mod)
    : _pdf_doc(std::move(doc))
    , _mod(mod)
    , _builder(UI::create_builder("extension-pdfinput.glade"))
    , _notebook(UI::get_widget<Gtk::Notebook>(_builder, "import-type"))
    , _page_numbers(UI::get_widget<Gtk::Entry>(_builder, "page-numbers"))
    , _preview_area(UI::get_widget<Gtk::DrawingArea>(_builder, "preview-area"))
    , _clip_to(UI::get_widget<Gtk::ComboBox>(_builder, "clip-to"))
    , _group_by(UI::get_widget<Gtk::ComboBox>(_builder, "group-by"))
    , _embed_images(UI::get_widget<Gtk::CheckButton>(_builder, "embed-images"))
    , _convert_colors(UI::get_widget<Gtk::CheckButton>(_builder, "convert-colors"))
    , _import_pages(UI::get_widget<Gtk::CheckButton>(_builder, "import-pages"))
    , _mesh_slider(UI::get_widget<Gtk::Scale>(_builder, "mesh-slider"))
    , _mesh_label(UI::get_widget<Gtk::Label>(_builder, "mesh-label"))
    , _next_page(UI::get_widget<Gtk::Button>(_builder, "next-page"))
    , _prev_page(UI::get_widget<Gtk::Button>(_builder, "prev-page"))
    , _current_page(UI::get_widget<Gtk::Label>(_builder, "current-page"))
    , _font_model(UI::get_object<Gtk::ListStore>(_builder, "font-list"))
    , _font_col(new FontModelColumns())
    , _ok_button(*Gtk::make_managed<Gtk::Button>(_("_OK"), true))
{
    assert(_pdf_doc);

    _setFonts(getPdfFonts(_pdf_doc));

    get_content_area()->set_homogeneous(false);
    get_content_area()->set_spacing(0);
    UI::pack_start(*get_content_area(), UI::get_widget<Gtk::Box>(_builder, "content"));

    this->set_title(_("PDF Import Settings"));
    this->set_modal(true);
    sp_transientize(*this);
    this->set_resizable(true);
    this->property_destroy_with_parent().set_value(false);

    this->add_action_widget(*Gtk::make_managed<Gtk::Button>(_("_Cancel"), true), -6);
    this->add_action_widget(_ok_button, -5);

    _render_thumb = false;

    // Connect signals
    _notebook.signal_switch_page().connect(sigc::mem_fun(*this, &PdfImportDialog::_onNotebookPageChanged));
    _next_page.signal_clicked().connect([this] { _setPreviewPage(_preview_page + 1); });
    _prev_page.signal_clicked().connect([this] { _setPreviewPage(_preview_page - 1); });
    _preview_area.set_draw_func(sigc::mem_fun(*this, &PdfImportDialog::_drawFunc));
    _page_numbers.signal_changed().connect(sigc::mem_fun(*this, &PdfImportDialog::_onPageNumberChanged));

#ifdef HAVE_POPPLER_CAIRO
    _render_thumb = true;

    // Disable the page selector when there's only one page
    _total_pages = _pdf_doc->getCatalog()->getNumPages();
    _page_numbers.set_sensitive(_total_pages > 1);

    // Create PopplerDocument
    std::string filename = _pdf_doc->getFileName()->getCString();
    if (!Glib::path_is_absolute(filename)) {
        filename = Glib::build_filename(Glib::get_current_dir(),filename);
    }
    Glib::ustring full_uri = Glib::filename_to_uri(filename);

    if (!full_uri.empty()) {
        _poppler_doc = poppler_document_new_from_file(full_uri.c_str(), NULL, NULL);
    }
#endif

    // Set default preview size
    _preview_width = 200;
    _preview_height = 300;
    _preview_area.set_size_request(_preview_width, _preview_height);

    // Init preview
    _thumb_data = nullptr;
    _current_pages = "all";
    _setPreviewPage(1);

    _ok_button.set_focusable();
    set_default_widget(_ok_button);
    set_focus(_ok_button);

    auto &font_strat = UI::get_object_raw<Gtk::CellRendererCombo>(_builder, "cell-strat");
    font_strat.signal_changed().connect([this](const Glib::ustring &path, const Gtk::TreeModel::iterator &source) {
        if (auto target = _font_model->get_iter(path)) {
            (*target)[_font_col->proc_id] = int((*source)[_font_col->id]);
            (*target)[_font_col->proc_label] = Glib::ustring((*source)[_font_col->family]);
        }
    });

    _mesh_slider.set_value(_mod->get_param_float("approximationPrecision", 2.0));
    _mesh_slider.get_adjustment()->signal_value_changed().connect([this]() {
          // Redisplay the comment on the current approximation precision setting
          // Evenly divides the interval of possible values between the available labels.
          static Glib::ustring labels[] = {
              Glib::ustring(C_("PDF input precision", "rough")), Glib::ustring(C_("PDF input precision", "medium")),
              Glib::ustring(C_("PDF input precision", "fine")), Glib::ustring(C_("PDF input precision", "very fine"))};

          auto adj = _mesh_slider.get_adjustment();
          double min = adj->get_lower();
          int comment_idx = (int)floor((adj->get_value() - min) / (adj->get_upper() - min) * 4.0);
          _mesh_label.set_label(labels[std::min(comment_idx, 3)]);
          _mod->set_param_float("approximationPrecision", _mesh_slider.get_value());
      });

    _clip_to.set_active_id(mod->get_param_optiongroup("clipTo"));
    _clip_to.signal_changed().connect([this]() {
        _mod->set_param_optiongroup("clipTo", _clip_to.get_active_id().c_str());
    });

    _group_by.set_active_id(mod->get_param_optiongroup("groupBy"));
    _group_by.signal_changed().connect([this]() {
        _mod->set_param_optiongroup("groupBy", _group_by.get_active_id().c_str());
    });

    _embed_images.set_active(mod->get_param_bool("embedImages", true));
    _embed_images.signal_toggled().connect([this]() {
        _mod->set_param_bool("embedImages", _embed_images.get_active());
    });
    _convert_colors.set_active(mod->get_param_bool("convertColors", true));
    _convert_colors.signal_toggled().connect([this]() {
        _mod->set_param_bool("convertColors", _convert_colors.get_active());
    });

    _import_pages.set_active(mod->get_param_bool("importPages", true));
    _import_pages.signal_toggled().connect([this]() {
        _mod->set_param_bool("importPages", _import_pages.get_active());
    });

    auto &font_render = UI::get_widget<Gtk::ComboBox>(_builder, "font-rendering");
    auto render_pref = mod->get_param_optiongroup("fontRendering");
    font_render.set_active_id(render_pref);
    // Update the font list with this as the default
    setFontStrategies(SvgBuilder::autoFontStrategies(pref_to_font_strategy(render_pref), _font_list));
    font_render.signal_changed().connect([this]() {
        auto &font_render = UI::get_widget<Gtk::ComboBox>(_builder, "font-rendering");
        auto active_id = font_render.get_active_id();
        _mod->set_param_optiongroup("fontRendering", active_id.c_str());
        // Reset any font specific items with the new global strategy
        setFontStrategies(SvgBuilder::autoFontStrategies(pref_to_font_strategy(active_id), _font_list));
    });
}

PdfImportDialog::~PdfImportDialog() {
#ifdef HAVE_POPPLER_CAIRO
    if (_poppler_doc) {
        g_object_unref(G_OBJECT(_poppler_doc));
    }
#endif
    if (_thumb_data) {
            gfree(_thumb_data);
    }
}

bool PdfImportDialog::showDialog() {
    gint b = UI::dialog_run(*this);
    if ( b == Gtk::ResponseType::OK ) {
        return TRUE;
    } else {
        return FALSE;
    }
}

bool PdfImportDialog::getImportPages()
{
    return _import_pages.get_active();
}

std::string PdfImportDialog::getSelectedPages()
{
    if (_page_numbers.get_sensitive()) {
        return _current_pages;
    }
    return "all";
}

PdfImportType PdfImportDialog::getImportMethod()
{
    return (PdfImportType)_notebook.get_current_page();
}

void PdfImportDialog::_onPageNumberChanged()
{
    _current_pages = _page_numbers.get_text();

    auto nums = parseIntRange(_current_pages, 1, _total_pages);
    if (!nums.empty()) {
        _setPreviewPage(*nums.begin());

        // The input is valid; treat it as such.
        _page_numbers.remove_css_class("error");
        _page_numbers.reset_state(Gtk::Accessible::State::INVALID);

        _ok_button.set_sensitive(true);
    } else {
        // Mark the entry as invalid.
        _page_numbers.add_css_class("error");

        // gtkmm doesn't seem to have a working version of this.
        gtk_accessible_update_state(GTK_ACCESSIBLE(_page_numbers.gobj()), GTK_ACCESSIBLE_STATE_INVALID, GTK_ACCESSIBLE_INVALID_TRUE, -1);

        // Disable OK button to ensure the user doesn't use an invalid page
        // selection.
        _ok_button.set_sensitive(false);
    }
}

void PdfImportDialog::_onNotebookPageChanged(Gtk::Widget *page, guint pageNum) {
    // If we're on the internal import tab, set OK sensitivity based on whether
    // inputs are valid.
    if (getImportMethod() == PdfImportType::PDF_IMPORT_INTERNAL) {
        auto page_nums = parseIntRange(_page_numbers.get_text(), 1, _total_pages);

        _ok_button.set_sensitive(!page_nums.empty());
    } else {
        _ok_button.set_sensitive(true);
    }
}

/**
 * Set a full list of all fonts in use for the whole PDF document.
 */
void PdfImportDialog::_setFonts(const FontList &fonts)
{
    _font_model->clear();
    _font_list = fonts;

    // Find all fonts on this one page
    /*std::set<int> found;
    FontInfoScanner page_scanner(_pdf_doc.get(), page-1);
    for (const FontInfo *font : page_scanner.scan(page)) {
        found.insert(font->getRef().num);
        delete font;
    }*/

    // Now add all fonts and mark the ones from this page
    for (auto pair : *fonts) {
        auto font = pair.first;
        auto &data = pair.second;
        auto row = *_font_model->append();

        row[_font_col->id] = font->getID()->num;
        row[_font_col->em] = false;
        row[_font_col->family] = !data.family.empty() ? data.family : data.name + " -> " + data.getSubstitute();
        row[_font_col->style] = data.style;
        row[_font_col->weight] = data.weight;
        row[_font_col->stretch] = data.stretch;
        // row[_font_col->pages] = data.pages;

        if (font->isCIDFont()) {
            row[_font_col->icon] = Glib::ustring("text-convert-to-regular");
        } else {
            row[_font_col->icon] = Glib::ustring(data.found ? "on" : "off-outline");
        }
    }
}

/**
 * Saves each decided font strategy to the Svg Builder object.
 */
FontStrategies PdfImportDialog::getFontStrategies()
{
    FontStrategies fs;
    for (auto child : _font_model->children()) {
        auto value = (FontFallback) int(child[_font_col->proc_id]);
        fs[child[_font_col->id]] = value;
    }
    return fs;
}

/**
 * Update the font strats.
 */
void PdfImportDialog::setFontStrategies(const FontStrategies &fs)
{
    for (auto child : _font_model->children()) {
        auto value = fs.at(child[_font_col->id]);
        child[_font_col->proc_id] = (int)value;
        switch (value) {
            case FontFallback::AS_SHAPES:
                child[_font_col->proc_label] = _("Convert to paths");
                break;
            case FontFallback::AS_TEXT:
                child[_font_col->proc_label] = _("Keep original font name");
                break;
            case FontFallback::AS_SUB:
                child[_font_col->proc_label] = _("Replace by closest-named installed font");
                break;
            case FontFallback::DELETE_TEXT:
                child[_font_col->proc_label] = _("Delete text");
                break;
        }
    }
}

#ifdef HAVE_POPPLER_CAIRO
/**
 * \brief Copies image data from a Cairo surface to a pixbuf
 *
 * Borrowed from libpoppler, from the file poppler-page.cc
 * Copyright (C) 2005, Red Hat, Inc.
 *
 */
static void copy_cairo_surface_to_pixbuf (cairo_surface_t *surface,
                                          GdkPixbuf       *pixbuf)
{
    int cairo_width, cairo_height, cairo_rowstride;
    unsigned char *pixbuf_data, *dst, *cairo_data;
    int pixbuf_rowstride, pixbuf_n_channels;
    unsigned int *src;
    int x, y;

    cairo_width = cairo_image_surface_get_width (surface);
    cairo_height = cairo_image_surface_get_height (surface);
    cairo_rowstride = cairo_image_surface_get_stride(surface);
    cairo_data = cairo_image_surface_get_data(surface);

    pixbuf_data = gdk_pixbuf_get_pixels (pixbuf);
    pixbuf_rowstride = gdk_pixbuf_get_rowstride (pixbuf);
    pixbuf_n_channels = gdk_pixbuf_get_n_channels (pixbuf);

    if (cairo_width > gdk_pixbuf_get_width (pixbuf))
        cairo_width = gdk_pixbuf_get_width (pixbuf);
    if (cairo_height > gdk_pixbuf_get_height (pixbuf))
        cairo_height = gdk_pixbuf_get_height (pixbuf);
    for (y = 0; y < cairo_height; y++)
    {
        src = reinterpret_cast<unsigned int *>(cairo_data + y * cairo_rowstride);
        dst = pixbuf_data + y * pixbuf_rowstride;
        for (x = 0; x < cairo_width; x++)
        {
            dst[0] = (*src >> 16) & 0xff;
            dst[1] = (*src >> 8) & 0xff;
            dst[2] = (*src >> 0) & 0xff;
            if (pixbuf_n_channels == 4)
                dst[3] = (*src >> 24) & 0xff;
            dst += pixbuf_n_channels;
            src++;
        }
    }
}

#endif

void PdfImportDialog::_drawFunc(const Cairo::RefPtr<Cairo::Context>& cr, int const width, int const height)
{
    // Check if we have a thumbnail at all
    if (!_thumb_data
#ifdef HAVE_POPPLER_CAIRO
        && !_cairo_surfaces[_preview_page]
#endif
    ) {
        return;
    }

    // Create the pixbuf for the thumbnail
    Glib::RefPtr<Gdk::Pixbuf> thumb;

    if (_render_thumb) {
        thumb = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, true,
                                    8, _thumb_width, _thumb_height);
    } else if (_thumb_data) {
        thumb = Gdk::Pixbuf::create_from_data(_thumb_data, Gdk::Colorspace::RGB,
            false, 8, _thumb_width, _thumb_height, _thumb_rowstride);
    }
    if (!thumb) {
        return;
    }

    // Set background to white
    if (_render_thumb) {
        thumb->fill(0xffffffff);
	Gdk::Cairo::set_source_pixbuf(cr, thumb, 0, 0);
	cr->paint();
    }

#ifdef HAVE_POPPLER_CAIRO
    // Copy the thumbnail image from the Cairo surface
    if (_render_thumb) {
        copy_cairo_surface_to_pixbuf(_cairo_surfaces[_preview_page].get(), thumb->gobj());
    }
#endif

    Gdk::Cairo::set_source_pixbuf(cr, thumb, 0, _render_thumb ? 0 : 20);
    cr->paint();
}

/**
 * \brief Renders the given page's thumbnail using Cairo
 */
void PdfImportDialog::_setPreviewPage(int page) {
    _previewed_page = _pdf_doc->getCatalog()->getPage(page);
    g_return_if_fail(_previewed_page);

    // Update the UI to select a different page
    _preview_page = page;
    _next_page.set_sensitive(page < _total_pages);
    _prev_page.set_sensitive(page > 1);
    std::ostringstream example;
    example << page << " / " << _total_pages;
    _current_page.set_label(example.str());

    // Update the font list with per-page highlighting
    // XXX Update this psuedo code with real code
    /*for (auto iter : _font_model->children()) {
        std::unorderd_list<int> *pages = row[_font_col->pages];
        row[_font_col->em] = bool(page in pages);
    }*/

    // Try to get a thumbnail from the PDF if possible
    if (!_render_thumb) {
        if (_thumb_data) {
            gfree(_thumb_data);
            _thumb_data = nullptr;
        }
        if (!_previewed_page->loadThumb(&_thumb_data,
             &_thumb_width, &_thumb_height, &_thumb_rowstride)) {
            return;
        }
        // Redraw preview area
        _preview_area.set_size_request(_thumb_width, _thumb_height + 20);
        _preview_area.queue_draw();
        return;
    }
#ifdef HAVE_POPPLER_CAIRO
    // Get page size by accounting for rotation
    double width, height;
    int rotate = _previewed_page->getRotate();
    if ( rotate == 90 || rotate == 270 ) {
        height = _previewed_page->getCropWidth();
        width = _previewed_page->getCropHeight();
    } else {
        width = _previewed_page->getCropWidth();
        height = _previewed_page->getCropHeight();
    }
    // Calculate the needed scaling for the page
    double scale_x = (double)_preview_width / width;
    double scale_y = (double)_preview_height / height;
    double scale_factor = ( scale_x > scale_y ) ? scale_y : scale_x;
    // Create new Cairo surface
    _thumb_width = (int)ceil( width * scale_factor );
    _thumb_height = (int)ceil( height * scale_factor );
    if (_poppler_doc && !_cairo_surfaces[page] && !_preview_rendering_in_progress) {
        // poppler_page_render() isn't safe to call concurrently for multiple
        // pages, so we render at most one page at a time. We'll restart
        // rendering in "onfinish" if necessary.
        _preview_rendering_in_progress = true;

        // Render page asynchronously in a separate thread. The dialog will
        // become available and responsive even if the rendering takes longer.

        auto cairo_surface = std::shared_ptr<cairo_surface_t>(
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, _thumb_width, _thumb_height), //
            cairo_surface_destroy);

        auto poppler_page = Util::GObjectPtr(poppler_document_get_page(_poppler_doc, page - 1));

        auto [src, dst] = Async::Channel::create();

        Async::fire_and_forget([cairo_surface, scale_factor, poppler_page = std::move(poppler_page),
                                channel = std::move(src), dialog = this, page]() mutable {
            cairo_t *cr = cairo_create(cairo_surface.get());
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0); // Set fill color to white
            cairo_paint(cr);                               // Clear it
            cairo_scale(cr, scale_factor, scale_factor);   // Use Cairo for resizing the image
            poppler_page_render(poppler_page.get(), cr);
            cairo_destroy(cr);
            channel.run([dialog, page] {
                dialog->_preview_rendering_in_progress = false;
                dialog->_preview_area.queue_draw();
                if (dialog->_preview_page != page) {
                    // Restart rendering for the current page
                    dialog->_setPreviewPage(dialog->_preview_page);
                }
            });
        });

        _channels.push_back(std::move(dst));
        _cairo_surfaces[page] = std::move(cairo_surface);
    }
    // Redraw preview area
    _preview_area.queue_draw();
#endif
}

////////////////////////////////////////////////////////////////////////////////

#ifdef HAVE_POPPLER_CAIRO
/// helper method
static cairo_status_t
        _write_ustring_cb(void *closure, const unsigned char *data, unsigned int length)
{
    Glib::ustring* stream = static_cast<Glib::ustring*>(closure);
    stream->append(reinterpret_cast<const char*>(data), length);

    return CAIRO_STATUS_SUCCESS;
}
#endif

/**
 * Parses the selected page of the given PDF document using PdfParser.
 */
std::unique_ptr<SPDocument> PdfInput::open(Input *mod, char const *uri, bool)
{
    // Initialize the globalParams variable for poppler
    if (!globalParams) {
        globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    }

    // PROOF: an .ai opens from Illustrator's own copy of its art when that reads fully and draws
    // like its PDF page, as Illustrator opens it; otherwise from its page, as before.
    if (mod && mod->get_id() && std::string_view(mod->get_id()) == "org.inkscape.input.ai" &&
        Inkscape::Preferences::get()->getBool("/options/aiimport/native", true)) {
        std::string reason;
        try {
            if (auto native = open_ai_native(uri, reason)) {
                for (auto const &note : native->notes) {
                    g_message("%s: %s", uri, note.c_str());
                }
                return std::move(native->document);
            }
        } catch (std::exception const &e) {
            reason = e.what();
        }
        g_message("%s opens from its PDF page: %s", uri, reason.c_str());
    }

    // Open the file using poppler
    // PDFDoc is from poppler. PDFDoc is used for preview and for native import.
    std::shared_ptr<PDFDoc> pdf_doc;

    // poppler does not use glib g_open. So on win32 we must use unicode call. code was copied from
    // glib gstdio.c
    pdf_doc = _POPPLER_MAKE_SHARED_PDFDOC(uri); // TODO: Could ask for password

    if (!pdf_doc->isOk()) {
        int error = pdf_doc->getErrorCode();
        if (error == errEncrypted) {
            g_message("Document is encrypted.");
        } else if (error == errOpenFile) {
            g_message("couldn't open the PDF file.");
        } else if (error == errBadCatalog) {
            g_message("couldn't read the page catalog.");
        } else if (error == errDamaged) {
            g_message("PDF file was damaged and couldn't be repaired.");
        } else if (error == errHighlightFile) {
            g_message("nonexistent or invalid highlight file.");
        } else if (error == errBadPrinter) {
            g_message("invalid printer.");
        } else if (error == errPrinting) {
            g_message("Error during printing.");
        } else if (error == errPermission) {
            g_message("PDF file does not allow that operation.");
        } else if (error == errBadPageNum) {
            g_message("invalid page number.");
        } else if (error == errFileIO) {
            g_message("file IO error.");
        } else {
            g_message("Failed to load document from data (error %d)", error);
        }

        return nullptr;
    }

    std::optional<PdfImportDialog> dlg;
    if (INKSCAPE.use_gui()) {
        dlg.emplace(pdf_doc, uri, mod);
        if (!dlg->showDialog()) {
            throw Input::open_cancelled();
        }
    }

    // Get options
    bool import_pages = true;
    std::string page_nums = "1";
    PdfImportType import_method = PdfImportType::PDF_IMPORT_INTERNAL;
    FontStrategies font_strats;
    bool convert_colors = true;
    std::string group_by = "by-xobject";
    if (dlg) {
        page_nums = dlg->getSelectedPages();
        import_pages = dlg->getImportPages();
        import_method = dlg->getImportMethod();
        font_strats = dlg->getFontStrategies();
    } else {
        page_nums = INKSCAPE.get_pages();
        auto strat = (FontStrategy)INKSCAPE.get_pdf_font_strategy();
        font_strats = SvgBuilder::autoFontStrategies(strat, getPdfFonts(pdf_doc));
#ifdef HAVE_POPPLER_CAIRO
        import_method = (PdfImportType)INKSCAPE.get_pdf_poppler();
#endif
        convert_colors = INKSCAPE.get_pdf_convert_colors();
        group_by = INKSCAPE.get_pdf_group_by();
    }
    // Both poppler and poppler+cairo can get page num info from poppler.
    auto pages = parseIntRange(page_nums, 1, pdf_doc->getCatalog()->getNumPages());
    g_assert(!pages.empty());

    // Create Inkscape document from file
    std::unique_ptr<SPDocument> doc;
    bool saved = false;
    if (import_method == PdfImportType::PDF_IMPORT_INTERNAL) {
        // Create document
        doc = SPDocument::createNewDoc(nullptr, true);
        saved = DocumentUndo::getUndoSensitive(doc.get());
        DocumentUndo::setUndoSensitive(doc.get(), false); // No need to undo in this temporary document

        // Create builder
        gchar *docname = g_path_get_basename(uri);
        gchar *dot = g_strrstr(docname, ".");
        if (dot) {
            *dot = 0;
        }
        SvgBuilder *builder = new SvgBuilder(doc.get(), docname, pdf_doc->getXRef());
        builder->setFontStrategies(font_strats);
        builder->setPageMode(import_pages);

        // Get preferences
        builder->setPageMode(mod->get_param_bool("importPages", true));
        builder->setEmbedImages(mod->get_param_bool("embedImages", true));
        builder->setConvertColors(dlg ? mod->get_param_bool("convertColors", true) : convert_colors);
        builder->setGroupBy(dlg ? mod->get_param_optiongroup("groupBy") : group_by);
        std::string crop_to = mod->get_param_optiongroup("clipTo", "none");

        double color_delta = mod->get_param_float("approximationPrecision", 2.0);

        for (auto p : pages) {
            // And then add each of the pages
            add_builder_page(pdf_doc, builder, doc.get(), p, crop_to, color_delta);
        }

        delete builder;
        g_free(docname);
#ifdef HAVE_POPPLER_CAIRO
    } else if (import_method == PdfImportType::PDF_IMPORT_CAIRO) {
        // the poppler import

        std::string full_path = uri;
        if (!Glib::path_is_absolute(uri)) {
            full_path = Glib::build_filename(Glib::get_current_dir(),uri);
        }
        Glib::ustring full_uri = Glib::filename_to_uri(full_path);

        GError *error = NULL;
        /// @todo handle password
        /// @todo check if win32 unicode needs special attention
        PopplerDocument* document = poppler_document_new_from_file(full_uri.c_str(), NULL, &error);

        if(error != NULL) {
            std::cerr << "PDFInput::open: error opening document: " << full_uri.raw() << std::endl;
            g_error_free (error);
            return nullptr;
        }

        int page_num = *pages.begin();
        if (PopplerPage* page = poppler_document_get_page(document, page_num - 1)) {
            double width, height;
            poppler_page_get_size(page, &width, &height);

            Glib::ustring output;
            cairo_surface_t* surface = cairo_svg_surface_create_for_stream(Inkscape::Extension::Internal::_write_ustring_cb,
                                                                           &output, width, height);

            // Reset back to PT for cairo 1.17.6 and above which sets to UNIT_USER
            cairo_svg_surface_set_document_unit(surface, CAIRO_SVG_UNIT_PT);

            // This magical function results in more fine-grain fallbacks. In particular, a mesh
            // gradient won't necessarily result in the whole PDF being rasterized. Of course, SVG
            // 1.2 never made it as a standard, but hey, we'll take what we can get. This trick was
            // found by examining the 'pdftocairo' code.
            cairo_svg_surface_restrict_to_version( surface, CAIRO_SVG_VERSION_1_2 );

            cairo_t* cr = cairo_create(surface);

            poppler_page_render_for_printing(page, cr);
            cairo_show_page(cr);

            cairo_destroy(cr);
            cairo_surface_destroy(surface);

            doc = SPDocument::createNewDocFromMem(output.raw());

            g_object_unref(G_OBJECT(page));
        } else if (document) {
            std::cerr << "PDFInput::open: error opening page " << page_num << " of document: " << full_uri.raw() << std::endl;
        }
        g_object_unref(G_OBJECT(document));

        if (!doc) {
            return nullptr;
        }

        saved = DocumentUndo::getUndoSensitive(doc.get());
        DocumentUndo::setUndoSensitive(doc.get(), false); // No need to undo in this temporary document
#endif
    }

    // Set viewBox if it doesn't exist
    if (!doc->getRoot()->viewBox_set) {
        doc->setViewBox(Geom::Rect::from_xywh(0, 0, doc->getWidth().value(doc->getDisplayUnit()), doc->getHeight().value(doc->getDisplayUnit())));
    }

    // Restore undo
    DocumentUndo::setUndoSensitive(doc.get(), saved);

    return doc;
}

/**
 * Parses the selected page object of the given PDF document using PdfParser.
 */
void
PdfInput::add_builder_page(std::shared_ptr<PDFDoc>pdf_doc, SvgBuilder *builder, SPDocument *doc, int page_num, std::string const &crop_to, double color_delta)
{
    // Check page exists
    Catalog *catalog = pdf_doc->getCatalog();
    sanitize_page_number(page_num, catalog->getNumPages());
    Page *page = catalog->getPage(page_num);
    if (!page) {
        std::cerr << "PDFInput::open: error opening page " << page_num << std::endl;
        return;
    }

    // Apply crop settings
#if POPPLER_CHECK_VERSION(26, 6, 0)
    std::optional<PDFRectangle> clipToBox;
#else
    _POPPLER_CONST PDFRectangle *clipToBox = nullptr;
#endif

    if (crop_to == "media-box") {
        clipToBox = page->getMediaBox();
    } else if (crop_to == "crop-box") {
        clipToBox = page->getCropBox();
    } else if (crop_to == "trim-box") {
        clipToBox = page->getTrimBox();
    } else if (crop_to == "bleed-box") {
        clipToBox = page->getBleedBox();
    } else if (crop_to == "art-box") {
        clipToBox = page->getArtBox();
    }

    std::optional<PDFRectangle> cropBox;
#if POPPLER_CHECK_VERSION(26, 6, 0)
    cropBox = clipToBox;
#else
    if (clipToBox) {
        cropBox = *clipToBox;
    }
#endif
    // Create parser  (extension/internal/pdfinput/pdf-parser.h)
    auto pdf_parser = PdfParser(pdf_doc, builder, page, cropBox);

    // Set up approximation precision for parser. Used for converting Mesh Gradients into tiles.
    if ( color_delta <= 0.0 ) {
        color_delta = 1.0 / 2.0;
    } else {
        color_delta = 1.0 / color_delta;
    }
    pdf_parser.setApproximationPrecision(color_delta, 6);

    // Parse the document structure
    Object obj = page->getContents();
    if (!obj.isNull()) {
        pdf_parser.parse(&obj);
    }

    // Parse the annotations
    if (auto annots = page->getAnnotsObject(); annots.isArray()) {
        auto* annotsArray = annots.getArray();
        auto const size = annotsArray->getLength();
        for (int i = 0; i < size; i++) {
            pdf_parser.build_annots(annotsArray->get(i), page_num);
        }
    }
}

#include "../clear-n_.h"

void PdfInput::init() {
    /* PDF in */
    // clang-format off
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("PDF Input") "</name>\n"
            "<id>org.inkscape.input.pdf</id>\n"
            PDF_COMMON_INPUT_PARAMS
            "<input>\n"
                "<extension>.pdf</extension>\n"
                "<mimetype>application/pdf</mimetype>\n"
                "<filetypename>" N_("Portable Document Format (*.pdf)") "</filetypename>\n"
                "<filetypetooltip>" N_("Portable Document Format") "</filetypetooltip>\n"
            "</input>\n"
        "</inkscape-extension>", std::make_unique<PdfInput>());
    // clang-format on

    /* AI in */
    // clang-format off
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("AI Input") "</name>\n"
            "<id>org.inkscape.input.ai</id>\n"
            PDF_COMMON_INPUT_PARAMS
            "<input>\n"
                "<extension>.ai</extension>\n"
                "<mimetype>image/x-adobe-illustrator</mimetype>\n"
                "<filetypename>" N_("Adobe Illustrator 9.0 and above (*.ai)") "</filetypename>\n"
                "<filetypetooltip>" N_("Open files saved in Adobe Illustrator 9.0 and newer versions") "</filetypetooltip>\n"
            "</input>\n"
        "</inkscape-extension>", std::make_unique<PdfInput>());
    // clang-format on
} // init

} } }  /* namespace Inkscape, Extension, Implementation */

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
