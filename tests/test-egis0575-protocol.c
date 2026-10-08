/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include "egis0575.h"

int
main (void)
{
  uint8_t frame[EH575_FRAME_SIZE], part[8192], reply[64], refs[3][EH575_FRAME_SIZE];
  uint8_t enlarged[EH575_FRAME_SIZE * 4];
  size_t used = 0;

  assert (sizeof eh575_init_commands / sizeof *eh575_init_commands == 47);
  for (size_t i = 0; i < 47; i++)
    {
      const Eh575Command *cmd = &eh575_init_commands[i];
      uint8_t op = cmd->data[4];
      size_t length = 7 + ((op == 0x62 || op == 0x63 || op == 0x71) ? cmd->data[6] : 0);
      memset (reply, 0, sizeof reply);
      memcpy (reply, "SIGE", 4);
      reply[4] = cmd->data[5];
      reply[5] = cmd->data[6];
      reply[6] = 1;
      assert (eh575_reply_valid (cmd, reply, length));
      assert (!eh575_reply_valid (cmd, reply, length - 1));
      reply[6] = 0;
      assert (!eh575_reply_valid (cmd, reply, length));
      reply[6] = 1;
      reply[4] ^= 1;
      assert (!eh575_reply_valid (cmd, reply, length));
    }
  memset (part, 128, sizeof part);
  assert (eh575_append_frame (frame, &used, part, 5120));
  assert (eh575_append_frame (frame, &used, part, 236));
  assert (used == EH575_FRAME_SIZE);
  assert (!eh575_append_frame (frame, &used, part, 1));
  used = 0;
  assert (!eh575_append_frame (frame, &used, part, 0));
  assert (!eh575_append_frame (frame, &used, part, sizeof part));
  assert (!eh575_append_frame (frame, &used, (const uint8_t *) "SIGE\x00\x00\x01", 7));
  assert (eh575_append_frame (frame, &used, part, EH575_FRAME_SIZE));
  assert (eh575_mean (frame) == 128);
  assert (eh575_deviation (frame, NULL) == 0);
  assert (eh575_clipped (frame) == 0);
  memset (refs[0], 100, EH575_FRAME_SIZE);
  memset (refs[1], 128, EH575_FRAME_SIZE);
  memset (refs[2], 150, EH575_FRAME_SIZE);
  eh575_median (frame, refs);
  assert (eh575_mean (frame) == 128);
  assert (eh575_deviation (frame, refs[0]) == 0); /* brightness drift is not contact */
  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    frame[i] = i % 2 ? 96 : 160;
  assert (eh575_deviation (frame, refs[1]) > 31);
  assert (eh575_stationary_correlation (frame, frame, refs[1]) > .999);
  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    refs[0][i] = frame[i] + 9 + (int) (i % 5) - 2;
  assert (eh575_stationary_correlation (frame, refs[0], refs[1]) >= .97);
  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    refs[0][i] = frame[(i + 1) % EH575_FRAME_SIZE];
  assert (eh575_stationary_correlation (frame, refs[0], refs[1]) < .97);
  assert (eh575_stationary_correlation (refs[1], refs[1], refs[1]) == -1);
  eh575_normalize (part, frame, refs[1]);
  assert (part[0] >= 191 && part[1] <= 65);
  memset (frame, 0, sizeof frame);
  assert (eh575_clipped (frame) == 1);
  memset (frame, 128, sizeof frame);
  eh575_enlarge (enlarged, frame);
  for (size_t i = 0; i < sizeof enlarged; i++)
    assert (enlarged[i] == 128);
  frame[0] = 0;
  frame[1] = 200;
  frame[EH575_WIDTH] = 100;
  frame[EH575_WIDTH + 1] = 100;
  eh575_enlarge (enlarged, frame);
  assert (enlarged[0] == 0 && enlarged[1] == 100 && enlarged[2] == 200);
  assert (enlarged[EH575_WIDTH * 2 + 1] == 100);
  assert (enlarged[sizeof enlarged - 1] == frame[EH575_FRAME_SIZE - 1]);
  puts ("EH575 native protocol/image tests passed");
  return 0;
}
