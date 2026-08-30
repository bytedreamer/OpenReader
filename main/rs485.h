/* RS-485 transport for the OSDP PD state machine.
 *
 * Supplies the three callbacks osdp_pd_set_transport() wants — a
 * non-blocking read, a write, and a monotonic millisecond clock — over
 * UART1 and the DSD TECH SH-U12 (MAX13487) transceiver.
 */
#ifndef RS485_H
#define RS485_H

#include "osdp/osdp_pd.h"
#include "esp_err.h"

/* Bring up UART1 at `baud`. Call once, before rs485_transport(). */
esp_err_t rs485_init(int baud);

/* The vtable to hand to osdp_pd_set_transport(). Valid after rs485_init();
 * the PD copies it, so the returned pointer need not outlive the call. */
const osdp_pd_transport_t *rs485_transport(void);

/* Wire-level counters, for telling apart the ways a silent bus can be
 * silent. `first` receives up to `first_cap` of the earliest bytes ever
 * received, which is the fastest way to confirm a baud rate: at the right
 * one the first byte of any OSDP frame is 0x53, and at the wrong one you
 * get plausible-looking garbage instead. `*first_len` is how many were
 * captured. Any out-pointer may be NULL. */
void rs485_stats(uint32_t *rx_bytes, uint32_t *tx_bytes,
                 uint8_t *first, size_t first_cap, size_t *first_len);

/* Bring-up aid: push a recognisable marker onto the bus.
 *
 * The PD only ever transmits in answer to a command, so a reader that is
 * receiving nothing also transmits nothing — which leaves the entire
 * outbound path (UART TX, the transceiver's auto-direction turnaround, and
 * whether A/B are connected at all) completely untested. This makes the
 * reader talk on demand so a monitor elsewhere on the bus can say whether
 * its transceiver is alive.
 *
 * Deliberately not a valid OSDP frame: an ACU will drop it on the integrity
 * check rather than act on it. */
void rs485_emit_marker(void);

#endif /* RS485_H */
