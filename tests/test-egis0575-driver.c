/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Deterministic USB/lifecycle tests. No reader, images from disk or system bus.
 * These test the driver callbacks, not the real image-device core or matcher.
 */
#include "drivers_api.h"

static GCancellable *test_get_cancellable (FpDevice *dev);
static void test_submit (FpiUsbTransfer        *transfer,
                         guint                  timeout,
                         GCancellable          *cancel,
                         FpiUsbTransferCallback callback,
                         gpointer               data);
static void test_activate_complete (FpImageDevice *dev,
                                    GError        *error);
static void test_deactivate_complete (FpImageDevice *dev,
                                      GError        *error);
static void test_session_error (FpImageDevice *dev,
                                GError        *error);
static void test_finger (FpImageDevice *dev,
                         gboolean       present);
static void test_image (FpImageDevice *dev,
                        FpImage       *image);
static void test_retry (FpImageDevice *dev,
                        FpDeviceRetry  reason);

#define fpi_device_get_cancellable test_get_cancellable
#define fpi_usb_transfer_submit test_submit
#define fpi_image_device_activate_complete test_activate_complete
#define fpi_image_device_deactivate_complete test_deactivate_complete
#define fpi_image_device_session_error test_session_error
#define fpi_image_device_report_finger_status test_finger
#define fpi_image_device_image_captured test_image
#define fpi_image_device_retry_scan test_retry
#include "egis0575.c"

typedef enum { GOOD, BAD_ACK, TRUNCATED, OVERFLOW, CANCEL_CALIBRATION, CANCEL_COMPARISON, EARLY_FINGER } Scenario;
static Scenario scenario;
static GCancellable *parent_cancel;
static FpiUsbTransfer *queued;
static GCancellable *queued_cancel;
static FpiUsbTransferCallback queued_cb;
static gpointer queued_data;
static guint activated, deactivated, errors, captured, submitted, frames;
static int dc;

static GCancellable *
test_get_cancellable (FpDevice *dev)
{
  return parent_cancel;
}

static void
test_submit (FpiUsbTransfer *transfer, guint timeout, GCancellable *cancel,
             FpiUsbTransferCallback callback, gpointer data)
{
  g_assert_null (queued);
  g_assert_cmpuint (timeout, >, 0);
  queued = transfer;
  queued_cb = callback;
  queued_data = data;
  queued_cancel = g_object_ref (cancel);
  submitted++;
}

static void
test_activate_complete (FpImageDevice *dev, GError *error)
{
  activated++;
  if (error)
    {
      errors++;
      g_error_free (error);
    }
  else
    {
      dev_change_state (dev, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON);
    }
}

static void
test_deactivate_complete (FpImageDevice *dev, GError *error)
{
  deactivated++;
  g_assert_no_error (error);
  dev_change_state (dev, FPI_IMAGE_DEVICE_STATE_INACTIVE);
}

static void
test_session_error (FpImageDevice *dev, GError *error)
{
  g_assert_nonnull (error);
  errors++;
  g_error_free (error);
  dev_change_state (dev, FPI_IMAGE_DEVICE_STATE_DEACTIVATING);
  dev_deactivate (dev);
}

static void
test_finger (FpImageDevice *dev, gboolean present)
{
  if (present)
    {
      dev_change_state (dev, FPI_IMAGE_DEVICE_STATE_CAPTURE);
    }
  else
    {
      dev_change_state (dev, FPI_IMAGE_DEVICE_STATE_DEACTIVATING);
      dev_deactivate (dev);
    }
}

static void
test_image (FpImageDevice *dev, FpImage *image)
{
  g_assert_cmpuint (image->width, ==, 206);
  g_assert_cmpuint (image->height, ==, 104);
  captured++;
  g_object_unref (image);
  if (scenario == CANCEL_COMPARISON)
    {
      dev_change_state (dev, FPI_IMAGE_DEVICE_STATE_DEACTIVATING);
      dev_deactivate (dev);
    }
  else
    {
      dev_change_state (dev, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF);
    }
}

static void
test_retry (FpImageDevice *dev, FpDeviceRetry reason)
{
  dev_change_state (dev, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF);
}

static void
dispatch (FpDeviceEgis0575 *self)
{
  FpiUsbTransfer *transfer = queued;
  FpiUsbTransferCallback callback = queued_cb;

  g_autoptr(GCancellable) cancel = queued_cancel;
  gpointer data = queued_data;
  GError *error = NULL;
  queued = NULL;
  queued_cancel = NULL;
  transfer->actual_length = transfer->length;
  if (g_cancellable_is_cancelled (cancel))
    {
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "test cancellation");
    }
  else if (transfer->endpoint == EH575_EP_OUT)
    {
      if (transfer->buffer[4] == 0x61 && transfer->buffer[5] == 0x0f)
        dc = transfer->buffer[6];
    }
  else if (transfer->length == 64)
    {
      uint8_t op = self->command.data[4];
      transfer->actual_length = 7 + ((op == 0x62 || op == 0x63 || op == 0x71) ? self->command.data[6] : 0);
      memcpy (transfer->buffer, "SIGE", 4);
      transfer->buffer[4] = self->command.data[5];
      transfer->buffer[5] = self->mode == COMMAND_DC_READ ? dc : self->command.data[6];
      transfer->buffer[6] = scenario == BAD_ACK ? 0 : 1;
    }
  else if (transfer->length == 512)
    {
      error = g_error_new_literal (G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT, "optional ACK absent");
    }
  else
    {
      int base = MAX (0, MIN (255, 128 + (dc - 32) * 23));
      int amplitude = self->activating ? 12 : (captured ? 12 : 40);
      if (scenario == EARLY_FINGER)
        amplitude = 40;
      transfer->actual_length = self->frame_used ? 236 : 5120;
      if (scenario == OVERFLOW)
        transfer->actual_length = 8192;
      if (scenario == TRUNCATED && self->frame_used)
        transfer->actual_length = 0;
      for (gssize i = 0; i < transfer->actual_length; i++)
        transfer->buffer[i] = MAX (0, MIN (255, base + ((i + self->frame_used) % 7 < 3 ? amplitude : -amplitude)));
      if (self->frame_used == 0)
        frames++;
      if (scenario == CANCEL_CALIBRATION && frames == 2)
        {
          g_cancellable_cancel (parent_cancel);
          error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancel background calibration");
        }
    }
  callback (transfer, FP_DEVICE (self), data, error);
  fpi_usb_transfer_unref (transfer);
}

static FpDeviceEgis0575 *
new_device (Scenario selected)
{
  scenario = selected;
  activated = deactivated = errors = captured = submitted = frames = 0;
  dc = 32;
  parent_cancel = g_cancellable_new ();
  return g_object_new (fpi_device_egis0575_get_type (), NULL);
}

static void
run_until_stopped (FpDeviceEgis0575 *self)
{
  gint64 deadline = g_get_monotonic_time () + 5000000;

  while (self->running && g_get_monotonic_time () < deadline)
    {
      if (queued)
        dispatch (self);
      else
        g_main_context_iteration (NULL, TRUE);
    }
  g_assert_false (self->running);
  g_assert_null (queued);
  g_assert_cmpuint (self->timer, ==, 0);
  g_assert_null (self->io_cancel);
}

static void
test_failure (gconstpointer data)
{
  FpDeviceEgis0575 *self = new_device (GPOINTER_TO_INT (data));

  dev_activate (FP_IMAGE_DEVICE (self));
  run_until_stopped (self);
  g_assert_cmpuint (activated, ==, 1);
  g_assert_cmpuint (errors, ==, 1);
  g_assert_cmpuint (captured, ==, 0);
  g_assert_true (self->poisoned);
  g_object_unref (self);
  g_object_unref (parent_cancel);
}

static void
test_capture (gconstpointer data)
{
  FpDeviceEgis0575 *self = new_device (GPOINTER_TO_INT (data));

  /* Also exercise the bounded DC search rather than only the fast path. */
  dc = 36;
  dev_activate (FP_IMAGE_DEVICE (self));
  run_until_stopped (self);
  g_assert_cmpuint (activated, ==, 1);
  g_assert_cmpuint (deactivated, ==, 1);
  g_assert_cmpuint (errors, ==, 0);
  g_assert_cmpuint (captured, ==, 1);
  g_assert_cmpint (dc, ==, 32);
  g_object_unref (self);
  g_object_unref (parent_cancel);
}

static void
test_initial_cancel (void)
{
  FpDeviceEgis0575 *self = new_device (GOOD);

  dev_activate (FP_IMAGE_DEVICE (self));
  g_cancellable_cancel (parent_cancel);
  run_until_stopped (self);
  g_assert_cmpuint (submitted, ==, 1);
  g_assert_cmpuint (errors, ==, 1);
  g_object_unref (self);
  g_object_unref (parent_cancel);
}

static void
test_clean_reactivation (void)
{
  FpDeviceEgis0575 *self = new_device (GOOD);

  dev_activate (FP_IMAGE_DEVICE (self));
  run_until_stopped (self);
  g_assert_false (self->poisoned);
  captured = 0;
  dev_activate (FP_IMAGE_DEVICE (self));
  run_until_stopped (self);
  g_assert_cmpuint (activated, ==, 2);
  g_assert_cmpuint (deactivated, ==, 2);
  g_assert_cmpuint (captured, ==, 1);
  g_assert_cmpuint (errors, ==, 0);
  g_object_unref (self);
  g_object_unref (parent_cancel);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/egis0575/bad-ack", GINT_TO_POINTER (BAD_ACK), test_failure);
  g_test_add_data_func ("/egis0575/truncated", GINT_TO_POINTER (TRUNCATED), test_failure);
  g_test_add_data_func ("/egis0575/overflow", GINT_TO_POINTER (OVERFLOW), test_failure);
  g_test_add_data_func ("/egis0575/calibration-cancel", GINT_TO_POINTER (CANCEL_CALIBRATION), test_failure);
  g_test_add_data_func ("/egis0575/early-finger", GINT_TO_POINTER (EARLY_FINGER), test_failure);
  g_test_add_data_func ("/egis0575/capture-lift", GINT_TO_POINTER (GOOD), test_capture);
  g_test_add_data_func ("/egis0575/capture-cancel", GINT_TO_POINTER (CANCEL_COMPARISON), test_capture);
  g_test_add_func ("/egis0575/initial-cancel", test_initial_cancel);
  g_test_add_func ("/egis0575/clean-reactivation", test_clean_reactivation);
  return g_test_run ();
}
