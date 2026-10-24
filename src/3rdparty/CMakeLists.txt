if(WITH_INTERNAL_2GEOM)
    set(2GEOM_BUILD_SHARED ${BUILD_SHARED_LIBS} CACHE BOOL "")
    include(FetchContent)
    FetchContent_Declare(2Geom SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/2geom" OVERRIDE_FIND_PACKAGE)
    FetchContent_MakeAvailable(2Geom)
endif()

if(WITH_INTERNAL_UEMF)
    add_subdirectory(libuemf)
endif()

add_subdirectory(libcroco)

if(WITH_INTERNAL_DEPIXELIZE)
    add_subdirectory(libdepixelize)
endif()

if(WITH_INTERNAL_ADAPTAGRAMS)
    add_subdirectory(adaptagrams)
endif()

if(WITH_INTERNAL_AUTOTRACE)
    add_subdirectory(autotrace)
endif()

if(WITH_CAPYPDF AND WITH_INTERNAL_CAPYPDF)
    add_library(capypdf_LIB STATIC)
    set_property(TARGET capypdf_LIB PROPERTY CXX_STANDARD 23)

    find_package(PNG REQUIRED)
    find_package(JPEG REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(TIFF REQUIRED)
    find_package(Freetype REQUIRED)
    pkg_check_modules(CAPY_DEPS REQUIRED IMPORTED_TARGET lcms2)
    target_link_libraries(capypdf_LIB PUBLIC PNG::PNG JPEG::JPEG ZLIB::ZLIB TIFF::TIFF Freetype::Freetype PkgConfig::CAPY_DEPS)

    set(SRC capypdf/src)
    include(capypdf-compat.cmake)
    target_sources(capypdf_LIB PRIVATE
        ${SRC}/pdfcommon.cpp
        ${SRC}/generator.cpp
        ${CAPYPDF_PROOF_DIR}/drawcontext.cpp
        ${CAPYPDF_PROOF_DIR}/document.cpp
        capypdf-version.cpp
        capypdf-version.h
        ${CAPYPDF_PROOF_DIR}/imagefileops.cpp
        ${CAPYPDF_PROOF_DIR}/utils.cpp
        ${SRC}/colorconverter.cpp
        ${SRC}/fontsubsetter.cpp
        ${CAPYPDF_PROOF_DIR}/ft_subsetter.cpp
        ${CAPYPDF_PROOF_DIR}/cffsubsetter.cpp
        ${SRC}/bitfiddling.cpp
        ${CAPYPDF_PROOF_DIR}/capi.cpp
        ${SRC}/errorhandling.cpp
        ${CAPYPDF_PROOF_DIR}/pdfwriter.cpp
        ${SRC}/objectformatter.cpp
        ${SRC}/commandstreamformatter.cpp
        ${CAPYPDF_PROOF_DIR}/mmapper.cpp
        ${SRC}/xmlformatter.cpp
    )

    set(CAPYPDF_VERSION_MAJOR 0)
    set(CAPYPDF_VERSION_MINOR 21)
    set(CAPYPDF_VERSION_MICRO 0)

    string(CONCAT CAPYPDF_HEADER_REGEX
        "s/^#mesondefine CAPYPDF_VERSION_STR$$/#define CAPYPDF_VERSION_STR \\\"${CAPYPDF_VERSION_MAJOR}.${CAPYPDF_VERSION_MINOR}.${CAPYPDF_VERSION_MICRO}\\\"/;"
        "s/^#mesondefine CAPYPDF_VERSION_MAJOR$$/#define CAPYPDF_VERSION_MAJOR ${CAPYPDF_VERSION_MAJOR}/;"
        "s/^#mesondefine CAPYPDF_VERSION_MINOR$$/#define CAPYPDF_VERSION_MINOR ${CAPYPDF_VERSION_MINOR}/;"
        "s/^#mesondefine CAPYPDF_VERSION_MICRO$$/#define CAPYPDF_VERSION_MICRO ${CAPYPDF_VERSION_MICRO}/;"
        "s/^#mesondefine CAPYPDF_STATIC$$/#define CAPYPDF_STATIC 1/"
    )

    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/capypdf.h"
        COMMAND sed "'${CAPYPDF_HEADER_REGEX}'" "${CMAKE_CURRENT_SOURCE_DIR}/capypdf/include/capypdf.h.in" > "${CMAKE_CURRENT_BINARY_DIR}/capypdf.h"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/capypdf/include/capypdf.h.in"
    )
    add_custom_target(capypdf_HEADER DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/capypdf.h")

    add_dependencies(capypdf_LIB capypdf_HEADER)

    target_include_directories(capypdf_LIB
        PRIVATE
            ${CAPYPDF_PROOF_DIR}
            capypdf/src
        PUBLIC
            capypdf/include # for capypdf.hpp
            ${CMAKE_CURRENT_BINARY_DIR} # for generated capypdf.h
    )

    add_library(Inkscape::CapyPDF ALIAS capypdf_LIB)
endif()
