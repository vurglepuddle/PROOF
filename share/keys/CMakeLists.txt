# SPDX-License-Identifier: GPL-2.0-or-later
file(GLOB _FILES "*.xml")
install(FILES ${_FILES} DESTINATION ${INKSCAPE_SHARE_INSTALL}/keys)

# PROOF: default.xml, the keymap used until the user chooses another, is a file of its own here
# (Illustrator's keys). Inkscape installs a duplicate of inkscape.xml under that name.
