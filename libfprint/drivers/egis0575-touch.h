/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Diagnostic contact/event classification; never an authentication decision.
 */
#pragma once
#include "egis0575.h"

typedef struct
{
  uint8_t      last[16];
  size_t       last_length;
  unsigned int packets, changes;
} Eh575EventStats;

static inline int
eh575_event_observe (Eh575EventStats *stats, const uint8_t *packet, size_t length)
{
  if (!length || length > sizeof stats->last)
    return 0;
  if (stats->last_length && (length != stats->last_length || memcmp (stats->last, packet, length)))
    stats->changes++;
  memset (stats->last, 0, sizeof stats->last);
  memcpy (stats->last, packet, length);
  stats->last_length = length;
  stats->packets++;
  return 1;
}

static inline int
eh575_touch_idle (const uint8_t *frame)
{
  double mean = eh575_mean (frame);

  return mean >= 96 && mean <= 160 && eh575_deviation (frame, NULL) <= 18 &&
         eh575_clipped (frame) <= 0.005;
}

static inline int
eh575_touch_present (const uint8_t *frame, const uint8_t *background)
{
  return eh575_deviation (frame, NULL) >= 20 && eh575_clipped (frame) <= 0.05 &&
         eh575_deviation (frame, background) >= 8;
}
