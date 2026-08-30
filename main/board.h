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

/* MFRC522 on its own SPI2 pins.
 *
 * It cannot share the LCD's bus: GPIO6/7 carry that clock and data and are
 * not brought out to the header. The ESP32-C6's GPIO matrix routes SPI2 to
 * any pins, so the RC522 gets header pins of its own instead. Routing
 * through the matrix rather than the dedicated IO_MUX pins lowers the
 * usable ceiling, but the RC522 runs at 5 MHz and the matrix is good for
 * well above that.
 *
 * GP4, GP5 and GP9 are avoided: they are strapping pins, and 4/5 are also
 * the SD slot's chip select and MISO. */
/* SPI2 is the ESP32-C6's only general-purpose SPI master, and an SPI host
 * has exactly one set of bus pins. The LCD's are fixed on-board at GPIO7/6;
 * the RC522's would be header pins of our choosing. There is no arrangement
 * that serves both, so CONFIG_OPENREADER_SPI2_* picks one and these three
 * macros follow it.
 *
 * If you want the display and a real reader at the same time, the way out
 * is a bit-banged SPI for the RC522 on spare header pins — it runs at 5 MHz
 * here and would be perfectly happy at 1 — not a second hardware host,
 * because there isn't one. */
#define BOARD_SPI_HOST      SPI2_HOST

#if CONFIG_OPENREADER_SPI2_LCD
#define BOARD_SPI_SCLK      BOARD_LCD_SCLK
#define BOARD_SPI_MOSI      BOARD_LCD_MOSI
#define BOARD_SPI_MISO      (-1)  /* the panel is write-only */
#else
#define BOARD_SPI_SCLK      2
#define BOARD_SPI_MOSI      3
#define BOARD_SPI_MISO      19  /* free while the UART sits on GP0/GP1.
                                 * NEVER 12 or 13 — see the guard below. */
#endif

#define BOARD_RC522_CS      23
#define BOARD_RC522_RST     20
/* GPIO10 is not on this board's header, so there is no IRQ pin to use. The
 * driver polls anyway, so nothing is lost. */
#define BOARD_RC522_IRQ     (-1)

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
#if BOARD_PIN_IS_USB(BOARD_SPI_SCLK) || BOARD_PIN_IS_USB(BOARD_SPI_MOSI) \
 || BOARD_PIN_IS_USB(BOARD_SPI_MISO)
#error "SPI pin set to GPIO12/13 - those are USB D-/D+; USB console and flashing would stop working"
#endif
#if BOARD_PIN_IS_USB(BOARD_RC522_CS) || BOARD_PIN_IS_USB(BOARD_RC522_RST)
#error "RC522 pin set to GPIO12/13 - those are USB D-/D+; USB console and flashing would stop working"
#endif

/* The two peripherals must not fight over a pin either. */
#if BOARD_RS485_TX == BOARD_RS485_RX
#error "RS-485 TX and RX are the same GPIO"
#endif
#if BOARD_RS485_TX == BOARD_SPI_SCLK || BOARD_RS485_TX == BOARD_SPI_MOSI \
 || BOARD_RS485_TX == BOARD_SPI_MISO
#error "RS-485 TX GPIO collides with an SPI bus pin"
#endif
#if BOARD_RS485_RX == BOARD_SPI_SCLK || BOARD_RS485_RX == BOARD_SPI_MOSI \
 || BOARD_RS485_RX == BOARD_SPI_MISO
#error "RS-485 RX GPIO collides with an SPI bus pin"
#endif

/* The peripheral-specific pins are only claimed by whichever half of the
 * SPI2 choice is built, so the collision checks follow the same switch —
 * otherwise selecting the LCD would still reject an RS-485 pin on GPIO23,
 * which in that build nothing is using. */
#if CONFIG_OPENREADER_SPI2_LCD
#if BOARD_RS485_TX == BOARD_LCD_CS  || BOARD_RS485_TX == BOARD_LCD_DC  \
 || BOARD_RS485_TX == BOARD_LCD_RST || BOARD_RS485_TX == BOARD_LCD_BL  \
 || BOARD_RS485_RX == BOARD_LCD_CS  || BOARD_RS485_RX == BOARD_LCD_DC  \
 || BOARD_RS485_RX == BOARD_LCD_RST || BOARD_RS485_RX == BOARD_LCD_BL
#error "RS-485 GPIO collides with an onboard LCD control pin"
#endif
#else
#if BOARD_RS485_TX == BOARD_RC522_CS || BOARD_RS485_TX == BOARD_RC522_RST \
 || BOARD_RS485_RX == BOARD_RC522_CS || BOARD_RS485_RX == BOARD_RC522_RST
#error "RS-485 GPIO collides with an RC522 control pin"
#endif
#endif

#endif /* BOARD_H */
