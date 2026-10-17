#ifndef IBT_HEX_H
#define IBT_HEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Selects the SSSE3 path when the CPU has it. Call once before parsing. */
void ibt_hex_init(void);

/* Parses up to 16 hex digits at p. Returns the digit count (0 when p does not start with one).
 * A run longer than 16 digits is reported as 17 so the caller can handle overflow.
 * At least 16 readable bytes must follow p. */
size_t ibt_hex_prefix(const char *p, uint64_t *out);

extern bool ibt_hex_simd;

#endif
