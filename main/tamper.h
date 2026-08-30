/* Enclosure tamper switch, on one GPIO.
 *
 * OSDP's local status carries a tamper byte and the ACU asks for it with
 * osdp_LSTAT. Until this existed the reader answered "normal" forever,
 * because a bare dev board has no switch to read — honest only in the sense
 * that it was not pretending to have looked. This is the switch.
 *
 * Polled, not interrupt-driven. A tamper switch changes state when somebody
 * opens a box, which is a handful of times a year, and the OSDP loop
 * already runs every millisecond. An ISR would buy nothing here and would
 * still need its result debounced somewhere.
 */
#ifndef TAMPER_H
#define TAMPER_H

#include "esp_err.h"
#include <stdbool.h>

/* Configure the pin and adopt whatever the switch is currently saying. */
esp_err_t tamper_init(void);

/* Sample the switch and fold the reading into the debounce filter.
 *
 * Call from the OSDP loop; it is a register read and a comparison, so it is
 * cheap enough to call every tick. Returns true on the tick where the
 * debounced state has just changed — the caller's cue to tell the ACU. */
bool tamper_poll(void);

/* The debounced state: true when the enclosure is open, or the switch wire
 * has been cut. */
bool tamper_active(void);

#endif /* TAMPER_H */
