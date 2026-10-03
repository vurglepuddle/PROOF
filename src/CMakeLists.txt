# SPDX-License-Identifier: GPL-2.0-or-later

add_library(inkscape_base)

target_sources(inkscape_base PRIVATE
  alignment-snapper.cpp
  attribute-rel-css.cpp
  attribute-rel-svg.cpp
  attribute-rel-util.cpp
  attribute-sort-util.cpp
  attributes.cpp
  auto-save.cpp
  axis-manip.cpp
  box-frame.cpp
  composite-undo-stack-observer.cpp
  conditions.cpp
  conn-avoid-ref.cpp
  context-fns.cpp
  css-chemistry.cpp
  desktop-events.cpp
  desktop-style.cpp
  desktop.cpp
  distribution-snapper.cpp
  document-subset.cpp
  document-undo.cpp
  document-update.cpp
  document.cpp
  event-log.cpp
  file.cpp
  filter-chemistry.cpp
  filter-enums.cpp
  gc-anchored.cpp
  gradient-chemistry.cpp
  gradient-drag.cpp
  guide-snapper.cpp
  grid-snapper.cpp
  id-clash.cpp
  inkscape.cpp
  inkscape-version-info.cpp
  layer-manager.cpp
  line-geometry.cpp
  line-snapper.cpp
  media.cpp
  message-context.cpp
  message-stack.cpp
  mod360.cpp
  object-colors.cpp
  object-hierarchy.cpp
  object-snapper.cpp
  page-manager.cpp
  path-chemistry.cpp
  path-prefix.cpp
  perspective-line.cpp
  preferences.cpp
  print.cpp
  proj_pt.cpp
  pure-transform.cpp
  rdf.cpp
  rubberband.cpp
  selcue.cpp
  selection-chemistry.cpp
  selection-describer.cpp
  selection.cpp
  seltrans-handles.cpp
  seltrans.cpp
  snap-preferences.cpp
  snap.cpp
  snapped-curve.cpp
  snapped-line.cpp
  snapped-point.cpp
  snapper.cpp
  spot-ink.cpp
  style-internal.cpp
  style-text.cpp
  style.cpp
  text-chemistry.cpp
  text-editing.cpp
  transf_mat_3x4.cpp
  unicoderange.cpp
  vanishing-point.cpp
  version.cpp

  # Headers

  alignment-snapper.h
  attribute-rel-css.h
  attribute-rel-svg.h
  attribute-rel-util.h
  attribute-sort-util.h
  attributes.h
  auto-save.h
  axis-manip.h
  bad-uri-exception.h
  box-frame.h
  composite-undo-stack-observer.h
  conditions.h
  conn-avoid-ref.h
  context-fns.h
  css-chemistry.h
  desktop-events.h
  desktop-style.h
  desktop.h
  distribution-snapper.h
  document-subset.h
  document-undo.h
  document-update.h
  document.h
  enums.h
  event-log.h
  event.h
  file.h
  fill-or-stroke.h
  filter-chemistry.h
  filter-enums.h
  gc-anchored.h
  gradient-chemistry.h
  gradient-drag.h
  guide-snapper.h
  grid-snapper.h
  id-clash.h
  inkscape-version.h
  inkscape-version-info.h
  inkscape.h
  layer-manager.h
  line-geometry.h
  line-snapper.h
  media.h
  message-context.h
  message-stack.h
  message.h
  mod360.h
  number-opt-number.h
  object-colors.h
  object-hierarchy.h
  object-snapper.h
  page-manager.h
  path-chemistry.h
  path-prefix.h
  pattern-manager.cpp
  pattern-manager.h
  pattern-manipulation.cpp
  pattern-manipulation.h
  perspective-line.h
  preferences-skeleton.h
  preferences.h
  print.h
  proj_pt.h
  pure-transform.h
  rdf.h
  rubberband.h
  selcue.h
  selection-chemistry.h
  selection-describer.h
  selection.h
  seltrans-handles.h
  seltrans.h
  snap-candidate.h
  snap-enums.h
  snap-preferences.h
  snap.h
  snapped-curve.h
  snapped-line.h
  snapped-point.h
  snapper.h
  spot-ink.h
  streq.h
  strneq.h
  style-enums.h
  style-internal.h
  style-text.h
  style.h
  syseq.h
  text-chemistry.h
  text-editing.h
  text-tag-attributes.h
  transf_mat_3x4.h
  undo-stack-observer.h
  unicoderange.h
  vanishing-point.h
  version.h

  inkscape-window.h
  inkscape-window.cpp

  inkscape-application.h
  inkscape-application.cpp
  inkview-application.h
  inkview-application.cpp
  inkview-window.h
  inkview-window.cpp

  manipulation/copy-resource.h
  manipulation/copy-resource.cpp
)

# -----------------------------------------------------------------------------
# Generate version file
# -----------------------------------------------------------------------------

# configure with values known at configure time
configure_file(inkscape-version.cpp.in ${CMAKE_BINARY_DIR}/src/inkscape-version.cpp.in @ONLY)

# configure further at build time (always built as run_always.txt is never actually created)
add_custom_command(
    OUTPUT inkscape-version.cpp run_always.txt
    COMMAND ${CMAKE_COMMAND}
        -DINKSCAPE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DINKSCAPE_BINARY_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_SOURCE_DIR}/CMakeScripts/inkscape-version.cmake
    COMMENT "Generating inkscape-version.cpp")

target_sources(inkscape_base PRIVATE
  ${CMAKE_BINARY_DIR}/src/inkscape-version.cpp
)


# -----------------------------------------------------------------------------
# Load in subdirectories
# -----------------------------------------------------------------------------

add_subdirectory(actions)
add_subdirectory(async)
add_subdirectory(colors)
add_subdirectory(css)
add_subdirectory(debug)
add_subdirectory(display)
add_subdirectory(extension)
add_subdirectory(helper)
add_subdirectory(include)
add_subdirectory(inkgc)
add_subdirectory(io)
add_subdirectory(libnrtype)
add_subdirectory(livarot)
add_subdirectory(live_effects)
add_subdirectory(object)
add_subdirectory(path)
add_subdirectory(svg)
add_subdirectory(trace)
add_subdirectory(ui)
add_subdirectory(util)
add_subdirectory(util-string)
add_subdirectory(widgets)
add_subdirectory(xml)

# Directories containing lists files that describe building internal libraries
add_subdirectory(3rdparty)

# -----------------------------------------------------------------------------
# Relative RPATH handling
# -----------------------------------------------------------------------------

file(RELATIVE_PATH
    INKSCAPE_INSTALL_LIBDIR_RELATIVE_TO_BINDIR
    "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}")

if(APPLE)
    set(CMAKE_MACOSX_RPATH TRUE)
    set(ORIGIN "@loader_path")
else()
    set(ORIGIN "$ORIGIN")
endif()

set(BINS_INSTALL_RPATH ${CMAKE_INSTALL_RPATH})
set(LIBINKSCAPE_INSTALL_RPATH ${CMAKE_INSTALL_RPATH})

# Binaries must be able to find libinkscape_base
list(APPEND BINS_INSTALL_RPATH "${ORIGIN}/${INKSCAPE_INSTALL_LIBDIR_RELATIVE_TO_BINDIR}/inkscape")

if(WITH_INTERNAL_2GEOM)
    # Binaries must be able to find lib2geom
    list(APPEND BINS_INSTALL_RPATH "${ORIGIN}/${INKSCAPE_INSTALL_LIBDIR_RELATIVE_TO_BINDIR}")
    # libinkscape_base must be able to find lib2geom
    list(APPEND LIBINKSCAPE_INSTALL_RPATH "${ORIGIN}/..")
endif()

# -----------------------------------------------------------------------------
# Set up the executable
# -----------------------------------------------------------------------------
set(main_SRC
  inkscape-main.cpp
)
set(view_SRC
  inkview-main.cpp
)

if(WIN32)
  # Configure and add the platform specific resource files (enabling the app icon).
  string(TIMESTAMP COPYRIGHT_YEAR %Y)
  
  set(FILE_NAME inkscape)
  set(PROGRAM_NAME Inkscape)
  set(PROGRAM_DESCRIPTION "Inkscape vector graphics editor")
  configure_file(inkscape.rc ${CMAKE_BINARY_DIR}/src/inkscape.rc)
  configure_file(inkscape-manifest.xml ${CMAKE_BINARY_DIR}/src/inkscape-manifest.xml)  
  list(APPEND main_SRC ${CMAKE_BINARY_DIR}/src/inkscape.rc)
  
  set(FILE_NAME inkview)
  set(PROGRAM_NAME Inkview)
  set(PROGRAM_DESCRIPTION "Inkview vector graphics viewer")
  configure_file(inkscape.rc ${CMAKE_BINARY_DIR}/src/inkview.rc)
  configure_file(inkscape-manifest.xml ${CMAKE_BINARY_DIR}/src/inkview-manifest.xml)
  list(APPEND view_SRC ${CMAKE_BINARY_DIR}/src/inkview.rc)
endif()

# make executables for inkscape and inkview
if(NOT ANDROID)
  add_executable(inkscape ${main_SRC})
  add_executable(inkview ${view_SRC})
else()
  add_library(inkscape ${main_SRC})
  add_library(inkview ${view_SRC})
endif()

if(WIN32)
  # Make the same executables again, but this time as console application (GUI applications can't print to the console)
  add_executable(inkscape_com ${main_SRC})
  set_target_properties(inkscape_com
                        PROPERTIES
                          LINK_FLAGS "-mconsole"
                          OUTPUT_NAME "inkscape.com"
                          SUFFIX "")
  add_executable(inkview_com ${view_SRC})
  set_target_properties(inkview_com
                        PROPERTIES
                          LINK_FLAGS "-mconsole"
                          OUTPUT_NAME "inkview.com"
                          SUFFIX "")
endif()


set_target_properties(inkscape_base PROPERTIES SOVERSION "${INKSCAPE_VERSION_MAJOR}.${INKSCAPE_VERSION_MINOR}.${INKSCAPE_VERSION_PATCH}.0")
set_target_properties(inkscape_base PROPERTIES INSTALL_RPATH "${LIBINKSCAPE_INSTALL_RPATH}")

# Link the inkscape_base library against all external dependencies
target_link_libraries(inkscape_base
    PRIVATE
        autotrace_LIB
        Depixelize::depixelize
        croco_LIB
        adaptagrams_LIB
        Uemf::uemf
        $<$<BOOL:WITH_CAPYPDF>:Inkscape::CapyPDF>
    PUBLIC
        ${INKSCAPE_LIBS}
        2Geom::2geom
)

# Link inkscape and inkview against inkscape_base
target_link_libraries(inkscape inkscape_base)
target_link_libraries(inkview inkscape_base)
set_target_properties(inkscape PROPERTIES INSTALL_RPATH "${BINS_INSTALL_RPATH}")
set_target_properties(inkview PROPERTIES INSTALL_RPATH "${BINS_INSTALL_RPATH}")
if(WIN32)
    target_link_libraries(inkscape_com inkscape_base)
    target_link_libraries(inkview_com inkscape_base)
    set_target_properties(inkscape_com PROPERTIES INSTALL_RPATH "${BINS_INSTALL_RPATH}")
    set_target_properties(inkview_com PROPERTIES INSTALL_RPATH "${BINS_INSTALL_RPATH}")
endif()



#Define the installation
install(TARGETS inkscape inkview RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    install(TARGETS inkscape_com)
    install(TARGETS inkview_com)
endif()
if(BUILD_SHARED_LIBS)
    install(TARGETS inkscape_base LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/inkscape")
endif()
