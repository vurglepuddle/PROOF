// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>
#include <mutex>

#include "framecheck.h"
#include "preferences.h"

namespace fs = std::filesystem;

namespace Inkscape::FrameCheck {

void Event::write()
{
    static std::mutex mutex;
    static auto logfile = [] {
        auto path = fs::temp_directory_path() / "framecheck.txt";
        auto mode = std::ios_base::out | std::ios_base::app | std::ios_base::binary;
        auto f = std::ofstream(path.string(), mode);
        f.imbue(std::locale::classic());
        return f;
    }();

    auto lock = std::lock_guard(mutex);
    logfile << name << ' ' << start << ' ' << g_get_monotonic_time() << ' ' << subtype << std::endl;
}

bool enabled()
{
    return Preferences::get()->getBool("/options/rendering/debug_framecheck");
}

} // namespace Inkscape::FrameCheck
