/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Pipe/control tests only; worker main is NEVER called. No USB or system bus.
 */
int eh575_sleep_unused_main (int argc, char **argv);
#define main eh575_sleep_unused_main
#include "eh575-sleep-worker.c"
#undef main

static void
control_messages (void)
{
  const char *messages[] = {"RESTORE\n", "RESTORE\nextra", "restore\n", "ARM\n",
                            "RESTORE", "abcdefghijklmnop", ""};
  for (guint i = 0; i < G_N_ELEMENTS (messages); i++)
    {
      int fds[2];
      g_assert_cmpint (pipe (fds), ==, 0);
      size_t length = strlen (messages[i]);
      g_assert_cmpint (write (fds[1], messages[i], length), ==, length);
      close (fds[1]);
      g_autoptr(GCancellable) cancel = g_cancellable_new ();
      g_autoptr(GError) error = NULL;
      gboolean ok = sleep_wait_restore (fds[0], cancel, g_get_monotonic_time () + 500000, &error);
      g_assert_cmpint (ok, ==, i == 0);
      if (ok)
        g_assert_no_error (error);
      else
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      close (fds[0]);
    }
}

static void
control_timeout (void)
{
  int fds[2];
  g_assert_cmpint (pipe (fds), ==, 0);
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(GError) error = NULL;
  gint64 start = g_get_monotonic_time ();
  g_assert_false (sleep_wait_restore (fds[0], cancel, start + 50000, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 500000);
  close (fds[0]);
  close (fds[1]);
}

static void
control_cancelled (void)
{
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(GError) error = NULL;
  g_cancellable_cancel (cancel);
  g_assert_false (sleep_wait_restore (-1, cancel, g_get_monotonic_time () + 500000, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/egis0575/sleep/control", control_messages);
  g_test_add_func ("/egis0575/sleep/timeout", control_timeout);
  g_test_add_func ("/egis0575/sleep/cancelled", control_cancelled);
  return g_test_run ();
}
