# SPDX-License-Identifier: GPL-2.0-or-later
target_sources(inkscape_base PRIVATE
    bridge.cpp
    bridge.h
    json.cpp
    json.h
    pipe.cpp
    pipe.h
    preferences.cpp
    preferences.h
    protocol.cpp
    protocol.h
)
target_link_libraries(inkscape_base PRIVATE advapi32 bcrypt)
