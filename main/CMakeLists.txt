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
             mbedtls             # SHA-256 and ECDSA P-256 for PKOC
             bootloader_support  # bootloader_random_enable — see pkoc.c
             espressif__led_strip)
