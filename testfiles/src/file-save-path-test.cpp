// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <glibmm/miscutils.h>

#include "document.h"
#include "extension/system.h"
#include "inkscape.h"
#include "preferences.h"

namespace {
using namespace Inkscape;
using namespace Extension;

class FileSavePathTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
    }
};

TEST_F(FileSavePathTest, FolderPreferenceControlsPriorityForSaveAsAndCopy)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    auto doc = SPDocument::createNewDocFromMem(std::string("<svg xmlns='http://www.w3.org/2000/svg'/>"));
    ASSERT_TRUE(doc);
    auto directory = Glib::get_tmp_dir();
    doc->setDocumentFilename(Glib::build_filename(directory, "proof-save-folder.svg").c_str());
    prefs->setString("/dialogs/file/last_path", Glib::get_home_dir());
    prefs->remove("/dialogs/save_as/use_current_dir");
    prefs->setBool("/dialogs/save_copy/use_current_dir", false);
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_AS), directory);
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_COPY), directory);
    prefs->setBool("/dialogs/save_as/use_current_dir", false);
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_AS), Glib::get_home_dir());
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_COPY), Glib::get_home_dir());
    prefs->setBool("/dialogs/save_as/use_current_dir", true);
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_AS), directory);
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_COPY), directory);
}

TEST_F(FileSavePathTest, MissingSharedFolderFallsBackToTheDocumentWithPreferenceOff)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    auto doc = SPDocument::createNewDocFromMem(std::string("<svg xmlns='http://www.w3.org/2000/svg'/>"));
    ASSERT_TRUE(doc);
    auto directory = Glib::get_tmp_dir();
    doc->setDocumentFilename(Glib::build_filename(directory, "proof-save-folder.svg").c_str());
    prefs->setBool("/dialogs/save_as/use_current_dir", false);
    prefs->setString("/dialogs/file/last_path", Glib::build_filename(directory, "proof-no-such-folder"));
    prefs->setString("/dialog/open/path", Glib::get_home_dir());
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_AS), directory);
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_COPY), directory);
}

TEST_F(FileSavePathTest, UnsavedDocumentSharesTheLatestOpenOrSaveFolder)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    auto doc = SPDocument::createNewDocFromMem(std::string("<svg xmlns='http://www.w3.org/2000/svg'/>"));
    ASSERT_TRUE(doc);
    prefs->setString("/dialogs/file/last_path", Glib::get_tmp_dir());
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_AS), Glib::get_tmp_dir());
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_COPY), Glib::get_tmp_dir());
    store_save_path_in_prefs(Glib::get_home_dir(), FILE_SAVE_METHOD_SAVE_COPY);
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_AS), Glib::get_home_dir());
    store_save_path_in_prefs(Glib::get_current_dir(), FILE_SAVE_METHOD_SAVE_AS);
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_COPY), Glib::get_current_dir());
}

TEST_F(FileSavePathTest, OldOpenHistoryAndMissingDocumentFoldersFallBackToAValidDirectory)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    auto doc = SPDocument::createNewDocFromMem(std::string("<svg xmlns='http://www.w3.org/2000/svg'/>"));
    ASSERT_TRUE(doc);
    prefs->setString("/dialogs/file/last_path", Glib::build_filename(Glib::get_tmp_dir(), "proof-no-such-folder"));
    prefs->setString("/dialog/open/path", Glib::get_home_dir());
    doc->setDocumentFilename(Glib::build_filename(Glib::get_tmp_dir(), "proof-no-such-folder", "drawing.svg").c_str());
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_AS), Glib::get_home_dir());
    EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_COPY), Glib::get_home_dir());
}

TEST_F(FileSavePathTest, NoValidFolderFallsBackToHomeWithEitherPreference)
{
    auto prefs = Preferences::get();
    auto restore = prefs->temporaryPreferences();
    auto doc = SPDocument::createNewDocFromMem(std::string("<svg xmlns='http://www.w3.org/2000/svg'/>"));
    ASSERT_TRUE(doc);
    for (auto key : {"/dialogs/file/last_path", "/dialog/open/path", "/dialogs/open/path",
                     "/dialogs/save_copy/path", "/dialogs/save_as/path"}) {
        prefs->setString(key, Glib::build_filename(Glib::get_tmp_dir(), "proof-no-such-folder"));
    }
    for (bool use_current_dir : {false, true}) {
        prefs->setBool("/dialogs/save_as/use_current_dir", use_current_dir);
        EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_AS), Glib::get_home_dir());
        EXPECT_EQ(get_file_save_path(doc.get(), FILE_SAVE_METHOD_SAVE_COPY), Glib::get_home_dir());
    }
}

} // namespace
