/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Callback/cancellation tests only. The real probe entrypoint is never invoked:
 * no USB discovery, system-bus connection, host sleep or authentication.
 */
int eh575_probe_unused_main (int argc, char **argv);
#define main eh575_probe_unused_main
#include "eh575-touch-probe.c"
#undef main

static void
sleep_signal (void)
{
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(GVariant) awake = g_variant_ref_sink (g_variant_new ("(b)", FALSE));
  g_autoptr(GVariant) asleep = g_variant_ref_sink (g_variant_new ("(b)", TRUE));
  g_autoptr(GVariant) malformed = g_variant_ref_sink (g_variant_new ("(s)", "bad"));
  cancel_on_sleep (NULL, NULL, NULL, NULL, NULL, awake, cancel);
  g_assert_false (g_cancellable_is_cancelled (cancel));
  cancel_on_sleep (NULL, NULL, NULL, NULL, NULL, asleep, cancel);
  g_assert_true (g_cancellable_is_cancelled (cancel));
  g_cancellable_reset (cancel);
  cancel_on_sleep (NULL, NULL, NULL, NULL, NULL, malformed, cancel);
  g_assert_true (g_cancellable_is_cancelled (cancel));
}

static void
owner_signal (void)
{
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  const char *events[][3] = {
    {"net.reactivated.Fprint", "", ":1.2"},
    {"net.reactivated.Fprint", ":1.2", ""},
    {"other.service", "", ":1.2"},
    {"net.reactivated.Fprint", ":1.2", ":1.3"},
  };
  for (guint n = 0; n < G_N_ELEMENTS (events); n++)
    {
      g_cancellable_reset (cancel);
      g_autoptr(GVariant) parameters = g_variant_ref_sink (g_variant_new (
        "(sss)", events[n][0], events[n][1], events[n][2]));
      cancel_on_fprintd (NULL, NULL, NULL, NULL, NULL, parameters, cancel);
      g_assert_cmpint (g_cancellable_is_cancelled (cancel), ==, n == 0 || n == 3);
    }
  g_cancellable_reset (cancel);
  g_autoptr(GVariant) malformed = g_variant_ref_sink (g_variant_new ("(b)", FALSE));
  cancel_on_fprintd (NULL, NULL, NULL, NULL, NULL, malformed, cancel);
  g_assert_true (g_cancellable_is_cancelled (cancel));
}

static void
lost_bus (void)
{
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  cancel_on_bus_close (NULL, FALSE, NULL, cancel);
  g_assert_true (g_cancellable_is_cancelled (cancel));
}

static void
cancelled_or_expired (void)
{
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(GError) error = NULL;
  ProbeIO io = {.cancel = cancel, .error = &error};
  deadline = g_get_monotonic_time () + 1000000;
  g_cancellable_cancel (cancel);
  g_assert_false (wait_for_contact (&io));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  g_cancellable_reset (cancel);
  deadline = g_get_monotonic_time () - 1;
  g_assert_false (wait_for_contact (&io));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
}

static void
early_transport_guards (void)
{
  g_autoptr(GError) error = NULL;
  ProbeIO io = {.error = &error}; /* NULL USB: both guards must return before I/O. */
  uint8_t reply[64];
  size_t length = sizeof reply;
  deadline = g_get_monotonic_time () - 1;
  g_assert_false (detector_io (&io, &eh575_init_commands[0], reply, &length));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_cmpuint (io.exchanges, ==, 0);
  g_clear_error (&error);
  deadline = g_get_monotonic_time () + 1000000;
  Eh575Command invalid = {.length = 6};
  g_assert_false (detector_io (&io, &invalid, reply, &length));
  g_assert_no_error (error);
  g_assert_cmpuint (io.exchanges, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/eh575/contact/sleep-signal", sleep_signal);
  g_test_add_func ("/eh575/contact/owner-signal", owner_signal);
  g_test_add_func ("/eh575/contact/lost-bus", lost_bus);
  g_test_add_func ("/eh575/contact/cancelled-or-expired", cancelled_or_expired);
  g_test_add_func ("/eh575/contact/early-transport-guards", early_transport_guards);
  return g_test_run ();
}
