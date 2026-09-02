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
| 8 | **[Active buzzer module](https://www.amazon.com/Buzzer-Module-Arduino-ESP8266-Raspberry/dp/B0DYDJMP16/)**, 3.3 V, three-pin *(optional)* | Gives the reader a voice for `osdp_BUZ`. **“Active” is the part that matters** — it makes its own tone and carries its own drive transistor, so it needs one GPIO and no PWM. A passive buzzer will click once and go quiet, because the firmware drives a static level rather than a waveform. | $2 |
| 9 | **Normally-closed tamper switch** *(optional)* | A plunger or lever microswitch the enclosure lid holds shut. Normally-closed matters — see §5.3. Only useful once the reader is in a box. | $2 |

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

### The header, as it sits in front of you

Waveshare's own pinout diagram, with the USB-C connector at the top. Only
`TX`/`RX` are silkscreened by name; every other pin is marked with its bare
GPIO number, so the label on the board *is* the GPIO.

| Left column | | Right column | |
| ----------- | - | ------------ | - |
| `5V` | | `TX` (GPIO16) | UART0 TX |
| `GND` | | `RX` (GPIO17) | UART0 RX |
| `3V3 (OUT)` | | `GP13` | USB D+ — do not use |
| `GP0` | **RS-485 TX** | `GP12` | USB D- — do not use |
| `GP1` | **RS-485 RX** | `GP23` | **RC522 SDA/CS** |
| `GP2` | **RC522 SCK** | `GP20` | **RC522 RST** |
| `GP3` | **RC522 MOSI** | `GP19` | **RC522 MISO** |
| `GP4` | **Tamper switch** *(opt.)* | `GP18` | free |
| `GP5` | **Buzzer** *(opt.)* | `GP9` | BOOT strap |

`GP4` and `GP5` are the microSD slot's chip select and data-out on this
carrier, so using them for the tamper switch and the sounder gives the card
slot up. Nothing in the firmware has ever used it, and the alternative was
having nowhere to put either part: the RC522 needs five pins and the RS-485
pair needs two, which between them account for everything else. `GP18` is
left free as a result.

They are also strapping pins, which is usually a reason to avoid them and
here is not. The ESP32-C6's five strapping pins do not carry equal weight:
`GP8`/`GP9` set the boot mode and `GP15` the JTAG source, but **`GP4` and
`GP5` select SDIO sampling and driving clock edges** — and this firmware
never uses SDIO. Whatever level a switch or a sounder holds them at during
reset picks a clock edge nothing reads. Both float by default with no
internal pull resistor. The build still rejects a tamper pin on 8, 9 or 15.

Note `GP23`, `GP20` and `GP19` are three consecutive pins on the right
column, and the RC522 uses all three. A one-position slip there puts CS on
the reset line and reset on MISO, which does not look like a wiring mistake
from the firmware side — it looks like a module that is simply not answering.
§8 has the diagnostic that tells the two apart.

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

The RC522 gets header pins of its own. It cannot share the LCD's bus: that
bus's clock and data (GPIO6/7) never leave the board. Its lines are picked
from the free header GPIOs below, avoiding the strapping pins.

Wire it once and it works either way the firmware chooses to clock it. The
default build bit-bangs these pins in software, which leaves the chip's only
hardware SPI master free for the LCD so the display and the reader run at the
same time; a build without the display can drive the same pins from hardware
SPI2 instead. Same wiring, same table — see "One SPI master, two
peripherals" in the README.

Listed in the order they appear on the module's own pin strip, so you can
work down it without hunting:

| RC522 pin | ESP32-C6 GPIO | Note |
| --------- | ------------- | ---- |
| `SDA` | GPIO23 (header `GP23`) | This is chip select, **not** I²C data |
| `SCK` | GPIO2 (header `GP2`) | |
| `MOSI` | GPIO3 (header `GP3`) | |
| `MISO` | GPIO19 (header `GP19`) | |
| `IRQ` | Leave unconnected | The driver polls; IRQ is unused |
| `GND` | GND | |
| `RST` | GPIO20 (header `GP20`) | |
| `3.3V` | 3.3 V | **Never 5 V** — the MFRC522 is a 3.3 V part |

Keep these leads short. Neither bus is fussy — hardware SPI2 runs them at
5&nbsp;MHz and the bit-banged bus at roughly 500&nbsp;kHz, both far under the
MFRC522's 10&nbsp;MHz ceiling — but long unshielded jumpers with no solid
ground return between the boards will misread before either rate does.

The RC522's `SDA` pin is a chip select, not I²C data. The silkscreen is
misleading and it catches almost everyone once.

### Audible output — active buzzer (optional)

Only worth wiring if you want the reader to answer `osdp_BUZ`. Everything
above works without it.

| Buzzer pin | ESP32-C6 GPIO | Note |
| ---------- | ------------- | ---- |
| `VCC` / `+` | 3.3 V | A 3.3 V part. Some modules are 5 V — check yours |
| `GND` / `-` | GND | |
| `I/O` / `S` | GPIO5 (header `GP5`) | |

**Make sure it is an *active* buzzer.** The firmware drives GPIO5 to a
static high or low and never generates a waveform, so an active module —
one with its own oscillator — beeps, and a passive one gives you a single
click and then silence. Listings are often ambiguous or sell mixed packs.
The test takes a second: put 3.3 V straight across the buzzer, and a
continuous tone means active.

Then turn it on — it is off by default:

```
idf.py menuconfig    # OpenReader → Audible output — active sounder on GPIO5
```

Off is the honest default rather than a cautious one. Enabling it also makes
`osdp_CAP` report an audible output, and an ACU told a reader can beep will
command beeps and has no way to discover they never happened. Claim the
sounder when the sounder exists.

If it shrieks continuously from boot and falls silent only when the ACU
commands a beep, the module is active low — there is a second option
directly beneath the first for that. The firmware drives the pin to its
silent level before anything else runs, so a correctly configured board is
quiet from power-up.

You do not need to write any beep logic. The PD library decodes `osdp_BUZ`
and resolves its `on_time`/`off_time`/`count` pattern over time, calling the
firmware on each edge; the reader just switches the pin. The sounder is also
silenced whenever the OSDP link drops, because `osdp_BUZ` can command a
continuous pattern and a reader that lost comms mid-beep would otherwise
sound until someone unplugged it.

### Enclosure tamper switch (optional)

Only worth wiring once the reader is in a box. Everything else works without
it, and the option is **off by default** — with it on and no switch fitted,
the input's pull-up reads the floating pin as a permanent tamper.

| Switch | ESP32-C6 GPIO | Note |
| ------ | ------------- | ---- |
| One leg | GPIO4 (header `GP4`) | |
| Other leg | GND | No resistor — the internal pull-up supplies the high level |

**Use a normally-closed switch** — one the enclosure lid holds *shut*. The
lid then holds `GP4` low, opening the lid releases it high, and so does a
cut, corroded or disconnected wire. Sabotage reads as tamper. A normally-open
switch cannot tell an intact quiet loop from a snipped one, so anyone with
wire cutters defeats it silently. If yours is normally-open anyway, there is
an option for it:

```
idf.py menuconfig    # OpenReader → Enclosure tamper switch on GPIO4
                     #            → The switch closes to ground when TAMPERED
```

You do not need to write any reporting logic. The switch is debounced over
50 ms and its state goes into the tamper byte of `osdp_LSTATR`, both when the
ACU asks with `osdp_LSTAT` and unsolicited on the next poll after a change.
The 50 ms is not about contact bounce, which settles far faster — it stops a
marginal switch, or a door that shakes in its frame, from turning into a
stream of status reports at the head end.

### Already on the board — nothing to wire

| Function | GPIO |
| -------- | ---- |
| LCD MOSI / SCLK | 6 / 7 |
| LCD CS / DC / RST / backlight | 14 / 15 / 21 / 22 |
| microSD CS / MISO | 4 / 5 — *reused for tamper and the sounder, see §5* |
| WS2812 RGB LED | 8 |

The LCD is the reason the pin map is shaped the way it is. GPIO6 and GPIO7
are soldered to the panel and reach no header, so the LCD is the one
peripheral here whose bus cannot be moved — which is why, when both are
built, it keeps hardware SPI2 and the RC522 is the one clocked in software.

**The WS2812 on this board is RGB-ordered, not the GRB nearly every WS2812
datasheet specifies.** Measured on the bench: with the strip declared GRB,
an ACU commanding green lit the pixel red. `led_strip` 2.5.5 offers only
GRB and GRBW, so the firmware corrects for it in one place — `paint()` in
`main/status_led.c` hands the driver red and green swapped. If a board
revision ever arrives with the orthodox order, that is the line to change;
§9 says what the symptom looks like.

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

### 8.2b The tamper switch, if you fitted one

Only if you enabled it in `menuconfig`. At boot:

```
I (107) tamper: tamper input on GPIO4, currently normal
```

Open the enclosure — or just lift the switch wire off `GP4` — and within a
tick you should see:

```
W (9120) tamper: enclosure TAMPERED
I (9120) osdp: queued unsolicited osdp_LSTATR reporting tamper
```

`currently TAMPER` at boot with the lid shut means the polarity is inverted
for your switch; see §5.3. Reporting tamper with nothing connected to `GP4`
is the option being on with no switch fitted — the pull-up is reading an open
pin, exactly as documented. And if it reports `normal` but never changes,
check the switch really is on `GP4`: a pin held low by something else looks
identical until you try to trip it.

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

From then on the LED belongs to the ACU: it shows whatever the last
`osdp_LED` commanded, and nothing at all until one arrives — an online
reader whose panel never commands a colour sits dark, which is correct and
looks alarming the first time. The LCD is the one that keeps saying
`ONLINE` regardless.

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
| PKOC card reports its UID instead of a credential | The applet did not answer SELECT, or the card was withdrawn mid-transaction — a PKOC exchange is several round trips and a signature long, so hold the card still. `idf.py monitor` at debug level says which |
| PKOC card reports nothing at all | Either the signature failed to verify, which is logged as a warning and is never fallen back from, or this is a PKOC-only build and the card has no applet |
| ESP32-C6 resets when the bus is connected | The 5 V TXD reached a GPIO — check §4. The pin may already be damaged |
| Reads work, but the panel never sees the card | The PD went offline between the read and the next poll; queued credentials are discarded on a comms loss by design (spec 7.11/7.12) |
| Nothing at all after wiring the divider | Divider fitted to the wrong pin — it belongs on TXD (transceiver → MCU), never on RXD |
| Tamper reported permanently, nothing wired to `GP4` | The tamper option is enabled with no switch fitted; the pull-up reads the open pin as tamper |
| Tamper inverted — normal when open, tamper when shut | Normally-open switch; set the active-low option (§5.3) |
| LED colours swapped — grant shows red, deny shows green | The pixel's byte order. Red/green and cyan/magenta trade places while blue, white and off look correct — see the note in §5. A colour sweep can appear to pass |
| Reader online, LED dark | Correct: the ACU owns the LED once the link is up and has commanded no colour. The LCD still reads `ONLINE` |
| Buzzer clicks once and goes silent | It is a passive buzzer. The firmware drives a static level, not a waveform — you need an active one (§5.2) |

---

## 10. Going beyond the bench

**Secure Channel.** As shipped, this reader speaks clear text. On a real
installation that means anyone with access to the cable can replay a
credential. The firmware is one crypto binding away from SC1 (AES-128) — see
the README's "Adding Secure Channel". Do this before you trust it with a
door.

**Enclosure and tamper.** Supported — a switch on `GP4`, §5 — but off by
default and unwired on a bench build, in which case the reader still answers
"normal" to every `osdp_LSTAT`. A reader in a wall box should have the switch
fitted and the option turned on. The ACU asks; give it a true answer.

**LED brightness.** `LED_LEVEL` in `main/status_led.c` is at full scale
(255), which is what a demo on a desk wants and more than a reader at a door
does — the WS2812 is genuinely unpleasant close up at that level. Turn it
down for an installed unit; every colour is mixed proportionally to it, so
amber stays amber and the offline breath keeps its ramp wherever it is set.

**The credential itself.** A MIFARE Classic UID is not a secret — it is
readable by anyone with a phone and clonable with commodity hardware.
Deciding whether a UID is sufficient authentication for a given door is the
access control system's problem, not the reader's; but when it isn't
sufficient, this reader has an answer.

It reads **PKOC** as well. The card holds a P-256 key pair in its secure
element, its public key is the credential, and at the door it proves it holds
the matching private key by signing a nonce the reader supplies. Cloning one
needs the private key rather than the bits it broadcasts. No extra hardware
is involved — the same RC522 carries it, over ISO/IEC 14443-4 — so this is a
`menuconfig` decision and a supply of PKOC cards, not another shopping list.
See the README's "PKOC" section.

Two settings there deserve a decision rather than a default. **Report the UID
when a card has no PKOC applet** is on, which keeps a mixed population of
badges working; turn it off for a PKOC-only door, because a panel that
accepts either is only as strong as the weaker. And **verify the card's
signature** must stay on for anything with a door behind it — with it off the
reader repeats whatever public key a card claims, which is worth exactly as
much as a UID.

---

## Sources

- [Waveshare ESP32-C6-LCD-1.47 documentation](https://docs.waveshare.com/ESP32-C6-LCD-1.47)
- [DSD TECH SH-U12 product page](https://www.deshide.com/product-details_SH-U12.html)
- [Active buzzer module](https://www.amazon.com/Buzzer-Module-Arduino-ESP8266-Raspberry/dp/B0DYDJMP16/) — the three-pin sounder in the BOM
- [How RFID works, and the RC522 module](https://lastminuteengineers.com/how-rfid-works-rc522-arduino-tutorial/)
  - background on 13.56 MHz RFID, the MFRC522 module's pinout and the
  MIFARE Classic 1K memory layout. Its wiring section is Arduino hardware
  SPI on an Uno, not this board's pin map; use the table in section 5.
- [OSDP-Embedded PD integration guide](https://github.com/Z-bit-Systems-LLC) — `docs/PD_GUIDE.md` in the library checkout
