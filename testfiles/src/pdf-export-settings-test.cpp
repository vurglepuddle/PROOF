// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include "extension/db.h"
#include "extension/implementation/implementation.h"
#include "extension/internal/pdfoutput/pdf-output.h"
#include "extension/output.h"
#include "extension/system.h"
#include "inkscape.h"
#include "preferences.h"
#include "ui/dialog/export-pdf-dialog.h"

namespace {
using namespace Inkscape;
using UI::Dialog::PdfExportSettings;
constexpr char const *MIGRATED = "/dialogs/export/pdf/compatibility_migrated";
constexpr char const *VERSION = "/extensions/org.inkscape.output.pdf.capypdf.PDFversion";

class PdfPrefsImplementation : public Extension::Implementation::Implementation
{
public:
    unsigned calls = 0;
    bool settings = false;
    bool accepted = false;
    std::optional<bool> output_prefs(Extension::Output *, bool mode) override
    {
        ++calls;
        settings = mode;
        return accepted;
    }
};

class PdfExportSettingsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
        _gui = INKSCAPE.use_gui();
        INKSCAPE.use_gui(false);
        _migrated = Preferences::get()->getBool(MIGRATED);
        Preferences::get()->setBool(MIGRATED, false);
    }

    void TearDown() override
    {
        INKSCAPE.use_gui(_gui);
        Preferences::get()->setBool(MIGRATED, _migrated);
    }

    Extension::Output *output(bool pdf17 = true)
    {
        static unsigned serial = 0;
        auto const id = "org.inkscape.test.pdf-options." + std::to_string(++serial);
        auto implementation = std::make_unique<PdfPrefsImplementation>();
        imp = implementation.get();
        std::string xml = "<inkscape-extension xmlns='http://www.inkscape.org/namespace/inkscape/extension'>"
            "<name>PDF options test</name><id>" + id + "</id>"
            "<param name='PDFversion' gui-text='Version' type='optiongroup'><option value='PDF-2.0'>PDF 2.0</option>";
        if (pdf17) xml += "<option value='PDF-1.7'>PDF 1.7</option>";
        xml += "</param><param name='textToPath' gui-text='Text' type='optiongroup'><option value='embed'>Embed</option>"
            "<option value='paths'>Paths</option><option value='LaTeX'>LaTeX</option></param>"
            "<param name='blurToBitmap' gui-text='Filters' type='bool'>true</param>"
            "<param name='resolution' gui-text='Resolution' type='int' min='1' max='10000'>96</param>"
            "<output><extension>.pdf</extension><mimetype>application/pdf</mimetype>"
            "<filetypename>PDF options test</filetypename></output></inkscape-extension>";
        Extension::build_from_mem(xml.c_str(), std::move(implementation));
        return dynamic_cast<Extension::Output *>(Extension::db.get(id.c_str()));
    }

    PdfPrefsImplementation *imp = nullptr;
    bool _gui = false;
    bool _migrated = false;
};

TEST_F(PdfExportSettingsTest, StagingAndDiscardDoNotChangeParametersOrMigration)
{
    auto out = output();
    ASSERT_TRUE(out);
    {
        auto settings = PdfExportSettings::load(*out);
        EXPECT_EQ(settings.version, "PDF-2.0");
        EXPECT_EQ(settings.text, "embed");
        EXPECT_TRUE(settings.rasterize);
        EXPECT_EQ(settings.resolution, 96);
        settings.text = "paths";
        settings.rasterize = false;
        settings.resolution = 300;
        EXPECT_TRUE(settings.has_warnings());
    }
    EXPECT_TRUE(out->get_param_optiongroup_is("PDFversion", "PDF-2.0"));
    EXPECT_TRUE(out->get_param_optiongroup_is("textToPath", "embed"));
    EXPECT_TRUE(out->get_param_bool("blurToBitmap"));
    EXPECT_EQ(out->get_param_int("resolution"), 96);
    EXPECT_FALSE(Preferences::get()->getBool(MIGRATED));
}

TEST_F(PdfExportSettingsTest, AcceptanceAppliesAllOptionsWithoutMigration)
{
    auto out = output();
    ASSERT_TRUE(out);
    auto settings = PdfExportSettings::load(*out);
    settings.version = "PDF-1.7";
    EXPECT_FALSE(settings.has_warnings());
    settings.text = "LaTeX";
    settings.rasterize = false;
    settings.resolution = 300;
    settings.apply(*out);
    EXPECT_TRUE(out->get_param_optiongroup_is("PDFversion", "PDF-1.7"));
    EXPECT_TRUE(out->get_param_optiongroup_is("textToPath", "LaTeX"));
    EXPECT_FALSE(out->get_param_bool("blurToBitmap"));
    EXPECT_EQ(out->get_param_int("resolution"), 300);
    EXPECT_FALSE(Preferences::get()->getBool(MIGRATED));
    settings.version = "PDF-2.0";
    settings.apply(*out);
    EXPECT_EQ(PdfExportSettings::load(*out).version, "PDF-2.0");
}

TEST_F(PdfExportSettingsTest, InitializationMigratesSavedPdf20BeforeReadingParametersOnlyOnce)
{
    using Extension::Internal::PdfOutput;
    if (!PdfOutput::supports_pdf17()) GTEST_SKIP() << "PDF 1.7 backend unavailable";
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    prefs->remove(MIGRATED);
    prefs->setString(VERSION, "PDF-2.0");
    PdfOutput::init();
    auto out = dynamic_cast<Extension::Output *>(Extension::db.get("org.inkscape.output.pdf.capypdf"));
    ASSERT_TRUE(out);
    EXPECT_TRUE(prefs->getBool(MIGRATED));
    EXPECT_EQ(prefs->getString(VERSION), "PDF-1.7");
    EXPECT_TRUE(out->get_param_optiongroup_is("PDFversion", "PDF-1.7"));
    out->set_param_optiongroup("PDFversion", "PDF-2.0");
    PdfOutput::init();
    out = dynamic_cast<Extension::Output *>(Extension::db.get("org.inkscape.output.pdf.capypdf"));
    ASSERT_TRUE(out);
    EXPECT_EQ(prefs->getString(VERSION), "PDF-2.0");
    EXPECT_TRUE(out->get_param_optiongroup_is("PDFversion", "PDF-2.0"));
}

TEST_F(PdfExportSettingsTest, InitializationMarksFreshAndPdf17ProfilesBeforeAnExplicitPdf20Choice)
{
    using Extension::Internal::PdfOutput;
    if (!PdfOutput::supports_pdf17()) GTEST_SKIP() << "PDF 1.7 backend unavailable";
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    for (char const *saved : {"", "PDF-1.7"}) {
        SCOPED_TRACE(saved);
        prefs->remove(MIGRATED);
        prefs->remove(VERSION);
        if (*saved) prefs->setString(VERSION, saved);
        PdfOutput::init();
        auto out = dynamic_cast<Extension::Output *>(Extension::db.get("org.inkscape.output.pdf.capypdf"));
        ASSERT_TRUE(out);
        EXPECT_TRUE(prefs->getBool(MIGRATED));
        EXPECT_TRUE(out->get_param_optiongroup_is("PDFversion", "PDF-1.7"));
        out->set_param_optiongroup("PDFversion", "PDF-2.0");
        PdfOutput::init();
        out = dynamic_cast<Extension::Output *>(Extension::db.get("org.inkscape.output.pdf.capypdf"));
        ASSERT_TRUE(out);
        EXPECT_EQ(prefs->getString(VERSION), "PDF-2.0");
        EXPECT_TRUE(out->get_param_optiongroup_is("PDFversion", "PDF-2.0"));
    }
}

TEST_F(PdfExportSettingsTest, BackendWithoutPdf17KeepsPdf20)
{
    auto out = output(false);
    ASSERT_TRUE(out);
    auto settings = PdfExportSettings::load(*out);
    EXPECT_EQ(settings.version, "PDF-2.0");
    EXPECT_TRUE(settings.has_warnings());
    EXPECT_FALSE(out->get_param_optiongroup_contains("PDFversion", "PDF-1.7"));
}

TEST_F(PdfExportSettingsTest, HeadlessPreferencesNeverInvokeTheDialog)
{
    auto out = output();
    ASSERT_TRUE(out);
    EXPECT_TRUE(out->prefs());
    EXPECT_TRUE(out->prefs(true));
    EXPECT_EQ(imp->calls, 0u);
}

TEST_F(PdfExportSettingsTest, ExtensionGuiFlagSuppressesDialogEvenInGuiApplication)
{
    auto out = output();
    ASSERT_TRUE(out);
    INKSCAPE.use_gui(true); // Only the flag: no GTK application or window is created.
    out->set_gui(false);
    EXPECT_TRUE(out->prefs());
    EXPECT_TRUE(out->prefs(true));
    EXPECT_EQ(imp->calls, 0u);
}

TEST_F(PdfExportSettingsTest, HookPreservesCancellationAndSettingsMode)
{
    auto out = output();
    ASSERT_TRUE(out);
    INKSCAPE.use_gui(true);
    EXPECT_FALSE(out->prefs(true));
    EXPECT_EQ(imp->calls, 1u);
    EXPECT_TRUE(imp->settings);
    imp->accepted = true;
    EXPECT_TRUE(out->prefs());
    EXPECT_EQ(imp->calls, 2u);
    EXPECT_FALSE(imp->settings);
}

} // namespace
