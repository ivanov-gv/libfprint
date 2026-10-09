/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Original synthetic register replies. No USB or vendor/biometric fixtures.
 */
#include <glib.h>
#include "egis0575-detector.h"

typedef struct
{
  Eh575Command commands[4096];
  unsigned int used, fail_at, malformed_at, polls;
  uint8_t dc, mean;
  int busy, invalid_stats;
} Fake;

static int
exchange (void *context, const Eh575Command *cmd, uint8_t *reply, size_t *length)
{
  Fake *fake = context;
  uint8_t op = cmd->data[4], reg = cmd->data[5];
  int counted = op == 0x62 || op == 0x63 || op == 0x71;
  g_assert_cmpuint (fake->used, <, G_N_ELEMENTS (fake->commands));
  fake->commands[fake->used++] = *cmd;
  if (fake->used == fake->fail_at)
    return 0;
  *length = 7 + (counted ? cmd->data[6] : 0);
  memcpy (reply, "SIGE", 4);
  reply[4] = reg;
  reply[5] = cmd->data[6];
  reply[6] = fake->used == fake->malformed_at ? 0 : 1;
  if (op == 0x60)
    {
      fake->polls++;
      reply[5] = fake->busy ? 0x80 : 0;
    }
  if (op == 0x62 && reg == 0x0d)
    {
      reply[7] = 3;
      reply[8] = 0x0c;
      reply[9] = fake->dc;
    }
  if (op == 0x62 && reg == 0x67)
    {
      reply[7] = fake->invalid_stats ? 200 : 0;
      reply[8] = 255;
      reply[9] = fake->mean;
    }
  return 1;
}

static void
trace_crosscheck (void)
{
  Fake fake = {.dc = 0x17, .mean = 92};
  Eh575Detector detector;
  g_assert_true (eh575_detector_calibrate (&detector, exchange, &fake));
  g_assert_true (detector.ready);
  g_assert_cmpuint (detector.threshold, ==, 0xac);
  fake.used = 0;
  g_assert_true (eh575_detector_enter (&detector, exchange, &fake));
  g_assert_cmpuint (fake.used, ==, 7);
  const uint8_t analogue[] = {'E', 'G', 'I', 'S', 0x63, 9, 11, 0x83, 0xf4, 3, 0x44, 3, 0x0c, 0x17, 0x20, 1, 0x0a, 0x72};
  const uint8_t bank[] = {'E', 'G', 'I', 'S', 0x71, 0x45, 6, 0, 0xac, 0x87, 0x13, 0, 3};
  g_assert_cmpmem (fake.commands[1].data, fake.commands[1].length, analogue, sizeof analogue);
  g_assert_cmpmem (fake.commands[6].data, fake.commands[6].length, bank, sizeof bank);
}

static void
bounds (void)
{
  Eh575Detector detector;
  Fake fake = {.dc = 0xff, .mean = 128};
  g_assert_false (eh575_detector_calibrate (&detector, exchange, &fake));
  g_assert_false (detector.ready);
  g_assert_cmpuint (fake.used, <, 1100);
  for (guint n = 0; n < fake.used; n++)
    if (fake.commands[n].data[4] == 0x61 && fake.commands[n].data[5] == 0x0f)
      g_assert_cmpuint (fake.commands[n].data[6], >, 0);
  fake = (Fake){.dc = 0, .mean = 92};
  g_assert_false (eh575_detector_calibrate (&detector, exchange, &fake));
  fake = (Fake){.dc = 23, .mean = 92, .invalid_stats = 1};
  g_assert_false (eh575_detector_calibrate (&detector, exchange, &fake));
  fake = (Fake){.dc = 23, .mean = 127};
  g_assert_true (eh575_detector_calibrate (&detector, exchange, &fake));
  g_assert_cmpuint (detector.threshold, ==, 207);
  detector.threshold = 0;
  fake.used = 0;
  g_assert_false (eh575_detector_enter (&detector, exchange, &fake));
  g_assert_cmpuint (fake.used, ==, 0);
}

static void
busy_bounded (void)
{
  Fake fake = {.dc = 23, .mean = 92, .busy = 1};
  Eh575Detector detector;
  g_assert_false (eh575_detector_calibrate (&detector, exchange, &fake));
  g_assert_cmpuint (fake.polls, ==, 64);
  g_assert_false (detector.ready);
}

static void
failures (void)
{
  Fake baseline = {.dc = 23, .mean = 92};
  Eh575Detector valid;
  g_assert_true (eh575_detector_calibrate (&valid, exchange, &baseline));
  guint calibration_commands = baseline.used;
  for (guint bad = 1; bad <= calibration_commands; bad++)
    for (guint malformed = 0; malformed < 2; malformed++)
      {
        Fake fake = {.dc = 23, .mean = 92, .fail_at = malformed ? 0 : bad, .malformed_at = malformed ? bad : 0};
        Eh575Detector detector;
        g_assert_false (eh575_detector_calibrate (&detector, exchange, &fake));
        g_assert_false (detector.ready);
        g_assert_cmpuint (fake.used, ==, bad);
        g_assert_false (eh575_detector_enter (&detector, exchange, &fake));
        g_assert_cmpuint (fake.used, ==, bad);
        fake.fail_at = fake.malformed_at = 0;
        guint before = fake.used;
        g_assert_true (eh575_detector_restore (exchange, &fake));
        g_assert_cmpuint (fake.used - before, ==, 5 + G_N_ELEMENTS (eh575_init_commands));
      }
  for (guint bad = 1; bad <= 7; bad++)
    {
      Fake fake = {.dc = 23, .mean = 92, .fail_at = bad};
      g_assert_false (eh575_detector_enter (&valid, exchange, &fake));
      g_assert_cmpuint (fake.used, ==, bad);
      fake.fail_at = 0;
      guint before = fake.used;
      g_assert_true (eh575_detector_restore (exchange, &fake));
      g_assert_cmpuint (fake.used - before, ==, 5 + G_N_ELEMENTS (eh575_init_commands));
    }
}

static void
restore_failures (void)
{
  guint count = 5 + G_N_ELEMENTS (eh575_init_commands);
  for (guint bad = 1; bad <= count; bad++)
    {
      Fake fake = {.fail_at = bad};
      g_assert_false (eh575_detector_restore (exchange, &fake));
      g_assert_cmpuint (fake.used, ==, bad);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/eh575/detector/trace-crosscheck", trace_crosscheck);
  g_test_add_func ("/eh575/detector/bounds", bounds);
  g_test_add_func ("/eh575/detector/busy-bounded", busy_bounded);
  g_test_add_func ("/eh575/detector/failures", failures);
  g_test_add_func ("/eh575/detector/restore-failures", restore_failures);
  return g_test_run ();
}
