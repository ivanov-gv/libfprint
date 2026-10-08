/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Synthetic coordinate textures only. No biometric images or reader needed.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "egis0575-swipe.h"

static uint8_t
texture (int x, int y)
{
  uint32_t value = (uint32_t) x * 0x9e3779b1U ^ (uint32_t) y * 0x85ebca6bU;

  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  return 40 + value % 160;
}

static void
make_frame (uint8_t *frame, int x, int y, int brightness)
{
  for (int row = 0; row < EH575_HEIGHT; row++)
    for (int col = 0; col < EH575_WIDTH; col++)
      frame[row * EH575_WIDTH + col] = texture (x + col, y + row) + brightness;
}

static void
test_translation (int direction)
{
  Eh575Swipe *swipe = calloc (1, sizeof *swipe);
  uint8_t frame[EH575_FRAME_SIZE], output[EH575_WIDTH * EH575_SWIPE_MAX_HEIGHT];
  size_t width, height;

  assert (swipe);
  for (int i = 0; i < 15; i++)
    {
      make_frame (frame, i % 3, i * 8 * direction, 0);
      assert (eh575_swipe_push (swipe, frame) == (i ? EH575_SWIPE_MOVED : EH575_SWIPE_FIRST));
    }
  assert (eh575_swipe_dimensions (swipe, &width, &height));
  assert (width == EH575_WIDTH - 2 && height == EH575_HEIGHT + 112);
  assert (!eh575_swipe_assemble (swipe, output, width * height - 1));
  assert (eh575_swipe_assemble (swipe, output, sizeof output));
  for (size_t y = 0; y < height; y++)
    for (size_t x = 0; x < width; x++)
      assert (output[y * width + x] == texture (x + swipe->max_x, y + swipe->min_y));
  make_frame (frame, 2, 104 * direction, 0);
  assert (eh575_swipe_push (swipe, frame) == EH575_SWIPE_REVERSED);
  assert (swipe->count == 15);
  free (swipe);
}

int
main (void)
{
  Eh575Swipe *swipe = calloc (1, sizeof *swipe);
  uint8_t frame[EH575_FRAME_SIZE], output[EH575_WIDTH * EH575_SWIPE_MAX_HEIGHT];
  size_t width, height;

  assert (swipe);
  test_translation (1);
  test_translation (-1);
  memset (frame, 128, sizeof frame);
  assert (eh575_swipe_push (swipe, frame) == EH575_SWIPE_UNCERTAIN);
  assert (!swipe->count);
  make_frame (frame, 0, 0, 0);
  assert (eh575_swipe_push (swipe, frame) == EH575_SWIPE_FIRST);
  for (int i = 0; i < 200; i++)
    assert (eh575_swipe_push (swipe, frame) == EH575_SWIPE_STILL);
  assert (swipe->count == 1);
  assert (!eh575_swipe_dimensions (swipe, &width, &height));
  make_frame (frame, 0, 8, 9);
  assert (eh575_swipe_push (swipe, frame) == EH575_SWIPE_MOVED); /* brightness independent */
  make_frame (frame, 0, 40, 0);
  assert (eh575_swipe_push (swipe, frame) == EH575_SWIPE_UNCERTAIN); /* too fast */
  assert (swipe->count == 2);
  memset (swipe, 0, sizeof *swipe);
  for (int i = 0; i < 7; i++)
    {
      make_frame (frame, i * 6, i * 8, 0);
      Eh575SwipeResult result = eh575_swipe_push (swipe, frame);
      assert (result == (i == 0 ? EH575_SWIPE_FIRST : (i == 6 ? EH575_SWIPE_LIMIT : EH575_SWIPE_MOVED)));
    }
  memset (swipe, 0, sizeof *swipe);
  for (int i = 0; i < 40; i++)
    {
      make_frame (frame, 0, i * 12, 0);
      Eh575SwipeResult result = eh575_swipe_push (swipe, frame);
      assert (result == (i == 0 ? EH575_SWIPE_FIRST : (i == 39 ? EH575_SWIPE_LIMIT : EH575_SWIPE_MOVED)));
    }
  memset (swipe, 0, sizeof *swipe);
  for (int i = 0; i <= EH575_SWIPE_MAX_FRAMES; i++)
    {
      make_frame (frame, 0, i * 2, 0);
      Eh575SwipeResult result = eh575_swipe_push (swipe, frame);
      assert (result == (i == 0 ? EH575_SWIPE_FIRST : (i == EH575_SWIPE_MAX_FRAMES ? EH575_SWIPE_LIMIT : EH575_SWIPE_MOVED)));
    }
  memset (swipe, 0, sizeof *swipe);
  for (int y = 0; y < EH575_HEIGHT; y++)
    for (int x = 0; x < EH575_WIDTH; x++)
      frame[y * EH575_WIDTH + x] = (y % 5) * 40 + 20;
  assert (eh575_swipe_push (swipe, frame) == EH575_SWIPE_FIRST);
  for (int y = 0; y < EH575_HEIGHT; y++)
    for (int x = 0; x < EH575_WIDTH; x++)
      frame[y * EH575_WIDTH + x] = ((y + 2) % 5) * 40 + 20;
  assert (eh575_swipe_push (swipe, frame) == EH575_SWIPE_UNCERTAIN); /* periodic alias */
  assert (swipe->count == 1);
  /* A corrupt/gapped geometry must never be completed by padding. */
  swipe->count = 8;
  swipe->max_y = 200;
  for (unsigned int i = 1; i < swipe->count; i++)
    swipe->y[i] = 200;
  assert (!eh575_swipe_assemble (swipe, output, sizeof output));
  free (swipe);
  puts ("Synthetic swipe translation/crop/ambiguity/bounds checks passed");
  return 0;
}
