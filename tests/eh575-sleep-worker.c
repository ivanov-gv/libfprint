/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Opt-in system-sleep worker: contact wake only, never authentication.
 * Launched by the root supervisor AFTER logind's sleep preparation.
 * The separate research probe remains unprivileged and unchanged in policy.
 */
#include <poll.h>
#include <errno.h>
#include "eh575-detector-usb.h"

#define SLEEP_ARMED "EH575_SLEEP_ARMED: interface released; USB handle retained."
#define SLEEP_RESTORED "EH575_SLEEP_RESTORED: capture restored; USB released/closed."

static void
sleep_bus_closed (GDBusConnection *bus G_GNUC_UNUSED,
                  gboolean vanished G_GNUC_UNUSED, GError *error G_GNUC_UNUSED,
                  gpointer cancel)
{
  g_cancellable_cancel (cancel);
}

static gboolean
sleep_preparing (GDBusConnection *bus, GCancellable *cancel, GError **error)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync (
    bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.DBus.Properties", "Get",
    g_variant_new ("(ss)", "org.freedesktop.login1.Manager", "PreparingForSleep"),
    G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, cancel, error);
  g_autoptr(GVariant) value = NULL;
  if (!reply)
    return FALSE;
  g_variant_get (reply, "(v)", &value);
  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN) || !g_variant_get_boolean (value))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY, "No logind sleep preparation; refusing to arm");
      return FALSE;
    }
  return TRUE;
}

/* No USB I/O here. The system-sleep POST hook supplies the restore command.
 * The awake-time limit also recovers if sleep was cancelled or POST was lost.
 * An EOF, signal or malformed control message requests cleanup, NOT success.
 */
static gboolean
sleep_wait_restore (int input, GCancellable *cancel, gint64 until, GError **error)
{
  char buffer[16] = {0};
  size_t used = 0;
  while (g_get_monotonic_time () < until)
    {
      pump_signals ();
      if (g_cancellable_is_cancelled (cancel))
        break;
      struct pollfd fd = {.fd = input, .events = POLLIN};
      int result = poll (&fd, 1, 100);
      if (result < 0 && errno == EINTR)
        continue;
      if (result < 0 || (fd.revents & (POLLERR | POLLNVAL)))
        break;
      if (!(fd.revents & (POLLIN | POLLHUP)))
        continue;
      ssize_t count = read (input, buffer + used, sizeof buffer - used);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        break;
      used += count;
      if (memchr (buffer, '\n', used))
        {
          if (used == 8 && !memcmp (buffer, "RESTORE\n", 8))
            return TRUE;
          break;
        }
      if (used == sizeof buffer)
        break;
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Restore control lost, cancelled, invalid or timed out; cleaning up");
  return FALSE;
}

static int
sleep_capture (void *context, uint8_t *frame)
{
  ProbeIO *io = context;
  return capture (io->usb, frame, io->cancel, io->error);
}

int
main (int argc, char **argv)
{
  if (argc != 2 || strcmp (argv[1], "--system-sleep-worker") || geteuid () != 0 || getuid () != 0)
    {
      g_printerr ("Only the opted-in root sleep supervisor may launch this worker.\n");
      return 2;
    }
  /* Never let a caller redirect a root worker to a user-controlled bus. */
  g_unsetenv ("DBUS_SYSTEM_BUS_ADDRESS");
  g_unsetenv ("DBUS_SESSION_BUS_ADDRESS");
  g_autoptr(GError) error = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(GUsbContext) context = NULL;
  g_autoptr(GPtrArray) devices = NULL;
  ProbeState state = {0};
  Eh575Detector detector = {0};
  Eh575EmptyCheck empty = {0};
  GUsbDevice *usb = NULL;
  gboolean changed = FALSE, ok = FALSE;
  gulong closed = 0;
  guint sigint = g_unix_signal_add (SIGINT, cancel_probe, cancel);
  guint sigterm = g_unix_signal_add (SIGTERM, cancel_probe, cancel);
  signal (SIGPIPE, SIG_IGN); /* Supervisor loss is handled by stdin EOF. */

  deadline = g_get_monotonic_time () + 12000000;
  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancel, &error);
  if (!bus)
    goto out;
  g_dbus_connection_set_exit_on_close (bus, FALSE);
  closed = g_signal_connect (bus, "closed", G_CALLBACK (sleep_bus_closed), cancel);
  if (!sleep_preparing (bus, cancel, &error))
    goto out;
  context = g_usb_context_new (&error);
  if (!context)
    goto out;
  g_usb_context_enumerate (context);
  devices = g_usb_context_get_devices (context);
  for (guint i = 0; i < devices->len; i++)
    {
      GUsbDevice *candidate = g_ptr_array_index (devices, i);
      if (g_usb_device_get_vid (candidate) != 0x1c7a || g_usb_device_get_pid (candidate) != 0x0575)
        continue;
      if (usb || g_usb_device_get_release (candidate) != EH575_REVISION)
        {
          g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Require exactly one EH575 revision 1072");
          goto out;
        }
      usb = candidate;
    }
  if (!usb)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No tested EH575 found");
      goto out;
    }
  if (!valid_interfaces (usb, &error) || !g_usb_device_open (usb, &error))
    goto out;
  state.opened = TRUE;
  /* No DETACH_KERNEL_DRIVER flag, resets, forced release or daemon stop. */
  if (!g_usb_device_claim_interface (usb, 0, 0, &error))
    goto out;
  state.claimed = TRUE;
  changed = TRUE;
  for (size_t i = 0; i < G_N_ELEMENTS (eh575_init_commands); i++)
    if (!command (usb, &eh575_init_commands[i], cancel, &error))
      goto out;
  ProbeIO io = {.usb = usb, .cancel = cancel, .error = &error};
  if (!eh575_detector_empty (&empty, detector_io, sleep_capture, &io) ||
      !eh575_detector_calibrate (&detector, detector_io, &io) ||
      !eh575_detector_enter (&detector, detector_io, &io) ||
      eh575_detector_contact_status (detector_io, &io) != 0 ||
      !sleep_preparing (bus, cancel, &error))
    goto out;
  g_print ("Sleep detector measured: reference=%u DC=%u/%u mean=%u threshold=%u.\n",
           detector.reference, detector.dc_p, detector.dc_c, detector.mean, detector.threshold);
  if (!g_usb_device_release_interface (usb, 0, 0, &error))
    goto out;
  state.claimed = FALSE;
  g_print (SLEEP_ARMED "\n");
  fflush (stdout);
  ok = sleep_wait_restore (STDIN_FILENO, cancel, g_get_monotonic_time () + 120000000, &error);
out:
  if (changed)
    {
      g_autoptr(GError) recovery_error = NULL;
      ProbeIO recovery = {.usb = usb, .error = &recovery_error};
      deadline = g_get_monotonic_time () + 5000000;
      if (!state.claimed)
        state.claimed = g_usb_device_claim_interface (usb, 0, 0, &recovery_error);
      if (state.claimed && eh575_detector_restore (detector_io, &recovery))
        state.restored = TRUE;
      else
        {
          ok = FALSE;
          g_printerr ("Sleep capture restoration FAILED: %s; last opcode=0x%02x register=0x%02x, exchanges=%u.\n",
                      recovery_error ? recovery_error->message : "status/busy timeout",
                      recovery.last_opcode, recovery.last_register, recovery.exchanges);
        }
    }
  memset (&empty, 0, sizeof empty);
  memset (&detector, 0, sizeof detector);
  if (state.claimed)
    {
      g_autoptr(GError) release_error = NULL;
      if (g_usb_device_release_interface (usb, 0, 0, &release_error))
        state.claimed = FALSE;
      else
        {
          ok = FALSE;
          g_printerr ("Sleep release FAILED: %s\n", release_error->message);
        }
    }
  if (state.opened)
    {
      g_autoptr(GError) close_error = NULL;
      if (g_usb_device_close (usb, &close_error))
        state.opened = FALSE;
      else
        {
          ok = FALSE;
          g_printerr ("Sleep close FAILED: %s\n", close_error->message);
        }
    }
  pump_signals ();
  if (closed)
    g_signal_handler_disconnect (bus, closed);
  if (error)
    g_printerr ("Sleep worker: %s\n", error->message);
  else if (!ok)
    g_printerr ("Sleep worker: detector not armed or operation failed; ordinary wake/password remain available.\n");
  if (ok && state.restored && !state.claimed && !state.opened && !g_cancellable_is_cancelled (cancel))
    g_print (SLEEP_RESTORED "\n");
  else
    ok = FALSE;
  g_source_remove (sigint);
  g_source_remove (sigterm);
  return ok ? 0 : 1;
}
