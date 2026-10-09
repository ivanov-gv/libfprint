/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Isolated unprivileged research, not a wake daemon or authenticator.
 * Detector mode adds cross-checked volatile detector calibration/entry/exit.
 * No reset, firmware/NVM upload, images or templates saved.
 */
#include <gusb.h>
#include <glib-unix.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include "egis0575-touch.h"
#include "egis0575-detector.h"

#include "eh575-detector-usb.h"

static void
cancel_on_sleep (GDBusConnection *connection G_GNUC_UNUSED,
                 const gchar *sender G_GNUC_UNUSED, const gchar *path G_GNUC_UNUSED,
                 const gchar *interface G_GNUC_UNUSED, const gchar *signal G_GNUC_UNUSED,
                 GVariant *parameters, gpointer data)
{
  gboolean sleeping;
  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(b)")))
    {
      g_cancellable_cancel (data);
      return;
    }
  g_variant_get (parameters, "(b)", &sleeping);
  if (sleeping)
    g_cancellable_cancel (data);
}

static void
cancel_on_fprintd (GDBusConnection *connection G_GNUC_UNUSED,
                   const gchar *sender G_GNUC_UNUSED, const gchar *path G_GNUC_UNUSED,
                   const gchar *interface G_GNUC_UNUSED, const gchar *signal G_GNUC_UNUSED,
                   GVariant *parameters, gpointer data)
{
  const char *name, *old_owner, *new_owner;
  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(sss)")))
    {
      g_cancellable_cancel (data);
      return;
    }
  g_variant_get (parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (!strcmp (name, "net.reactivated.Fprint") && *new_owner)
    g_cancellable_cancel (data);
}

static void
cancel_on_bus_close (GDBusConnection *connection G_GNUC_UNUSED,
                     gboolean vanished G_GNUC_UNUSED, GError *error G_GNUC_UNUSED,
                     gpointer data)
{
  g_cancellable_cancel (data);
}

/* Metadata-only startup check, after subscribing so transitions cannot be
 * missed between querying and USB setup. Never activate or stop fprintd.
 */
static gboolean
contact_preflight (GDBusConnection *bus, GCancellable *cancel, GError **error)
{
  g_autoptr(GVariant) sleep_reply = g_dbus_connection_call_sync (
    bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.DBus.Properties", "Get",
    g_variant_new ("(ss)", "org.freedesktop.login1.Manager", "PreparingForSleep"),
    G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, cancel, error);
  g_autoptr(GVariant) value = NULL;
  if (!sleep_reply)
    return FALSE;
  g_variant_get (sleep_reply, "(v)", &value);
  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN) || g_variant_get_boolean (value))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY, "Cannot start awake contact test while sleep is pending");
      return FALSE;
    }
  g_autoptr(GVariant) owner_reply = g_dbus_connection_call_sync (
    bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
    g_variant_new ("(s)", "net.reactivated.Fprint"), G_VARIANT_TYPE ("(b)"),
    G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, cancel, error);
  gboolean owned;
  if (!owner_reply)
    return FALSE;
  g_variant_get (owner_reply, "(b)", &owned);
  if (owned)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY, "System fprintd already owns its bus name; wait for its normal idle exit");
      return FALSE;
    }
  return TRUE;
}

static gboolean
interrupt_probe (GUsbDevice *usb, GCancellable *cancel, GError **error, gboolean detector)
{
  const char *phases[] = {"Keep the reader EMPTY", "TOUCH and HOLD any finger", "LIFT fully; leave the reader empty"};
  Eh575EventStats stats[2] = {0};

  for (size_t phase = 0; phase < G_N_ELEMENTS (phases); phase++)
    {
      g_print ("Phase %zu/3: %s for six seconds.\n", phase + 1, phases[phase]);
      fflush (stdout);
      gint64 until = MIN (deadline, g_get_monotonic_time () + 6000000);
      guint status_changes = 0, status_bits = 0;
      gint last_status = -1;
      stats[0].packets = stats[0].changes = stats[1].packets = stats[1].changes = 0;
      while (g_get_monotonic_time () < until && !g_cancellable_is_cancelled (cancel))
        {
          pump_signals ();
          if (detector)
            {
              const Eh575Command cmd = {7, {'E', 'G', 'I', 'S', 0x60, 1, 0}};
              uint8_t reply[64];
              ProbeIO io = {.usb = usb, .cancel = cancel, .error = error};
              if (!eh575_detector_io (detector_io, &io, &cmd, reply))
                return FALSE;
              status_changes += last_status >= 0 && last_status != reply[5];
              last_status = reply[5];
              status_bits |= reply[5];
            }
          for (guint ep = 0; ep < 2; ep++)
          {
            uint8_t packet[16];
            gsize length = 0;
            g_autoptr(GError) event_error = NULL;
            if (!g_usb_device_interrupt_transfer (usb, 0x83 + ep, packet, sizeof packet, &length, 100, cancel, &event_error))
              {
                if (g_error_matches (event_error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
                  continue;
                g_propagate_error (error, g_steal_pointer (&event_error));
                return FALSE;
              }
            if (!eh575_event_observe (&stats[ep], packet, length))
              {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unexpected interrupt packet length");
                return FALSE;
              }
            memset (packet, 0, sizeof packet);
          }
        }
      for (guint ep = 0; ep < 2; ep++)
        g_print ("phase=%zu endpoint=0x%02x packets=%u payload_changes=%u\n", phase + 1, 0x83 + ep, stats[ep].packets, stats[ep].changes);
      if (detector)
        g_print ("phase=%zu detector_status_bits=0x%02x status_changes=%u\n", phase + 1, status_bits, status_changes);
    }
  memset (stats, 0, sizeof stats);
  return !g_cancellable_is_cancelled (cancel);
}

static gboolean
touch_probe (GUsbDevice *usb, GCancellable *cancel, GError **error)
{
  uint8_t frame[EH575_FRAME_SIZE], samples[3][EH575_FRAME_SIZE], background[EH575_FRAME_SIZE];
  guint empty = 0, present = 0;
  gboolean touched = FALSE, ok = FALSE;

  g_print ("Leave EMPTY until 'Touch detector ready'. Current DC is not changed.\n");
  while (g_get_monotonic_time () < deadline && !g_cancellable_is_cancelled (cancel))
    {
      if (!capture (usb, frame, cancel, error))
        goto out;
      if (empty < 3)
        {
          if (!eh575_touch_idle (frame))
            {
              empty = 0;
              continue;
            }
          memcpy (samples[empty++], frame, sizeof frame);
          if (empty == 3)
            {
              eh575_median (background, samples);
              g_print ("Touch detector ready. TOUCH and HOLD; then LIFT fully.\n");
              fflush (stdout);
            }
        }
      else if (!touched)
        {
          present = eh575_touch_present (frame, background) ? present + 1 : 0;
          if (present >= 2)
            {
              touched = TRUE;
              present = 0;
              g_print ("CONTACT detected (not a fingerprint match). Lift now.\n");
              fflush (stdout);
            }
        }
      else
        {
          present = eh575_touch_idle (frame) && eh575_deviation (frame, background) < 4 ? present + 1 : 0;
          if (present >= 3)
            {
              g_print ("RELEASE detected. Both transitions observed.\n");
              ok = TRUE;
              goto out;
            }
        }
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Touch/release test timed out; no wake support established");
out:
  memset (frame, 0, sizeof frame);
  memset (samples, 0, sizeof samples);
  memset (background, 0, sizeof background);
  return ok;
}

static gboolean
sample_clocks (uint64_t *boot, uint64_t *mono, GError **error)
{
  struct timespec b, m;
  if (clock_gettime (CLOCK_BOOTTIME, &b) || clock_gettime (CLOCK_MONOTONIC, &m))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot read suspend-aware clocks");
      return FALSE;
    }
  *boot = (uint64_t) b.tv_sec * 1000000 + b.tv_nsec / 1000;
  *mono = (uint64_t) m.tv_sec * 1000000 + m.tv_nsec / 1000;
  return TRUE;
}

static int
detector_capture (void *context, uint8_t *frame)
{
  ProbeIO *io = context;
  return capture (io->usb, frame, io->cancel, io->error);
}

static gboolean
wait_for_suspend (GCancellable *cancel, GError **error)
{
  uint64_t start_boot, start_mono, boot, mono;
  if (!sample_clocks (&start_boot, &start_mono, error))
    return FALSE;
  /* This deadline measures awake time: the process is frozen during host sleep.
   * No USB polling, inhibitor, automatic suspend, synthetic input or wake timer.
   */
  deadline = g_get_monotonic_time () + 120000000;
  g_print ("SUSPEND TEST READY: detector armed; USB traffic stopped.\n"
           "Within 120 awake seconds, suspend using the menu or 'systemctl suspend' in ANOTHER terminal.\n"
           "Wait until truly asleep, then touch the sensor once. If no wake after 15 seconds, use the keyboard or power button.\n"
           "Do not run fingerprint clients. Capture restoration runs after resume; wait for it before testing login.\n");
  fflush (stdout);
  while (g_get_monotonic_time () < deadline)
    {
      pump_signals ();
      if (g_cancellable_is_cancelled (cancel))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Suspend observation cancelled");
          return FALSE;
        }
      if (!sample_clocks (&boot, &mono, error))
        return FALSE;
      uint64_t slept = eh575_detector_sleep_elapsed (start_boot, start_mono, boot, mono);
      if (slept >= 2000000)
        {
          g_print ("Resume observed: approximately %.2f seconds asleep. Clocks do NOT identify the wake source.\n",
                   slept / 1000000.0);
          return TRUE;
        }
      g_usleep (100000);
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "No sleep interval of at least two seconds observed; restoring capture");
  return FALSE;
}

static gboolean
wait_for_contact (ProbeIO *io)
{
  uint64_t start_boot, start_mono, boot, mono;
  if (!sample_clocks (&start_boot, &start_mono, io->error))
    return FALSE;
  g_print ("CONTACT TEST READY: awake-only detector polling; touch once and hold. Do NOT lock or suspend.\n");
  fflush (stdout);
  while (g_get_monotonic_time () < deadline)
    {
      pump_signals ();
      if (g_cancellable_is_cancelled (io->cancel))
        {
          g_set_error_literal (io->error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "Contact observation cancelled (signal, sleep preparation, fprintd start or lost system bus); restoring capture");
          return FALSE;
        }
      if (!sample_clocks (&boot, &mono, io->error))
        return FALSE;
      if (eh575_detector_sleep_elapsed (start_boot, start_mono, boot, mono) >= 100000)
        {
          g_set_error_literal (io->error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Host sleep observed during awake test; no contact event emitted");
          return FALSE;
        }
      int status = eh575_detector_contact_status (detector_io, io);
      if (status < 0)
        {
          if (!*io->error)
            g_set_error_literal (io->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Uncharacterized detector status; no contact event emitted");
          return FALSE;
        }
      if (status)
        return TRUE;
      g_usleep (250000);
    }
  g_set_error_literal (io->error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "No contact within awake test budget; restoring capture");
  return FALSE;
}

static gboolean
detector_probe (GUsbDevice *usb, GCancellable *cancel, GError **error, gboolean suspend_test,
                Eh575Handoff handoff, gboolean contact_test, ProbeState *state)
{
  ProbeIO io = {.usb = usb, .cancel = cancel, .error = error};
  Eh575Detector detector;
  Eh575EmptyCheck empty;
  gboolean ok = FALSE, changed = FALSE;

  g_print ("Keep EMPTY: settling and measuring usable exposure before volatile detector calibration.\n");
  changed = TRUE; /* Recovery also covers interrupted/failed exposure writes. */
  if (!eh575_detector_empty (&empty, detector_io, detector_capture, &io))
    {
      if (!*error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Empty-reader check failed: mean=%.1f texture=%.1f clipped=%.3f at DC=%u; detector NOT armed. Keep empty and retry",
                     empty.mean, empty.texture, empty.clipped, empty.dc);
      goto out;
    }
  g_print ("Empty reader ready: mean=%.1f texture=%.1f DC=%u (%u exposure checks).\n",
           empty.mean, empty.texture, empty.dc, empty.attempts);
  if (!eh575_detector_calibrate (&detector, detector_io, &io))
    goto out;
  g_print ("Measured detector: reference=%u DC=%u/%u mean=%u threshold=%u.\n",
           detector.reference, detector.dc_p, detector.dc_c, detector.mean, detector.threshold);
  if (!eh575_detector_enter (&detector, detector_io, &io))
    goto out;
  if (suspend_test || contact_test)
    {
      int status = eh575_detector_contact_status (detector_io, &io);
      if (status != 0)
        {
          if (!*error)
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Touch already latched or status uncharacterized before observation; detector test aborted. Leave empty and retry");
          goto out;
        }
      if (handoff != EH575_HOLD_CLAIM)
        {
          /* Leave the volatile detector armed, but relinquish the exclusive
           * claim. Keeping versus closing the handle isolates runtime PM.
           * Do not reset, detach a kernel driver or stop/steal from fprintd.
           */
          if (!g_usb_device_release_interface (usb, 0, 0, error))
            goto out;
          state->claimed = FALSE;
          if (handoff == EH575_CLOSE_HANDLE)
            {
              if (!g_usb_device_close (usb, error))
                goto out;
              state->opened = FALSE;
              g_print ("USB interface released and handle closed with detector armed.\n");
            }
          else
            g_print ("USB interface released; handle kept open for the runtime-power test. No USB I/O until resume.\n");
        }
      if (contact_test)
        ok = state->contact = wait_for_contact (&io);
      else
        ok = wait_for_suspend (cancel, error);
    }
  else
    {
      g_print ("Volatile detector armed for AWAKE testing. Do NOT suspend.\n");
      ok = interrupt_probe (usb, cancel, error, TRUE);
    }
out:
  memset (&empty, 0, sizeof empty);
  memset (&detector, 0, sizeof detector);
  if (!ok && !*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Detector test failed; calibration/status bounds or cancellation prevented completion");
  if (changed)
    {
      g_autoptr(GError) restore_error = NULL;
      ProbeIO recovery = {.usb = usb, .error = &restore_error};
      deadline = g_get_monotonic_time () + 5000000;
      gboolean access = TRUE;
      if (!state->opened)
        {
          access = g_usb_device_open (usb, &restore_error);
          state->opened = access;
        }
      if (access && !state->claimed)
        {
          access = g_usb_device_claim_interface (usb, 0, 0, &restore_error);
          state->claimed = access;
        }
      if (!access || !eh575_detector_restore (detector_io, &recovery))
        {
          g_printerr ("Capture restoration FAILED: %s. Last exchange opcode=0x%02x register=0x%02x, %u exchanges, busy-bit=%s.\n"
                      "No interface is stolen; close the probe and test fprintd normally before another suspend.\n",
                      restore_error ? restore_error->message : "unexpected status or busy timeout",
                      recovery.last_opcode, recovery.last_register, recovery.exchanges,
                      recovery.busy_known ? (recovery.busy_bit ? "set" : "clear") : "unknown");
          ok = FALSE;
        }
      else
        {
          state->restored = TRUE;
          g_print ("Detector exited; characterized capture initialization restored.\n");
        }
    }
  return ok;
}

int
main (int argc, char **argv)
{
  const char *mode = argc >= 2 ? argv[1] : "";
  gboolean released_test = !strcmp (mode, "detector-suspend-released");
  gboolean unclaimed_test = !strcmp (mode, "detector-suspend-unclaimed");
  gboolean suspend_test = !strcmp (mode, "detector-suspend") || released_test || unclaimed_test;
  Eh575Handoff handoff = released_test ? EH575_CLOSE_HANDLE : unclaimed_test ? EH575_HOLD_HANDLE : EH575_HOLD_CLAIM;
  gboolean contact_test = !strcmp (mode, "detector-contact");
  gboolean initialized = !strcmp (mode, "interrupt-initialized") || !strcmp (mode, "touch") || !strcmp (mode, "detector") || suspend_test || contact_test;

  if ((suspend_test || contact_test ? argc != 3 || strcmp (argv[2], contact_test ? "--allow-contact-test" : "--allow-suspend-test") : argc != 2) ||
      (!initialized && strcmp (mode, "interrupt") && strcmp (mode, "open")) || geteuid () == 0)
    {
      g_printerr ("Run WITHOUT sudo: eh575-touch-probe open|interrupt|interrupt-initialized|touch|detector OR detector-contact --allow-contact-test OR detector-suspend[-released|-unclaimed] --allow-suspend-test\n");
      return 2;
    }
  g_autoptr(GError) error = NULL;
  g_autoptr(GUsbContext) context = g_usb_context_new (&error);
  g_autoptr(GPtrArray) devices = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(GDBusConnection) contact_bus = NULL;
  GUsbDevice *usb = NULL;
  ProbeState state = {0};
  gboolean ok = FALSE;
  guint sleep_subscription = 0, owner_subscription = 0;
  gulong bus_closed = 0;
  guint sigint = g_unix_signal_add (SIGINT, cancel_probe, cancel);
  guint sigterm = g_unix_signal_add (SIGTERM, cancel_probe, cancel);

  deadline = g_get_monotonic_time () + 45000000;
  if (!context)
    goto out;
  if (contact_test)
    {
      contact_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancel, &error);
      if (!contact_bus)
        goto out;
      /* g_bus_get_sync defaults to exiting on bus loss. Keep this isolated
       * process alive long enough to run its USB restoration/cleanup instead.
       */
      g_dbus_connection_set_exit_on_close (contact_bus, FALSE);
      bus_closed = g_signal_connect (contact_bus, "closed", G_CALLBACK (cancel_on_bus_close), cancel);
      sleep_subscription = g_dbus_connection_signal_subscribe (
        contact_bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager", "PrepareForSleep",
        "/org/freedesktop/login1", NULL, G_DBUS_SIGNAL_FLAGS_NONE, cancel_on_sleep, cancel, NULL);
      owner_subscription = g_dbus_connection_signal_subscribe (
        contact_bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", "net.reactivated.Fprint", G_DBUS_SIGNAL_FLAGS_NONE, cancel_on_fprintd, cancel, NULL);
      if (!contact_preflight (contact_bus, cancel, &error))
        goto out;
      pump_signals ();
      if (g_cancellable_is_cancelled (cancel))
        goto out;
    }
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
  if (!g_usb_device_claim_interface (usb, 0, 0, &error))
    goto out;
  state.claimed = TRUE;
  g_print ("Isolated EH575 %s probe. No authentication, files or raw packet dumps.\n", mode);
  if (!strcmp (mode, "open"))
    {
      ok = TRUE;
      goto out;
    }
  if (initialized)
    {
      g_print ("Using only the characterized 47-command volatile initialization.\n");
      for (size_t i = 0; i < G_N_ELEMENTS (eh575_init_commands); i++)
        if (!command (usb, &eh575_init_commands[i], cancel, &error))
          goto out;
    }
  if (!strcmp (mode, "detector") || suspend_test || contact_test)
    ok = detector_probe (usb, cancel, &error, suspend_test, handoff, contact_test, &state);
  else
    ok = !strcmp (mode, "touch") ? touch_probe (usb, cancel, &error) : interrupt_probe (usb, cancel, &error, FALSE);
out:
  if (state.claimed)
    {
      g_autoptr(GError) close_error = NULL;
      if (!g_usb_device_release_interface (usb, 0, 0, &close_error))
        {
          g_printerr ("Release failed: %s\n", close_error->message);
          ok = FALSE;
        }
      else
        state.claimed = FALSE;
    }
  if (state.opened)
    {
      g_autoptr(GError) close_error = NULL;
      if (!g_usb_device_close (usb, &close_error))
        {
          g_printerr ("Close failed: %s\n", close_error->message);
          ok = FALSE;
        }
      else
        state.opened = FALSE;
    }
  /* Drain queued guards while subscriptions are still live. A companion must
   * recheck its own lock/sleep state after process exit as well.
   */
  pump_signals ();
  if (contact_bus && g_dbus_connection_is_closed (contact_bus))
    g_cancellable_cancel (cancel);
  if (sleep_subscription)
    g_dbus_connection_signal_unsubscribe (contact_bus, sleep_subscription);
  if (owner_subscription)
    g_dbus_connection_signal_unsubscribe (contact_bus, owner_subscription);
  if (bus_closed)
    g_signal_handler_disconnect (contact_bus, bus_closed);
  pump_signals ();
  if (ok && eh575_detector_contact_ready (state.contact, state.restored, !state.claimed,
                                         !state.opened, g_cancellable_is_cancelled (cancel)))
    g_print ("EH575_CONTACT_READY: contact only, NOT authenticated; capture restored and USB released/closed.\n");
  g_source_remove (sigint);
  g_source_remove (sigterm);
  if (error)
    g_printerr ("Probe stopped: %s\n", error->message);
  g_print ("Probe finished; reader release/close attempted. Interrupt activity alone does not establish wake-on-touch.\n");
  return g_cancellable_is_cancelled (cancel) ? 130 : (ok ? 0 : 1);
}
