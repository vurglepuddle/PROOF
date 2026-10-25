// SPDX-License-Identifier: GPL-2.0-or-later
/** @file PROOF's PDF export options, shared by saving and the Export panel. */

#include "export-pdf-dialog.h"

#include <algorithm>
#include <array>
#include <glibmm/i18n.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/dialog.h>
#include <gtkmm/frame.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stack.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/textview.h>

#include "desktop.h"
#include "extension/output.h"
#include "inkscape.h"
#include "ui/dialog-run.h"

namespace Inkscape::UI::Dialog {

PdfExportSettings PdfExportSettings::load(Extension::Output const &output)
{
    PdfExportSettings settings;
    settings.version = output.get_param_optiongroup("PDFversion");
    bool const pdf17 = output.get_param_optiongroup_contains("PDFversion", "PDF-1.7");
    if (!output.get_param_optiongroup_contains("PDFversion", settings.version.c_str())) {
        settings.version = pdf17 ? "PDF-1.7" : "PDF-2.0";
    }
    settings.text = output.get_param_optiongroup("textToPath");
    if (!output.get_param_optiongroup_contains("textToPath", settings.text.c_str())) settings.text = "embed";
    settings.strokes = output.get_param_bool("strokesToPath");
    settings.rasterize = output.get_param_bool("blurToBitmap");
    settings.resolution = std::clamp(output.get_param_int("resolution"), 1, 10000);
    return settings;
}

void PdfExportSettings::apply(Extension::Output &output) const
{
    output.set_param_optiongroup("PDFversion", version.c_str());
    output.set_param_optiongroup("textToPath", text.c_str());
    output.set_param_bool("strokesToPath", strokes);
    output.set_param_bool("blurToBitmap", rasterize);
    output.set_param_int("resolution", std::clamp(resolution, 1, 10000));
}

namespace {

Gtk::Label *label(Glib::ustring const &text)
{
    auto widget = Gtk::make_managed<Gtk::Label>(text);
    widget->set_xalign(0);
    widget->set_wrap();
    return widget;
}

Gtk::Box &group(Gtk::Box &page, Glib::ustring const &title)
{
    auto frame = Gtk::make_managed<Gtk::Frame>(title);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(10);
    frame->set_child(*box);
    page.append(*frame);
    return *box;
}

class ExportPdfDialog : public Gtk::Dialog
{
public:
    ExportPdfDialog(Extension::Output &output, bool settings)
        : Gtk::Dialog(_("Export PDF"), true)
        , _settings(PdfExportSettings::load(output))
    {
        set_name("ExportPdfDialog");
        _css = Gtk::CssProvider::create();
        _css->load_from_data("#ExportPdfDialog, #ExportPdfDialog * { border-radius: 0; }");
        Gtk::StyleContext::add_provider_for_display(get_display(), _css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        set_default_size(665, 563);
        if (auto desktop = SP_ACTIVE_DESKTOP) desktop->setWindowTransient(*this);

        auto content = get_content_area();
        content->set_margin(10);
        content->set_spacing(10);
        auto top = Gtk::make_managed<Gtk::Grid>();
        top->set_column_spacing(8);
        top->set_row_spacing(10);
        content->append(*top);
        auto preset = Gtk::make_managed<Gtk::ComboBoxText>();
        preset->append("[PROOF Default]");
        preset->set_active(0);
        preset->set_sensitive(false);
        preset->set_hexpand();
        auto standard = Gtk::make_managed<Gtk::ComboBoxText>();
        standard->append(_("None"));
        standard->set_active(0);
        standard->set_sensitive(false);
        standard->set_hexpand();
        top->attach(*label(_("PDF Preset:")), 0, 0);
        top->attach(*preset, 1, 0, 3, 1);
        top->attach(*label(_("Standard:")), 0, 1);
        top->attach(*standard, 1, 1);
        top->attach(*label(_("Compatibility:")), 2, 1);
        top->attach(_compatibility, 3, 1);
        if (output.get_param_optiongroup_contains("PDFversion", "PDF-1.7")) {
            _compatibility.append("PDF-1.7", _("Acrobat 8 (PDF 1.7)"));
        }
        _compatibility.append("PDF-2.0", _("PDF 2.0"));
        _compatibility.set_active_id(_settings.version);

        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        body->set_vexpand();
        content->append(*body);
        auto sidebar = Gtk::make_managed<Gtk::Frame>();
        sidebar->set_child(_sections);
        sidebar->set_size_request(105, -1);
        body->append(*sidebar);
        _pages.set_hexpand();
        body->append(_pages);
        _sections.set_selection_mode(Gtk::SelectionMode::SINGLE);
        _sections.set_activate_on_single_click();

        std::array<Glib::ustring, 7> const titles = {
            _("General"), _("Compression"), _("Marks and Bleeds"), _("Output"),
            _("Advanced"), _("Security"), _("Summary")
        };
        std::array<Gtk::Box *, 7> pages;
        for (unsigned i = 0; i < titles.size(); ++i) {
            auto name = label(titles[i]);
            name->set_margin(4);
            _sections.append(*name);
            if (i == 6) _summary_title = name;
            auto page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
            page->append(*label(titles[i]));
            page->append(*Gtk::make_managed<Gtk::Separator>());
            _pages.add(*page, std::to_string(i));
            pages[i] = page;
        }

        auto description = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto description_label = label(_("Description:"));
        description_label->set_valign(Gtk::Align::START);
        description->append(*description_label);
        auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
        scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scroll->set_has_frame();
        scroll->set_min_content_height(95);
        scroll->set_hexpand();
        _description.set_editable(false);
        _description.set_cursor_visible(false);
        _description.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
        _description.set_margin(6);
        scroll->set_child(_description);
        description->append(*scroll);
        pages[0]->append(*description);
        group(*pages[0], _("Options")).append(*label(_("Editing capabilities, viewing after saving, layers, hyperlinks, thumbnails and Fast Web View options will come in later versions.")));
        pages[1]->append(*label(_("Image downsampling, compression and quality options will come in later versions.")));
        pages[2]->append(*label(_("Printer's marks and bleed options will come in later versions. The bleed is each page's own, set with the Page tool.")));
        pages[3]->append(*label(_("Color conversion, profiles and output intent options will come in later versions.")));

        auto text_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        text_row->append(*label(_("Text:")));
        _text.append("embed", _("Embed fonts"));
        _text.append("paths", _("Convert to outlines"));
        _text.append("LaTeX", _("Omit text, write a LaTeX file"));
        _text.set_active_id(_settings.text);
        _text.set_hexpand();
        text_row->append(_text);
        auto &outlines = group(*pages[4], _("Outlines"));
        outlines.append(*text_row);
        _strokes.set_label(_("Convert strokes to outlines"));
        _strokes.set_active(_settings.strokes);
        outlines.append(_strokes);
        outlines.append(*label(_("Layers and inks named for cutting (Cut, CutContour, Die line, Thru-cut, ...) are never converted.")));
        auto &effects_group = group(*pages[4], _("Effects"));
        _rasterize.set_label(_("Rasterize filter effects"));
        _rasterize.set_active(_settings.rasterize);
        effects_group.append(_rasterize);
        auto effects = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        effects->append(*label(_("Resolution:")));
        _resolution_preset.append("72", _("Screen (72 ppi)"));
        _resolution_preset.append("150", _("Medium (150 ppi)"));
        _resolution_preset.append("300", _("High (300 ppi)"));
        _resolution_preset.append("other", _("Other"));
        auto const dpi = _settings.resolution;
        _resolution_preset.set_active_id(dpi == 72 || dpi == 150 || dpi == 300 ? std::to_string(dpi) : "other");
        effects->append(_resolution_preset);
        _resolution.set_range(1, 10000);
        _resolution.set_increments(1, 10);
        _resolution.set_digits(0);
        _resolution.set_numeric();
        _resolution.set_value(_settings.resolution);
        _resolution.set_width_chars(5);
        _custom_resolution.append(_resolution);
        _custom_resolution.append(*label(_("ppi")));
        effects->append(_custom_resolution);
        effects_group.append(*effects);
        pages[5]->append(*label(_("Passwords and permissions aren't offered.")));
        _summary.set_xalign(0);
        _summary.set_yalign(0);
        _summary.set_wrap();
        pages[6]->append(_summary);

        _sections.signal_row_selected().connect([this](Gtk::ListBoxRow *row) {
            if (row) _pages.set_visible_child(std::to_string(row->get_index()));
        });
        _sections.select_row(*_sections.get_row_at_index(0));
        _compatibility.signal_changed().connect(sigc::mem_fun(*this, &ExportPdfDialog::update));
        _text.signal_changed().connect(sigc::mem_fun(*this, &ExportPdfDialog::update));
        _strokes.signal_toggled().connect(sigc::mem_fun(*this, &ExportPdfDialog::update));
        _rasterize.signal_toggled().connect(sigc::mem_fun(*this, &ExportPdfDialog::update));
        _resolution_preset.signal_changed().connect(sigc::mem_fun(*this, &ExportPdfDialog::update));
        _resolution.signal_value_changed().connect(sigc::mem_fun(*this, &ExportPdfDialog::update));
        update();

        auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        buttons->set_halign(Gtk::Align::END);
        buttons->set_margin_top(0);
        auto save = Gtk::make_managed<Gtk::Button>(settings ? _("OK") : _("Save PDF"));
        auto cancel = Gtk::make_managed<Gtk::Button>(_("Cancel"));
        for (auto button : {save, cancel}) {
            button->set_size_request(90, -1);
            button->add_css_class("dialog-cmd-button");
            buttons->append(*button);
        }
        save->signal_clicked().connect([this] {
            _resolution.update();
            update();
            response(Gtk::ResponseType::OK);
        });
        cancel->signal_clicked().connect([this] { response(Gtk::ResponseType::CANCEL); });
        content->append(*buttons);
        set_default_widget(*save);
    }

    ~ExportPdfDialog() override
    {
        Gtk::StyleContext::remove_provider_for_display(get_display(), _css);
    }

    PdfExportSettings const &settings() const { return _settings; }

private:
    void update()
    {
        _settings.version = _compatibility.get_active_id();
        _settings.text = _text.get_active_id();
        _settings.strokes = _strokes.get_active();
        _settings.rasterize = _rasterize.get_active();
        auto const preset = _resolution_preset.get_active_id();
        bool const custom = preset == "other";
        _settings.resolution = custom ? _resolution.get_value_as_int() : std::stoi(preset.raw());
        _custom_resolution.set_visible(custom);
        _resolution_preset.set_sensitive(_settings.rasterize);
        _resolution.set_sensitive(_settings.rasterize);
        bool const pdf17 = _settings.version == "PDF-1.7";
        _description.get_buffer()->set_text(pdf17
            ? _("Creates PDF 1.7 files that open in Illustrator and in Acrobat and Adobe Reader 8 and later.")
            : _("Creates PDF 2.0 files. Illustrator 29 can't open them; Acrobat and Adobe Reader open them."));
        Glib::ustring text = _("Compatibility:") + Glib::ustring(" ") + _compatibility.get_active_text() + "\n" +
            _("Text:") + Glib::ustring(" ") + _text.get_active_text() + "\n";
        text += _settings.strokes ? _("Strokes: Convert to outlines\n") : _("Strokes: Keep strokes\n");
        text += _settings.rasterize
            ? Glib::ustring::compose(_("Rasterize filter effects at %1 ppi"), _settings.resolution)
            : _("Filter effects are not rasterized.");
        if (!pdf17) text += "\n\n" + Glib::ustring(_("PDF 2.0 cannot be opened in Illustrator 29."));
        if (_settings.text == "paths") text += "\n\n" + Glib::ustring(_("Text will be converted to outlines."));
        if (_settings.text == "LaTeX") text += "\n\n" + Glib::ustring(_("Text will be omitted from the PDF and written to a LaTeX file."));
        if (_settings.strokes) text += "\n\n" + Glib::ustring(_("Strokes will be converted to outlines."));
        _summary.set_text(text);
        _summary_title->set_text(_settings.has_warnings() ? _("Summary!") : _("Summary"));
    }

    PdfExportSettings _settings;
    Glib::RefPtr<Gtk::CssProvider> _css;
    Gtk::ComboBoxText _compatibility;
    Gtk::ComboBoxText _text;
    Gtk::CheckButton _strokes;
    Gtk::CheckButton _rasterize;
    Gtk::ComboBoxText _resolution_preset;
    Gtk::Box _custom_resolution{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::SpinButton _resolution;
    Gtk::ListBox _sections;
    Gtk::Stack _pages;
    Gtk::TextView _description;
    Gtk::Label _summary;
    Gtk::Label *_summary_title = nullptr;
};

} // namespace

bool export_pdf_dialog(Extension::Output &output, bool settings)
{
    ExportPdfDialog dialog(output, settings);
    if (UI::dialog_run(dialog) != Gtk::ResponseType::OK) return false;
    dialog.settings().apply(output);
    return true;
}

} // namespace Inkscape::UI::Dialog
