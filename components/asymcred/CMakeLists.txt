# Wraps the AsymCred asymmetric-credential library (a plain CMake project)
# as an ESP-IDF component.
#
# Same arrangement, and the same reasoning, as components/osdp: we do not
# add_subdirectory() the upstream project, because its CMakeLists builds host
# tests and sets target-level compile options written for a host toolchain.
# Registering the sources directly leaves IDF in charge of flags, and IDF
# already compiles with -ffunction-sections/-fdata-sections and links with
# --gc-sections — which is exactly the linker garbage collection AsymCred's
# one-translation-unit-per-concern layout is designed around.
#
# Point this at your checkout with -DASYMCRED_DIR=... or the ASYMCRED_DIR
# environment variable. The default assumes the sibling layout:
#   <parent>/AsymCred  and  <parent>/OpenReader
# A build with PKOC turned off should not need the checkout at all, so the
# component registers itself empty and gets on with it. IDF still wants the
# registration call — a component that never makes one is a build error.
if(NOT CONFIG_OPENREADER_PKOC)
    idf_component_register()
    return()
endif()

if(DEFINED ENV{ASYMCRED_DIR})
    set(ASYM_DIR "$ENV{ASYMCRED_DIR}")
else()
    set(ASYM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../AsymCred"
        CACHE PATH "Path to the AsymCred checkout")
endif()
get_filename_component(ASYM_DIR "${ASYM_DIR}" ABSOLUTE)

if(NOT EXISTS "${ASYM_DIR}/core/include/asymcred/asymcred_types.h")
    message(FATAL_ERROR
        "AsymCred not found at '${ASYM_DIR}'. Set ASYMCRED_DIR (CMake cache "
        "variable or environment variable) to your checkout, or turn "
        "CONFIG_OPENREADER_PKOC off to build without PKOC support.")
endif()

# Listed rather than globbed, unlike the OSDP component. The difference is
# that OSDP-Embedded gains message codecs over time and a stale list there
# fails as a confusing link error; AsymCred gains whole credential standards
# instead, each of which is a deliberate decision to link. A reader that has
# not chosen to support one should not silently start compiling it.
idf_component_register(
    SRCS         "${ASYM_DIR}/core/src/shared/tlv.c"
                 "${ASYM_DIR}/core/src/shared/apdu.c"
                 "${ASYM_DIR}/pkoc/src/pkoc.c"
                 "${ASYM_DIR}/pkoc/src/pkoc_credential.c"
    INCLUDE_DIRS "${ASYM_DIR}/core/include"
                 "${ASYM_DIR}/pkoc/include")

# The library is warning-clean under its own -Wall -Wextra -Wconversion, but
# IDF's warning set differs slightly by version. Keep third-party warnings
# out of the build log without weakening them for OpenReader's own code.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter)
