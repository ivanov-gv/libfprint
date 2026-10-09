/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Original synthetic register replies. No USB or vendor/biometric fixtures.
 */
#include <glib.h>
#include "egis0575-detector.h"

typedef struct
{
  Eh575Command commands[4096];
  unsigned int used, fail_at, malformed_at, polls;
  uint8_t dc, mean, status;
  int busy, invalid_stats;
  int image_dc, target_dc, fail_capture, textured, stale_first, impossible, unstable;
  unsigned int images;
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
      if (reg == 0x0f)
        reply[5] = fake->image_dc;
      if (reg == 1)
        reply[5] = fake->status;
    }
  if (op == 0x61 && reg == 0x0f)
    fake->image_dc = cmd->data[6];
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
  Fake busy = {.busy = 1};
  g_assert_false (eh575_detector_restore (exchange, &busy));
  g_assert_cmpuint (busy.polls, ==, 64);
  g_assert_cmpuint (busy.used, ==, 67);
  g_assert_cmpuint (busy.commands[busy.used - 1].data[4], ==, 0x60);
  g_assert_cmpuint (busy.commands[busy.used - 1].data[5], ==, 0x40);
}

static void
sleep_evidence (void)
{
  g_assert_cmpuint (eh575_detector_sleep_elapsed (100, 70, 1100, 1070), ==, 0);
  g_assert_cmpuint (eh575_detector_sleep_elapsed (100, 70, 6100, 1070), ==, 5000);
  g_assert_cmpuint (eh575_detector_sleep_elapsed (100, 70, 99, 1070), ==, 0);
  g_assert_cmpuint (eh575_detector_sleep_elapsed (100, 70, 6100, 69), ==, 0);
  g_assert_cmpuint (eh575_detector_sleep_elapsed (100, 70, 1100, 1071), ==, 0);
  g_assert_cmpuint (eh575_detector_sleep_elapsed (0, 0, UINT64_MAX, UINT64_MAX), ==, 0);
}

static int
image (void *context, uint8_t *frame)
{
  Fake *fake = context;
  fake->images++;
  if ((int) fake->images == fake->fail_capture)
    return 0;
  int level = fake->impossible ? 40 : CLAMP (128 + 4 * (fake->image_dc - fake->target_dc), 5, 250);
  if (fake->unstable)
    level = fake->images % 4 == 2 ? 88 : fake->images % 4 == 3 ? 128 : 168;
  int contrast = fake->textured || (fake->stale_first && fake->images == 1) ? 40 : 5;
  for (guint n = 0; n < EH575_FRAME_SIZE; n++)
    frame[n] = CLAMP (level + (n % 2 ? contrast : -contrast), 0, 255);
  return 1;
}

static void
empty_exposure (void)
{
  Eh575EmptyCheck check;
  Fake fake = {.image_dc = 32, .target_dc = 32, .stale_first = 1};
  g_assert_true (eh575_detector_empty (&check, exchange, image, &fake));
  g_assert_cmpuint (fake.images, ==, 4);
  g_assert_cmpuint (fake.used, ==, 1); /* No setting changes if already usable. */
  for (int target = 0; target <= 63; target++)
    {
      fake = (Fake){.image_dc = 32, .target_dc = target};
      g_assert_true (eh575_detector_empty (&check, exchange, image, &fake));
      g_assert_cmpuint (check.attempts, <=, 7);
      g_assert_cmpuint (check.dc, <=, 63);
      g_assert_cmpfloat (check.mean, >=, 96);
      g_assert_cmpfloat (check.mean, <=, 160);
    }
  fake = (Fake){.image_dc = 32, .target_dc = 32, .textured = 1};
  g_assert_false (eh575_detector_empty (&check, exchange, image, &fake));
  g_assert_cmpuint (fake.used, ==, 1); /* Never change DC on contact texture. */
  fake = (Fake){.image_dc = 32, .impossible = 1};
  g_assert_false (eh575_detector_empty (&check, exchange, image, &fake));
  g_assert_cmpuint (fake.images, <=, 28);
  fake = (Fake){.image_dc = 32, .unstable = 1};
  g_assert_false (eh575_detector_empty (&check, exchange, image, &fake));
  g_assert_cmpuint (fake.images, <=, 28); /* An average cannot hide bad frames. */
  fake = (Fake){.image_dc = 32, .target_dc = 60};
  g_assert_true (eh575_detector_empty (&check, exchange, image, &fake));
  guint captures = fake.images, commands = fake.used;
  for (guint bad = 1; bad <= captures; bad++)
    {
      fake = (Fake){.image_dc = 32, .target_dc = 60, .fail_capture = bad};
      g_assert_false (eh575_detector_empty (&check, exchange, image, &fake));
      g_assert_cmpuint (fake.images, ==, bad);
    }
  fake = (Fake){.image_dc = 64};
  g_assert_false (eh575_detector_empty (&check, exchange, image, &fake));
  g_assert_cmpuint (fake.images, ==, 0);
  for (guint bad = 1; bad <= commands; bad++)
    for (guint malformed = 0; malformed < 2; malformed++)
      {
        fake = (Fake){.image_dc = 32, .target_dc = 60,
                      .fail_at = malformed ? 0 : bad, .malformed_at = malformed ? bad : 0};
        g_assert_false (eh575_detector_empty (&check, exchange, image, &fake));
        g_assert_cmpuint (fake.used, ==, bad);
      }
}

static void
contact_handoff (void)
{
  for (guint status = 0; status < 256; status++)
    {
      Fake fake = {.status = status};
      g_assert_cmpint (eh575_detector_contact_status (exchange, &fake), ==,
                       status == 0 ? 0 : status == 4 ? 1 : -1);
      g_assert_cmpuint (fake.used, ==, 1);
    }
  Fake fake = {.status = 4, .fail_at = 1};
  g_assert_cmpint (eh575_detector_contact_status (exchange, &fake), ==, -1);
  fake = (Fake){.status = 4, .malformed_at = 1};
  g_assert_cmpint (eh575_detector_contact_status (exchange, &fake), ==, -1);
  for (guint mask = 0; mask < 32; mask++)
    g_assert_cmpint (eh575_detector_contact_ready (mask & 1, mask & 2, mask & 4,
                                                  mask & 8, mask & 16), ==, mask == 15);
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
  g_test_add_func ("/eh575/detector/sleep-evidence", sleep_evidence);
  g_test_add_func ("/eh575/detector/empty-exposure", empty_exposure);
  g_test_add_func ("/eh575/detector/contact-handoff", contact_handoff);
  return g_test_run ();
}
