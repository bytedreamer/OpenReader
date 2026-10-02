/* wolfCrypt configuration for OpenReader's SC2 and PQC pairing.
 *
 * Everything that includes a wolfSSL header reads this file, because the
 * component defines WOLFSSL_USER_SETTINGS publicly. So this is the one
 * place the configuration is set, and wolfCrypt and its callers cannot
 * disagree about it.
 *
 * Deliberately narrow: exactly what OSDP-Embedded's ports/wolfcrypt needs
 * for SC2 and the PD side of pairing, and nothing else. Anything not
 * switched on here is not compiled in. */
#ifndef OPENREADER_WOLFSSL_USER_SETTINGS_H
#define OPENREADER_WOLFSSL_USER_SETTINGS_H

#include "sdkconfig.h"

/* ---- Platform ------------------------------------------------------------ */

#define WOLFSSL_ESPIDF
#define WOLFSSL_ESP32           /* the ESP32 family, which includes the C6 */

/* Software crypto for now. PKOC and the SC1 key store already drive the C6's
 * AES and SHA peripherals through mbedTLS, from the card task. wolfSSL's
 * hardware port hasn't been checked for sharing them safely with mbedTLS,
 * and SC2's traffic is small enough that software AES costs nothing
 * noticeable. Revisit once that sharing has been verified. */
#define NO_ESP32_CRYPT
/* Spelled out per feature as well: some wolfCrypt sources only test these,
 * and sha.c / sha256.c reach for the hardware SHA lock without them. */
#define NO_WOLFSSL_ESP32_CRYPT_HASH
#define NO_WOLFSSL_ESP32_CRYPT_AES
#define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI

#define WOLFCRYPT_ONLY          /* no TLS */
#define NO_FILESYSTEM
#define NO_WRITEV
#define NO_MAIN_DRIVER
#define WOLFSSL_SMALL_STACK     /* large temporaries on the heap */

/* ---- Not used, so not built ---------------------------------------------- */

#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_MD4
#define NO_MD5
#define NO_DES3
#define NO_RC4
#define NO_OLD_TLS
#define NO_PSK

/* ---- SC2 ----------------------------------------------------------------- */

#define HAVE_AESGCM             /* AES-256-GCM, the channel cipher         */
#define HAVE_AES_ECB            /* the raw AES-256 block                   */
#define WOLFSSL_AES_DIRECT
#define WOLFSSL_SHA3
#define WOLFSSL_SHAKE256
#define WOLFSSL_KMAC            /* KMAC256, the session-key derivation     */

/* ---- Pairing ------------------------------------------------------------- */

#define HAVE_HKDF

/* ML-KEM-768 only, and only the PD's half of it: the PD encapsulates to the
 * ACU's key and never generates or decapsulates. The port answers the
 * ACU-side calls with NOT_SUPPORTED in this configuration. */
#define WOLFSSL_HAVE_MLKEM
#define WOLFSSL_WC_MLKEM
#define WOLFSSL_SHAKE128
#define WOLFSSL_NO_ML_KEM_512
#define WOLFSSL_NO_ML_KEM_1024
#define WOLFSSL_MLKEM_NO_MAKE_KEY
#define WOLFSSL_MLKEM_NO_DECAPSULATE

/* ML-DSA-44 only. The small-memory variants trade speed for stack and heap.
 * Signing happens once, when the reader pairs, so the speed doesn't matter
 * and the memory does. */
#define HAVE_DILITHIUM
#define WOLFSSL_WC_DILITHIUM
#define WOLFSSL_NO_ML_DSA_65
#define WOLFSSL_NO_ML_DSA_87
#define WOLFSSL_DILITHIUM_SIGN_SMALL_MEM
#define WOLFSSL_DILITHIUM_VERIFY_SMALL_MEM
#define WOLFSSL_DILITHIUM_MAKE_KEY_SMALL_MEM

#endif /* OPENREADER_WOLFSSL_USER_SETTINGS_H */
