/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Private in-memory replay and synthetic testing. No USB or image output.
 */
#include "egis0575-ridge.h"
#include <stdio.h>
#include <string.h>

int
main (int argc, char **argv)
{
  uint8_t header[4];
  unsigned int count;
  g_autofree Eh575RidgeTouch *touches = NULL;
  g_autofree Eh575RidgeProbe *probe = g_new0 (Eh575RidgeProbe, 1);

  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  Eh575RidgeResult result;
  if (argc > 2 || (argc == 2 && strcmp (argv[1], "cancel")))
    return 2;
  if (fread (header, 1, 4, stdin) != 4)
    return 2;
  count = header[0] | ((unsigned int) header[1] << 8) | ((unsigned int) header[2] << 16) | ((unsigned int) header[3] << 24);
  if (count < 6 || count > EH575_RIDGE_MAX_TOUCHES)
    return 2;
  touches = g_new0 (Eh575RidgeTouch, count);
  if (fread (touches, sizeof *touches, count, stdin) != count ||
      fread (probe, sizeof *probe, 1, stdin) != 1 || fgetc (stdin) != EOF)
    {
      memset (touches, 0, sizeof *touches * count);
      memset (probe, 0, sizeof *probe);
      return 2;
    }
  if (argc == 2)
    g_cancellable_cancel (cancel);
  result = eh575_ridge_compare (touches, count, probe, cancel);
  printf ("{\"status\":%d,\"accepted\":%s,\"matched_frames\":%u,\"correlation\":%.6f,\"margin\":%.6f}\n",
          result.status, result.status == EH575_RIDGE_MATCH ? "true" : "false",
          result.matched_frames, result.correlation, result.margin);
  memset (touches, 0, sizeof *touches * count);
  memset (probe, 0, sizeof *probe);
  return 0;
}
