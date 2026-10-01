# OpenReader — an OSDP card reader on the Waveshare ESP32-C6-LCD-1.47.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openreader)

# The managed wolfSSL component configures itself from its own bundled
# user_settings.h, which has a menuconfig switch for ML-KEM but none for
# ML-DSA. Editing that copy wouldn't survive a re-download, so ML-DSA is
# switched on here instead. PUBLIC because the definitions change wolfCrypt's
# struct layouts: every component that includes its headers (OSDP-Embedded's
# ports/wolfcrypt) must see the same set.
if(CONFIG_OPENREADER_SC2)
    idf_component_get_property(wolfssl_lib wolfssl__wolfssl COMPONENT_LIB)
    target_compile_definitions(${wolfssl_lib} PUBLIC
        HAVE_DILITHIUM
        WOLFSSL_WC_DILITHIUM
        # Pairing uses ML-DSA-44 only.
        WOLFSSL_NO_ML_DSA_65
        WOLFSSL_NO_ML_DSA_87
        # Trade speed for stack. Signing happens once, at pairing, and runs
        # on the OSDP task, so its stack size is what limits us.
        WOLFSSL_DILITHIUM_SIGN_SMALL_MEM
        WOLFSSL_DILITHIUM_VERIFY_SMALL_MEM
        WOLFSSL_DILITHIUM_MAKE_KEY_SMALL_MEM)
endif()
