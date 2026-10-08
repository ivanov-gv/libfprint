/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Experimental stationary ridge matcher. Not a certified biometric matcher.
 */
#pragma once
#include <gio/gio.h>
#include <stdint.h>

G_BEGIN_DECLS
#define EH575_RIDGE_SIZE (103 * 52)
#define EH575_RIDGE_STAGES 15
#define EH575_RIDGE_MAX_TOUCHES 24
#define EH575_RIDGE_SCHEMA "eh575-ridge-v1"
typedef struct
{
  uint8_t image[EH575_RIDGE_SIZE];
  uint8_t background[EH575_RIDGE_SIZE];
} Eh575RidgeTouch;
typedef struct
{
  uint8_t images[5][EH575_RIDGE_SIZE];
  uint8_t background[EH575_RIDGE_SIZE];
} Eh575RidgeProbe;
typedef enum {
  EH575_RIDGE_NO_MATCH,
  EH575_RIDGE_MATCH,
  EH575_RIDGE_POOR_IMAGE,
  EH575_RIDGE_INVALID,
  EH575_RIDGE_CANCELLED,
  EH575_RIDGE_FAILED,
} Eh575RidgeStatus;
typedef struct
{
  Eh575RidgeStatus status;
  unsigned int     matched_frames, template_touch;
  double           correlation, overlap, edge, regional, margin;
} Eh575RidgeResult;
Eh575RidgeResult eh575_ridge_compare (const Eh575RidgeTouch *touches,
                                      unsigned int           count,
                                      const Eh575RidgeProbe *probe,
                                      GCancellable          *cancel);
gboolean eh575_ridge_touch_usable (const Eh575RidgeTouch *touch);
G_END_DECLS
