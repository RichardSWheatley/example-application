/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "../../include/app/lib/diag.h"

/* Define a few common reset-cause bit names for formatting. */
#define DIAG_CAUSE_POR      (1U << 0)
#define DIAG_CAUSE_WDT      (1U << 1)
#define DIAG_CAUSE_SW       (1U << 2)
#define DIAG_CAUSE_BOR      (1U << 3)
#define DIAG_CAUSE_LOCKUP   (1U << 4)

void diag_reset_cause_to_str(uint32_t cause, char *buf, size_t len)
{
    if (!buf || len == 0) return;

    int written = 0;
    if (cause == 0) {
        snprintf(buf, len, "NONE");
        return;
    }

    /* Append known names */
    if (cause & DIAG_CAUSE_POR) {
        written += snprintf(buf + written, (written < (int)len) ? len - written : 0, "%s", written ? ",POR" : "POR");
    }
    if (cause & DIAG_CAUSE_WDT) {
        written += snprintf(buf + written, (written < (int)len) ? len - written : 0, "%s", written ? ",WDT" : "WDT");
    }
    if (cause & DIAG_CAUSE_SW) {
        written += snprintf(buf + written, (written < (int)len) ? len - written : 0, "%s", written ? ",SW" : "SW");
    }
    if (cause & DIAG_CAUSE_BOR) {
        written += snprintf(buf + written, (written < (int)len) ? len - written : 0, "%s", written ? ",BOR" : "BOR");
    }
    if (cause & DIAG_CAUSE_LOCKUP) {
        written += snprintf(buf + written, (written < (int)len) ? len - written : 0, "%s", written ? ",LOCKUP" : "LOCKUP");
    }

    /* If there are unknown bits, append hex value */
    uint32_t known = DIAG_CAUSE_POR | DIAG_CAUSE_WDT | DIAG_CAUSE_SW | DIAG_CAUSE_BOR | DIAG_CAUSE_LOCKUP;
    uint32_t unknown = cause & ~known;
    if (unknown) {
        written += snprintf(buf + written, (written < (int)len) ? len - written : 0, "%s0x%X", written ? "," : "", unknown);
    }

    /* Ensure NUL-termination */
    if (len > 0) buf[len - 1] = '\0';
}
