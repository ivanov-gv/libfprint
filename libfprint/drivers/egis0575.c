/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 fingerprint contributors
 * Experimental stationary driver for USB 1c7a:0575, revision 1072.
 * Default image mode retains NBIS; opt-in ridge mode owns native matching.
 * No vendor matcher, firmware upload or Python bridge.
 */
#define FP_COMPONENT "egis0575"
#include "drivers_api.h"
#include "egis0575.h"
#include "egis0575-calibration.h"
#include "config.h"
#ifdef EH575_TEST_IMAGE
#undef EH575_EXPERIMENTAL_RIDGE
#define EH575_EXPERIMENTAL_RIDGE 0
#endif
#if EH575_EXPERIMENTAL_RIDGE
#include "egis0575-ridge.h"
typedef FpDevice Eh575Device;
#define EH575_DEVICE(dev) FP_DEVICE (dev)
#else
typedef FpImageDevice Eh575Device;
#define EH575_DEVICE(dev) FP_IMAGE_DEVICE (dev)
#endif

typedef enum { COMMAND_INIT, COMMAND_DC_READ, COMMAND_DC_WRITE, COMMAND_REARM } CommandMode;

struct _FpDeviceEgis0575
{
  Eh575Device              parent;
  GCancellable            *io_cancel;
  gulong                   cancel_handler;
  guint                    timer;
  gboolean                 running, pending, activating, stopping, poisoned, wire_dirty;
  CommandMode              mode;
  FpiImageDeviceState      image_state;
  size_t                   command_index;
  Eh575Command             command;
  uint8_t                  frame[EH575_FRAME_SIZE], samples[5][EH575_FRAME_SIZE], background[EH575_FRAME_SIZE];
  size_t                   frame_used;
  gint64                   frame_deadline, operation_deadline, settle_until;
  unsigned int             sample_count, clear_count, bad_count;
  int                      dc, best_dc, low, high;
  double                   best_distance;
  gboolean                 searching, final_measurement;
  Eh575Calibration         calibration;
  Eh575CalibrationIdentity calibration_identity;
  gboolean                 calibration_candidate, calibration_identity_valid, early_notified;
#if EH575_EXPERIMENTAL_RIDGE
  FpiDeviceAction          action;
  GPtrArray               *enrollment;
  FpPrint                 *enroll_print;
  FpImage                 *capture_image;
  GError                  *action_error;
  Eh575RidgeResult         match;
  guint                    stages;
  gboolean                 matching;
#endif
};
#if EH575_EXPERIMENTAL_RIDGE
G_DECLARE_FINAL_TYPE (FpDeviceEgis0575, fpi_device_egis0575, FPI, DEVICE_EGIS0575, FpDevice);
G_DEFINE_TYPE (FpDeviceEgis0575, fpi_device_egis0575, FP_TYPE_DEVICE);
#else
G_DECLARE_FINAL_TYPE (FpDeviceEgis0575, fpi_device_egis0575, FPI, DEVICE_EGIS0575, FpImageDevice);
G_DEFINE_TYPE (FpDeviceEgis0575, fpi_device_egis0575, FP_TYPE_IMAGE_DEVICE);
#endif

static void send_command (FpDeviceEgis0575 *self);
static void start_frame (FpDeviceEgis0575 *self);
static void read_frame (FpDeviceEgis0575 *self);
static void process_frame (FpDeviceEgis0575 *self);

static const char *
calibration_directory (FpDeviceEgis0575 *self)
{
  /* Virtual test devices can use memory only. Never write to a guessed path. */
  return self->calibration_identity.bus && self->calibration_identity.address ?
         g_getenv ("FP_EH575_CALIBRATION_DIR") : NULL;
}

static void
forget_calibration (FpDeviceEgis0575 *self)
{
  memset (&self->calibration, 0, sizeof self->calibration);
  self->calibration_candidate = FALSE;
  if (self->calibration_identity_valid)
    eh575_calibration_store (&self->calibration, &self->calibration_identity, calibration_directory (self));
}
#if EH575_EXPERIMENTAL_RIDGE
static void finish (FpDeviceEgis0575 *self,
                    GError           *error);
static void dev_activate (Eh575Device *dev);
static void dev_deactivate (Eh575Device *dev);
#include "egis0575-ridge-adapter.h"
#endif

static void
finish (FpDeviceEgis0575 *self, GError *error)
{
  self->running = FALSE;
  self->pending = FALSE;
  g_clear_handle_id (&self->timer, g_source_remove);
  if (self->cancel_handler)
    {
      g_signal_handler_disconnect (fpi_device_get_cancellable (FP_DEVICE (self)), self->cancel_handler);
      self->cancel_handler = 0;
    }
  g_clear_object (&self->io_cancel);
  /* An interrupted wire transaction may leave an ACK or image queued. */
  if (self->wire_dirty || (error && error->domain != FP_DEVICE_RETRY))
    {
      self->poisoned = TRUE;
      forget_calibration (self);
    }
  memset (self->samples, 0, sizeof self->samples);
  memset (self->frame, 0, sizeof self->frame);
  memset (self->background, 0, sizeof self->background);
  if (self->stopping)
    {
      g_clear_error (&error);
      fpi_image_device_deactivate_complete (EH575_DEVICE (self), NULL);
    }
  else if (self->activating)
    {
      fpi_image_device_activate_complete (EH575_DEVICE (self), error);
    }
  else
    {
      fpi_image_device_session_error (EH575_DEVICE (self), error);
    }
}

static gboolean
check_transfer (FpDeviceEgis0575 *self, GError *error)
{
  self->pending = FALSE;
  if (self->stopping || error)
    {
      finish (self, error);
      return FALSE;
    }
  if (g_get_monotonic_time () >= self->operation_deadline)
    {
      finish (self, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "EH575 scan deadline exceeded"));
      return FALSE;
    }
  return TRUE;
}

static void
protocol_error (FpDeviceEgis0575 *self)
{
  finish (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Invalid EH575 response; close and reopen the device"));
}

static void
submit_transfer (FpDeviceEgis0575 *self, FpiUsbTransfer *transfer, guint timeout,
                 FpiUsbTransferCallback callback)
{
  gint64 remaining = self->operation_deadline - g_get_monotonic_time ();

  if (remaining <= 0)
    {
      fpi_usb_transfer_unref (transfer);
      finish (self, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "EH575 scan deadline exceeded"));
      return;
    }
  self->pending = TRUE;
  fpi_usb_transfer_submit (transfer, MIN (timeout, MAX (1, remaining / 1000)),
                           self->io_cancel, callback, NULL);
}

static gboolean
continue_command (gpointer data)
{
  FpDeviceEgis0575 *self = data;

  self->timer = 0;
  send_command (self);
  return G_SOURCE_REMOVE;
}

static gboolean
continue_frame (gpointer data)
{
  FpDeviceEgis0575 *self = data;

  self->timer = 0;
  if (g_cancellable_is_cancelled (self->io_cancel))
    finish (self, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "EH575 scan cancelled"));
  else
    start_frame (self);
  return G_SOURCE_REMOVE;
}

static guint
frame_interval (FpDeviceEgis0575 *self, gint64 now)
{
  /* React faster to initial contact, but retain the 80 ms interval between
   * captured samples and lift checks. Do not shorten the 250 ms settling gate.
   * Its final wait need not round up to another full polling interval.
   */
  if (self->image_state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    return 20;
  if (self->image_state == FPI_IMAGE_DEVICE_STATE_CAPTURE && self->sample_count == 0 && now < self->settle_until)
    return MIN (80, MAX (1, (self->settle_until - now + 999) / 1000));
  return 80;
}

static void
set_dc (FpDeviceEgis0575 *self, int dc)
{
  if (self->calibration_candidate && dc != self->calibration.bytes[10])
    forget_calibration (self);
  self->dc = dc;
  self->sample_count = 0;
  self->mode = COMMAND_DC_WRITE;
  self->command = (Eh575Command){ 7, { 'E', 'G', 'I', 'S', 0x61, 0x0f, dc } };
  send_command (self);
}

static void
drain_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  /* The trailing ACK is optional. Only timeout, not other USB errors, is normal. */
  if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
    {
      g_clear_error (&error);
    }
  else if (!error && (transfer->actual_length != 7 || !eh575_status_valid (transfer->buffer, transfer->actual_length)))
    {
      self->pending = FALSE;
      protocol_error (self);
      return;
    }
  if (check_transfer (self, error))
    {
      self->wire_dirty = FALSE;
      process_frame (self);
    }
}

static void
frame_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiUsbTransfer *next;

  if (!check_transfer (self, error))
    return;
  if (!eh575_append_frame (self->frame, &self->frame_used, transfer->buffer, transfer->actual_length))
    {
      protocol_error (self);
      return;
    }
  if (self->frame_used < EH575_FRAME_SIZE)
    {
      read_frame (self);
      return;
    }
  next = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (next, EH575_EP_IN, 512);
  submit_transfer (self, next, 20, drain_cb);
}

static void
read_frame (FpDeviceEgis0575 *self)
{
  gint64 remaining = self->frame_deadline - g_get_monotonic_time ();
  FpiUsbTransfer *transfer;

  if (remaining <= 0)
    {
      protocol_error (self);
      return;
    }
  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  /* Packet-aligned buffer even for the final 236-byte fragment. */
  fpi_usb_transfer_fill_bulk (transfer, EH575_EP_IN, 8192);
  submit_transfer (self, transfer, MAX (1, remaining / 1000), frame_cb);
}

static void
trigger_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  if (!check_transfer (self, error))
    return;
  self->frame_used = 0;
  self->frame_deadline = MIN (self->operation_deadline, g_get_monotonic_time () + 1500000);
  read_frame (self);
}

static void
command_reply_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  if (!check_transfer (self, error))
    return;
  if (!eh575_reply_valid (&self->command, transfer->buffer, transfer->actual_length))
    {
      protocol_error (self);
      return;
    }
  switch (self->mode)
    {
    case COMMAND_INIT:
      if (++self->command_index < G_N_ELEMENTS (eh575_init_commands))
        {
          self->command = eh575_init_commands[self->command_index];
        }
      else
        {
          self->mode = COMMAND_DC_READ;
          self->command = (Eh575Command){ 7, { 'E', 'G', 'I', 'S', 0x60, 0x0f, 0 } };
        }
      self->timer = g_timeout_add (2, continue_command, self);
      break;

    case COMMAND_DC_READ:
      if (transfer->buffer[5] > 63)
        protocol_error (self);
      else
        set_dc (self, self->calibration_candidate ? self->calibration.bytes[10] : transfer->buffer[5]);
      break;

    case COMMAND_DC_WRITE:
      start_frame (self);
      break;

    case COMMAND_REARM:
      if (++self->command_index < G_N_ELEMENTS (eh575_rearm_commands))
        {
          self->command = eh575_rearm_commands[self->command_index];
          send_command (self);
        }
      else
        {
          FpiUsbTransfer *next = fpi_usb_transfer_new (dev);
          fpi_usb_transfer_fill_bulk_full (next, EH575_EP_OUT, (guint8 *) eh575_trigger.data, eh575_trigger.length, NULL);
          fpi_usb_transfer_set_short_error (next, TRUE);
          submit_transfer (self, next, 500, trigger_cb);
        }
      break;
    }
}

static void
command_sent_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer data, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiUsbTransfer *next;

  if (!check_transfer (self, error))
    return;
  next = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (next, EH575_EP_IN, 64);
  submit_transfer (self, next, 500, command_reply_cb);
}

static void
send_command (FpDeviceEgis0575 *self)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (self));

  fpi_usb_transfer_fill_bulk_full (transfer, EH575_EP_OUT, self->command.data, self->command.length, NULL);
  fpi_usb_transfer_set_short_error (transfer, TRUE);
  self->wire_dirty = TRUE;
  submit_transfer (self, transfer, 500, command_sent_cb);
}

static void
start_frame (FpDeviceEgis0575 *self)
{
  self->mode = COMMAND_REARM;
  self->command_index = 0;
  self->command = eh575_rearm_commands[0];
  send_command (self);
}

static void
calibration_ready (FpDeviceEgis0575 *self, gboolean measured_empty)
{
  if (measured_empty && self->calibration_identity_valid &&
      eh575_calibration_identity (&self->calibration_identity))
    {
      eh575_calibration_record (&self->calibration, &self->calibration_identity, self->dc, self->background);
      eh575_calibration_store (&self->calibration, &self->calibration_identity, calibration_directory (self));
    }
  self->calibration_candidate = FALSE;
  self->early_notified = FALSE;
  self->sample_count = 0;
  self->activating = FALSE;
  self->operation_deadline = g_get_monotonic_time () + 30000000;
  fpi_image_device_activate_complete (EH575_DEVICE (self), NULL);
  if (!self->stopping)
    self->timer = g_timeout_add (frame_interval (self, g_get_monotonic_time ()), continue_frame, self);
}

static void
calibrate_frame (FpDeviceEgis0575 *self)
{
  double mean = 0, clipped = 0;

  /* Discard one settling frame, then measure three independently. */
  if (self->sample_count++ == 0)
    {
      start_frame (self);
      return;
    }
  if (self->calibration_candidate)
    {
      const guint8 *background = self->calibration.bytes + 80;
      double change = eh575_deviation (self->frame, background);
      if (eh575_deviation (self->frame, NULL) >= 20 && eh575_clipped (self->frame) <= .05 && change >= 8)
        {
          /* Only a previously measured empty background may bootstrap contact.
           * Keep the normal settling, quality, five-frame and matching gates.
           * Do not renew the cache's lifetime from a finger-present frame.
           */
          memcpy (self->background, background, EH575_FRAME_SIZE);
          fp_dbg ("Reusing compatible empty-reader calibration for early contact");
          calibration_ready (self, FALSE);
          return;
        }
      if (change > 4 || fabs (eh575_mean (self->frame) - eh575_mean (background)) > 16)
        forget_calibration (self);
    }
  if (eh575_deviation (self->frame, NULL) > 18)
    {
      /* Do not measure exposure or a background from a finger. Restart the
       * complete empty burst after lift, without extending the deadline.
       */
      self->sample_count = 0;
      if (!self->early_notified)
        {
          self->early_notified = TRUE;
          fp_dbg ("No usable calibration: lift finger briefly, then touch again");
#if EH575_EXPERIMENTAL_RIDGE
          if (self->action == FPI_DEVICE_ACTION_VERIFY)
            {
              /* libfprint allows one report per verify operation. Stock
               * fprintd restarts retry results within the same D-Bus request.
               * Suppress repeated retries while the restarted scan waits.
               */
              finish (self, fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER,
                                                      "Lift briefly to calibrate EH575, then touch again"));
              return;
            }
          if (self->action == FPI_DEVICE_ACTION_ENROLL)
            fpi_device_enroll_progress (FP_DEVICE (self), self->stages, NULL,
                                        fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
#endif
        }
      if (self->running && !self->stopping)
        self->timer = g_timeout_add (80, continue_frame, self);
      return;
    }
  memcpy (self->samples[self->sample_count - 2], self->frame, EH575_FRAME_SIZE);
  if (self->sample_count < 4)
    {
      start_frame (self);
      return;
    }
  for (unsigned int i = 0; i < 3; i++)
    {
      mean += eh575_mean (self->samples[i]) / 3;
      clipped += eh575_clipped (self->samples[i]) / 3;
    }
  if (fabs (mean - 128) < self->best_distance)
    {
      self->best_distance = fabs (mean - 128);
      self->best_dc = self->dc;
    }
  if (self->final_measurement || (!self->searching && fabs (mean - 128) <= 16 && clipped <= .005))
    {
      if (mean < 96 || mean > 160 || clipped > .005)
        {
          finish (self, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "EH575 exposure did not stabilize"));
          return;
        }
      eh575_median (self->background, self->samples);
      calibration_ready (self, TRUE);
      return;
    }
  if (self->searching)
    {
      if (mean < 128)
        self->low = self->dc + 1;
      else
        self->high = self->dc - 1;
    }
  self->searching = TRUE;
  if (self->low > self->high)
    {
      self->final_measurement = TRUE;
      set_dc (self, self->best_dc);
    }
  else
    {
      set_dc (self, (self->low + self->high) / 2);
    }
}

static void
process_frame (FpDeviceEgis0575 *self)
{
  Eh575Device *image_dev = EH575_DEVICE (self);
  FpiImageDeviceState state;
  gboolean present;

  if (self->activating)
    {
      calibrate_frame (self);
      return;
    }
  state = self->image_state;
  present = eh575_deviation (self->frame, self->background) >= 8;
  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON && present)
    {
      self->sample_count = self->bad_count = self->clear_count = 0;
      self->settle_until = g_get_monotonic_time () + 250000;
      fpi_image_device_report_finger_status (image_dev, TRUE);
    }
  else if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE && !present)
    {
      fpi_image_device_retry_scan (image_dev, FP_DEVICE_RETRY_TOO_SHORT);
      if (!self->stopping)
        fpi_image_device_report_finger_status (image_dev, FALSE);
    }
  else if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE && g_get_monotonic_time () >= self->settle_until)
    {
      if (eh575_deviation (self->frame, NULL) < 20 || eh575_clipped (self->frame) > .05)
        {
          self->sample_count = 0;
          if (++self->bad_count == 5)
            fpi_image_device_retry_scan (image_dev, FP_DEVICE_RETRY_CENTER_FINGER);
        }
      else
        {
          self->bad_count = 0;
          if (self->sample_count)
            {
              double correlation = eh575_stationary_correlation (self->samples[0], self->frame,
                                                                 self->background);
              if (correlation < .97)
                {
                  /* Wait for a steady burst, not a swipe or an immediate
                   * motion retry. The existing scan deadline stays bounded.
                   */
                  fp_dbg ("Press contact settling: correlation=%.4f; restarting steady burst", correlation);
                  self->sample_count = 0;
                }
            }
          memcpy (self->samples[self->sample_count++], self->frame, EH575_FRAME_SIZE);
          if (self->sample_count ==
#if EH575_EXPERIMENTAL_RIDGE
              (self->action == FPI_DEVICE_ACTION_VERIFY ? 5 : 3)
#else
              3
#endif
             )
            {
#if EH575_EXPERIMENTAL_RIDGE
              ridge_burst (self);
#else
              FpImage *image = fp_image_new (EH575_WIDTH * 2, EH575_HEIGHT * 2);
              eh575_median (self->frame, self->samples);
              eh575_normalize (self->frame, self->frame, self->background);
              eh575_enlarge (image->data, self->frame);
              image->flags = FPI_IMAGE_PARTIAL;
              fp_dbg ("Stationary press captured from three stable frames; raw area %d x %d",
                      EH575_WIDTH, EH575_HEIGHT);
              fpi_image_device_image_captured (image_dev, image);
              self->sample_count = 0;
#endif
            }
        }
    }
  else if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
    {
      self->clear_count = present ? 0 : self->clear_count + 1;
      if (self->clear_count == 3)
        {
          /* Each separate presentation gets its own bounded wait. */
          self->operation_deadline = g_get_monotonic_time () + 30000000;
          fpi_image_device_report_finger_status (image_dev, FALSE);
        }
    }
  if (!self->running)
    return;
#if EH575_EXPERIMENTAL_RIDGE
  if (self->matching)
    return;
#endif
  if (!self->stopping)
    self->timer = g_timeout_add (frame_interval (self, g_get_monotonic_time ()), continue_frame, self);
  else if (!self->pending)
    finish (self, NULL);
}

static void
dev_open (Eh575Device *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  GUsbDevice *usb = fpi_device_get_usb_device (FP_DEVICE (dev));

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) interfaces = NULL;
  gboolean valid = FALSE;
  if (g_usb_device_get_release (usb) != EH575_REVISION)
    error = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED, "Untested EH575 hardware revision");
  else
    interfaces = g_usb_device_get_interfaces (usb, &error);
  if (interfaces)
    {
      for (guint i = 0; i < interfaces->len; i++)
        {
          GUsbInterface *iface = g_ptr_array_index (interfaces, i);
          if (g_usb_interface_get_number (iface) == 0 && g_usb_interface_get_alternate (iface) == 0 &&
              g_usb_interface_get_class (iface) == 255 && g_usb_interface_get_subclass (iface) == 255 &&
              g_usb_interface_get_protocol (iface) == 0)
            {
              g_autoptr(GPtrArray) endpoints = g_usb_interface_get_endpoints (iface);
              unsigned int found = 0;
              if (!endpoints)
                continue;
              for (guint j = 0; j < endpoints->len; j++)
                {
                  GUsbEndpoint *ep = g_ptr_array_index (endpoints, j);
                  /* GUsb's "kind" is bDescriptorType (5), not bmAttributes.
                   * Transfer attributes are not exposed by this GUsb API.
                   */
                  if (g_usb_endpoint_get_kind (ep) == 5 && g_usb_endpoint_get_maximum_packet_size (ep) == 512)
                    {
                      if (g_usb_endpoint_get_address (ep) == EH575_EP_IN)
                        found |= 1;
                      if (g_usb_endpoint_get_address (ep) == EH575_EP_OUT)
                        found |= 2;
                    }
                }
              valid = found == 3;
            }
        }
    }
  if (!error && !valid)
    error = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED, "Unsupported EH575 USB descriptors");
  /* Flags 0: do not detach a kernel driver or reset the device. */
  if (!error)
    g_usb_device_claim_interface (usb, 0, 0, &error);
  self->poisoned = error != NULL;
  self->early_notified = FALSE;
  fpi_image_device_open_complete (dev, g_steal_pointer (&error));
}

static void
dev_close (Eh575Device *dev)
{
  g_autoptr(GError) error = NULL;
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);
  fpi_image_device_close_complete (dev, g_steal_pointer (&error));
}

static void
dev_activate (Eh575Device *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  GCancellable *parent = fpi_device_get_cancellable (FP_DEVICE (dev));

  if (self->poisoned)
    {
      fpi_image_device_activate_complete (dev, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Close and reopen EH575 before another scan"));
      return;
    }
  self->running = self->activating = TRUE;
  self->stopping = self->searching = self->final_measurement = FALSE;
  self->sample_count = 0;
  self->low = 0;
  self->high = 63;
  self->best_distance = 256;
  self->calibration_identity_valid = eh575_calibration_identity (&self->calibration_identity);
  if (FP_DEVICE_GET_CLASS (dev)->type == FP_DEVICE_TYPE_USB)
    {
      GUsbDevice *usb = fpi_device_get_usb_device (FP_DEVICE (dev));
      if (usb)
        {
          self->calibration_identity.bus = g_usb_device_get_bus (usb);
          self->calibration_identity.address = g_usb_device_get_address (usb);
        }
    }
  self->calibration_candidate = FALSE;
  if (self->calibration_identity_valid)
    {
      if (!eh575_calibration_compatible (&self->calibration, &self->calibration_identity))
        eh575_calibration_load (&self->calibration, &self->calibration_identity, calibration_directory (self));
      self->calibration_candidate = eh575_calibration_compatible (&self->calibration, &self->calibration_identity);
    }
  self->operation_deadline = g_get_monotonic_time () + 30000000;
  self->io_cancel = g_cancellable_new ();
  self->cancel_handler = g_signal_connect_swapped (parent, "cancelled", G_CALLBACK (g_cancellable_cancel), self->io_cancel);
  if (g_cancellable_is_cancelled (parent))
    g_cancellable_cancel (self->io_cancel);
  self->mode = COMMAND_INIT;
  self->command_index = 0;
  self->command = eh575_init_commands[0];
  send_command (self);
}

static void G_GNUC_UNUSED
dev_change_state (Eh575Device *dev, FpiImageDeviceState state)
{
  FPI_DEVICE_EGIS0575 (dev)->image_state = state;
}

static void
dev_deactivate (Eh575Device *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  self->stopping = TRUE;
  g_clear_handle_id (&self->timer, g_source_remove);
  if (self->io_cancel)
    g_cancellable_cancel (self->io_cancel);
  if (!self->pending)
    {
      if (self->running)
        finish (self, NULL);
      else
        fpi_image_device_deactivate_complete (dev, NULL);
    }
}

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x0575 },
  { .vid = 0, .pid = 0 },
};

static void
fpi_device_egis0575_init (FpDeviceEgis0575 *self)
{
}

static void
fpi_device_egis0575_class_init (FpDeviceEgis0575Class *klass)
{
  FpDeviceClass *device_class = FP_DEVICE_CLASS (klass);

#if !EH575_EXPERIMENTAL_RIDGE
  FpImageDeviceClass *image_class = FP_IMAGE_DEVICE_CLASS (klass);
#endif

  device_class->id = "egis0575";
  device_class->full_name = "EgisTec EH575 (experimental)";
  device_class->type = FP_DEVICE_TYPE_USB;
  device_class->id_table = id_table;
  device_class->scan_type = FP_SCAN_TYPE_PRESS;
#if EH575_EXPERIMENTAL_RIDGE
  device_class->full_name = "EgisTec EH575 (experimental stationary ridge)";
  device_class->nr_enroll_stages = EH575_RIDGE_STAGES;
  device_class->open = dev_open;
  device_class->close = dev_close;
  device_class->enroll = ridge_start;
  device_class->verify = ridge_start;
  device_class->capture = ridge_start;
  device_class->cancel = ridge_cancel;
  fpi_device_class_auto_initialize_features (device_class);
#else
  device_class->nr_enroll_stages = 10;
  image_class->img_open = dev_open;
  image_class->img_close = dev_close;
  image_class->activate = dev_activate;
  image_class->deactivate = dev_deactivate;
  image_class->change_state = dev_change_state;
  image_class->img_width = EH575_WIDTH * 2;
  image_class->img_height = EH575_HEIGHT * 2;
  /* Keep libfprint's default Bozorth3 threshold (40). */
#endif
}
