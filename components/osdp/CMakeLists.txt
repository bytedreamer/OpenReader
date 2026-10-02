# Wraps the OSDP-Embedded C library (a plain CMake project) as an ESP-IDF
# component.
#
# We deliberately do NOT add_subdirectory() the upstream project: its
# CMakeLists builds host tools and tests, and its target-level compile
# options are written for a host toolchain. Registering the sources
# directly keeps the IDF build system in charge of flags, and IDF already
# compiles with -ffunction-sections/-fdata-sections and links with
# --gc-sections, which is exactly the linker garbage collection the
# library's one-TU-per-message layout is designed around. Unreferenced
# codecs cost nothing in the final binary.
#
# Point this at your checkout with -DOSDP_EMBEDDED_DIR=... or the
# OSDP_EMBEDDED_DIR environment variable. The default assumes the sibling
# layout:  <parent>/OSDP-Embedded  and  <parent>/OpenReader
if(DEFINED ENV{OSDP_EMBEDDED_DIR})
    set(OSDP_DIR "$ENV{OSDP_EMBEDDED_DIR}")
else()
    set(OSDP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../OSDP-Embedded"
        CACHE PATH "Path to the OSDP-Embedded checkout")
endif()
get_filename_component(OSDP_DIR "${OSDP_DIR}" ABSOLUTE)

if(NOT EXISTS "${OSDP_DIR}/core/include/osdp/osdp_types.h")
    message(FATAL_ERROR
        "OSDP-Embedded not found at '${OSDP_DIR}'. Set OSDP_EMBEDDED_DIR "
        "(CMake cache variable or environment variable) to your checkout.")
endif()

# Globbed rather than listed: upstream adds message codecs over time, and a
# stale hand-maintained list fails as a confusing link error rather than an
# obvious one. src/dispatch is intentionally excluded — it is the Monitor
# role's bulk decoder and a PD never calls it.
file(GLOB OSDP_SRCS
    "${OSDP_DIR}/core/src/shared/*.c"
    "${OSDP_DIR}/core/src/commands/*.c"
    "${OSDP_DIR}/core/src/replies/*.c"
    "${OSDP_DIR}/core/src/sc/*.c"
    "${OSDP_DIR}/core/src/sc2/*.c"
    "${OSDP_DIR}/core/src/pair/*.c"
    "${OSDP_DIR}/pd/src/*.c")

idf_component_register(
    SRCS              ${OSDP_SRCS}
    INCLUDE_DIRS      "${OSDP_DIR}/core/include"
                      "${OSDP_DIR}/pd/include"
    PRIV_INCLUDE_DIRS "${OSDP_DIR}/core/src"
                      "${OSDP_DIR}/pd/src")

# The library is freestanding C11 and warning-clean under -Wall -Wextra
# -Wconversion upstream, but IDF's warning set differs slightly by version.
# Keep third-party warnings out of our build log without weakening them for
# OpenReader's own code.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter)
