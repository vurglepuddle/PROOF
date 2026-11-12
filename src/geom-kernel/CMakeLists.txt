# SPDX-License-Identifier: GPL-2.0-or-later
add_library(proof_gk STATIC interval.cpp dd.cpp det-math.cpp arc-decode.cpp)
target_include_directories(proof_gk PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(proof_gk PUBLIC 2Geom::2geom)
target_compile_options(proof_gk PRIVATE -ffp-contract=off -fno-fast-math)
