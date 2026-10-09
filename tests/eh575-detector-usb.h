/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Shared bounded USB backend for the isolated probe and sleep worker.
 * Callers own the deadline, cancellation and restoration lifecycle.
 */
#pragma once
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
  guint exchanges;
  guint8 last_opcode, last_register, busy_bit;
  gboolean busy_known;
} ProbeIO;

typedef enum
{
  EH575_HOLD_CLAIM,
  EH575_HOLD_HANDLE,
  EH575_CLOSE_HANDLE,
} Eh575Handoff;

typedef struct
{
  gboolean opened, claimed, contact, restored;
} ProbeState;

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
  io->exchanges++;
  io->last_opcode = cmd->data[4];
  io->last_register = cmd->data[5];
  io->busy_known = FALSE;
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
  if (cmd->data[4] == 0x60 && cmd->data[5] == 0x40)
    {
      io->busy_known = TRUE;
      io->busy_bit = reply[5] & 0x80;
    }
  g_usleep (2000);
  return TRUE;
}

static gboolean
command (GUsbDevice *usb, const Eh575Command *cmd, GCancellable *cancel, GError **error)
{
  ProbeIO io = {.usb = usb, .cancel = cancel, .error = error};
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
