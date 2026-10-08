/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Bounded experimental swipe acquisition. Alignment is not authentication.
 * No fabricated padding, forced motion, ridge synthesis or matcher changes.
 */
#pragma once
#include "egis0575.h"
#include <stdlib.h>

#define EH575_SWIPE_MAX_FRAMES 128
#define EH575_SWIPE_MAX_HEIGHT 512
#define EH575_SWIPE_MIN_HEIGHT 156
#define EH575_SWIPE_MIN_WIDTH 71
#define EH575_SWIPE_DX 8
#define EH575_SWIPE_DY 16

typedef enum {
  EH575_SWIPE_FIRST,
  EH575_SWIPE_STILL,
  EH575_SWIPE_MOVED,
  EH575_SWIPE_UNCERTAIN,
  EH575_SWIPE_REVERSED,
  EH575_SWIPE_LIMIT,
} Eh575SwipeResult;

typedef struct
{
  uint8_t      frames[EH575_SWIPE_MAX_FRAMES][EH575_FRAME_SIZE];
  int          x[EH575_SWIPE_MAX_FRAMES], y[EH575_SWIPE_MAX_FRAMES];
  unsigned int count;
  int          min_x, max_x, min_y, max_y, direction;
  /* Last pair's acquisition diagnostics, never authentication evidence. */
  double       zero, best, margin;
  int          dx, dy;
} Eh575Swipe;

/* Incoming frame at (dx,dy) in the reference frame's coordinates. Compare
 * only actual overlapping pixels; remove each overlap's brightness offset.
 */
static inline double
eh575_swipe_correlation (const uint8_t *previous, const uint8_t *current, int dx, int dy)
{
  int width = EH575_WIDTH - abs (dx), height = EH575_HEIGHT - abs (dy);
  int px = dx > 0 ? dx : 0, py = dy > 0 ? dy : 0;
  int cx = dx < 0 ? -dx : 0, cy = dy < 0 ? -dy : 0;
  double a = 0, b = 0, aa = 0, bb = 0, ab = 0, n = width * height;

  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      {
        double av = previous[(py + y) * EH575_WIDTH + px + x];
        double bv = current[(cy + y) * EH575_WIDTH + cx + x];
        a += av;
        b += bv;
        aa += av * av;
        bb += bv * bv;
        ab += av * bv;
      }
  aa -= a * a / n;
  bb -= b * b / n;
  if (aa / n < 64 || bb / n < 64)
    return -1; /* Blank/low-contrast areas cannot establish motion. */
  return (ab - a * b / n) / sqrt (aa * bb);
}

static inline Eh575SwipeResult
eh575_swipe_push (Eh575Swipe *swipe, const uint8_t *frame)
{
  double scores[EH575_SWIPE_DY * 2 + 1][EH575_SWIPE_DX * 2 + 1];
  double best = -1, second = -1;
  int best_x = 0, best_y = 0, x, y, min_x, max_x, min_y, max_y;

  swipe->zero = swipe->best = -1;
  swipe->margin = 0;
  swipe->dx = swipe->dy = 0;
  if (swipe->count == 0)
    {
      if (eh575_deviation (frame, NULL) < 8)
        return EH575_SWIPE_UNCERTAIN;
      memcpy (swipe->frames[0], frame, EH575_FRAME_SIZE);
      swipe->x[0] = swipe->y[0] = 0;
      swipe->min_x = swipe->max_x = swipe->min_y = swipe->max_y = swipe->direction = 0;
      swipe->count = 1;
      return EH575_SWIPE_FIRST;
    }
  if (swipe->count >= EH575_SWIPE_MAX_FRAMES)
    return EH575_SWIPE_LIMIT;
  /* A stationary frame must not be assigned a nonzero shift by periodic
   * ridges. Keep the last accepted anchor so slow subpixel motion accumulates.
   */
  swipe->zero = eh575_swipe_correlation (swipe->frames[swipe->count - 1], frame, 0, 0);
  if (swipe->zero >= .995)
    return EH575_SWIPE_STILL;
  for (int dy = -EH575_SWIPE_DY; dy <= EH575_SWIPE_DY; dy++)
    for (int dx = -EH575_SWIPE_DX; dx <= EH575_SWIPE_DX; dx++)
      {
        double score = eh575_swipe_correlation (swipe->frames[swipe->count - 1], frame, dx, dy);
        scores[dy + EH575_SWIPE_DY][dx + EH575_SWIPE_DX] = score;
        if (score > best)
          {
            best = score;
            best_x = dx;
            best_y = dy;
          }
      }
  /* Adjacent peak samples are one alignment basin, not separate evidence.
   * A competing distant peak (e.g. another ridge period) makes motion unsafe.
   */
  for (int dy = -EH575_SWIPE_DY; dy <= EH575_SWIPE_DY; dy++)
    for (int dx = -EH575_SWIPE_DX; dx <= EH575_SWIPE_DX; dx++)
      if (abs (dx - best_x) > 2 || abs (dy - best_y) > 2)
        second = fmax (second, scores[dy + EH575_SWIPE_DY][dx + EH575_SWIPE_DX]);
  swipe->best = best;
  swipe->margin = best - second;
  swipe->dx = best_x;
  swipe->dy = best_y;
  /* Discard plausible stationary/noisy frames BEFORE requiring unique
   * motion. Periodic ridges may have competing peaks even with no motion.
   * This never appends a frame, changes the anchor or grants coverage.
   * Keeping the anchor lets actual slow movement accumulate. Movement's
   * acceptance thresholds below remain unchanged.
   */
  if (best >= .90 && ((abs (best_x) <= 2 && abs (best_y) < 2) ||
                      (swipe->zero >= .90 && swipe->zero >= best - .025)))
    return EH575_SWIPE_STILL;
  if (best < .90 || best - second < .025 || abs (best_x) == EH575_SWIPE_DX ||
      abs (best_y) == EH575_SWIPE_DY)
    return EH575_SWIPE_UNCERTAIN;
  if (abs (best_y) < 2)
    return abs (best_x) <= 2 ? EH575_SWIPE_STILL : EH575_SWIPE_UNCERTAIN;
  if (swipe->direction && best_y * swipe->direction < 0)
    return EH575_SWIPE_REVERSED;
  x = swipe->x[swipe->count - 1] + best_x;
  y = swipe->y[swipe->count - 1] + best_y;
  min_x = x < swipe->min_x ? x : swipe->min_x;
  max_x = x > swipe->max_x ? x : swipe->max_x;
  min_y = y < swipe->min_y ? y : swipe->min_y;
  max_y = y > swipe->max_y ? y : swipe->max_y;
  if (EH575_WIDTH - (max_x - min_x) < EH575_SWIPE_MIN_WIDTH ||
      EH575_HEIGHT + max_y - min_y > EH575_SWIPE_MAX_HEIGHT)
    return EH575_SWIPE_LIMIT;
  swipe->direction = best_y > 0 ? 1 : -1;
  swipe->min_x = min_x;
  swipe->max_x = max_x;
  swipe->min_y = min_y;
  swipe->max_y = max_y;
  swipe->x[swipe->count] = x;
  swipe->y[swipe->count] = y;
  memcpy (swipe->frames[swipe->count++], frame, EH575_FRAME_SIZE);
  return EH575_SWIPE_MOVED;
}

static inline int
eh575_swipe_dimensions (const Eh575Swipe *swipe, size_t *width, size_t *height)
{
  *width = EH575_WIDTH - (swipe->max_x - swipe->min_x);
  *height = EH575_HEIGHT + swipe->max_y - swipe->min_y;
  return swipe->count >= 8 && *width >= EH575_SWIPE_MIN_WIDTH &&
         *height >= EH575_SWIPE_MIN_HEIGHT && *height <= EH575_SWIPE_MAX_HEIGHT;
}

/* Crop horizontally to the intersection of ALL frame footprints. Monotonic
 * vertical progress and >=65% pairwise overlap cover every remaining pixel.
 * Average only measured samples; fail rather than fill any unseen pixels.
 */
static inline int
eh575_swipe_assemble (const Eh575Swipe *swipe, uint8_t *out, size_t capacity)
{
  size_t width, height;

  if (!eh575_swipe_dimensions (swipe, &width, &height) || capacity < width * height)
    return 0;
  for (size_t y = 0; y < height; y++)
    for (size_t x = 0; x < width; x++)
      {
        unsigned int sum = 0, count = 0;
        for (unsigned int i = 0; i < swipe->count; i++)
          {
            int fx = (int) x + swipe->max_x - swipe->x[i];
            int fy = (int) y + swipe->min_y - swipe->y[i];
            if (fx >= 0 && fx < EH575_WIDTH && fy >= 0 && fy < EH575_HEIGHT)
              {
                sum += swipe->frames[i][fy * EH575_WIDTH + fx];
                count++;
              }
          }
        if (!count)
          return 0;
        out[y * width + x] = sum / count;
      }
  return 1;
}
