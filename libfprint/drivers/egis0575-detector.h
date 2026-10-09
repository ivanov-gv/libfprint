/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Experimental, volatile detector protocol facts, not vendor implementation.
 * Cross-check: pinned Acer 3.7.1.1 binary and published 575-[0-3] USB traces.
 * Only for isolated revision-1072 probes; not used by authentication.
 */
#pragma once
#include "egis0575.h"

typedef int (*Eh575DetectorIO) (void *context, const Eh575Command *cmd, uint8_t *reply, size_t *length);
typedef int (*Eh575DetectorCapture) (void *context, uint8_t *frame);

typedef struct
{
  unsigned int dc, attempts;
  double mean, texture, clipped;
} Eh575EmptyCheck;

typedef struct
{
  uint8_t reference, dc_p, dc_c, mean, threshold;
  int ready;
} Eh575Detector;

/* A contact indication may be handed to the UI only AFTER successful capture
 * recovery and USB release/close. This is never an authentication result.
 */
static inline int
eh575_detector_contact_ready (int seen, int restored, int released, int closed, int cancelled)
{
  return seen && restored && released && closed && !cancelled;
}

/* CLOCK_BOOTTIME includes host sleep; CLOCK_MONOTONIC does not. Units must match.
 * A positive difference is sleep evidence, never identification of its wake source.
 */
static inline uint64_t
eh575_detector_sleep_elapsed (uint64_t start_boot, uint64_t start_mono, uint64_t boot, uint64_t mono)
{
  if (boot < start_boot || mono < start_mono)
    return 0;
  uint64_t b = boot - start_boot, m = mono - start_mono;
  return b > m ? b - m : 0;
}

static inline int
eh575_detector_io (Eh575DetectorIO io, void *context, const Eh575Command *cmd, uint8_t *reply)
{
  size_t length = 64;
  memset (reply, 0, 64);
  return io (context, cmd, reply, &length) && eh575_reply_valid (cmd, reply, length);
}

static inline int
eh575_detector_write (Eh575DetectorIO io, void *context, uint8_t reg, uint8_t value)
{
  Eh575Command cmd = {7, {'E', 'G', 'I', 'S', 0x61, reg, value}};
  uint8_t reply[64];
  return eh575_detector_io (io, context, &cmd, reply);
}

/* Only 0 (empty) and 4 (latched contact) were characterized on this unit.
 * Fail closed on an unknown status or malformed/failed exchange.
 */
static inline int
eh575_detector_contact_status (Eh575DetectorIO io, void *context)
{
  const Eh575Command cmd = {7, {'E', 'G', 'I', 'S', 0x60, 1, 0}};
  uint8_t reply[64];
  if (!eh575_detector_io (io, context, &cmd, reply) || (reply[5] != 0 && reply[5] != 4))
    return -1;
  return reply[5] == 4;
}

/* Match the installed capture calibration's characterized DC range and final
 * quality gates. Discard one settling frame after each setting, then require
 * three low-texture frames. Never calibrate detector parameters from a finger.
 * A transport/capture failure or a textured frame aborts, without further writes.
 */
static inline int
eh575_detector_empty (Eh575EmptyCheck *check, Eh575DetectorIO io, Eh575DetectorCapture capture, void *context)
{
  const Eh575Command read_dc = {7, {'E', 'G', 'I', 'S', 0x60, 0x0f, 0}};
  uint8_t frame[EH575_FRAME_SIZE], reply[64];
  int low = 0, high = 63, ok = 0;
  memset (check, 0, sizeof *check);
  if (!eh575_detector_io (io, context, &read_dc, reply) || reply[5] > 63)
    goto out;
  check->dc = reply[5];
  for (check->attempts = 1; check->attempts <= 7; check->attempts++)
    {
      if (!capture (context, frame)) /* settling */
        goto out;
      check->mean = check->texture = check->clipped = 0;
      int usable = 1;
      for (unsigned int n = 0; n < 3; n++)
        {
          if (!capture (context, frame))
            goto out;
          double texture = eh575_deviation (frame, NULL);
          if (texture > 18)
            {
              check->mean = eh575_mean (frame);
              check->texture = texture;
              check->clipped = eh575_clipped (frame);
              goto out;
            }
          double mean = eh575_mean (frame), clipped = eh575_clipped (frame);
          usable &= mean >= 96 && mean <= 160 && clipped <= .005;
          check->mean += mean / 3;
          check->texture = fmax (check->texture, texture);
          check->clipped += clipped / 3;
        }
      if (usable)
        {
          ok = 1;
          goto out;
        }
      if (check->mean < 128)
        low = (int) check->dc + 1;
      else
        high = (int) check->dc - 1;
      if (low > high || check->attempts == 7)
        goto out;
      check->dc = (low + high) / 2;
      if (!eh575_detector_write (io, context, 0x0f, check->dc))
        goto out;
    }
out:
  memset (frame, 0, sizeof frame);
  memset (reply, 0, sizeof reply);
  return ok;
}

/* A bounded status wait. The USB backend also enforces a wall-clock deadline. */
static inline int
eh575_detector_wait (Eh575DetectorIO io, void *context, uint8_t reg)
{
  Eh575Command cmd = {7, {'E', 'G', 'I', 'S', 0x60, reg, 0}};
  uint8_t reply[64];
  for (unsigned int n = 0; n < 64; n++)
    {
      if (!eh575_detector_io (io, context, &cmd, reply))
        return 0;
      if (!(reply[5] & 0x80))
        return 1;
    }
  return 0;
}

static inline int
eh575_detector_calibrate (Eh575Detector *detector, Eh575DetectorIO io, void *context)
{
  const Eh575Command setup[] = {
    {10, {'E', 'G', 'I', 'S', 0x63, 0x11, 3, 1, 0, 0x72}},
    {9, {'E', 'G', 'I', 'S', 0x63, 0x34, 2, 7, 1}},
  };
  const Eh575Command dc = {7, {'E', 'G', 'I', 'S', 0x62, 0x0d, 3}};
  const Eh575Command measure = {9, {'E', 'G', 'I', 'S', 0x63, 0x2c, 2, 0, 0x57}};
  const Eh575Command stats = {7, {'E', 'G', 'I', 'S', 0x62, 0x67, 3}};
  uint8_t reply[64];

  memset (detector, 0, sizeof *detector);
  for (size_t n = 0; n < sizeof setup / sizeof setup[0]; n++)
    if (!eh575_detector_io (io, context, &setup[n], reply))
      return 0;
  if (!eh575_detector_wait (io, context, 0x35) || !eh575_detector_io (io, context, &dc, reply))
    return 0;
  detector->reference = reply[7];
  detector->dc_p = reply[8];
  detector->dc_c = reply[9];
  if (!detector->dc_c || !eh575_detector_write (io, context, 0x12, 0x0a))
    return 0;
  /* Refuse zero/wrap instead of emulating the OEM's byte underflow behavior. */
  while (detector->dc_c)
    {
      if (!eh575_detector_write (io, context, 0x0f, detector->dc_c) ||
          !eh575_detector_io (io, context, &measure, reply) ||
          !eh575_detector_wait (io, context, 0x2d) ||
          !eh575_detector_io (io, context, &stats, reply))
        return 0;
      detector->mean = reply[9];
      if (reply[7] > detector->mean || detector->mean > reply[8])
        return 0;
      if (detector->mean < 128)
        {
          /* 80 is the margin byte at OEM VA 0x18004a7b0; max result 207. */
          detector->threshold = detector->mean + 80;
          detector->ready = 1;
          return 1;
        }
      detector->dc_c--;
    }
  return 0;
}

static inline int
eh575_detector_enter (const Eh575Detector *detector, Eh575DetectorIO io, void *context)
{
  Eh575Command commands[] = {
    {13, {'E', 'G', 'I', 'S', 0x63, 0x26, 6, 6, 0x60, 6, 5, 0x2f, 6}},
    {18, {'E', 'G', 'I', 'S', 0x63, 9, 11, 0x83, 0xf4, 3, 0x44,
          detector->reference, detector->dc_p, detector->dc_c, 0x20, 1, 0x0a, 0x72}},
    {9, {'E', 'G', 'I', 'S', 0x63, 1, 2, 0x0c, 3}},
    {7, {'E', 'G', 'I', 'S', 0x61, 0x0c, 0x22}},
    {7, {'E', 'G', 'I', 'S', 0x61, 0x0b, 3}},
    {7, {'E', 'G', 'I', 'S', 0x61, 0x0a, 0xfc}},
    {13, {'E', 'G', 'I', 'S', 0x71, 0x45, 6, 0, detector->threshold, 0x87, 0x13, 0, 3}},
  };
  uint8_t reply[64];
  if (!detector->ready || !detector->dc_c || detector->mean >= 128 ||
      detector->threshold != (unsigned int) detector->mean + 80)
    return 0;
  for (size_t n = 0; n < sizeof commands / sizeof commands[0]; n++)
    if (!eh575_detector_io (io, context, &commands[n], reply))
      return 0;
  return 1;
}

static inline int
eh575_detector_restore (Eh575DetectorIO io, void *context)
{
  const Eh575Command exit = {9, {'E', 'G', 'I', 'S', 0x71, 2, 2, 1, 0x0c}};
  uint8_t reply[64];
  if (!eh575_detector_write (io, context, 0x0a, 0xf4) ||
      !eh575_detector_write (io, context, 0x0c, 0x44) ||
      !eh575_detector_write (io, context, 0x40, 0) ||
      !eh575_detector_wait (io, context, 0x40) ||
      !eh575_detector_io (io, context, &exit, reply))
    return 0;
  for (size_t n = 0; n < sizeof eh575_init_commands / sizeof eh575_init_commands[0]; n++)
    if (!eh575_detector_io (io, context, &eh575_init_commands[n], reply))
      return 0;
  return 1;
}
