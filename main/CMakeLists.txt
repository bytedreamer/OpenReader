set(OPENREADER_SRCS
    "main.c"
    "rs485.c"
    "rc522.c"
    "credential.c"
    "status_led.c"
    "buzzer.c"
    "tamper.c"
    "display.c"
    "osdp_reader.c")

# Secure Channel drags in the key store and the physical reset that undoes an
# osdp_KEYSET. Both are dead weight in a clear-text build — sc_key.c would sit
# on an NVS namespace nothing reads, and key_reset.c would hold a GPIO in
# order to erase a key that does not exist — so neither is compiled.
if(CONFIG_OPENREADER_SECURE_CHANNEL)
    list(APPEND OPENREADER_SRCS "sc_key.c")
    if(CONFIG_OPENREADER_KEY_RESET)
        list(APPEND OPENREADER_SRCS "key_reset.c")
    endif()
endif()

# pkoc.c is the only file that includes the AsymCred headers, so a build with
# PKOC turned off must not compile it — the component that supplies those
# headers registers itself empty in that configuration.
if(CONFIG_OPENREADER_PKOC)
    list(APPEND OPENREADER_SRCS "pkoc.c")
endif()

idf_component_register(
    SRCS ${OPENREADER_SRCS}
    INCLUDE_DIRS "."
    REQUIRES osdp
             asymcred            # PKOC transaction and credential derivation
             driver              # uart, spi_master, gpio
             esp_lcd             # ST7789 panel on SPI2
             esp_timer
             mbedtls             # SHA-256 and ECDSA P-256 for PKOC;
                                 # AES-128 for Secure Channel, AES-GCM to
                                 # wrap the key at rest
             nvs_flash           # where the Secure Channel key survives a
                                 # power cycle
             efuse               # finding the HMAC key block, if one is
                                 # burned — see sc_key.h
             esp_hw_support      # esp_hmac_calculate: derives the wrapping
                                 # key without the eFuse ever being readable
             bootloader_support  # bootloader_random_enable — see pkoc.c
             espressif__led_strip)
