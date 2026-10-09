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

static gint64 deadline;

typedef struct
{
  GUsbDevice *usb;
  GCancellable *cancel;
  GError **error;
} ProbeIO;

static void
pump_signals (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static guint
remaining (guint maximum)
{
  gint64 left = (deadline - g_get_monotonic_time ()) / 1000;

  return (guint) CLAMP (left, 1, maximum);
}

static gboolean
cancel_probe (gpointer data)
{
  g_cancellable_cancel (data);
  return G_SOURCE_CONTINUE;
}

static int
detector_io (void *context, const Eh575Command *cmd, uint8_t *reply, size_t *reply_length)
{
  ProbeIO *io = context;
  uint8_t bytes[20];
  gsize length = 0;

  pump_signals ();
  if (g_get_monotonic_time () >= deadline)
    {
      g_set_error_literal (io->error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Probe deadline expired");
      return FALSE;
    }
  if (cmd->length < 7 || cmd->length > sizeof bytes)
    return FALSE;
  memcpy (bytes, cmd->data, cmd->length);
  if (!g_usb_device_bulk_transfer (io->usb, EH575_EP_OUT, bytes, cmd->length, &length, remaining (500), io->cancel, io->error))
    return FALSE;
  if (length != cmd->length)
    {
      g_set_error_literal (io->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Short command write");
      return FALSE;
    }
  if (!g_usb_device_bulk_transfer (io->usb, EH575_EP_IN, reply, 64, &length, remaining (500), io->cancel, io->error))
    return FALSE;
  *reply_length = length;
  if (!eh575_reply_valid (cmd, reply, length))
    {
      g_set_error_literal (io->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unexpected command reply");
      return FALSE;
    }
  g_usleep (2000);
  return TRUE;
}

static gboolean
command (GUsbDevice *usb, const Eh575Command *cmd, GCancellable *cancel, GError **error)
{
  ProbeIO io = {usb, cancel, error};
  uint8_t reply[64];
  size_t length = sizeof reply;
  return detector_io (&io, cmd, reply, &length);
}

static gboolean
capture (GUsbDevice *usb, uint8_t *frame, GCancellable *cancel, GError **error)
{
  uint8_t trigger[20], part[8192];
  gsize length = 0;
  size_t used = 0;
  gint64 frame_deadline;

  for (size_t i = 0; i < G_N_ELEMENTS (eh575_rearm_commands); i++)
    if (!command (usb, &eh575_rearm_commands[i], cancel, error))
      return FALSE;
  memcpy (trigger, eh575_trigger.data, eh575_trigger.length);
  if (!g_usb_device_bulk_transfer (usb, EH575_EP_OUT, trigger, eh575_trigger.length, &length, remaining (500), cancel, error))
    return FALSE;
  if (length != eh575_trigger.length)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Short trigger write");
      return FALSE;
    }
  frame_deadline = MIN (deadline, g_get_monotonic_time () + 1500000);
  while (used < EH575_FRAME_SIZE)
    {
      gint64 left = (frame_deadline - g_get_monotonic_time ()) / 1000;
      if (left <= 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Image deadline expired");
          return FALSE;
        }
      if (!g_usb_device_bulk_transfer (usb, EH575_EP_IN, part, sizeof part, &length, (guint) left, cancel, error))
        return FALSE;
      if (!eh575_append_frame (frame, &used, part, length))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unexpected image framing");
          return FALSE;
        }
    }
  g_autoptr(GError) drain_error = NULL;
  if (!g_usb_device_bulk_transfer (usb, EH575_EP_IN, part, 512, &length, 20, cancel, &drain_error))
    {
      if (!g_error_matches (drain_error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_propagate_error (error, g_steal_pointer (&drain_error));
          return FALSE;
        }
    }
  else if (!eh575_status_valid (part, length))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unexpected trailing image reply");
      return FALSE;
    }
  memset (part, 0, sizeof part);
  return TRUE;
}

static gboolean
valid_interfaces (GUsbDevice *usb, GError **error)
{
  g_autoptr(GPtrArray) interfaces = g_usb_device_get_interfaces (usb, error);
  unsigned int found = 0;

  if (!interfaces)
    return FALSE;
  for (guint i = 0; i < interfaces->len; i++)
    {
      GUsbInterface *iface = g_ptr_array_index (interfaces, i);
      if (g_usb_interface_get_number (iface) != 0 || g_usb_interface_get_alternate (iface) != 0 ||
          g_usb_interface_get_class (iface) != 255 || g_usb_interface_get_subclass (iface) != 255 ||
          g_usb_interface_get_protocol (iface) != 0)
        continue;
      g_autoptr(GPtrArray) endpoints = g_usb_interface_get_endpoints (iface);
      if (!endpoints || endpoints->len != 4)
        continue;
      for (guint j = 0; j < endpoints->len; j++)
        {
          GUsbEndpoint *ep = g_ptr_array_index (endpoints, j);
          guint8 address = g_usb_endpoint_get_address (ep);
          guint16 size = g_usb_endpoint_get_maximum_packet_size (ep);
          if (g_usb_endpoint_get_kind (ep) != 5)
            continue;
          if (address == EH575_EP_OUT && size == 512)
            found |= 1;
          if (address == EH575_EP_IN && size == 512)
            found |= 2;
          if (address == 0x83 && size == 16)
            found |= 4;
          if (address == 0x84 && size == 16)
            found |= 8;
        }
    }
  if (found != 15)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unexpected EH575 interface/endpoints");
  return found == 15;
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
              ProbeIO io = {usb, cancel, error};
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
detector_probe (GUsbDevice *usb, GCancellable *cancel, GError **error, gboolean suspend_test)
{
  ProbeIO io = {usb, cancel, error};
  Eh575Detector detector;
  uint8_t frame[EH575_FRAME_SIZE];
  gboolean ok = FALSE, changed = FALSE;

  g_print ("Keep EMPTY: checking three idle frames before volatile detector calibration.\n");
  for (guint n = 0; n < 3; n++)
    {
      if (!capture (usb, frame, cancel, error))
        goto out;
      if (!eh575_touch_idle (frame))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Reader is not empty at usable exposure; detector NOT armed");
          goto out;
        }
    }
  changed = TRUE; /* Recovery is required even if the first setup transfer fails. */
  if (!eh575_detector_calibrate (&detector, detector_io, &io))
    goto out;
  g_print ("Measured detector: reference=%u DC=%u/%u mean=%u threshold=%u.\n",
           detector.reference, detector.dc_p, detector.dc_c, detector.mean, detector.threshold);
  if (!eh575_detector_enter (&detector, detector_io, &io))
    goto out;
  if (suspend_test)
    {
      const Eh575Command status = {7, {'E', 'G', 'I', 'S', 0x60, 1, 0}};
      uint8_t reply[64];
      if (!eh575_detector_io (detector_io, &io, &status, reply))
        goto out;
      if (reply[5] & 0x04)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Touch already latched before suspend; detector test aborted. Leave empty and retry");
          goto out;
        }
      ok = wait_for_suspend (cancel, error);
    }
  else
    {
      g_print ("Volatile detector armed for AWAKE testing. Do NOT suspend.\n");
      ok = interrupt_probe (usb, cancel, error, TRUE);
    }
out:
  memset (frame, 0, sizeof frame);
  memset (&detector, 0, sizeof detector);
  if (!ok && !*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Detector test failed; calibration/status bounds or cancellation prevented completion");
  if (changed)
    {
      g_autoptr(GError) restore_error = NULL;
      ProbeIO recovery = {usb, NULL, &restore_error};
      deadline = g_get_monotonic_time () + 5000000;
      if (!eh575_detector_restore (detector_io, &recovery))
        {
          g_printerr ("Capture restoration FAILED: %s. Do not suspend; close the probe and test fprintd normally.\n",
                      restore_error ? restore_error->message : "unexpected status or busy timeout");
          ok = FALSE;
        }
      else
        g_print ("Detector exited; characterized capture initialization restored.\n");
    }
  return ok;
}

int
main (int argc, char **argv)
{
  const char *mode = argc >= 2 ? argv[1] : "";
  gboolean suspend_test = !strcmp (mode, "detector-suspend");
  gboolean initialized = !strcmp (mode, "interrupt-initialized") || !strcmp (mode, "touch") || !strcmp (mode, "detector") || suspend_test;

  if ((suspend_test ? argc != 3 || strcmp (argv[2], "--allow-suspend-test") : argc != 2) ||
      (!initialized && strcmp (mode, "interrupt") && strcmp (mode, "open")) || geteuid () == 0)
    {
      g_printerr ("Run WITHOUT sudo: eh575-touch-probe open|interrupt|interrupt-initialized|touch|detector OR detector-suspend --allow-suspend-test\n");
      return 2;
    }
  g_autoptr(GError) error = NULL;
  g_autoptr(GUsbContext) context = g_usb_context_new (&error);
  g_autoptr(GPtrArray) devices = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  GUsbDevice *usb = NULL;
  gboolean opened = FALSE, claimed = FALSE, ok = FALSE;
  guint sigint = g_unix_signal_add (SIGINT, cancel_probe, cancel);
  guint sigterm = g_unix_signal_add (SIGTERM, cancel_probe, cancel);

  deadline = g_get_monotonic_time () + 45000000;
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
  opened = TRUE;
  if (!g_usb_device_claim_interface (usb, 0, 0, &error))
    goto out;
  claimed = TRUE;
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
  if (!strcmp (mode, "detector") || suspend_test)
    ok = detector_probe (usb, cancel, &error, suspend_test);
  else
    ok = !strcmp (mode, "touch") ? touch_probe (usb, cancel, &error) : interrupt_probe (usb, cancel, &error, FALSE);
out:
  if (claimed)
    {
      g_autoptr(GError) close_error = NULL;
      if (!g_usb_device_release_interface (usb, 0, 0, &close_error))
        {
          g_printerr ("Release failed: %s\n", close_error->message);
          ok = FALSE;
        }
    }
  if (opened)
    {
      g_autoptr(GError) close_error = NULL;
      if (!g_usb_device_close (usb, &close_error))
        {
          g_printerr ("Close failed: %s\n", close_error->message);
          ok = FALSE;
        }
    }
  g_source_remove (sigint);
  g_source_remove (sigterm);
  if (error)
    g_printerr ("Probe stopped: %s\n", error->message);
  g_print ("Probe finished; reader release/close attempted. Interrupt activity alone does not establish wake-on-touch.\n");
  return g_cancellable_is_cancelled (cancel) ? 130 : (ok ? 0 : 1);
}
