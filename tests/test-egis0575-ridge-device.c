/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Real public FpDevice/GTask lifecycle with synthetic USB frames only.
 * Open/close transport is virtualized; enrollment/verification APIs are real.
 */
#include "drivers_api.h"
static void fake_submit (FpiUsbTransfer *,
                         guint,
                         GCancellable *,
                         FpiUsbTransferCallback,
                         gpointer);
static guint
fast_timeout (guint delay, GSourceFunc callback, gpointer data)
{
  return g_timeout_add (delay == 80 ? 1 : delay, callback, data);
}
#define fpi_usb_transfer_submit fake_submit
#define g_timeout_add fast_timeout
#include "egis0575.c"
#undef g_timeout_add

static guint submitted;
static gboolean wrong_finger;
static GCancellable *cancel_enroll;
typedef struct { FpiUsbTransfer        *transfer;
                 FpiUsbTransferCallback callback;
                 gpointer               data;
                 GCancellable          *cancel;
} Pending;

static gboolean
fake_dispatch (gpointer data)
{
  Pending *pending = data;
  FpiUsbTransfer *t = pending->transfer;
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (t->device);
  GError *error = NULL;

  t->actual_length = t->length;
  if (g_cancellable_is_cancelled (pending->cancel))
    {
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Synthetic cancellation");
    }
  else if (t->endpoint == EH575_EP_OUT)
    {                                     /* Commands acknowledged below. */
    }
  else if (t->length == 64)
    {
      guint8 op = self->command.data[4];
      t->actual_length = 7 + ((op == 0x62 || op == 0x63 || op == 0x71) ? self->command.data[6] : 0);
      memcpy (t->buffer, "SIGE", 4);
      t->buffer[4] = self->command.data[5];
      t->buffer[5] = self->mode == COMMAND_DC_READ ? 32 : self->command.data[6];
      t->buffer[6] = 1;
    }
  else if (t->length == 512)
    {
      error = g_error_new_literal (G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT, "No optional ACK");
    }
  else
    {
      t->actual_length = self->frame_used ? 236 : 5120;
      gboolean contact = !self->activating && self->image_state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF;
      for (gssize i = 0; i < t->actual_length; i++)
        {
          guint index = i + self->frame_used;
          guint value = index * 0x9e3779b1U ^ (wrong_finger ? 0x75abe8U : 0x957e23U);
          value ^= value >> 16;
          value *= 0x7feb352dU;
          value ^= value >> 15;
          int bg = index % 7 < 3 ? 12 : -12;
          t->buffer[i] = 128 + bg + (contact ? (int) (value % 121) - 60 : 0);
        }
    }
  pending->callback (t, FP_DEVICE (self), pending->data, error);
  fpi_usb_transfer_unref (t);
  g_object_unref (pending->cancel);
  g_free (pending);
  return G_SOURCE_REMOVE;
}

static void
fake_submit (FpiUsbTransfer *t, guint timeout, GCancellable *cancel, FpiUsbTransferCallback callback, gpointer data)
{
  Pending *pending = g_new (Pending, 1);

  *pending = (Pending){t, callback, data, g_object_ref (cancel)};
  submitted++;
  g_idle_add (fake_dispatch, pending);
}

static void
fake_open (FpDevice *dev)
{
  fpi_device_open_complete (dev, NULL);
}
static void
fake_close (FpDevice *dev)
{
  fpi_device_close_complete (dev, NULL);
}

static FpDevice *
new_device (void)
{
  FpDeviceClass *klass = g_type_class_ref (fpi_device_egis0575_get_type ());

  klass->type = FP_DEVICE_TYPE_VIRTUAL;
  klass->open = fake_open;
  klass->close = fake_close;
  klass->temp_hot_seconds = -1;
  FpDevice *dev = g_object_new (fpi_device_egis0575_get_type (), NULL);
  g_type_class_unref (klass);
  g_autoptr(GError) error = NULL;
  g_assert_true (fp_device_open_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_assert_nonnull (FP_DEVICE_GET_CLASS (dev)->enroll);
  g_assert_true (fp_device_has_feature (dev, FP_DEVICE_FEATURE_VERIFY));
  return dev;
}

static void
progress (FpDevice *dev, gint stage, FpPrint *print, gpointer data, GError *error)
{
  g_assert_no_error (error);
  g_assert_cmpint (stage, >=, 1);
  if (cancel_enroll)
    g_cancellable_cancel (cancel_enroll);
}

static FpPrint *
enroll (FpDevice *dev, GCancellable *cancel, GError **error)
{
  g_autoptr(FpPrint) template = fp_print_new (dev);
  g_object_ref_sink (template);
  fp_print_set_finger (template, FP_FINGER_RIGHT_INDEX);
  return fp_device_enroll_sync (dev, template, cancel, progress, NULL, error);
}

static gboolean
cancel_when_matching (gpointer data)
{
  gpointer *items = data;

  if (!FPI_DEVICE_EGIS0575 (items[0])->matching)
    return G_SOURCE_CONTINUE;
  g_cancellable_cancel (items[1]);
  return G_SOURCE_REMOVE;
}

static void
assert_clean (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  g_assert_false (self->running);
  g_assert_false (self->matching);
  g_assert_false (self->pending);
  g_assert_null (self->io_cancel);
  g_assert_null (self->enrollment);
  g_assert_null (self->enroll_print);
  g_assert_null (self->capture_image);
  g_assert_cmpuint (self->timer, ==, 0);
}

static void
test_roundtrip (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  wrong_finger = FALSE;
  g_autoptr(FpPrint) print = enroll (dev, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  assert_clean (dev);
  guint8 *bytes;
  gsize length;
  g_assert_true (fp_print_serialize (print, &bytes, &length, &error));
  g_autoptr(FpPrint) restored = fp_print_deserialize (bytes, length, &error);
  memset (bytes, 0, length);
  g_free (bytes);
  g_assert_no_error (error);
  g_assert_true (fp_print_equal (print, restored));
  gboolean match;
  g_assert_true (fp_device_verify_sync (dev, restored, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  assert_clean (dev);
  wrong_finger = TRUE;
  g_assert_true (fp_device_verify_sync (dev, restored, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (match);
  assert_clean (dev);
  wrong_finger = FALSE;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  gpointer items[] = {dev, cancel};
  g_timeout_add (1, cancel_when_matching, items);
  g_assert_false (fp_device_verify_sync (dev, restored, cancel, NULL, NULL, &match, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  assert_clean (dev);
  g_assert_true (fp_device_verify_sync (dev, restored, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  assert_clean (dev);
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
}

static void
test_cancel_enroll (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  cancel_enroll = cancel;
  g_autoptr(FpPrint) print = enroll (dev, cancel, &error);
  cancel_enroll = NULL;
  g_assert_null (print);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  assert_clean (dev);
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
}

static void
test_malformed (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = fp_print_new (dev);
  g_object_ref_sink (print);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", g_variant_new_string ("wrong-schema"), NULL);
  guint before = submitted;
  gboolean match;
  g_assert_false (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_clear_error (&error);
  g_assert_cmpuint (submitted, ==, before);
  assert_clean (dev);
  for (guint variant = 0; variant < 3; variant++)
    {
      GVariantBuilder builder;
      guint8 raw[EH575_RIDGE_SIZE] = {0};
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));
      for (guint i = 0; i < (variant == 2 ? 25 : 6); i++)
        g_variant_builder_add (&builder, "(@ay@ay)",
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, raw, variant == 1 ? EH575_RIDGE_SIZE - 1 : EH575_RIDGE_SIZE, 1),
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, raw, EH575_RIDGE_SIZE, 1));
      g_autoptr(GVariant) data = g_variant_ref_sink (g_variant_new ("(s@a(ayay))", variant == 0 ? "unknown-version" : EH575_RIDGE_SCHEMA,
                                                                    g_variant_builder_end (&builder)));
      g_object_set (print, "fpi-data", data, NULL);
      g_assert_false (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
      g_clear_error (&error);
      g_assert_cmpuint (submitted, ==, before);
      assert_clean (dev);
    }
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/egis0575-ridge/public-roundtrip-match-reject-cancel-reactivate", test_roundtrip);
  g_test_add_func ("/egis0575-ridge/cancel-enrollment-progress", test_cancel_enroll);
  g_test_add_func ("/egis0575-ridge/malformed-template-no-usb", test_malformed);
  return g_test_run ();
}
