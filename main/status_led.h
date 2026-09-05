/* The board's single WS2812, driven from two places.
 *
 * The ACU owns the reader LED: osdp_LED commands arrive, the PD library
 * folds them into a colour resolver, and status_led_set_osdp() paints
 * whatever it resolves to. But an unconfigured or offline reader gets no
 * LED commands at all, and a dark reader is indistinguishable from a dead
 * one — so when the PD is offline we take the LED back and show link state
 * instead. status_led_set_link() is that override; the ACU's colour wins
 * again as soon as the PD is online.
 */
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t status_led_init(void);

/* Paint an osdp_led_color_t (0x00 black .. 0x07 white). Ignored while the
 * PD is offline. */
void status_led_set_osdp(uint8_t osdp_color);

/* Tell the LED whether the OSDP link is up. While it is down the LED shows
 * a slow blue breath instead of the ACU's colour. */
void status_led_set_link(bool online);

/* Advance the offline animation. Call from the main loop; a no-op while
 * online, and a no-op while an override holds the LED. */
void status_led_tick(void);

/* Take the LED away from both the ACU and the offline animation.
 *
 * There is exactly one thing this is for: a local, physical operation that
 * the person standing at the reader needs to see the progress of, and that
 * outranks anything the panel has to say about a light. Holding the key
 * reset button is that operation — ten seconds with no feedback is
 * indistinguishable from a button that is not wired, and an installer with
 * no signal lets go early and concludes the feature is broken.
 *
 * `osdp_color` is an osdp_led_color_t so it goes through the same resolver
 * the ACU's colours do. Pass STATUS_LED_NO_OVERRIDE to release, which hands
 * the LED straight back to whichever of the two owners should have it. */
#define STATUS_LED_NO_OVERRIDE (-1)
void status_led_override(int osdp_color);

#endif /* STATUS_LED_H */
