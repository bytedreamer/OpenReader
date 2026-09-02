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
| ⬜ | **Secure Channel** — see below. Do not deploy without it |
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

## Adding Secure Channel

**The reader ships speaking clear text.** Anyone with access to the cable can
watch a credential go by and replay it. Fix this before it guards anything.

SC1 needs AES-128 ECB encrypt/decrypt and an RNG bound through
`osdp_sc_crypto_t`. Both are already on the ESP32-C6: mbedTLS ships with
ESP-IDF and is backed by the chip's AES accelerator, and `esp_fill_random()`
is a true hardware RNG. Sketch:

```c
static const osdp_sc_crypto_t crypto = {
    .aes128_ecb_encrypt = esp_aes_encrypt_adapter,
    .aes128_ecb_decrypt = esp_aes_decrypt_adapter,
    .rng                = esp_rng_adapter,
};
osdp_pd_set_sc_crypto(&s_pd, &crypto, NULL);
osdp_pd_set_sc_scbk(&s_pd, scbk /* 16 bytes from NVS */);
osdp_pd_set_sc_cuid(&s_pd, cuid, sizeof(cuid));
s_sc_configured = true;   /* what the LCD reads to stop saying CLEAR TEXT */
```

Two things need care beyond the wiring:

- **Where the SCBK lives.** It belongs in NVS with flash encryption enabled,
  not in a `static const` array in the firmware image. A key you can read
  back off the flash with `esptool` is not a key.
- **Key rotation.** `osdp_KEYSET` rotates the key on the wire, and the
  library applies it — but persisting the new key across a reboot is the
  application's job. Get that wrong and the reader works until it is power
  cycled, then never talks to the panel again. See PD_GUIDE.md, "Key
  rotation with `osdp_KEYSET`".

Bring it up in install mode (SCBK-D) first, confirm the handshake, then move
to a per-device operational key.

## Testing without a panel

The OSDP-Embedded repo ships two tools that make this much less painful:

- `osdp-acu-mock` — a live ACU on a PC serial port. Drives this reader over a
  USB-RS485 adapter.
- `osdp-mcp` — the same thing driven by an AI agent, which can script replies,
  inject NAKs, force session loss and dump the decoded wire history.

## Licence

The OSDP-Embedded library is GPL-3.0-or-later; see its `LICENSING` file for
how that applies to a device built on it.
