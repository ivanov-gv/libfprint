/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Isolated unprivileged research, not a wake daemon or authenticator.
 * Reads only tested interrupt endpoints or uses characterized acquisition.
 * No speculative register writes, reset, firmware upload, images or templates saved.
 */
#include <gusb.h>
#include <glib-unix.h>
#include <stdio.h>
#include <unistd.h>
#include "egis0575-touch.h"

static gint64 deadline;

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

static gboolean
command (GUsbDevice *usb, const Eh575Command *cmd, GCancellable *cancel, GError **error)
{
  uint8_t bytes[20], reply[512];
  gsize length = 0;

  if (g_get_monotonic_time () >= deadline)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Probe deadline expired");
      return FALSE;
    }
  memcpy (bytes, cmd->data, cmd->length);
  if (!g_usb_device_bulk_transfer (usb, EH575_EP_OUT, bytes, cmd->length, &length, remaining (500), cancel, error))
    return FALSE;
  if (length != cmd->length)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Short command write");
      return FALSE;
    }
  if (!g_usb_device_bulk_transfer (usb, EH575_EP_IN, reply, sizeof reply, &length, remaining (500), cancel, error))
    return FALSE;
  if (!eh575_reply_valid (cmd, reply, length))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unexpected initialization/rearm reply");
      return FALSE;
    }
  return TRUE;
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
interrupt_probe (GUsbDevice *usb, GCancellable *cancel, GError **error)
{
  const char *phases[] = {"Keep the reader EMPTY", "TOUCH and HOLD any finger", "LIFT fully; leave the reader empty"};
  Eh575EventStats stats[2] = {0};

  for (size_t phase = 0; phase < G_N_ELEMENTS (phases); phase++)
    {
      g_print ("Phase %zu/3: %s for six seconds.\n", phase + 1, phases[phase]);
      fflush (stdout);
      gint64 until = MIN (deadline, g_get_monotonic_time () + 6000000);
      stats[0].packets = stats[0].changes = stats[1].packets = stats[1].changes = 0;
      while (g_get_monotonic_time () < until && !g_cancellable_is_cancelled (cancel))
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
      for (guint ep = 0; ep < 2; ep++)
        g_print ("phase=%zu endpoint=0x%02x packets=%u payload_changes=%u\n", phase + 1, 0x83 + ep, stats[ep].packets, stats[ep].changes);
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

int
main (int argc, char **argv)
{
  const char *mode = argc == 2 ? argv[1] : "";
  gboolean initialized = !strcmp (mode, "interrupt-initialized") || !strcmp (mode, "touch");

  if (argc != 2 || (!initialized && strcmp (mode, "interrupt") && strcmp (mode, "open")) || geteuid () == 0)
    {
      g_printerr ("Run WITHOUT sudo: eh575-touch-probe open|interrupt|interrupt-initialized|touch\n");
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
  ok = !strcmp (mode, "touch") ? touch_probe (usb, cancel, &error) : interrupt_probe (usb, cancel, &error);
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
