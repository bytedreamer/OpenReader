/* OSDP Secure Channel 2 and the PQC pairing that keys it.
 *
 * SC2 runs alongside SC1, not instead of it: the ACU chooses by the
 * Security Control Block it sends. What SC2 changes is where the key comes
 * from. SC1's comes from osdp_KEYSET, sent over a session that starts on
 * SCBK-D, which is a published key. SC2's comes from pairing: the PD and
 * the ACU authenticate each other's certificates with ML-DSA-44 and agree
 * a key over ML-KEM-768, so it never crosses the wire.
 *
 * The pairing key policy mirrors SC1's install-mode policy:
 *
 *   nothing stored     pairing is open; the first ACU to pair keys the PD.
 *   a key came back    SC2 answers on that key. With
 *                      OPENREADER_SC2_DENY_REPAIR, a second pairing is
 *                      refused, so nothing on the bus can displace it.
 *   stored, unreadable SC2 and pairing both stay off. Pairing over a key
 *                      store that looks damaged would let anyone who can
 *                      corrupt flash re-key the reader.
 *
 * The physical key reset (key_reset.h) erases the paired key with the SC1
 * one and is the only way back from the last two states.
 *
 * sc2_bind() runs from osdp_reader_init(), before the OSDP task is created,
 * so it can't race with it. After that, every pairing call happens on the
 * OSDP task, inside osdp_pd_tick(), as osdp_reader.h requires. */
#ifndef SC2_H
#define SC2_H

#include "osdp/osdp_pd.h"

/* Bind SC2 and, if this build can, pairing. `cuid` is the PD's cUID, the
 * same eight bytes SC1 uses. Call after the SC1 setup, which turns on the
 * entropy source. */
void sc2_bind(osdp_pd_t *pd, const uint8_t cuid[OSDP_SC2_CUID_LEN]);

/* Put the PD's in-RAM SC2 key back in step with flash, after an SC2
 * osdp_KEYSET this reader refused. The library rotates its RAM copy even
 * when the handler NAKs, so a refusal leaves RAM holding a key the reader
 * won't have after a power cycle. Call from the OSDP task, on the tick after
 * the refusal; osdp_reader.c does. */
void sc2_reconcile(osdp_pd_t *pd);

#endif /* SC2_H */
