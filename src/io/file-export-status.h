// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INK_FILE_EXPORT_STATUS_H
#define INK_FILE_EXPORT_STATUS_H

namespace Inkscape::IO {
// Preserve an export failure across files/actions until main returns to the caller.
inline int export_exit_status = 0;
}

#endif
