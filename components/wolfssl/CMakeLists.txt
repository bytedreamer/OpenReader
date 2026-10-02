# Wraps wolfCrypt, from a wolfSSL checkout, as an ESP-IDF component, for
# SC2 and PQC pairing.
#
# Not the ESP Component Registry's wolfssl/wolfssl, because the registry
# stops at 5.8.2 and SC2's KMAC256 arrived in wolfSSL 5.9.4. Not wolfSSL's
# own IDE/Espressif component file either: that is 1,200 lines of discovery
# logic built around its menuconfig. Same arrangement as components/osdp and
# components/asymcred instead: register the sources directly, leave IDF in
# charge of flags, and configure through this component's own
# include/user_settings.h, which every user of the headers sees.
#
# Point this at a checkout of wolfSSL v5.9.4-stable or later with
# -DWOLFSSL_ROOT=... or the WOLFSSL_ROOT environment variable (wolfSSL's own
# name for it). The default assumes the sibling layout:
#   <parent>/wolfssl  and  <parent>/OpenReader
#
# Only SC2 uses wolfCrypt, so any other build registers this empty and
# needs no checkout. IDF still wants the registration call.
if(NOT CONFIG_OPENREADER_SC2)
    idf_component_register()
    return()
endif()

if(DEFINED ENV{WOLFSSL_ROOT})
    set(WOLFSSL_ROOT "$ENV{WOLFSSL_ROOT}")
else()
    set(WOLFSSL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../wolfssl"
        CACHE PATH "Path to the wolfSSL checkout")
endif()
get_filename_component(WOLFSSL_ROOT "${WOLFSSL_ROOT}" ABSOLUTE)

# sha3.h has carried KMAC since 5.9.4, so finding it there both locates the
# checkout and rules out one too old for SC2.
if(NOT EXISTS "${WOLFSSL_ROOT}/wolfssl/wolfcrypt/sha3.h")
    message(FATAL_ERROR
        "wolfSSL not found at '${WOLFSSL_ROOT}'. Clone v5.9.4-stable or "
        "later there, or set WOLFSSL_ROOT to a checkout:\n"
        "  git clone --branch v5.9.4-stable --depth 1 "
        "https://github.com/wolfSSL/wolfssl.git")
endif()
file(STRINGS "${WOLFSSL_ROOT}/wolfssl/wolfcrypt/sha3.h" HAS_KMAC
     REGEX "WOLFSSL_KMAC")
if(NOT HAS_KMAC)
    message(FATAL_ERROR
        "The wolfSSL at '${WOLFSSL_ROOT}' predates KMAC, which SC2 needs. "
        "Use v5.9.4-stable or later.")
endif()

# wolfCrypt only: SC2 needs no TLS, so src/ is not compiled at all. With
# NO_ESP32_CRYPT the Espressif port directory compiles to little more than
# the platform helpers the rest of wolfCrypt calls on ESP-IDF.
file(GLOB WOLFSSL_SRCS
    "${WOLFSSL_ROOT}/wolfcrypt/src/*.c"
    "${WOLFSSL_ROOT}/wolfcrypt/src/port/Espressif/*.c")

# Files wolfSSL's own component also leaves out: misc.c and evp.c are
# #included by other sources rather than compiled alone, ext_*.c bind
# external PQC libraries, and the SM2 assembly variants target other CPUs.
list(FILTER WOLFSSL_SRCS EXCLUDE REGEX "/wolfcrypt/src/(misc|evp)\\.c$")
list(FILTER WOLFSSL_SRCS EXCLUDE REGEX "/wolfcrypt/src/ext_[^/]*\\.c$")
list(FILTER WOLFSSL_SRCS EXCLUDE REGEX "/wolfcrypt/src/sp_sm2_[^/]*\\.c$")

# wolfSSL's settings.h includes <FreeRTOS.h> with no directory on ESP-IDF,
# so FreeRTOS's inner include directory has to be on the path. Public,
# because everything that includes a wolfSSL header includes settings.h.
# wolfSSL's own component does the same.
idf_component_register(
    SRCS         ${WOLFSSL_SRCS}
    INCLUDE_DIRS "include"            # user_settings.h
                 "${WOLFSSL_ROOT}"
                 "${IDF_PATH}/components/freertos/FreeRTOS-Kernel/include/freertos")

# PUBLIC so that every component including wolfSSL's headers reads
# user_settings.h, and so sees the configuration wolfCrypt was compiled
# with. Without it a consumer would see different struct layouts.
target_compile_definitions(${COMPONENT_LIB} PUBLIC WOLFSSL_USER_SETTINGS)

# Third-party code: keep its warnings out of OpenReader's build log.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
