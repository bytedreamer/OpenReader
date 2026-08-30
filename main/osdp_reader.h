/* The OSDP Peripheral Device: everything that talks to the ACU.
 *
 * Ownership rule for this module: every osdp_pd_* call happens on the task
 * running osdp_reader_run(). The library keeps no locks and no globals, so
 * calling into it from two tasks would be a data race. The card-polling
 * task therefore does not touch the PD at all — it hands UIDs over a queue
 * with osdp_reader_submit_card() and the OSDP task does the enqueueing.
 */
#ifndef OSDP_READER_H
#define OSDP_READER_H

#include "rc522.h"
#include "esp_err.h"

/* Build the PD, bind the transport, identity, capabilities and handlers.
 * Call after rs485_init() and status_led_init(). */
esp_err_t osdp_reader_init(void);

/* Hand a card read to the OSDP task. Safe from any task. Returns
 * ESP_ERR_NO_MEM if the handoff queue is full, which in practice means the
 * OSDP task is wedged. */
esp_err_t osdp_reader_submit_card(const rc522_uid_t *uid);

/* Service the bus forever. Never returns. */
void osdp_reader_run(void);

#endif /* OSDP_READER_H */
