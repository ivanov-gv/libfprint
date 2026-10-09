/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <glib.h>
#include "egis0575-touch.h"

static void
event_stats (void)
{
  Eh575EventStats stats = {0};
  uint8_t first[16] = {1}, second[16] = {2};

  g_assert_false (eh575_event_observe (&stats, first, 0));
  g_assert_false (eh575_event_observe (&stats, first, 17));
  g_assert_cmpuint (stats.packets, ==, 0);
  g_assert_true (eh575_event_observe (&stats, first, 16));
  g_assert_true (eh575_event_observe (&stats, first, 16));
  g_assert_cmpuint (stats.changes, ==, 0);
  g_assert_true (eh575_event_observe (&stats, second, 16));
  g_assert_true (eh575_event_observe (&stats, second, 7));
  g_assert_cmpuint (stats.changes, ==, 2);
  g_assert_cmpuint (stats.packets, ==, 4);
}

static void
touch_quality (void)
{
  uint8_t idle[EH575_FRAME_SIZE], contact[EH575_FRAME_SIZE];

  memset (idle, 128, sizeof idle);
  g_assert_true (eh575_touch_idle (idle));
  g_assert_false (eh575_touch_present (idle, idle));
  memset (contact, 140, sizeof contact);
  g_assert_true (eh575_touch_idle (contact));
  g_assert_false (eh575_touch_present (contact, idle));
  for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
    contact[i] = i % 2 ? 170 : 86;
  g_assert_false (eh575_touch_idle (contact));
  g_assert_true (eh575_touch_present (contact, idle));
  /* Persistent sensor texture alone must not be mistaken for contact. */
  g_assert_false (eh575_touch_present (contact, contact));
  memset (contact, 0, sizeof contact);
  g_assert_false (eh575_touch_idle (contact));
  g_assert_false (eh575_touch_present (contact, idle));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/eh575/touch/event-stats", event_stats);
  g_test_add_func ("/eh575/touch/quality", touch_quality);
  return g_test_run ();
}
