# Building an OpenReader

> There is a nicer-looking version of this same guide at
> [`build-guide.html`](build-guide.html) — open it in a browser for rendered
> wiring diagrams and colour-band charts. The two are kept in step; change
> one and change the other.

This is a complete build guide for an OSDP card reader you can put on a real
RS-485 bus and poll from a real access control unit. It assumes you can
solder, or are willing to use screw terminals and jumper wires for a bench
prototype.

Total parts cost is roughly **$35–45**. Build time is about an hour for the
bench version.

---

## 1. What you are building

A **PD** (Peripheral Device, in OSDP's vocabulary) — the reader at the door.
It sits on an RS-485 bus, answers polls from an **ACU** (Access Control
Unit, the panel), and when someone presents a card it reports the credential
on the next poll. The ACU decides whether the door opens; the reader never
does. That split is the whole point of OSDP, and it is why a reader is a
comparatively simple device to build.

```
   ┌──────────────┐         RS-485 (2-wire, half duplex)        ┌─────────┐
   │  OpenReader  │  A ────────────────────────────────────── A │         │
   │   (this PD)  │  B ────────────────────────────────────── B │   ACU   │
   │              │ GND ──────────────────────────────────── GND│ (panel) │
   └──────────────┘                                             └─────────┘
          │
      ┌───┴────┐
      │ RC522  │  ← the card sits here
      └────────┘
```

---

## 2. Bill of materials

| # | Part | Why this one | Approx. |
| - | ---- | ------------ | ------- |
| 1 | **Waveshare ESP32-C6-LCD-1.47** dev board | RISC-V ESP32-C6 with a 1.47" LCD, an RGB LED and a microSD slot already on board. The LCD gives you somewhere to show status without adding parts, and the RGB LED becomes the reader LED the ACU drives. | $16 |
| 2 | **DSD TECH SH-U12** RS-485↔TTL board | Uses the **MAX13487**, which switches direction on its own. No DE/RE pin to toggle means no timing bug to write — a real simplification on a half-duplex bus. | $9 |
| 3 | **SunFounder RC522** MIFARE reader kit | MFRC522 over SPI at 3.3 V, and the kit includes a card and a fob so you have something to present. | $8 |
| 4 | **Resistors: 1 kΩ and 2 kΩ**, ¼ W | The level shifter. See §4 — this is not optional. | $1 |
| 5 | **120 Ω resistor**, ¼ W | RS-485 bus termination, if your reader is at the physical end of the bus. | — |
| 6 | Jumper wires (female–female), or perfboard and wire | Bench wiring. | $3 |
| 7 | USB-C cable | Power, flashing and the serial console, all on the one cable. | — |

For a permanent install you will also want shielded twisted-pair cable
(22–24 AWG, e.g. Belden 3105A or equivalent) and a 12 V supply with a buck
converter to 5 V.

**Tools:** a soldering iron for the level shifter (or a breadboard), and a
multimeter. An oscilloscope or logic analyser is not required but makes §8
much faster if something is wrong.

### Identifying your resistors

Getting the divider wrong is the one parts mistake in this build that damages
hardware, so it is worth thirty seconds with the markings and thirty more
with a meter.

**Through-hole, 4 bands (typically 5%)** — two digits, a multiplier, then
tolerance:

| Value | Bands |
| ----- | ----- |
| **120 Ω** (termination) | brown · red · brown · gold |
| **1 kΩ** (divider, upper) | brown · black · red · gold |
| **2 kΩ** (divider, lower) | red · black · red · gold |
| 1.8 kΩ (alternative) | brown · grey · red · gold |
| 2.2 kΩ (alternative) | red · red · red · gold |
| 10 kΩ (**not** this build) | brown · black · orange · gold |

**Through-hole, 5 bands (typically 1%)** — three digits, a multiplier, then
tolerance. Note that the *third* band is now a digit, not the multiplier,
which is the classic way to misread a 1% part as a 5% one:

| Value | Bands |
| ----- | ----- |
| **120 Ω** | brown · red · black · black · brown |
| **1 kΩ** | brown · black · black · brown · brown |
| **2 kΩ** | red · black · black · brown · brown |
| 1.8 kΩ | brown · grey · black · brown · brown |
| 2.2 kΩ | red · red · black · brown · brown |
| 10 kΩ | brown · black · black · red · brown |

**Which end do you read from?** The tolerance band — gold or silver on most
parts — goes on the right. When both ends look plausible, the wider gap sits
between the multiplier and the tolerance band. If it is still ambiguous,
measure it; a resistor is not directional, so there is no harm in guessing
wrong about which way round it reads.

**Colour values**

| Colour | Digit | Multiplier | Tolerance |
| ------ | ----- | ---------- | --------- |
| Black  | 0 | ×1 | — |
| Brown  | 1 | ×10 | ±1% |
| Red    | 2 | ×100 | ±2% |
| Orange | 3 | ×1 k | — |
| Yellow | 4 | ×10 k | — |
| Green  | 5 | ×100 k | ±0.5% |
| Blue   | 6 | ×1 M | ±0.25% |
| Violet | 7 | ×10 M | ±0.1% |
| Grey   | 8 | — | — |
| White  | 9 | — | — |
| Gold   | — | ×0.1 | ±5% |
| Silver | — | ×0.01 | ±10% |

**Surface mount** parts are printed, not banded:

| Marking style | Reading | 1 kΩ | 2 kΩ | 120 Ω |
| ------------- | ------- | ---- | ---- | ----- |
| 3-digit (5%) | 2 digits + zero count | `102` | `202` | `121` |
| 4-digit (1%) | 3 digits + zero count | `1001` | `2001` | `1200` |
| EIA-96 (1%) | value code + multiplier letter | `01B` | `30B` | — |

`R` marks a decimal point — `4R7` is 4.7 Ω, and `000` or `0R0` is a
zero-ohm jumper. The trap in the 4-digit scheme is the last digit: `1001`
is 1 kΩ but `1002` is 10 kΩ, and on an 0603 part that difference is a
fraction of a millimetre of ink.

**Verify with a meter regardless.** Expect 990–1010 Ω for a 1 kΩ 1% part,
or 950–1050 Ω at 5%. Measure with the resistor out of circuit and touching
nothing else: one connected end is enough to shunt the reading through
whatever it is attached to, and your fingers across the probes will read
low on high-value parts.

---

## 3. Read this before you connect anything

> ### ⚠ The SH-U12 is a 5 V board. The ESP32-C6 is not 5 V tolerant.
>
> The SH-U12's `TXD` pin — the transceiver's output, the one that carries
> data *from* the bus *to* your microcontroller — idles and swings at 5 V.
> The ESP32-C6's GPIOs are rated to 3.3 V and have no tolerance for more.
> Wiring `TXD` straight to a GPIO will damage the pin, and often the chip.
>
> The other direction is fine. The MAX13487's input threshold (V_IH) is
> 2.0 V on a 5 V supply, so the ESP32-C6's 3.3 V output drives it reliably
> with margin. **Only the TXD → RX path needs shifting.**

A tempting shortcut is to power the SH-U12 from 3.3 V instead. Don't. The
MAX13487 is specified for 4.5–5.5 V; below that the differential output
voltage it puts on the bus falls out of spec, and you get a reader that
works on the bench with 30 cm of wire and fails on 200 m of real cable. That
is the worst kind of bug to chase.

---

## 4. The level shifter

A resistor divider is the right tool here. It is one direction only, the
data rate is low, and there is nothing to go wrong.

```
    SH-U12 TXD ────┬──[ 1 kΩ ]──┬── ESP32-C6 GPIO1 (RX)
     (5 V out)     │            │
                   │         [ 2 kΩ ]
                   │            │
                  n/c          GND  (common with the ESP32-C6 GND)
```

5 V × (2 kΩ / (1 kΩ + 2 kΩ)) = **3.33 V**. Comfortably inside spec, and
still well above the ESP32-C6's ~2.3 V input-high threshold, so you have
margin at both ends.

Keep these two resistors close to the ESP32-C6 end of the wire, and keep the
wire short — a divider raises the source impedance, so a long run after it
picks up noise more easily. At the baud rates OSDP uses this is not
demanding; 10 cm of jumper wire is fine.

**If you would rather use a part than two resistors:** a 74AHCT1G125 or
74LVC1G17 running from 3.3 V works and is faster, and a TXS0108E breakout
does the job with more pins than you need. All of these are more parts for
the same result at 9600 baud.

---

## 5. Wiring

The onboard peripherals already claim GPIO 4, 5, 6, 7, 8, 14, 15, 21 and 22.
GPIO 12/13 are the native USB pins and GPIO 9 is the BOOT strap, so all of
those are off limits too. Everything below is chosen from what remains.

### Why not the pins labelled TX and RX?

The header's `TX`/`RX` are **GPIO16/GPIO17** — UART0's default mapping, which
is what the silkscreen is naming. They really are free: this board has no
USB-UART bridge chip, so flashing and the console both ride the ESP32-C6's
native USB on GPIO12/13, and nothing is holding UART0.

We still don't put RS-485 there. **The ROM bootloader transmits its banner on
GPIO16 at 115200 baud on every reset**, before any firmware runs. On a bench
that is harmless noise the panel drops on a CRC check; on a live multi-drop
bus it is garbage arriving on top of another device's reply.

That behaviour belongs to the *pin*, not the UART peripheral — the ESP32-C6
routes any UART to any GPIO through its GPIO matrix, so assigning UART1 to
GPIO16/17 would not avoid it, because the ROM has already transmitted by
then. You can suppress ROM printing (GPIO15 is the strapping pin for it, and
there is an eFuse) but that is a permanent change to make for tidier wiring.

Left alone, `TX`/`RX` are a debug console you can attach while the RS-485
side is busy talking to a panel — worth more than the neater cable run,
especially while bringing up Secure Channel.

Strapping pins on the ESP32-C6 are GPIO 4, 5, 8, 9 and 15. Nothing below
uses one.

> **Verify against your own board before wiring.** Waveshare has shipped more
> than one revision of the 1.47" carrier, and the *Touch* variant differs
> again. The LCD/SD/LED pins below are from the ESP32-C6-LCD-1.47 wiki page.
> These same values live in [`main/board.h`](../main/board.h) — change them
> in one place and both the firmware and this table stay true.

### RS-485 — DSD TECH SH-U12

| SH-U12 pin | Goes to | Note |
| ---------- | ------- | ---- |
| `VCC` | ESP32-C6 board **5 V** pin | Not 3.3 V — see §3 |
| `GND` | ESP32-C6 board `GND` | |
| `RXD` | ESP32-C6 **GPIO0** (header `GP0`, UART1 TX) | Direct. 3.3 V drives it fine |
| `TXD` | **Through the divider** → GPIO1 (header `GP1`, UART1 RX) | ⚠ §4 |
| `A` | Bus line A | Twisted pair, with B |
| `B` | Bus line B | |
| `GND` | Bus signal ground | Yes, really — see §6 |

There is no DE or RE pin to wire. The MAX13487 senses the start of your
transmission and turns the driver on itself, then releases the bus when you
stop. This is why the firmware's `write()` waits for the UART to fully drain
before returning: on a transceiver with no explicit direction pin, the end of
the last bit *is* the end of your turn on the wire.

### Card reader — MFRC522

The RC522 gets SPI2 on header pins of its own. It cannot share the LCD's
bus: that bus's clock and data (GPIO6/7) never leave the board. The
ESP32-C6's GPIO matrix routes SPI2 to any pin, so the RC522's lines are
picked from the free header GPIOs below, avoiding the strapping pins.

| RC522 pin | ESP32-C6 GPIO | Note |
| --------- | ------------- | ---- |
| `3.3V` | 3.3 V | **Never 5 V** — the MFRC522 is a 3.3 V part |
| `GND` | GND | |
| `SCK` | GPIO2 | |
| `MOSI` | GPIO3 | |
| `MISO` | GPIO19 | |
| `SDA` (this is CS) | GPIO23 | The RC522's own chip select |
| `RST` | GPIO20 | |
| `IRQ` | Leave unconnected | The driver polls; IRQ is unused |

The RC522's `SDA` pin is a chip select, not I²C data. The silkscreen is
misleading and it catches almost everyone once.

### Already on the board — nothing to wire

| Function | GPIO |
| -------- | ---- |
| LCD MOSI / SCLK | 6 / 7 |
| LCD CS / DC / RST / backlight | 14 / 15 / 21 / 22 |
| microSD CS / MISO | 4 / 5 |
| WS2812 RGB LED | 8 |

---

## 6. The RS-485 bus itself

Three things matter, and skipping any of them produces a reader that works
across a desk and fails down a corridor.

**Twisted pair.** A and B must be the two conductors of one twisted pair.
This is what makes RS-485 reject noise; using two random wires from a ribbon
cable throws that away.

**Ground.** Connect the bus's signal ground. RS-485 is differential but not
magic: the receivers still need both ends to sit inside a common-mode range
(−7 V to +12 V for this part). Two devices on separate supplies with no
ground reference can drift outside it and stop communicating, or damage
something. On a shielded cable, ground the shield at *one* end only.

**Termination.** 120 Ω across A and B at each of the **two physical ends** of
the bus — and only there. A reader in the middle of a daisy chain gets no
terminator. Check whether your SH-U12 already has one fitted (look for a
resistor marked `121` or `120` near the A/B terminals); if it does and your
reader is mid-bus, remove it or the bus will be over-terminated and the
signal will collapse.

You do not need an external bias network. The MAX13487E has a true failsafe
receiver, so an idle (undriven) bus reads as a logical high — which is the
UART's idle state, so nothing appears on the line until someone actually
transmits.

---

## 7. Power

For the bench, the USB-C cable powers everything. The ESP32-C6 board
regulates 3.3 V for itself and the RC522, and passes 5 V through to its 5 V
pin for the SH-U12. Current draw is modest — around 30–80 mA for the ESP32-C6
with Wi-Fi idle, plus roughly 25 mA for the RC522 with its antenna on.

For an installed reader, take the 12 V that access control panels normally
distribute and buck it down to 5 V at the reader. Do **not** try to run 5 V
down a long cable from the panel; the drop over a couple of hundred metres of
24 AWG will bring you below what the regulator needs.

---

## 8. Bring-up, in order

Do these in sequence. Each one rules out a layer, so a failure tells you
where to look instead of leaving you guessing.

### 8.1 Board alive

Flash the firmware with nothing but USB connected:

```
idf.py set-target esp32c6
idf.py build flash monitor
```

You should see the RGB LED breathing blue — that is the firmware saying "no
ACU is polling me", which is correct, because none is. In the log:

```
I (312) main: OpenReader starting
I (318) led: WS2812 on GPIO8
I (330) rs485: UART1 up at 9600 baud (TX=0 RX=1)
```

### 8.2 The RC522 answers

Same run, a line or two later:

```
I (352) rc522: MFRC522 online, VersionReg=0x92
```

`0x91` or `0x92` are the genuine v1.0/v2.0 parts; clones report other values
and work fine. What you must not see is:

```
E (352) rc522: no response (VersionReg=0x00) — check SPI wiring ...
```

`0x00` or `0xFF` means the SPI bus is not talking at all — MISO stuck low or
high. In order of likelihood: `SDA`/CS on the wrong pin, MISO and MOSI
swapped, or the module powered from 5 V.

Now present the card from the kit. You should get:

```
I (5012) main: card A3:7F:2C:19 (SAK 0x08)
```

If the UID reads but only intermittently, the antenna is marginal — usually
long jumper wires to the RC522, or the module lying against something
metallic.

### 8.3 Does your transceiver echo?

The MAX13487 handles direction automatically. Some auto-direction
transceivers leave the receiver enabled while they are driving the line, so
every byte you transmit arrives straight back on RX.

**This is not fatal either way.** The OSDP library filters frames by
direction, so it recognises its own replies as replies and drops them. Echo
costs some wasted buffer churn, nothing more.

But if you would rather not have it, find out which kind of board you have.
With the SH-U12 wired and the A/B lines going nowhere, add this to
`app_main()` temporarily:

```c
const char *probe = "ECHO?";
uart_write_bytes(BOARD_RS485_UART, probe, strlen(probe));
uart_wait_tx_done(BOARD_RS485_UART, pdMS_TO_TICKS(100));
uint8_t back[16];
int n = uart_read_bytes(BOARD_RS485_UART, back, sizeof(back),
                        pdMS_TO_TICKS(50));
ESP_LOGI("probe", "got %d bytes back", n);
```

`got 5 bytes back` means your board echoes; `got 0 bytes back` means it does
not. If it echoes and you want it suppressed:

```
idf.py menuconfig   →   OpenReader   →   [*] Discard local echo of transmitted bytes
```

**Only turn that on if the probe actually showed an echo.** If the board does
not echo and suppression is enabled, it will eat the first bytes of every
command the ACU sends and your reader will never answer a poll.

### 8.4 The ACU polls you

Connect A, B and ground to your panel. Set the panel to poll address 0 at
9600 baud — or change the reader to match, under
`menuconfig → OpenReader → OSDP PD address`.

The RGB LED stops breathing blue the moment the first poll is answered, and
the log says:

```
I (18420) osdp: link online
```

If you have no panel yet, the OSDP-Embedded repo ships `osdp-acu-mock`,
which drives a PD from a PC serial port — the fastest way to test a reader
without a panel on the desk. See its README.

Then present a card. The panel should report the credential; the reader
logs the read as it hands it to the OSDP task.

---

## 9. When it does not work

| Symptom | Most likely cause |
| ------- | ----------------- |
| LED breathes blue forever, panel reports the device offline | Address or baud mismatch; A and B swapped |
| Panel sees garbage, or intermittent frames | Missing or doubled 120 Ω termination; A/B not on the same twisted pair; no common ground |
| `VersionReg=0x00` at boot | RC522 CS on the wrong pin, or MISO/MOSI swapped |
| Card reads on the bench, not in the enclosure | Metal near the antenna, or the RC522 mounted too far behind a thick faceplate |
| ESP32-C6 resets when the bus is connected | The 5 V TXD reached a GPIO — check §4. The pin may already be damaged |
| Reads work, but the panel never sees the card | The PD went offline between the read and the next poll; queued credentials are discarded on a comms loss by design (spec 7.11/7.12) |
| Nothing at all after wiring the divider | Divider fitted to the wrong pin — it belongs on TXD (transceiver → MCU), never on RXD |

---

## 10. Going beyond the bench

**Secure Channel.** As shipped, this reader speaks clear text. On a real
installation that means anyone with access to the cable can replay a
credential. The firmware is one crypto binding away from SC1 (AES-128) — see
the README's "Adding Secure Channel". Do this before you trust it with a
door.

**Enclosure and tamper.** The status provider in `osdp_reader.c` reports
tamper as permanently normal because a bare dev board has no tamper switch.
A reader in a wall box should have one, wired to a spare GPIO and driving
`s_tamper`. The ACU asks; give it a true answer.

**The credential itself.** A MIFARE Classic UID is not a secret — it is
readable by anyone with a phone and clonable with commodity hardware. This
reader reports the UID, which is the right thing for it to do; deciding
whether a UID is sufficient authentication for a given door is the access
control system's problem, not the reader's. If it isn't sufficient, the
answer is a credential that authenticates cryptographically (DESFire, SEOS,
or a mobile credential), not a change to this firmware.

---

## Sources

- [Waveshare ESP32-C6-LCD-1.47 documentation](https://docs.waveshare.com/ESP32-C6-LCD-1.47)
- [DSD TECH SH-U12 product page](https://www.deshide.com/product-details_SH-U12.html)
- [OSDP-Embedded PD integration guide](https://github.com/Z-bit-Systems-LLC) — `docs/PD_GUIDE.md` in the library checkout
