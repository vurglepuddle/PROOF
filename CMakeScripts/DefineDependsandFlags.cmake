set(INKSCAPE_LIBS "")
set(INKSCAPE_CXX_FLAGS "")
set(INKSCAPE_CXX_FLAGS_DEBUG "")

include_directories(
    ${PROJECT_SOURCE_DIR}/src

    # generated includes
    ${CMAKE_BINARY_DIR}/include
)

# NDEBUG implies G_DISABLE_ASSERT
string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UPPER)
if(CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE_UPPER} MATCHES "-DNDEBUG")
    list(APPEND INKSCAPE_CXX_FLAGS "-DG_DISABLE_ASSERT")
endif()

# AddressSanitizer
# Clang's AddressSanitizer can detect more memory errors and is more powerful
# than compiling with _FORTIFY_SOURCE but has a performance impact (approx. 2x
# slower), so it's not suitable for release builds.
if(WITH_ASAN)
    list(APPEND INKSCAPE_CXX_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
    list(APPEND INKSCAPE_LIBS "-fsanitize=address")
elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Undefine first, to suppress 'warning: "_FORTIFY_SOURCE" redefined'
    list(APPEND INKSCAPE_CXX_FLAGS "-U_FORTIFY_SOURCE")
    list(APPEND INKSCAPE_CXX_FLAGS "-D_FORTIFY_SOURCE=2")
endif()

# Disable deprecated Gtk and friends
#list(APPEND INKSCAPE_CXX_FLAGS "-DGLIBMM_DISABLE_DEPRECATED")
#list(APPEND INKSCAPE_CXX_FLAGS "-DGTKMM_DISABLE_DEPRECATED")
#list(APPEND INKSCAPE_CXX_FLAGS "-DGDKMM_DISABLE_DEPRECATED")
#list(APPEND INKSCAPE_CXX_FLAGS "-DGTK_DISABLE_DEPRECATED")
#list(APPEND INKSCAPE_CXX_FLAGS "-DGDK_DISABLE_DEPRECATED")

# Disable deprecation warnings for Gtk
list(APPEND INKSCAPE_CXX_FLAGS "-DGDK_DISABLE_DEPRECATION_WARNINGS")

# Errors for common mistakes
list(APPEND INKSCAPE_CXX_FLAGS "-fstack-protector-strong")
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=format")                # e.g.: printf("%s", std::string("foo"))
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=format-security")       # e.g.: printf(variable);
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=ignored-qualifiers")    # e.g.: const int foo();
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=return-type")           # non-void functions that don't return a value
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=vla")                   # variable-length arrays
list(APPEND INKSCAPE_CXX_FLAGS "-Wno-switch")                   # See !849 for discussion
list(APPEND INKSCAPE_CXX_FLAGS "-Wmisleading-indentation")
list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-Wcomment")
list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-Wunused-function")
list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-Wunused-variable")
list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-D_GLIBCXX_ASSERTIONS")
if (CMAKE_COMPILER_IS_GNUCC)
    list(APPEND INKSCAPE_CXX_FLAGS "-Wstrict-null-sentinel")    # For NULL instead of nullptr
    list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-fexceptions -grecord-gcc-switches -fasynchronous-unwind-tables")
    if(CXX_COMPILER_VERSION VERSION_GREATER 8.0)
        list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-fstack-clash-protection -fcf-protection")
    endif()
endif()
if(APPLE)
    list(APPEND INKSCAPE_CXX_FLAGS "-fexperimental-library") # for jthread, stop_token
endif()

# Define the flags for profiling if desired:
if(WITH_PROFILING)
    set(BUILD_SHARED_LIBS off)
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pg")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
endif()

include(CheckCXXSourceCompiles)
CHECK_CXX_SOURCE_COMPILES("
#include <atomic>
#include <cstdint>
std::atomic<uint64_t> x (0);
int main() {
  uint64_t i = x.load(std::memory_order_relaxed);
  return 0;
}
"
LIBATOMIC_NOT_NEEDED)
IF (NOT LIBATOMIC_NOT_NEEDED)
    message(STATUS "  Adding -latomic to the libs.")
    list(APPEND INKSCAPE_LIBS "-latomic")
ENDIF()


# ----------------------------------------------------------------------------
# Files we include
# ----------------------------------------------------------------------------
if(WIN32)
    # Set the link and include directories
    get_property(dirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)

    list(APPEND INKSCAPE_LIBS "-lmscms")
    list(APPEND INKSCAPE_LIBS "-ldwmapi")

    list(APPEND INKSCAPE_CXX_FLAGS "-mms-bitfields")
    if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
      list(APPEND INKSCAPE_CXX_FLAGS "-mwindows")
      list(APPEND INKSCAPE_CXX_FLAGS "-mthreads")
    endif()

    list(APPEND INKSCAPE_LIBS "-lwinpthread")

    if(HAVE_MINGW64)
        list(APPEND INKSCAPE_CXX_FLAGS "-m64")
    else()
        list(APPEND INKSCAPE_CXX_FLAGS "-m32")
    endif()

    # Fixes for windows.h and GTK4
    add_definitions(-DNOGDI)
    add_definitions(-D_NO_W32_PSEUDO_MODIFIERS)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(INKSCAPE_DEP REQUIRED IMPORTED_TARGET
                  harfbuzz>=2.6.5
                  pangocairo>=1.44
                  pangoft2
                  fontconfig
                  gmodule-2.0
                  bdw-gc #boehm-demers-weiser gc
                  lcms2)

list(APPEND INKSCAPE_LIBS PkgConfig::INKSCAPE_DEP)

if(WITH_JEMALLOC)
    find_package(JeMalloc)
    if (JEMALLOC_FOUND)
        list(APPEND INKSCAPE_LIBS ${JEMALLOC_LIBRARIES})
    else()
        set(WITH_JEMALLOC OFF)
    endif()
endif()

pkg_search_module(ICU_UC REQUIRED IMPORTED_TARGET icu-uc)
list(APPEND INKSCAPE_LIBS PkgConfig::ICU_UC)

find_package(Iconv REQUIRED)
list(APPEND INKSCAPE_LIBS Iconv::Iconv)

find_package(Intl REQUIRED)
list(APPEND INKSCAPE_LIBS Intl::Intl)

find_package(GSL REQUIRED)
list(APPEND INKSCAPE_LIBS GSL::gsl)

find_package(double-conversion CONFIG REQUIRED)
list(APPEND INKSCAPE_LIBS double-conversion::double-conversion)

# Check for system-wide version of 2geom and fallback to internal copy if not found
if(NOT WITH_INTERNAL_2GEOM)
    find_package(2Geom ${INKSCAPE_VERSION_MAJOR}.${INKSCAPE_VERSION_MINOR} CONFIG QUIET)
    if(NOT TARGET 2Geom::2geom)
        set(WITH_INTERNAL_2GEOM ON CACHE BOOL "Prefer internal copy of lib2geom" FORCE)
        message(STATUS "lib2geom not found, using internal copy in src/3rdparty/2geom")
    endif()
endif()

if(NOT WITH_INTERNAL_UEMF)
    find_package(Uemf 1.0.0 CONFIG QUIET)
    if(NOT TARGET Uemf::uemf)
        set(WITH_INTERNAL_UEMF ON CACHE BOOL "Prefer internal copy of libuemf" FORCE)
    endif()
endif()

if(NOT WITH_INTERNAL_DEPIXELIZE)
    find_package(Depixelize 0.1.0 CONFIG QUIET)
    if(NOT TARGET Depixelize::depixelize)
        set(WITH_INTERNAL_DEPIXELIZE ON CACHE BOOL "Prefer internal copy of libdepixelize" FORCE)
    endif()
endif()

if(NOT WITH_INTERNAL_AUTOTRACE)
    pkg_check_modules(AUTOTRACE QUIET IMPORTED_TARGET autotrace>=0.31.10)
    if(AUTOTRACE_FOUND)
        add_library(autotrace_LIB ALIAS PkgConfig::AUTOTRACE)
    else()
        set(WITH_INTERNAL_AUTOTRACE ON CACHE BOOL "Prefer internal copy of autotrace" FORCE)
    endif()
endif()

if(NOT WITH_INTERNAL_ADAPTAGRAMS)
    pkg_check_modules(ADAPTAGRAMS QUIET IMPORTED_TARGET libavoid>=0.1 libcola>=0.1 libvpsc>=0.1)
    if(ADAPTAGRAMS_FOUND)
        add_library(adaptagrams_LIB ALIAS PkgConfig::ADAPTAGRAMS)
    else()
        set(WITH_INTERNAL_ADAPTAGRAMS ON CACHE BOOL "Prefer internal copy of adaptagrams" FORCE)
    endif()
endif()

if(WITH_CAPYPDF)
    if(NOT WITH_INTERNAL_CAPYPDF)
        pkg_check_modules(CAPYPDF QUIET IMPORTED_TARGET capypdf>=0.21)
        if(CAPYPDF_FOUND)
            add_library(Inkscape::CapyPDF ALIAS PkgConfig::CAPYPDF)
        else()
            message(STATUS "CapyPDF not found, using internal copy in src/3rdparty/capypdf")
            set(WITH_INTERNAL_CAPYPDF ON CACHE BOOL "Prefer internal copy of capypdf" FORCE)
        endif()
    endif()
    add_definitions(-DWITH_CAPYPDF)
endif()

if(WITH_POPPLER)
    pkg_check_modules(POPPLER IMPORTED_TARGET poppler>=0.20.0 poppler-glib>=0.20.0)
    if(POPPLER_FOUND)
        list(APPEND INKSCAPE_LIBS PkgConfig::POPPLER)
        set(HAVE_POPPLER_CAIRO ${ENABLE_POPPLER_CAIRO})
        add_definitions(-DWITH_POPPLER)
    else()
        set(WITH_POPPLER OFF)
        set(ENABLE_POPPLER_CAIRO OFF)
    endif()
else()
    set(ENABLE_POPPLER_CAIRO OFF)
endif()

if(WITH_LIBWPG)
    pkg_check_modules(LIBWPG IMPORTED_TARGET libwpg-0.3 librevenge-0.0 librevenge-stream-0.0)
    if(LIBWPG_FOUND)
        list(APPEND INKSCAPE_LIBS PkgConfig::LIBWPG)
    else()
        set(WITH_LIBWPG OFF)
    endif()
endif()

if(WITH_LIBVISIO)
    pkg_check_modules(LIBVISIO IMPORTED_TARGET libvisio-0.1 librevenge-0.0 librevenge-stream-0.0)
    if(LIBVISIO_FOUND)
        list(APPEND INKSCAPE_LIBS PkgConfig::LIBVISIO)
    else()
        set(WITH_LIBVISIO OFF)
    endif()
endif()

if(WITH_LIBCDR)
    pkg_check_modules(LIBCDR IMPORTED_TARGET libcdr-0.1 librevenge-0.0 librevenge-stream-0.0)
    if(LIBCDR_FOUND)
        list(APPEND INKSCAPE_LIBS PkgConfig::LIBCDR)
    else()
        set(WITH_LIBCDR OFF)
    endif()
endif()

find_package(JPEG)
if(JPEG_FOUND)
    list(APPEND INKSCAPE_LIBS JPEG::JPEG)
    set(HAVE_JPEG ON)
endif()

find_package(PNG REQUIRED)
list(APPEND INKSCAPE_LIBS PNG::PNG)

find_package(Potrace REQUIRED)
list(APPEND INKSCAPE_LIBS Potrace::Potrace)

if(WITH_SVG2)
    add_definitions(-DWITH_MESH -DWITH_CSSBLEND -DWITH_SVG2)
else()
    add_definitions(-UWITH_MESH -UWITH_CSSBLEND -UWITH_SVG2)
endif()

# ----------------------------------------------------------------------------
# CMake's builtin
# ----------------------------------------------------------------------------

# Include dependencies:

pkg_check_modules(
    MM REQUIRED IMPORTED_TARGET
    cairomm-1.16
    pangomm-2.48
    gdk-pixbuf-2.0
    graphene-1.0
)
list(APPEND INKSCAPE_LIBS PkgConfig::MM)

# if system's gtk is new enough for gtkmm, pick it, otherwise ignore it and gtkmm build will build it
pkg_check_modules(GTK IMPORTED_TARGET gtk4>=4.14.0)
if(GTK_FOUND)
    list(APPEND INKSCAPE_LIBS PkgConfig::GTK)
endif()

pkg_check_modules(GLIBMM IMPORTED_TARGET glibmm-2.68>=2.78.1 giomm-2.68)
if(GLIBMM_FOUND)
    add_library(GLibmm::GLibmm ALIAS PkgConfig::GLIBMM)
else()
    message("GLIBMM too old, glibmm 2.78.1 will be compiled from source")
    include(ExternalProject)
    ExternalProject_Add(glibmm
        URL https://download.gnome.org/sources/glibmm/2.78/glibmm-2.78.1.tar.xz
        URL_HASH SHA256=f473f2975d26c3409e112ed11ed36406fb3843fa975df575c22d4cb843085f61
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND meson setup --libdir lib . ../glibmm --prefix=${CMAKE_CURRENT_BINARY_DIR}/deps
        BUILD_COMMAND meson compile
        INSTALL_COMMAND meson install
    )
    add_library(GLibmm_LIB INTERFACE)
    target_include_directories(GLibmm_LIB INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/deps/include/glibmm-2.68/ ${CMAKE_CURRENT_BINARY_DIR}/deps/lib/glibmm-2.68/include ${CMAKE_CURRENT_BINARY_DIR}/deps/include/giomm-2.68/ ${CMAKE_CURRENT_BINARY_DIR}/deps/lib/giomm-2.68/include)
    target_link_directories(GLibmm_LIB INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/deps/lib)
    target_link_libraries(GLibmm_LIB INTERFACE -lglibmm-2.68)
    add_library(GLibmm::GLibmm ALIAS GLibmm_LIB)
endif()

pkg_check_modules(GTKMM IMPORTED_TARGET gtkmm-4.0>=4.13.3)
if(GTKMM_FOUND)
    add_library(GTKmm::GTKmm ALIAS PkgConfig::GTKMM)
else()
    message("GTKMM too old, gtkmm 4.14.0 will be compiled from source")
    include(ExternalProject)
    ExternalProject_Add(gtkmm
        URL https://download.gnome.org/sources/gtkmm/4.14/gtkmm-4.14.0.tar.xz
        URL_HASH SHA256=9350a0444b744ca3dc69586ebd1b6707520922b6d9f4f232103ce603a271ecda
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND meson setup --libdir lib . ../gtkmm --prefix=${CMAKE_CURRENT_BINARY_DIR}/deps
        BUILD_COMMAND meson compile
        INSTALL_COMMAND meson install
    )
    add_library(GTKmm_LIB INTERFACE)
    target_include_directories(GTKmm_LIB INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/deps/include/gtkmm-4.0 ${CMAKE_CURRENT_BINARY_DIR}/deps/lib/gtkmm-4.0/include ${CMAKE_CURRENT_BINARY_DIR}/deps/include/gtk-4.0/)
    target_link_directories(GTKmm_LIB INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/deps/lib)
    target_link_libraries(GTKmm_LIB INTERFACE -lgtkmm-4.0 PkgConfig::MM)
    add_library(GTKmm::GTKmm ALIAS GTKmm_LIB)
endif()
if(NOT (GTKMM_FOUND AND GLIBMM_FOUND))
    list(APPEND CMAKE_INSTALL_RPATH "${CMAKE_BINARY_DIR}/deps/lib")

    # check we can actually build it
    message("To build gtkmm4, you need the packages glslc, mm-common, and libgstreamer-plugins-bad1.0-dev")

    find_program(glslc glslc REQUIRED)
    find_program(mmcp mm-common-prepare REQUIRED)
    pkg_check_modules(TMP-gtkmm-gstreamer gstreamer-player-1.0 REQUIRED)
endif()
list(APPEND INKSCAPE_LIBS GLibmm::GLibmm GTKmm::GTKmm)

if(WITH_LIBSPELLING)
    pkg_check_modules(LIBSPELLING IMPORTED_TARGET libspelling-1)
    if("${LIBSPELLING_FOUND}")
        message(STATUS "Using libspelling")
        list(APPEND INKSCAPE_LIBS PkgConfig::LIBSPELLING)
    else()
        set(WITH_LIBSPELLING OFF)
    endif()
endif()

if(WITH_GSOURCEVIEW)
    pkg_check_modules(GSOURCEVIEW IMPORTED_TARGET gtksourceview-5)
    if("${GSOURCEVIEW_FOUND}")
        message(STATUS "Using gtksourceview-5")
        list(APPEND INKSCAPE_LIBS PkgConfig::GSOURCEVIEW)
    else()
        set(WITH_GSOURCEVIEW OFF)
    endif()
endif()

# stacktrace print on crash
if(WIN32)
    find_package(Boost 1.19.0 REQUIRED COMPONENTS stacktrace_windbg)
    list(APPEND INKSCAPE_LIBS "-lole32")
    list(APPEND INKSCAPE_LIBS "-ldbgeng")
    add_definitions("-DBOOST_STACKTRACE_USE_WINDBG")
elseif(APPLE)
    find_package(Boost 1.19.0 REQUIRED COMPONENTS stacktrace_basic)
    list(APPEND INKSCAPE_CXX_FLAGS "-D_GNU_SOURCE")
else()
    find_package(Boost 1.19.0 REQUIRED)
    # The package stacktrace_backtrace may not be available on all distros.
    find_package(Boost 1.19.0 COMPONENTS stacktrace_backtrace)
    if (BOOST_FOUND)
        list(APPEND INKSCAPE_LIBS "-lbacktrace")
        add_definitions("-DBOOST_STACKTRACE_USE_BACKTRACE")
    else() # fall back to stacktrace_basic
        find_package(Boost 1.19.0 REQUIRED COMPONENTS stacktrace_basic)
        list(APPEND INKSCAPE_CXX_FLAGS "-D_GNU_SOURCE")
    endif()
endif()



if (CMAKE_COMPILER_IS_GNUCC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 7 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    list(APPEND INKSCAPE_LIBS "-lstdc++fs")
endif()

list(APPEND INKSCAPE_LIBS Boost::headers)

find_package(LibXslt REQUIRED)
list(APPEND INKSCAPE_LIBS LibXslt::LibXslt)

find_package(LibXml2 REQUIRED)
list(APPEND INKSCAPE_LIBS LibXml2::LibXml2)

find_package(ZLIB REQUIRED)
list(APPEND INKSCAPE_LIBS ZLIB::ZLIB)

# PROOF: Illustrator 2020+ stores its native records as Zstandard (AI swatch libraries, AI import).
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
list(APPEND INKSCAPE_LIBS PkgConfig::ZSTD)

if(WITH_GNU_READLINE)
  pkg_check_modules(Readline IMPORTED_TARGET readline)
  if(Readline_FOUND)
    message(STATUS "Found GNU Readline: ${Readline_LIBRARY}")
    list(APPEND INKSCAPE_LIBS PkgConfig::Readline)
  else()
    message(STATUS "Did not find GNU Readline")
    set(WITH_GNU_READLINE OFF)
  endif()
endif()

if(WITH_IMAGE_MAGICK)
    # we want "<" but pkg_check_modules only offers "<=" for some reason; let's hope nobody actually has 7.0.0
    pkg_check_modules(MAGICK IMPORTED_TARGET ImageMagick++<=7)
    if(MAGICK_FOUND)
        set(WITH_GRAPHICS_MAGICK OFF)  # prefer ImageMagick for now and disable GraphicsMagick if found
    else()
        set(WITH_IMAGE_MAGICK OFF)
    endif()
endif()
if(WITH_GRAPHICS_MAGICK)
    pkg_check_modules(MAGICK IMPORTED_TARGET GraphicsMagick++)
    if(NOT MAGICK_FOUND)
        set(WITH_GRAPHICS_MAGICK OFF)
    endif()
endif()
if(MAGICK_FOUND)
    list(APPEND INKSCAPE_LIBS PkgConfig::MAGICK)
    set(WITH_MAGICK ON) # enable 'Extensions > Raster'
endif()

set(ENABLE_NLS OFF)
if(WITH_NLS)
    find_package(Gettext)
    if(GETTEXT_FOUND)
        message(STATUS "Found gettext + msgfmt to convert language files. Translation enabled")
        set(ENABLE_NLS ON)
    else(GETTEXT_FOUND)
        message(STATUS "Cannot find gettext + msgfmt to convert language file. Translation won't be enabled")
        set(WITH_NLS OFF)
    endif(GETTEXT_FOUND)
    find_program(GETTEXT_XGETTEXT_EXECUTABLE xgettext)
    if(GETTEXT_XGETTEXT_EXECUTABLE)
        message(STATUS "Found xgettext. inkscape.pot will be re-created if missing.")
    else()
        message(STATUS "Did not find xgettext. inkscape.pot can't be re-created.")
    endif()
endif(WITH_NLS)

pkg_check_modules(SIGC++ REQUIRED IMPORTED_TARGET sigc++-3.0>=3.6)
list(APPEND INKSCAPE_LIBS PkgConfig::SIGC++)
list(APPEND INKSCAPE_CXX_FLAGS "-DSIGCXX_DISABLE_DEPRECATED")

pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
list(APPEND INKSCAPE_LIBS PkgConfig::EPOXY)


# end Dependencies



# Set include directories and CXX flags
# (INKSCAPE_LIBS are set as target_link_libraries for inkscape_base in src/CMakeLists.txt)

foreach(flag ${INKSCAPE_CXX_FLAGS})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${flag}")
endforeach()
foreach(flag ${INKSCAPE_CXX_FLAGS_DEBUG})
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${flag}")
endforeach()

# Add color output to ninja
if ("${CMAKE_GENERATOR}" MATCHES "Ninja")
    add_compile_options (-fdiagnostics-color)
endif ()

list(REMOVE_DUPLICATES INKSCAPE_LIBS)

include(${CMAKE_CURRENT_LIST_DIR}/ConfigChecks.cmake) # TODO: Check if this needs to be "hidden" here

unset(INKSCAPE_CXX_FLAGS)
unset(INKSCAPE_CXX_FLAGS_DEBUG)
