#ifndef KFSW_SERVICES_BOOT_BUDGET_H
#define KFSW_SERVICES_BOOT_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

/** Read the early flash budget; UINT8_MAX means unavailable. */
void kfsw_boot_budget_get(uint8_t *left, uint8_t *maximum);
/** Validate and persist the ground's remaining-attempt write. */
int kfsw_boot_budget_set_left(uint8_t left);
/** Set the live limit (3, 5 or 10), clamping the count without refilling it. */
int kfsw_boot_budget_set_max(uint8_t maximum);
/** Refill after FWU has installed an image. Returns a store error on failure. */
int kfsw_boot_budget_refill(void);
/** Consume one early startup. Commanded must already be backed by reset evidence. */
int kfsw_boot_budget_start(bool commanded);
/** Whether the last startup found the budget already empty and owes Golden. */
bool kfsw_boot_budget_spent(void);
/** Whether the reset mask and CRC-validated retained note justify an exemption. */
bool kfsw_boot_budget_commanded(uint32_t cause, bool valid_commanded_note);

#endif
