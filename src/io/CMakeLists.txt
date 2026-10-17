# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(inkscape_base PRIVATE
  dir-util.cpp
  file.cpp
  file-export-cmd.cpp
  recent-files.cpp
  resource.cpp
  sandbox.cpp
  split-path.cpp
  fix-broken-links.cpp
  query-file-info.cpp
  stream/bufferstream.cpp
  stream/gzipstream.cpp
  stream/inkscapestream.cpp
  stream/stringstream.cpp
  stream/uristream.cpp
  stream/xsltstream.cpp
  sys.cpp

  # -------
  # Headers
  dir-util.h
  file.h
  file-export-cmd.h
  file-export-status.h
  recent-files.h
  resource.h
  sandbox.h
  split-path.h
  fix-broken-links.h
  query-file-info.h
  stream/bufferstream.h
  stream/gzipstream.h
  stream/inkscapestream.h
  stream/stringstream.h
  stream/uristream.h
  stream/xsltstream.h
  sys.h
)
