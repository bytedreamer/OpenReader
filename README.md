# OpenReader

An OSDP v2.2.2 access control reader on a $16 dev board.

OpenReader is a **PD** (Peripheral Device) — the reader at the door. It sits
on an RS-485 bus, answers polls from an access control unit, reports card
reads, and lets the ACU drive its LED. It is built on Z-bit Systems'
[OSDP-Embedded](../OSDP-Embedded) PD library, which supplies the protocol;
this repository is the hardware, the drivers and the device logic around it.

**Want to build one?** The build guide has the parts list, the wiring, the
resistor colour codes and a bring-up sequence that tells you where to look
when something is wrong. Two formats, same content:

- **[docs/build-guide.html](docs/build-guide.html)** — open in a browser.
  Rendered wiring diagrams and colour-band charts; the nicer read.
- **[docs/HARDWARE.md](docs/HARDWARE.md)** — plain Markdown. Renders on
  GitHub, diffs cleanly, greps.

---

## Hardware

| | |
| - | - |
| MCU | Waveshare ESP32-C6-LCD-1.47 (RISC-V, 160 MHz, 512 KB SRAM, 4 MB flash) |
| Bus | DSD TECH SH-U12, MAX13487 auto-direction RS-485 transceiver |
| Credential | SunFounder RC522 (MFRC522), ISO/IEC 14443-A over SPI — UIDs and PKOC |
| Reader LED | The board's onboard WS2812, driven by the ACU's `osdp_LED` |

## Status

| | |
| - | - |
| ✅ | PD state machine on RS-485: polls, sequencing, online/offline tracking |
| ✅ | `osdp_ID` / `osdp_CAP` identity and capability reporting |
| ✅ | Card reads reported as `osdp_RAW` on the next poll |
| ✅ | **PKOC 1.1** — the card signs a nonce, the reader verifies it, the credential follows |
| ✅ | `osdp_LED` driving the RGB LED; `osdp_BUZ` driving a fitted sounder |
| ✅ | `osdp_LSTAT` / `ISTAT` / `OSTAT` / `RSTAT` status reporting |
| ✅ | **Secure Channel (SC1)** — AES-128 on the chip's accelerator, install mode by default |
| ✅ | `osdp_KEYSET` key rotation, persisted across power loss and wrapped to the chip |
| ✅ | Physical key reset — hold BOOT for 10 s to return the reader to install mode |
| ✅ | LCD reader face — link speed, address, Secure Channel state, card panel |
| ✅ | LCD and RC522 together — the reader is clocked in software so the panel keeps SPI2 |
| ✅ | Restart reported to the ACU on the first poll (unsolicited `osdp_LSTATR`) |
| ✅ | Audible output — an active sounder on GP5, driven by `osdp_BUZ` |
| ✅ | Tamper switch input on GP4 — reported in `osdp_LSTATR`, unsolicited on change |

## Building

Needs [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/)
**v5.1 or newer** (the ESP32-C6 is not supported before that).

```bash
git clone <this repo>
# Two libraries are expected as sibling directories by default:
#   <parent>/OSDP-Embedded    the PD protocol stack
#   <parent>/AsymCred         PKOC — only needed if PKOC is built
#   <parent>/OpenReader
# Point elsewhere with:
#   export OSDP_EMBEDDED_DIR=/path/to/OSDP-Embedded
#   export ASYMCRED_DIR=/path/to/AsymCred

cd OpenReader
idf.py set-target esp32c6
idf.py menuconfig      # OpenReader → display, card reader, address, baud, ...
idf.py build flash monitor
```

## Configuration

Everything tunable lives under `OpenReader` in `menuconfig`:

| Option | Default | |
| ------ | ------- | - |
| OSDP PD address | 0 | Must match what the ACU polls (0x00–0x7E) |
| RS-485 baud rate | 9600 | The spec's mandatory default; must match the ACU |
| OSDP Secure Channel | on | Turn off only for bring-up against a panel that cannot do SC |
| Start in install mode | on | Answer SCBK-D until an `osdp_KEYSET` arrives. See below |
| Physical key reset | on | Hold BOOT to erase the key and return to install mode |
| Key reset hold time | 10000 ms | How long the button must be held, continuously |
| Reader face on the LCD | on | The link and card panels; owns hardware SPI2 when built |
| MFRC522 card reader | on | Turn off for a display-only or bus-only build |
| PKOC credentials | on | Needs the AsymCred checkout. Off leaves a UID-only reader |
| PKOC credential width | 256-bit | Must match how the credential was enrolled. See below |
| Verify the card's signature | on | Leave it on. Off is for enrolment and the bench |
| Report the UID with no applet | on | Off makes a PKOC-only door |
| PKOC site / location identifier | zeros | Sent in AUTHENTICATE; the card need not act on it |
| How the RC522 is clocked | bit-banged | Software SPI, so the panel keeps SPI2. See below |
| Audible output | off | An active sounder on GPIO5. Enabling it also makes `osdp_CAP` claim one |
| Enclosure tamper switch | off | A switch on GPIO4. Leave off until one is wired — an open pin reads as tamper |
| Discard local echo | off | Only enable if your transceiver echoes — [HARDWARE.md §8.3](docs/HARDWARE.md) |
| Card repeat window | 2000 ms | Suppresses re-reads of a card left on the antenna |
| PD serial number | 1 | Give each unit on a bus a distinct value |

### One SPI master, two peripherals

The ESP32-C6 has a single general-purpose SPI master (SPI2) and an SPI host
has one set of bus pins, so only one peripheral can be on it. That looks like
a straight choice between the display and the card reader, and it was one for
a while — but the constraint only really binds the panel. The LCD's clock and
data are soldered to GPIO7/6 and cannot be moved anywhere; the RC522's pins
were always ours to pick.

So the RC522 is the one that gives way, and it does so by being clocked
differently rather than by being switched off. The default build bit-bangs
it, leaving hardware SPI2 to the panel, and both run at once.

The cost is close to nothing. Hardware SPI2 clocks the reader at 5&nbsp;MHz and
the software bus at roughly 500&nbsp;kHz against a 10&nbsp;MHz part, but a card poll
is only a few dozen two-byte register accesses, ten times a second. Select
**Hardware SPI2** instead if you are building without the display and want
the headroom — `menuconfig` only offers it when the display is off, and the
wiring is identical either way.

Pin assignments are in [`main/board.h`](main/board.h) and are the single
source of truth — the wiring tables in HARDWARE.md follow it.

## How it fits together

```
main.c            three tasks: OSDP service, RC522 polling, LCD repaint
├── rs485.c       UART1 + MAX13487 → the PD's read/write/now_ms transport
├── rc522.c       MFRC522 driver. 14443-3 (REQA, anticollision, SELECT, UID)
│                 and 14443-4 ISO-DEP (RATS, chaining, WTX) for APDUs.
│                 Hardware SPI2 or bit-banged, chosen at one seam
├── pkoc.c        the PKOC join: mbedTLS crypto, reader identity, one
│                 transaction from card to verified credential
├── credential.c  what was read, in the shape the rest of the reader wants
├── tamper.c      the enclosure switch on GP4, debounced
├── osdp_reader.c the PD: identity, capabilities, handlers, event queue
├── display.c     the reader face on the ST7789: link panel, card panel
└── status_led.c  the WS2812, driven by osdp_LED (or by link state offline)

components/osdp/      wraps the OSDP-Embedded C sources as an IDF component
components/asymcred/  the same, for AsymCred's PKOC library
```

Two design rules hold the concurrency together:

**Every `osdp_pd_*` call happens on one task.** The library is deliberately
free of locks and globals, which makes it portable but means calling into it
from two tasks is a data race. The card task never touches the PD; it hands
credentials over a FreeRTOS queue and the OSDP task does the enqueueing.

**The OSDP task outranks the card task.** A card read that waits an extra
millisecond is invisible to everyone. A poll answered late is a comms
failure the panel will log. That ordering is also what makes it safe to
verify a P-256 signature on the card task: tens of milliseconds of
arithmetic that the OSDP task simply preempts.

### What the library handles, so this firmware doesn't

Most of an OSDP PD is protocol mechanics, and the library absorbs them:
sequence-number policing with byte-identical retransmit detection, `osdp_BUSY`
and its sequencing rules, `osdp_COMSET` address changes applied after the
reply goes out, `osdp_ABORT` / `osdp_ACURXSIZE` / `osdp_KEEPACTIVE`, and the
spec 7.11/7.12 rule that queued credentials are discarded on a comms loss
rather than replayed at a reconnecting ACU.

What is left in `osdp_reader.c` — an identity, a capability set, a handler
that answers POLL and ID, and somewhere to put a card read — is close to the
irreducible minimum, which is why it is short.

## PKOC

A UID is not a secret. It is readable by anyone with a phone and clonable
with commodity hardware, and a reader that reports one is telling the panel
what the card broadcast, not who is holding it.

PKOC — Public Key Open Credential, PSIA's open standard — fixes that with
asymmetric cryptography. The card generates a P-256 key pair inside its
secure element at provisioning and the private key never leaves it. The
public key *is* the credential: it is registered with the access control
system at enrolment, and at the door the card proves it holds the matching
private key by signing a nonce the reader supplies.

Two things follow, and they are the whole reason to want it. This reader
holds no secret worth stealing — nothing on it would help you forge a
credential. And cloning a card requires the private key rather than the bits
it transmits, which is the difference between needing to break a secure
element and needing to stand next to someone.

```
Reader                                                    Card
  |-- RATS ------------------------------------------------>|
  |<-- ATS: frame size, how long you may wait for me --------|
  |                                                         |
  |-- SELECT (AID A000000898000001) ----------------------->|
  |<-- the protocol versions I support ------------ 9000 ----|
  |                                                         |
  |   pick a version both sides understand                  |
  |   draw a 16-byte transaction identifier from the CSPRNG |
  |                                                         |
  |-- AUTHENTICATE (version, transaction id, reader id) --->|
  |                                     sign(txid) on-card  |
  |<-- S(WTX) "still working" ------------------------------|
  |<-- public key (65) + signature (64) ----------- 9000 ----|
  |                                                         |
  |   verify ECDSA-P256-SHA256 over the transaction id      |
  |   THEN derive the credential from the X component       |
  v
osdp_RAW to the panel
```

The protocol is [AsymCred](../AsymCred)'s; the card transport is this
repository's ISO-DEP layer in `rc522.c`; `pkoc.c` is the join.

### The ordering matters

The signature is verified *before* a credential is derived, and there is no
path around it. AsymCred will not hand back a result whose signature did not
verify, and this firmware never falls back to a UID when one fails — a card
that cannot prove it holds the private key has told the reader nothing at
all, and reporting the UID it also happens to carry would quietly turn a
failed proof into a successful read.

A card with no PKOC applet is different, and is treated differently: it is an
ordinary badge and its UID is reported as before. Turn off **Report the UID
when a card has no PKOC applet** for a PKOC-only door — the stricter setting,
and the defensible one if the panel would accept both, since a door that
takes either is only as strong as the weaker.

### Credential width

All three widths defined by the specification are taken from the X component
of the public key; they differ only in how much of it reaches the panel.

| Setting | Bits | Bytes | |
| ------- | ---- | ----- | - |
| 256-bit | 256 | 32 | The whole X component, no truncation. **The default** |
| 75-bit | 75 | 10 | PSIA's recommendation where a legacy panel cannot carry the full key |
| 64-bit | 64 | 8 | The minimum the specification defines |

Whatever you choose has to match how the credential was enrolled in the
access control system. A 75-bit reader and a 256-bit enrolment do not meet in
the middle; they simply never match.

The reply is `osdp_RAW` with format code 0x00 and the true bit count — the
75-bit form occupies ten bytes whose top five bits are zero, and the reply
says 75, not 80.

### Where the randomness comes from

The transaction identifier is the only thing making the card's signature
fresh. A predictable one — a counter, a timestamp, a PRNG from a fixed seed
— makes a captured card response replayable, which is exactly what the
protocol exists to prevent.

`esp_random()` is a true random number generator only while the RF subsystem
is running, and this firmware never brings up Wi-Fi or Bluetooth. So
`pkoc_init()` calls `bootloader_random_enable()`, which is ESP-IDF's answer
for precisely this case: it enables the SAR-ADC entropy source. Nothing here
uses the ADC, so it stays on for the life of the device. If you add anything
that does use the ADC, this is the line to revisit — the two cannot run at
once, and getting it wrong degrades the nonce silently.

### What it costs

A transaction is a handful of ISO-DEP frames plus a signature verification,
which lands somewhere in the low hundreds of milliseconds — most of it the
card's own arithmetic, announced with S(WTX) blocks the reader honours. It
runs on the card task at priority 5, so the OSDP task preempts all of it and
the bus never waits. The card task's stack is doubled to 8 KB in a PKOC
build; that is where mbedTLS's elliptic curve code goes.

### Cards

The PKOC applet is an open standard, so any card carrying one works. If you
have none, AsymCred ships the card half too — a JavaCard applet under
`card/`, Apache-2.0 licensed, with a prebuilt CAP file. Load it onto a blank
JavaCard and both ends of this exchange come from the same repository.

## Secure Channel

**The reader speaks Secure Channel, and it starts in install mode.** SC1 is
built by default: AES-128 from mbedTLS on the C6's own accelerator, the
handshake nonce from the hardware RNG, and the key held in flash wrapped so
that a copy of the flash is not a copy of the key.

### Bringing one up

A reader that has never been keyed answers the handshake on **SCBK-D** — the
constant `0x30 0x31 … 0x3F` from spec D.4, which every OSDP implementation
knows. That is how a panel reaches a factory-fresh reader:

1. Power the reader. The LCD says `INSTALL / SCBK-D` and the console says
   `INSTALL MODE`.
2. Point the ACU at it with Secure Channel enabled and the default key. The
   session comes up; the LCD says `INSTALL / OPEN KEY`, still in amber,
   because a session wrapped under a published key is not a secure one.
3. Send **`osdp_KEYSET`** with a 16-byte SCBK. The reader stores it, ACKs,
   and stops answering SCBK-D from that moment.
4. The ACU re-handshakes with key selector 1. The LCD says `SECURE` in
   green.

Step 3 is one-way. From then on the only party that can talk to the reader
is one holding that key — which is the point, and which is why there is a
button for step 5.

### Where the key lives

In NVS, and by default encrypted with AES-256-GCM under a key the CPU cannot
read: `esp_hmac_calculate()` drives the C6's HMAC peripheral against a
read-protected eFuse block, so the wrapping key exists in the hardware and
nowhere else. Someone who desolders the flash gets a ciphertext only that one
chip can open.

That needs one eFuse block burned, once, per device:

```bash
head -c 32 /dev/urandom > hmac_key.bin
espefuse.py burn_key BLOCK_KEY0 hmac_key.bin HMAC_UP
rm hmac_key.bin        # nothing ever needs it again
```

Unlike flash encryption this is not a whole-device, one-way commitment — it
burns one key block and leaves the rest of the chip alone. Do it before the
reader is keyed, or after: a key found stored in the clear is rewrapped on
the next boot once the block exists.

**With no such block burned the key is stored in plaintext**, and every boot
says so at `ESP_LOGE`. That is deliberate — a bare board can still bring
Secure Channel up on the bench — but a reader on a door in that state is one
`esptool read_flash` away from having no Secure Channel at all.

Note what this does and does not do. It binds the key to the chip, not to the
firmware: anything that can execute code on this ESP32-C6 can ask the same
peripheral to unwrap the blob. **Secure Boot and flash encryption remain the
right answer** for a reader guarding anything that matters. This raises the
floor from "readable with esptool" to "requires code execution on this
specific device".

### Getting back: the physical key reset

`osdp_KEYSET` has no undo on the wire, and it must not have one — a path back
to a published key that could be reached over the bus would be the very hole
the key exists to close. So the way back is physical:

**Hold the BOOT button for 10 seconds while the reader is running.** The LCD
counts down, the LED goes amber, and at zero the stored key is erased and the
reader restarts into install mode. Releasing at any point cancels it.

This does not disturb what BOOT already does. The ROM samples that pin at
reset; the firmware reads it long afterwards. Holding BOOT *across* a reset
still enters download mode, key untouched.

Once the board is in an enclosure the button is inside it — so reaching it
means opening the box, and on a build with the tamper switch fitted that is
an event the ACU is told about. The reader cannot be quietly downgraded.

The hold time is `CONFIG_OPENREADER_KEY_RESET_HOLD_MS`; the whole feature is
`CONFIG_OPENREADER_KEY_RESET`, and turning it off means a lost key can only
be recovered by re-flashing.

### What the reader face says

| | |
| - | - |
| `CLEAR TEXT` | Secure Channel compiled out. Nothing on this bus is protected |
| `INSTALL SCBK-D` | Never keyed, waiting for an `osdp_KEYSET` |
| `INSTALL OPEN KEY` | Session up — under the key from the specification |
| `NO SESSION` | Keyed, and the ACU has not established a session. Usually a key mismatch |
| `SECURE` | Keyed, session established. The only green one |
| `KEY FAULT` | A key is stored and will not come back. See below |

`KEY FAULT` means the key store is damaged, or the flash was moved to a
different chip than the eFuse that wrapped it. The reader then refuses
**every** handshake, including SCBK-D. That is on purpose: a reader that fell
back to install mode whenever its key store looked damaged would hand anyone
who can corrupt flash a downgrade to a published key. Hold BOOT to return it
to install mode deliberately.

## Testing without a panel

The OSDP-Embedded repo ships two tools that make this much less painful:

- `osdp-acu-mock` — a live ACU on a PC serial port. Drives this reader over a
  USB-RS485 adapter.
- `osdp-mcp` — the same thing driven by an AI agent, which can script replies,
  inject NAKs, force session loss and dump the decoded wire history.

## Licence

The OSDP-Embedded library is GPL-3.0-or-later; see its `LICENSING` file for
how that applies to a device built on it.
