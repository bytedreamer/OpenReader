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
| Credential | SunFounder RC522 (MFRC522), ISO/IEC 14443-A over SPI |
| Reader LED | The board's onboard WS2812, driven by the ACU's `osdp_LED` |

## Status

| | |
| - | - |
| ✅ | PD state machine on RS-485: polls, sequencing, online/offline tracking |
| ✅ | `osdp_ID` / `osdp_CAP` identity and capability reporting |
| ✅ | Card reads reported as `osdp_RAW` on the next poll |
| ✅ | `osdp_LED` driving the RGB LED; `osdp_BUZ` decoded (no sounder fitted) |
| ✅ | `osdp_LSTAT` / `ISTAT` / `OSTAT` / `RSTAT` status reporting |
| ⬜ | **Secure Channel** — see below. Do not deploy without it |
| ✅ | LCD reader face — colour disc mirroring the LED, address, link state, card panel |
| ⬜ | LCD and RC522 at the same time — one SPI2 host, see below |
| ⬜ | Tamper switch input |

## Building

Needs [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/)
**v5.1 or newer** (the ESP32-C6 is not supported before that).

```bash
git clone <this repo>
# OSDP-Embedded is expected as a sibling directory by default:
#   <parent>/OSDP-Embedded
#   <parent>/OpenReader
# Point elsewhere with:  export OSDP_EMBEDDED_DIR=/path/to/OSDP-Embedded

cd OpenReader
idf.py set-target esp32c6
idf.py menuconfig      # OpenReader → SPI2 peripheral, address, baud, card repeat window
idf.py build flash monitor
```

## Configuration

Everything tunable lives under `OpenReader` in `menuconfig`:

| Option | Default | |
| ------ | ------- | - |
| OSDP PD address | 0 | Must match what the ACU polls (0x00–0x7E) |
| RS-485 baud rate | 9600 | The spec's mandatory default; must match the ACU |
| Discard local echo | off | Only enable if your transceiver echoes — [HARDWARE.md §8.3](docs/HARDWARE.md) |
| Card repeat window | 2000 ms | Suppresses re-reads of a card left on the antenna |
| PD serial number | 1 | Give each unit on a bus a distinct value |

Pin assignments are in [`main/board.h`](main/board.h) and are the single
source of truth — the wiring tables in HARDWARE.md follow it.

## How it fits together

```
main.c            two tasks: OSDP service, and RC522 polling
├── rs485.c       UART1 + MAX13487 → the PD's read/write/now_ms transport
├── rc522.c       MFRC522 SPI driver: REQA, anticollision, SELECT, UID
├── osdp_reader.c the PD: identity, capabilities, handlers, event queue
└── status_led.c  the WS2812, driven by osdp_LED (or by link state offline)

components/osdp/  wraps the OSDP-Embedded C sources as an IDF component
```

Two design rules hold the concurrency together:

**Every `osdp_pd_*` call happens on one task.** The library is deliberately
free of locks and globals, which makes it portable but means calling into it
from two tasks is a data race. The card task never touches the PD; it hands
UIDs over a FreeRTOS queue and the OSDP task does the enqueueing.

**The OSDP task outranks the card task.** A card read that waits an extra
millisecond is invisible to everyone. A poll answered late is a comms
failure the panel will log.

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
