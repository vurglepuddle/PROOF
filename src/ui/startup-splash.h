// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_STARTUP_SPLASH_H
#define INKSCAPE_UI_STARTUP_SPLASH_H

#include <memory>
#include <string>

namespace Inkscape::UI {

/** A Windows startup indicator, independent of the editor's busy GTK thread.
 *  Owns only its picture and native window; never accesses editor/GTK objects.
 *  Other platforms currently leave their existing startup behaviour unchanged.
 */
class StartupSplash
{
public:
    explicit StartupSplash(std::string const &picture);
    ~StartupSplash();
    StartupSplash(StartupSplash const &) = delete;
    StartupSplash &operator=(StartupSplash const &) = delete;

    void set_message(std::string const &message);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Inkscape::UI

#endif
