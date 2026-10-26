# SPDX-License-Identifier: GPL-2.0-or-later
add_subdirectory(attributes)
add_subdirectory(branding)
add_subdirectory(examples)
add_subdirectory(filters)
add_subdirectory(fonts)
add_subdirectory(icons)
add_subdirectory(keys)
add_subdirectory(markers)
add_subdirectory(paint)
add_subdirectory(palettes)
add_subdirectory(screens)
add_subdirectory(symbols)
add_subdirectory(templates)
add_subdirectory(tutorials)
add_subdirectory(ui)
add_subdirectory(doc)

if(WITH_MCP_BRIDGE)
    install(FILES mcp/proof_mcp.py DESTINATION ${INKSCAPE_SHARE_INSTALL}/mcp)
endif()


# extensions (git submodule!)
install(DIRECTORY extensions DESTINATION ${INKSCAPE_SHARE_INSTALL}
    COMPONENT extensions
    PATTERN ".git*" EXCLUDE
    PATTERN ".pylintrc" EXCLUDE
    PATTERN ".darglint" EXCLUDE
    PATTERN ".pre-commit-config.yaml" EXCLUDE
    PATTERN "tests" EXCLUDE
    PATTERN "MANIFEST.in" EXCLUDE
    PATTERN "pyproject.toml" EXCLUDE
    PATTERN "Makefile" EXCLUDE
    PATTERN "__pycache__" EXCLUDE
    PATTERN "tox.ini" EXCLUDE
    PATTERN "setup.cfg" EXCLUDE
    PATTERN "requirements.txt" EXCLUDE
    PATTERN "poetry.lock" EXCLUDE
    PATTERN "*.dox" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*.sh" EXCLUDE
    PATTERN "*.bat" EXCLUDE
    PATTERN "*.md" EXCLUDE
    PATTERN "print_win32_vector.*" EXCLUDE
    PATTERN "other/inkman" EXCLUDE
)

install(DIRECTORY extensions/other/inkman DESTINATION ${INKSCAPE_SHARE_INSTALL}/extensions/inkman/
    COMPONENT extension_manager
    PATTERN "tests" EXCLUDE
    PATTERN ".git*" EXCLUDE
    PATTERN "MANIFEST.in" EXCLUDE
    PATTERN "requirements.txt" EXCLUDE
    PATTERN "pyproject.toml" EXCLUDE
)

if(WIN32)
    install(
        FILES extensions/print_win32_vector.py extensions/print_win32_vector.inx
        DESTINATION ${INKSCAPE_SHARE_INSTALL}/extensions
        COMPONENT extensions
    )
  install(CODE
    "MESSAGE(\"Pre-compiling Python extensions to byte-code (.pyc files)\")
     execute_process(COMMAND \${CMAKE_INSTALL_PREFIX}/bin/python -m compileall -qq \${CMAKE_INSTALL_PREFIX}/${INKSCAPE_SHARE_INSTALL})"
    COMPONENT extensions)
endif()


# themes (git submodule!)
add_subdirectory(themes)


# appdata and desktop files, bash completions
if(UNIX)
  if (ENABLE_BINRELOC)
    set(INKSCAPE_ICONPATH "\${SNAP}/share/inkscape/branding/inkscape.svg")
  else()
    set(INKSCAPE_ICONPATH "org.inkscape.Inkscape")
  endif()
  set(INKSCAPE_MIMETYPE
	  "image/svg+xml"
	  "image/svg+xml-compressed"
	  "application/vnd.corel-draw"
	  "application/pdf"
	  "application/postscript"
	  "image/x-eps"
	  "application/illustrator"
	  "image/x-wmf"
	  "image/x-emf"
	  "application/x-xccx"
	  "application/x-xcdt"
	  "application/x-xcmx"
	  "image/x-xcdr"
	  "application/visio"
	  "application/x-visio"
	  "application/vnd.visio"
	  "application/vnd.ms-visio.viewer"
	  "application/visio.drawing"
	  "application/vsd"
	  "application/x-vsd"
	  "image/x-vsd"
	  ""
  )

  foreach(APP Inkscape Inkview)
    configure_file(${CMAKE_SOURCE_DIR}/org.inkscape.${APP}.desktop.template
      ${CMAKE_BINARY_DIR}/org.inkscape.${APP}.desktop.in)
  endforeach()

  if(ENABLE_NLS)
    foreach(APP Inkscape Inkview)
      add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/org.inkscape.${APP}.desktop
        DEPENDS ${LANGUAGES}
        COMMAND ${GETTEXT_MSGFMT_EXECUTABLE}
        --desktop
        --template ${CMAKE_BINARY_DIR}/org.inkscape.${APP}.desktop.in
        -d ${CMAKE_SOURCE_DIR}/po
        -o ${CMAKE_BINARY_DIR}/org.inkscape.${APP}.desktop
        --keyword=Name
        --keyword=GenericName
        --keyword=X-GNOME-FullName
        --keyword=Comment
        --keyword=Keywords
      )
    endforeach()
    add_custom_target(inkscape_desktop ALL DEPENDS
      ${CMAKE_BINARY_DIR}/org.inkscape.Inkscape.desktop
      ${CMAKE_BINARY_DIR}/org.inkscape.Inkview.desktop
    )

    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/org.inkscape.Inkscape.appdata.xml
      DEPENDS ${LANGUAGES}
      COMMAND ${GETTEXT_MSGFMT_EXECUTABLE}
      --xml
      --template ${CMAKE_SOURCE_DIR}/org.inkscape.Inkscape.appdata.xml.in
      -d ${CMAKE_SOURCE_DIR}/po -o ${CMAKE_BINARY_DIR}/org.inkscape.Inkscape.appdata.xml
    )
    add_custom_target(inkscape_appdata ALL DEPENDS ${CMAKE_BINARY_DIR}/org.inkscape.Inkscape.appdata.xml)
  else()
    configure_file(${CMAKE_SOURCE_DIR}/org.inkscape.Inkscape.appdata.xml.in ${CMAKE_BINARY_DIR}/org.inkscape.Inkscape.appdata.xml COPYONLY)
    foreach(APP Inkscape Inkview)
      configure_file(${CMAKE_BINARY_DIR}/org.inkscape.${APP}.desktop.in
        ${CMAKE_BINARY_DIR}/org.inkscape.${APP}.desktop COPYONLY)
    endforeach()
  endif()

  foreach(APP Inkscape Inkview)
    install(FILES ${CMAKE_BINARY_DIR}/org.inkscape.${APP}.desktop
      DESTINATION ${SHARE_INSTALL}/applications)
  endforeach()

  install(FILES ${CMAKE_BINARY_DIR}/org.inkscape.Inkscape.appdata.xml
    DESTINATION ${SHARE_INSTALL}/metainfo)


  install(FILES
    bash-completion/completions/inkscape
    DESTINATION ${SHARE_INSTALL}/bash-completion/completions)
  install(FILES
    application-registry/inkscape.applications
    DESTINATION ${SHARE_INSTALL}/application-registry/)
  install(FILES
    branding/inkscape.xpm
    DESTINATION ${SHARE_INSTALL}/pixmaps/)

endif()


