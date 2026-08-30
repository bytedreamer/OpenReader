/* The reader's audible output — an active sounder on a single GPIO.
 *
 * "Active" means the part generates its own tone: drive the pin and it
 * sounds, release it and it stops. There is nothing to modulate, which suits
 * OSDP exactly — osdp_BUZ carries a tone code with only two defined values
 * (0x01 off, 0x02 default tone), so a reader has precisely one noise to make.
 *
 * The beeping pattern is not ours either. The PD library decodes osdp_BUZ
 * into a resolver and calls the buzzer handler on every on/off edge of the
 * on_time/off_time/count pattern, plus once more when it finishes. So this
 * module is only ever told "sound" or "stop"; it owns no timers.
 *
 * Built only when CONFIG_OPENREADER_BUZZER is set, because enabling it also
 * makes osdp_CAP claim an audible output. Claiming a sounder that is not
 * fitted is a lie an ACU may act on.
 */
#ifndef BUZZER_H
#define BUZZER_H

#include "esp_err.h"
#include <stdbool.h>

/* Configure the pin and leave the sounder silent. */
esp_err_t buzzer_init(void);

/* What the ACU last asked for, straight from the library's buzzer handler.
 * Ignored while the link is down. */
void buzzer_set_osdp(bool sounding);

/* Tell the buzzer whether the OSDP link is up.
 *
 * A sounder is silenced the moment the link drops and stays silent until it
 * returns. This matters more than it looks: osdp_BUZ can command a
 * continuous pattern (count 0), so a reader that lost comms mid-beep would
 * otherwise sound until someone unplugged it. A dark LED is a puzzle; a
 * buzzer stuck on is a callout. On reconnect the ACU's next command decides
 * what happens, which is the right authority. */
void buzzer_set_link(bool online);

#endif /* BUZZER_H */
