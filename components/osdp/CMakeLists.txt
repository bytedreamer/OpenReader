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

set(OSDP_INCLUDE_DIRS "${OSDP_DIR}/core/include" "${OSDP_DIR}/pd/include")

# SC2 and pairing need crypto the core leaves to the application. It comes
# from the library's ports/wolfcrypt binding, compiled here rather than in
# main because it ships with the library. The port directory is globbed, so
# files added to it upstream are picked up without editing this list.
if(CONFIG_OPENREADER_SC2)
    if(NOT EXISTS "${OSDP_DIR}/core/include/osdp/osdp_sc2_crypto.h")
        message(FATAL_ERROR
            "OPENREADER_SC2 is on, but '${OSDP_DIR}' has no SC2. Point "
            "OSDP_EMBEDDED_DIR at a checkout of feature/osdp-sc2.")
    endif()
    if(NOT EXISTS "${OSDP_DIR}/ports/wolfcrypt/osdp_pair_wolfcrypt.h")
        message(FATAL_ERROR
            "'${OSDP_DIR}' has no wolfCrypt SC2/pairing port "
            "(ports/wolfcrypt/osdp_pair_wolfcrypt.h). Update the checkout.")
    endif()
    file(GLOB OSDP_WOLFCRYPT_SRCS "${OSDP_DIR}/ports/wolfcrypt/*.c")
    list(APPEND OSDP_SRCS ${OSDP_WOLFCRYPT_SRCS})
    list(APPEND OSDP_INCLUDE_DIRS "${OSDP_DIR}/ports/wolfcrypt")
endif()

idf_component_register(
    SRCS              ${OSDP_SRCS}
    INCLUDE_DIRS      ${OSDP_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS "${OSDP_DIR}/core/src"
                      "${OSDP_DIR}/pd/src"
    # Unconditional, because IDF works out REQUIRES before Kconfig values
    # exist. Without SC2 the wolfssl component registers empty, so this
    # costs nothing there.
    REQUIRES          wolfssl)

# ESP-IDF's UART driver hands received bytes over in batches, when its
# 120-byte RX FIFO fills or the line goes idle, not one at a time. Any frame
# longer than the FIFO therefore arrives in at least two batches with a gap
# between them, about 33 ms at 9600 baud. That is longer than the default
# 20 ms inter-character timeout, so every such frame was thrown away and
# never answered. This is the library's switch for batching transports,
# raising the timeout to 150 ms. PUBLIC because it selects a constant in
# osdp_pd.h, which every includer must agree on.
#
# main/rs485.c now asks the driver for bytes every 8, which keeps the gap
# under the spec's 20 ms on its own. This stays until long frames have been
# checked on the bench with that alone; then it can come out and restore the
# spec's fast abort of a frame cut short.
target_compile_definitions(${COMPONENT_LIB} PUBLIC OSDP_BUFFERED_TRANSPORT)

# The library is freestanding C11 and warning-clean under -Wall -Wextra
# -Wconversion upstream, but IDF's warning set differs slightly by version.
# Keep third-party warnings out of our build log without weakening them for
# OpenReader's own code.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter)
