/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APP_LIB_DIAG_H_
#define APP_LIB_DIAG_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Format a reset-cause bitmask into a short human-readable string.
 * @param cause Bitmask of reset cause bits (platform-specific)
 * @param buf Output buffer
 * @param len Buffer length
 */
void diag_reset_cause_to_str(uint32_t cause, char *buf, size_t len);

#endif /* APP_LIB_DIAG_H_ */
