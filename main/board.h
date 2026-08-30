/* Pin map for the Waveshare ESP32-C6-LCD-1.47.
 *
 * The onboard peripherals claim GPIO 4, 5, 6, 7, 8, 14, 15, 21 and 22.
 * GPIO 12/13 are the native USB D-/D+ (this board has no USB-UART bridge
 * chip — flashing and the console both ride native USB), and GPIO 9 is the
 * BOOT strap. That leaves GPIO 0-3, 10, 11, 16-20 and 23 for us.
 *
 * GPIO 16/17 are the pins the silkscreen labels TX/RX: they are UART0's
 * default mapping, and since nothing else claims them here they are free.
 * We deliberately do NOT put RS-485 on them. The ROM bootloader transmits
 * its banner on GPIO16 at 115200 on every reset, before any of our code
 * runs — on a live multi-drop bus that is garbage landing on top of another
 * device's reply. It is a property of the pin, not of the UART peripheral,
 * so routing UART1 there instead would not help. Left free, they are a
 * debug console you can attach while the RS-485 side is talking to a panel.
 *
 * Strapping pins on the ESP32-C6 are GPIO 4, 5, 8, 9 and 15 — nothing we
 * assign below.
 *
 * VERIFY THESE AGAINST YOUR BOARD before wiring anything: Waveshare has
 * shipped more than one revision of the 1.47" carrier, and the Touch
 * variant differs again. The LCD/SD/LED assignments below are from the
 * ESP32-C6-LCD-1.47 wiki page; the RS-485 and RC522 assignments are our
 * choice from what remains.
 */
#ifndef BOARD_H
#define BOARD_H

/* Self-contained: the RS-485 pins below resolve to Kconfig values, so this
 * header supplies them rather than relying on each includer to remember. */
#include "sdkconfig.h"

/* ---- What the header actually exposes ----------------------------------
 *
 * From Waveshare's own pinout diagram, the 2x9 header carries:
 *
 *   left   5V  GND  3V3(OUT)  GP0  GP1  GP2  GP3  GP4  GP5
 *   right  TX  RX   GP13      GP12 GP23 GP20 GP19 GP18 GP9
 *
 * TX/RX are silkscreened as such and are GPIO16/17 (UART0's default map).
 * Everything else is silkscreened with its bare GPIO number.
 *
 * Usable GPIOs, therefore: 0 1 2 3 4 5 9 12 13 16 17 18 19 20 23.
 *
 * NOTE what is NOT here: GPIO6, 7, 8, 10, 11, 14, 15, 21, 22. The LCD's SPI
 * clock and data (6/7), its control lines (14/15/21/22), the WS2812 (8) and
 * the SD chip select (4, also on the header) live on-board only. That makes
 * the LCD's SPI bus unreachable from outside, so an external SPI peripheral
 * cannot share it — it needs its own pins routed through the GPIO matrix,
 * which is what BOARD_SPI_* below does.
 *
 * Strapping pins are GPIO 4, 5, 8, 9 and 15; of those only 4, 5 and 9 reach
 * the header, and nothing below uses them. */

/* ---- Onboard, fixed by the carrier board ------------------------------- */

#define BOARD_LCD_CS        14
#define BOARD_LCD_DC        15
#define BOARD_LCD_RST       21
#define BOARD_LCD_BL        22
/* The microSD slot's chip select and data-out. Listed because they are
 * what the carrier wires here, not because we use them: GPIO4 is the tamper
 * input below and GPIO5 the sounder, which means this build gives the SD
 * slot up. Nothing in the firmware has ever touched it. */
#define BOARD_SD_CS         4

/* The LCD's own SPI clock and data. Board-internal — neither reaches the
 * header — which is the whole reason an external SPI peripheral cannot
 * share this bus. See the SPI2 note below. */
#define BOARD_LCD_MOSI      6
#define BOARD_LCD_SCLK      7

#define BOARD_RGB_LED       8   /* single WS2812 */

/* ---- Ours, on the header ----------------------------------------------- */

/* RS-485 via the DSD TECH SH-U12. No DE/RE pin: the MAX13487 switches
 * direction on its own, so UART1 needs only TX and RX.
 *
 * !! The SH-U12 is a 5 V board and the ESP32-C6 is NOT 5 V tolerant. Its
 *    TXD output must go through a divider or level shifter before reaching
 *    BOARD_RS485_RX. See docs/HARDWARE.md. */
/* Settable under menuconfig -> OpenReader, because these are the one pair
 * that has to match however the transceiver actually got wired, and
 * Waveshare's published documentation does not enumerate this board's
 * header. Trust the silkscreen, not a default. */
#define BOARD_RS485_UART    UART_NUM_1
#define BOARD_RS485_TX      CONFIG_OPENREADER_RS485_TX_GPIO  /* -> SH-U12 RXD */
#define BOARD_RS485_RX      CONFIG_OPENREADER_RS485_RX_GPIO  /* <- SH-U12 TXD,
                                                              * VIA THE DIVIDER */

/* MFRC522 on header pins of its own.
 *
 * It cannot share the LCD's bus: GPIO6/7 carry that clock and data and are
 * not brought out to the header. These five are the RC522's regardless of
 * how it is clocked — hardware SPI2 routes them through the GPIO matrix,
 * the bit-banged driver toggles them directly — so the wiring is the same
 * either way and switching CONFIG_OPENREADER_RC522_BUS needs no rework at
 * the bench.
 *
 * GP4, GP5 and GP9 are avoided: they are strapping pins, and 4/5 are also
 * the SD slot's chip select and MISO. */
#define BOARD_RC522_SCLK    2
#define BOARD_RC522_MOSI    3
#define BOARD_RC522_MISO    19  /* free while the UART sits on GP0/GP1.
                                 * NEVER 12 or 13 — see the guard below. */
#define BOARD_RC522_CS      23
#define BOARD_RC522_RST     20
/* GPIO10 is not on this board's header, so there is no IRQ pin to use. The
 * driver polls anyway, so nothing is lost. */
#define BOARD_RC522_IRQ     (-1)

/* Audible output — an active sounder, one pin. And the enclosure tamper
 * switch, one more.
 *
 * Both live on pins the microSD slot would otherwise have. That is a
 * deliberate trade: the RC522 needs five pins for SPI and the RS-485 pair
 * needs two, which between them take GP0/GP1 and GP2/3/19/20/23 and leave
 * almost nothing. Giving up a card slot the firmware has never used buys
 * back two pins and leaves GP18 spare as well.
 *
 * The header now reads: GP0/GP1 RS-485, GP2/3/19/20/23 the RC522, GP4
 * tamper, GP5 the sounder, GP12/13 the USB data lines, GP16/17 the debug
 * console, and GP18 free.
 *
 * ---- On GPIO4 and GPIO5 being strapping pins ----
 *
 * They are, and it does not matter here, which is worth stating plainly
 * because the opposite rule is the usual one. The ESP32-C6's five strapping
 * pins do not all carry the same weight:
 *
 *   GPIO8, GPIO9  boot mode. Genuinely dangerous — a switch holding GPIO9
 *                 low across reset drops the board into download mode every
 *                 time the enclosure is opened.
 *   GPIO15        JTAG signal source, and ROM message printing.
 *   GPIO4, GPIO5  SDIO sampling and driving clock edges (C6 datasheet,
 *                 "Strapping Pins"). Both float by default with no internal
 *                 pull resistor.
 *
 * This firmware never uses SDIO — the carrier's card slot is wired for SPI
 * and is unused regardless — so whatever level a switch or a sounder module
 * holds these at during reset selects a clock edge that nothing ever reads.
 * The guard below rejects 8, 9 and 15 and permits these two. */
#define BOARD_BUZZER        5
#define BOARD_TAMPER        4

/* ---- Who owns hardware SPI2 --------------------------------------------
 *
 * SPI2 is the ESP32-C6's only general-purpose SPI master and a host has
 * exactly one set of bus pins, so at most one peripheral can be on it.
 *
 * The LCD wins when it is built, and not by preference: its clock and data
 * are soldered to GPIO7/6 and cannot be moved anywhere, whereas the RC522's
 * pins are ours to pick. So the RC522 is the one that can give way, which
 * it does by being clocked in software instead — see
 * CONFIG_OPENREADER_RC522_BUS. That is what lets both run at once.
 *
 * BOARD_SPI_* is left undefined when nothing claims the host. */
#if CONFIG_OPENREADER_DISPLAY
#define BOARD_SPI_HOST      SPI2_HOST
#define BOARD_SPI_SCLK      BOARD_LCD_SCLK
#define BOARD_SPI_MOSI      BOARD_LCD_MOSI
#define BOARD_SPI_MISO      (-1)  /* the panel is write-only */
#elif CONFIG_OPENREADER_RC522_HW_SPI
#define BOARD_SPI_HOST      SPI2_HOST
#define BOARD_SPI_SCLK      BOARD_RC522_SCLK
#define BOARD_SPI_MOSI      BOARD_RC522_MOSI
#define BOARD_SPI_MISO      BOARD_RC522_MISO
#endif

/* Kconfig already makes this unselectable (the hardware-SPI option depends
 * on the display being off). Restated here because the failure it prevents
 * is two drivers quietly reconfiguring the same bus pins at runtime, which
 * presents as an intermittently dead panel rather than as a build error. */
#if CONFIG_OPENREADER_DISPLAY && CONFIG_OPENREADER_RC522_HW_SPI
#error "LCD and RC522 cannot both own hardware SPI2 - pick the bit-banged RC522 bus"
#endif

/* ---- Guards ------------------------------------------------------------
 *
 * GPIO12 and GPIO13 are the ESP32-C6's native USB D-/D+. Assigning either to
 * a peripheral disables USB-Serial-JTAG, which on this board is the console
 * AND the flashing path — so the mistake takes away the very channel you
 * would use to notice and undo it. Recovery means holding BOOT and tapping
 * RESET to get the ROM loader back. Caught at compile time instead. */
#define BOARD_PIN_IS_USB(p) ((p) == 12 || (p) == 13)

#if BOARD_PIN_IS_USB(BOARD_RS485_TX) || BOARD_PIN_IS_USB(BOARD_RS485_RX)
#error "RS-485 pin set to GPIO12/13 - those are USB D-/D+; USB console and flashing would stop working"
#endif
#if CONFIG_OPENREADER_RC522
#if BOARD_PIN_IS_USB(BOARD_RC522_SCLK) || BOARD_PIN_IS_USB(BOARD_RC522_MOSI) \
 || BOARD_PIN_IS_USB(BOARD_RC522_MISO) || BOARD_PIN_IS_USB(BOARD_RC522_CS)   \
 || BOARD_PIN_IS_USB(BOARD_RC522_RST)
#error "RC522 pin set to GPIO12/13 - those are USB D-/D+; USB console and flashing would stop working"
#endif
#endif

#if CONFIG_OPENREADER_TAMPER
#if BOARD_PIN_IS_USB(BOARD_TAMPER)
#error "Tamper pin set to GPIO12/13 - those are USB D-/D+; USB console and flashing would stop working"
#endif
/* Only the strapping pins that actually matter. A tamper switch holds one
 * level for the life of the install, so on GPIO8/9 the enclosure lid would
 * be selecting a boot mode and on GPIO15 the JTAG source. GPIO4 and GPIO5
 * strap SDIO clock edges, which nothing here reads - see the note above
 * BOARD_BUZZER. */
#if BOARD_TAMPER == 8 || BOARD_TAMPER == 9 || BOARD_TAMPER == 15
#error "Tamper GPIO is a boot-mode or JTAG strapping pin (8/9/15) - the enclosure lid would select a boot mode"
#endif
#if BOARD_TAMPER == BOARD_RS485_TX || BOARD_TAMPER == BOARD_RS485_RX
#error "Tamper GPIO collides with an RS-485 pin"
#endif
#if BOARD_TAMPER == BOARD_RGB_LED
#error "Tamper GPIO collides with the onboard WS2812"
#endif
#if CONFIG_OPENREADER_BUZZER
#if BOARD_TAMPER == BOARD_BUZZER
#error "Tamper GPIO collides with the sounder pin"
#endif
#endif
#if CONFIG_OPENREADER_RC522
#if BOARD_TAMPER == BOARD_RC522_SCLK || BOARD_TAMPER == BOARD_RC522_MOSI \
 || BOARD_TAMPER == BOARD_RC522_MISO || BOARD_TAMPER == BOARD_RC522_CS   \
 || BOARD_TAMPER == BOARD_RC522_RST
#error "Tamper GPIO collides with an RC522 pin"
#endif
#endif
#if CONFIG_OPENREADER_DISPLAY
#if BOARD_TAMPER == BOARD_LCD_SCLK || BOARD_TAMPER == BOARD_LCD_MOSI \
 || BOARD_TAMPER == BOARD_LCD_CS   || BOARD_TAMPER == BOARD_LCD_DC   \
 || BOARD_TAMPER == BOARD_LCD_RST  || BOARD_TAMPER == BOARD_LCD_BL
#error "Tamper GPIO collides with an onboard LCD pin"
#endif
#endif
#endif

/* Peripherals must not fight over a pin either. Only the RS-485 pair is
 * configurable, so these all check that pair against whatever else the
 * selected build actually claims — and only against what it claims, so a
 * display-only build does not reject an RS-485 pin on the RC522's GPIO23. */
#if BOARD_RS485_TX == BOARD_RS485_RX
#error "RS-485 TX and RX are the same GPIO"
#endif

#if CONFIG_OPENREADER_DISPLAY
#if BOARD_RS485_TX == BOARD_LCD_SCLK || BOARD_RS485_TX == BOARD_LCD_MOSI \
 || BOARD_RS485_TX == BOARD_LCD_CS   || BOARD_RS485_TX == BOARD_LCD_DC   \
 || BOARD_RS485_TX == BOARD_LCD_RST  || BOARD_RS485_TX == BOARD_LCD_BL   \
 || BOARD_RS485_RX == BOARD_LCD_SCLK || BOARD_RS485_RX == BOARD_LCD_MOSI \
 || BOARD_RS485_RX == BOARD_LCD_CS   || BOARD_RS485_RX == BOARD_LCD_DC   \
 || BOARD_RS485_RX == BOARD_LCD_RST  || BOARD_RS485_RX == BOARD_LCD_BL
#error "RS-485 GPIO collides with an onboard LCD pin"
#endif
#endif

#if CONFIG_OPENREADER_BUZZER
#if BOARD_PIN_IS_USB(BOARD_BUZZER)
#error "Buzzer pin set to GPIO12/13 - those are USB D-/D+; USB console and flashing would stop working"
#endif
#if BOARD_BUZZER == BOARD_RS485_TX || BOARD_BUZZER == BOARD_RS485_RX
#error "Buzzer GPIO collides with an RS-485 pin"
#endif
#if CONFIG_OPENREADER_DISPLAY
#if BOARD_BUZZER == BOARD_LCD_SCLK || BOARD_BUZZER == BOARD_LCD_MOSI  || BOARD_BUZZER == BOARD_LCD_CS   || BOARD_BUZZER == BOARD_LCD_DC    || BOARD_BUZZER == BOARD_LCD_RST  || BOARD_BUZZER == BOARD_LCD_BL
#error "Buzzer GPIO collides with an onboard LCD pin"
#endif
#endif
#if CONFIG_OPENREADER_RC522
#if BOARD_BUZZER == BOARD_RC522_SCLK || BOARD_BUZZER == BOARD_RC522_MOSI  || BOARD_BUZZER == BOARD_RC522_MISO || BOARD_BUZZER == BOARD_RC522_CS    || BOARD_BUZZER == BOARD_RC522_RST
#error "Buzzer GPIO collides with an RC522 pin"
#endif
#endif
#if BOARD_BUZZER == BOARD_RGB_LED
#error "Buzzer GPIO collides with the onboard WS2812"
#endif
#endif

#if CONFIG_OPENREADER_RC522
#if BOARD_RS485_TX == BOARD_RC522_SCLK || BOARD_RS485_TX == BOARD_RC522_MOSI \
 || BOARD_RS485_TX == BOARD_RC522_MISO || BOARD_RS485_TX == BOARD_RC522_CS   \
 || BOARD_RS485_TX == BOARD_RC522_RST  || BOARD_RS485_RX == BOARD_RC522_SCLK \
 || BOARD_RS485_RX == BOARD_RC522_MOSI || BOARD_RS485_RX == BOARD_RC522_MISO \
 || BOARD_RS485_RX == BOARD_RC522_CS   || BOARD_RS485_RX == BOARD_RC522_RST
#error "RS-485 GPIO collides with an RC522 pin"
#endif
#endif

#endif /* BOARD_H */
