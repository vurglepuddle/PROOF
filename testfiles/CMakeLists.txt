# SPDX-License-Identifier: GPL-2.0-or-later
# -----------------------------------------------------------------------------

include_directories(${PROJECT_SOURCE_DIR})

# custom "check" target with proper dependencies (builds inkscape and tests)
file(TO_NATIVE_PATH "/" _separator)
add_definitions(-DINKSCAPE_TESTS_DIR="${CMAKE_SOURCE_DIR}/testfiles")
add_definitions(-DINKSCAPE_SHARE_DIR="${CMAKE_SOURCE_DIR}/share")
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                        DEPENDS tests unit_tests
                        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(check inkscape)

# create symlink "inkscape_datadir" to use as INKSCAPE_DATADIR
# - ensures tests can be run without installing the project
# - also helpful for running Inkscape uninstalled: 'INKSVAPE_DATADIR=inkscape_datadir bin/inkscape'
set(INKSCAPE_DATADIR ${CMAKE_BINARY_DIR}/inkscape_datadir)
if(NOT EXISTS ${INKSCAPE_DATADIR}/inkscape)
    set(link_source ${INKSCAPE_DATADIR}/inkscape)
    set(link_target ${CMAKE_SOURCE_DIR}/share)
    message(STATUS "Creating link '${link_source}' --> '${link_target}'")
    file(MAKE_DIRECTORY "${INKSCAPE_DATADIR}")
    file(CREATE_LINK "${link_target}" "${link_source}" SYMBOLIC RESULT result)
    if(result)
        message(WARNING "Creation of link failed: ${result}")
    endif()
endif()
# check if creation succeeded
if(EXISTS ${INKSCAPE_DATADIR}/inkscape)
    set(CMAKE_CTEST_ENV INKSCAPE_DATADIR=${INKSCAPE_DATADIR})
else()
    message(WARNING "Directory 'inkscape_datadir/inkscape' missing. Tests might not run properly.\n"
                    "Possible solutions:\n"
                    " - create a suitable symlink yourself, e.g.\n"
                    "   ln -s ${CMAKE_SOURCE_DIR}/share ${INKSCAPE_DATADIR}/inkscape\n"
                    " - run '${CMAKE_MAKE_PROGRAM} install' before running tests (only for not relocatable packages.\n"
                    " - set the environment variable 'INKSCAPE_DATADIR' manually (every time you run tests)")
endif()


# Set custom profile directory for tests using environment variable.
# Copy CTestCustom.cmake into binary dir, where it will be picked up automatically by ctest for cleanup.
set(INKSCAPE_TEST_PROFILE_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_profile_dir)
set(INKSCAPE_TEST_PROFILE_DIR_ENV INKSCAPE_PROFILE_DIR=${INKSCAPE_TEST_PROFILE_DIR})
configure_file(CTestCustom.cmake.in ${CMAKE_BINARY_DIR}/CTestCustom.cmake)

### tests using gtest

if(${CMAKE_SIZEOF_VOID_P} EQUAL 8)
    set(LPE_TESTS_64bit
        #0.92 or lower LPEs
        # (test not stable on 32bit Windows)
        lpe64-test
    )
endif()

set(LIBNRTYPE_TESTS
    libnrtype/Layout-TNG-Compute
    libnrtype/Layout-TNG-Compute-lang
    )

# Integration tests (linking all of Inkscape)
set(TEST_SOURCES
    actions-svg-processing
    async_channel-test
    async_funclog-test
    async_progress-test
    boolop-attr-test
    spot-ink-test
    ai-native-reader-test
    box-frame-test
    cmyk-picker-test
    colors/cms-system-test
    colors/document-cms-test
    colors/dragndrop-test
    colors/color-set-test
    colors/xml-color-test
    object/color-profile-test
    uri-test
    uri-references-test
    util-test
    util-expression-evaluator-test
    util-uri-test
    drag-and-drop-svgz
    drawing-pattern-test
    split-path-test
    attributes-test
    dir-util-test
    sp-item-test
    sp-object-test
    sp-object-lang-test
    sp-object-tags-test
    object-links-test
    object-set-test
    object-style-test
    page-management
    path-reverse-lpe-test
    preferences-test
    rebase-hrefs-test
    stream-test
    style-elem-test
    style-internal-test
    style-test
    svg-affine-test
    svg-box-test
    svg-path-parser-test
    sp-gradient-test
    svg-path-geom-test
    visual-bounds-test
    geom-pathstroke-test
    livarot-pathoutline-test
    object-test
    sp-glyph-kerning-test
    cairo-utils-test
    svg-extension-test
    2geom-characterization-test
    test-feDropShadow
    xml-test
    sp-item-group-test
    store-test
    lpe-test
    ui-util-test
    sp-document-test
    object-colors-test
    multi-marker-color-wheel-test
    pixel_align-test
    ${LPE_TESTS_64bit}
    ${LIBNRTYPE_TESTS}
    )

if(WITH_POPPLER)
    list(APPEND TEST_SOURCES
        pdf-utils-test
        poppler-utils-test
    )
endif()

if(WITH_CAPYPDF)
    list(APPEND TEST_SOURCES
        extensions-pdfoutput-test
    )
endif()

# Match inkscape_base's linkage. A shared support DLL over a static base can
# duplicate base globals across DLL/executable boundaries on MinGW, leading
# to double destruction of colour registries after the tests finish.
add_library(cpp_test_static_library inkscape-test.cpp doc-per-case-test.cpp compare-paths-test.h lpespaths-test.h store-integrity-test.h test-with-svg-object-pairs.cpp)
target_link_libraries(cpp_test_static_library PUBLIC ${GTEST_LIBRARIES} inkscape_base)

add_custom_target(tests)
foreach(test_source ${TEST_SOURCES})
    string(REPLACE "-test" "" testname "test_${test_source}")
    string(REPLACE "/" "_" testname "${testname}")
    add_executable(${testname} src/${test_source}.cpp)
    target_include_directories(${testname} SYSTEM PRIVATE ${GTEST_INCLUDE_DIRS})
    target_link_libraries(${testname} cpp_test_static_library 2Geom::2geom)
    add_test(NAME ${testname} COMMAND ${testname})
    set_tests_properties(${testname} PROPERTIES ENVIRONMENT "${INKSCAPE_TEST_PROFILE_DIR_ENV}/${testname};${CMAKE_CTEST_ENV}")
    add_dependencies(tests ${testname})
endforeach()
foreach(test_source ${LIBNRTYPE_TESTS})
    string(REPLACE "/" "_" testname "test_${test_source}")
    set_property(TEST ${testname} APPEND PROPERTY ENVIRONMENT "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
endforeach()

include(${CMAKE_SOURCE_DIR}/CMakeScripts/UnitTest.cmake)

### Unit tests
#
# In order to add a unit test, call add_unit_test() providing a unique name for the test,
# and then the following named parameters:
# TEST_SOURCE followed by the file containing the test code (relative to testfiles/src)
# SOURCES     followed by a list of files containing the code under test (relative to src/)
# EXTRA_LIBS  followed by libraries which you need to be linked to the test.
#
add_unit_test(assertions-in-tests TEST_SOURCE "assertions-in-tests.cpp")
add_unit_test(version-test TEST_SOURCE "version-test.cpp"
                               SOURCES "version.cpp")

# These all depend on each other as a single unit
add_unit_tests(TEST_SOURCES "svg-length-test.cpp"
                            "svg-stringstream-test.cpp"
                            "util-units-test.cpp"
                    SOURCES "svg/css-ostringstream.cpp"
                            "svg/svg-length.cpp"
                            "svg/stringstream.cpp"
                            "svg/strip-trailing-zeros.cpp"
                            "util/numeric/precision.cpp"
                            "util/units.cpp"
                            # MOCK object files to unitise these tests
                            "mocks/util-units-filename.cpp"
                    EXTRA_LIBS GLibmm::GLibmm 2Geom::2geom)

include("${CMAKE_SOURCE_DIR}/src/libnrtype/CMakeUnit.txt")
get_libnrtype_unit_lib()

add_unit_tests(TEST_SOURCES "libnrtype/font-factory-test.cpp"
                            "libnrtype/font-instance-test.cpp"
                            "libnrtype/font-utils-test.cpp"

                ENVIRONMENT "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
                            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.suppression"
                 EXTRA_LIBS libnrtype_unit_lib)

include("${CMAKE_SOURCE_DIR}/src/colors/CMakeUnit.txt")
get_color_unit_lib()
add_unit_tests(TEST_SOURCES "colors/cms-profile-test.cpp"
                            "colors/cms-transform-color-test.cpp"
                            "colors/cms-transform-surface-test.cpp"
                            "colors/color-test.cpp"
                            "colors/gamut-test.cpp"
                            "colors/manager-test.cpp"
                            "colors/parser-test.cpp"
                            "colors/printer-test.cpp"
                            "colors/gradient-averager-test.cpp"
                            "colors/spaces-alpha-test.cpp"
                            "colors/spaces-cms-test.cpp"
                            "colors/spaces-cmyk-test.cpp"
                            "colors/spaces-convert-static-test.cpp"
                            "colors/spaces-gray-test.cpp"
                            "colors/spaces-hsl-test.cpp"
                            "colors/spaces-hsluv-test.cpp"
                            "colors/spaces-hsv-test.cpp"
                            "colors/spaces-lab-test.cpp"
                            "colors/spaces-lch-test.cpp"
                            "colors/spaces-linear-rgb-test.cpp"
                            "colors/spaces-luv-test.cpp"
                            "colors/spaces-named-test.cpp"
                            "colors/spaces-okhsl-test.cpp"
                            "colors/spaces-okhsv-test.cpp"
                            "colors/spaces-oklab-test.cpp"
                            "colors/spaces-oklch-test.cpp"
                            "colors/spaces-rgb-test.cpp"
                            "colors/spaces-xyz-test.cpp"
                            "colors/utils-test.cpp"
                 EXTRA_LIBS colors_unit_lib)

include("${CMAKE_SOURCE_DIR}/src/path/CMakeUnit.txt")
get_path_unit_lib()
add_unit_tests(TEST_SOURCES "path/path-boolop-test.cpp"
                            "path/path-curve-test.cpp"
                 EXTRA_LIBS path_unit_lib)

add_unit_test(css-syntactic-decomposition-test TEST_SOURCE "css-syntactic-decomposition-test.cpp"
                                                   SOURCES "css/syntactic-decomposition.cpp"
                                                EXTRA_LIBS GLibmm::GLibmm croco_LIB)
if(NOT WIN32)
    # This test does not link on Windows
    add_unit_test(xslt-extension-test TEST_SOURCE "xslt-extension-test.cpp"
                                          SOURCES "extension/implementation/xslt.cpp"
                                       EXTRA_LIBS LibXml2::LibXml2 PkgConfig::SIGC++ GLibmm::GLibmm 2Geom::2geom)
endif()
add_unit_test(mod360-test TEST_SOURCE "mod360-test.cpp"
                              SOURCES "mod360.cpp"
                           EXTRA_LIBS 2Geom::2geom)
add_unit_test(treeify-test TEST_SOURCE "treeify-test.cpp"
                               SOURCES "util/treeify.cpp")

add_dependencies(tests unit_tests)

### CLI rendering tests and LPE
add_subdirectory(cli_tests)
add_subdirectory(rendering_tests)
add_subdirectory(lpe_tests)

### Fuzz test
if(WITH_FUZZ)
    # to use the fuzzer, make sure you use the right compiler (clang)
    # with the right flags -fsanitize=address -fsanitize-coverage=edge,trace-pc-guard,indirect-calls,trace-cmp,trace-div,trace-gep -fno-omit-frame-pointer
    # (see libfuzzer doc for info in flags)
    # first line is for integration into oss-fuzz https://github.com/google/oss-fuzz
    add_executable(fuzz fuzzer.cpp)
    if(LIB_FUZZING_ENGINE)
        target_link_libraries(fuzz inkscape_base -lFuzzingEngine)
    else()
        target_link_libraries(fuzz inkscape_base -lFuzzer)
    endif()
endif()
