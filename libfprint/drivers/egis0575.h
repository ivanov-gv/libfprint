/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 fingerprint contributors
 * Register sequences derived from SandroRoque/python-egistec-eh575,
 * commit 38de042e0103607c83f7ec4b0497504449e7bd5e (MIT).
 * The accompanying egis0575-reference-MIT.txt notice must be retained.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define EH575_WIDTH 103
#define EH575_HEIGHT 52
#define EH575_FRAME_SIZE (EH575_WIDTH * EH575_HEIGHT)
#define EH575_EP_OUT 0x01
#define EH575_EP_IN 0x82
#define EH575_REVISION 0x1072

typedef struct
{
  size_t  length;
  uint8_t data[20];
} Eh575Command;

#define CMD(...) { sizeof ((uint8_t[]){ __VA_ARGS__ }) + 4, { 'E', 'G', 'I', 'S', __VA_ARGS__ } }
static const Eh575Command eh575_init_commands[] = {
  CMD (0x60, 0x00, 0x06), CMD (0x60, 0x01, 0x06), CMD (0x60, 0x40, 0x06),
  CMD (0x61, 0x0a, 0xf4), CMD (0x61, 0x0c, 0x44), CMD (0x61, 0x40, 0x00),
  CMD (0x60, 0x40, 0x00), CMD (0x71, 0x02, 0x02, 0x01, 0x0c),
  CMD (0x61, 0x0c, 0x22), CMD (0x61, 0x0b, 0x03), CMD (0x61, 0x0a, 0xfc),
  CMD (0x60, 0x00, 0xfc), CMD (0x60, 0x01, 0xfc), CMD (0x60, 0x41, 0xfc),
  CMD (0x97, 0x00, 0x00), CMD (0x60, 0x00, 0x00), CMD (0x60, 0x00, 0x00),
  CMD (0x60, 0x00, 0x00), CMD (0x60, 0x00, 0x00), CMD (0x60, 0x00, 0x00),
  CMD (0x60, 0x01, 0x00), CMD (0x61, 0x0a, 0xfd), CMD (0x61, 0x35, 0x02),
  CMD (0x61, 0x80, 0x00), CMD (0x60, 0x80, 0x00), CMD (0x61, 0x0a, 0xfc),
  CMD (0x63, 0x01, 0x02, 0x0f, 0x03), CMD (0x61, 0x0c, 0x22),
  CMD (0x61, 0x09, 0x83), CMD (0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06),
  CMD (0x61, 0x0a, 0xf4), CMD (0x61, 0x0c, 0x44), CMD (0x61, 0x50, 0x03),
  CMD (0x60, 0x50, 0x03), CMD (0x60, 0x40, 0xec), CMD (0x61, 0x0c, 0x22),
  CMD (0x61, 0x0b, 0x03), CMD (0x61, 0x0a, 0xfc), CMD (0x60, 0x40, 0xfc),
  CMD (0x63, 0x09, 0x0b, 0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12),
  CMD (0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06),
  CMD (0x61, 0x23, 0x00), CMD (0x61, 0x24, 0x33), CMD (0x61, 0x20, 0x00),
  CMD (0x61, 0x21, 0x66), CMD (0x60, 0x00, 0x66), CMD (0x60, 0x01, 0x66),
};
static const Eh575Command eh575_rearm_commands[] = {
  CMD (0x61, 0x2d, 0x20), CMD (0x60, 0x00, 0x20), CMD (0x60, 0x01, 0x20),
  CMD (0x63, 0x2c, 0x02, 0x00, 0x57), CMD (0x60, 0x2d, 0x02),
  CMD (0x62, 0x67, 0x03), CMD (0x63, 0x2c, 0x02, 0x00, 0x13), CMD (0x60, 0x00, 0x02),
};
static const Eh575Command eh575_trigger = CMD (0x64, 0x14, 0xec);
#undef CMD

static inline int
eh575_status_valid (const uint8_t *reply, size_t length)
{
  return length >= 7 && length <= 64 &&
         (!memcmp (reply, "SIGE", 4) || !memcmp (reply, "EGIS", 4)) && reply[6] == 1;
}

static inline int
eh575_reply_valid (const Eh575Command *cmd, const uint8_t *reply, size_t length)
{
  uint8_t op = cmd->data[4];
  int counted = op == 0x62 || op == 0x63 || op == 0x71;

  return eh575_status_valid (reply, length) && length == (size_t) (7 + (counted ? cmd->data[6] : 0)) &&
         reply[4] == cmd->data[5] &&
         (!(counted || op == 0x61) || reply[5] == cmd->data[6]);
}

/* Reject status packets, overflow and zero-length fragments before copying. */
static inline int
eh575_append_frame (uint8_t *frame, size_t *used, const uint8_t *part, size_t length)
{
  if (*used > EH575_FRAME_SIZE || !length || length > EH575_FRAME_SIZE - *used ||
      (length == 7 && (!memcmp (part, "SIGE", 4) || !memcmp (part, "EGIS", 4))))
    return 0;
  memcpy (frame + *used, part, length);
  *used += length;
  return 1;
}

static inline double
eh575_mean (const uint8_t *image)
{
  double sum = 0;

  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    sum += image[i];
  return sum / EH575_FRAME_SIZE;
}

static inline double
eh575_deviation (const uint8_t *image, const uint8_t *background)
{
  double sum = 0, squares = 0;

  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    {
      double value = image[i] - (background ? background[i] : 0);
      sum += value;
      squares += value * value;
    }
  return sqrt (fmax (0, squares / EH575_FRAME_SIZE - pow (sum / EH575_FRAME_SIZE, 2)));
}

static inline double
eh575_clipped (const uint8_t *image)
{
  size_t count = 0;

  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    count += image[i] <= 2 || image[i] >= 253;
  return (double) count / EH575_FRAME_SIZE;
}

static inline void
eh575_median (uint8_t *out, uint8_t frames[3][EH575_FRAME_SIZE])
{
  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    {
      uint8_t a = frames[0][i], b = frames[1][i], c = frames[2][i];
      out[i] = a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
    }
}

/* Background removal and fixed contrast gain, never a matching decision. */
static inline void
eh575_normalize (uint8_t *out, const uint8_t *frame, const uint8_t *background)
{
  double offset = eh575_mean (frame) - eh575_mean (background);

  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    out[i] = (uint8_t) fmax (0, fmin (255, 128 + 2 * (frame[i] - background[i] - offset)));
}

/* The raw 103-byte row stride is not compatible with libfprint's pixman
 * resizing helper. Use bounded bilinear 2x interpolation without padding or
 * inventing additional sensor area. Border samples replicate the final pixel.
 */
static inline void
eh575_enlarge (uint8_t *out, const uint8_t *raw)
{
  for (size_t y = 0; y < EH575_HEIGHT * 2; y++)
    for (size_t x = 0; x < EH575_WIDTH * 2; x++)
      {
        size_t x0 = x / 2, y0 = y / 2;
        size_t x1 = x0 + (x0 + 1 < EH575_WIDTH), y1 = y0 + (y0 + 1 < EH575_HEIGHT);
        unsigned int a = raw[y0 * EH575_WIDTH + x0], b = raw[y0 * EH575_WIDTH + x1];
        unsigned int c = raw[y1 * EH575_WIDTH + x0], d = raw[y1 * EH575_WIDTH + x1];
        out[y * EH575_WIDTH * 2 + x] = ((2 - x % 2) * (2 - y % 2) * a +
                                        (x % 2) * (2 - y % 2) * b +
                                        (2 - x % 2) * (y % 2) * c +
                                        (x % 2) * (y % 2) * d) / 4;
      }
}
