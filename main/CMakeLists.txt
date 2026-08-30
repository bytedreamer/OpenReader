idf_component_register(
    SRCS "main.c"
         "rs485.c"
         "rc522.c"
         "status_led.c"
         "buzzer.c"
         "tamper.c"
         "display.c"
         "osdp_reader.c"
    INCLUDE_DIRS "."
    REQUIRES osdp
             driver              # uart, spi_master, gpio
             esp_lcd              # ST7789 panel on SPI2
             esp_timer
             espressif__led_strip)
