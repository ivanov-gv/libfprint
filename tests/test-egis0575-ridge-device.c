/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Real public FpDevice/GTask lifecycle with synthetic USB frames only.
 * Open/close transport is virtualized; enrollment/verification APIs are real.
 */
#include "drivers_api.h"
#include "egis0575-calibration.h"
#include <glib/gstdio.h>
static const char *test_platform_id;
static char *test_cache_directory;
static gboolean
fake_identity (Eh575CalibrationIdentity *identity, const char *platform_id)
{
  return eh575_calibration_identity (identity, test_platform_id ? test_platform_id : platform_id);
}
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
#define eh575_calibration_identity fake_identity
#define g_timeout_add fast_timeout
#include "egis0575.c"
#undef g_timeout_add
#undef eh575_calibration_identity

static guint submitted;
static gboolean wrong_finger;
static gboolean early_contact;
static guint calibration_frames, release_after;
static int register_dc = 32;
static gboolean bad_saved_frames, poor_saved_contact;
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
    {
      if (t->buffer[4] == 0x61 && t->buffer[5] == 0x0f)
        register_dc = t->buffer[6];
    }
  else if (t->length == 64)
    {
      guint8 op = self->command.data[4];
      t->actual_length = 7 + ((op == 0x62 || op == 0x63 || op == 0x71) ? self->command.data[6] : 0);
      memcpy (t->buffer, "SIGE", 4);
      t->buffer[4] = self->command.data[5];
      t->buffer[5] = self->mode == COMMAND_DC_READ ? register_dc : self->command.data[6];
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
      if (self->activating && early_contact)
        {
          if (!self->frame_used)
            calibration_frames++;
          contact = !release_after || calibration_frames <= release_after;
        }
      for (gssize i = 0; i < t->actual_length; i++)
        {
          guint index = i + self->frame_used;
          guint value = index * 0x9e3779b1U ^ (wrong_finger ? 0x75abe8U : 0x957e23U);
          value ^= value >> 16;
          value *= 0x7feb352dU;
          value ^= value >> 15;
          int bg = index % 7 < 3 ? 12 : -12;
          t->buffer[i] = 128 + bg + (contact ? (int) (value % 121) - 60 : 0);
          if (poor_saved_contact && contact && !self->activating && self->using_saved_calibration)
            t->buffer[i] = 128 + bg + (index < EH575_FRAME_SIZE * .15 ? (value & 1 ? 80 : -80) : 0);
          if (bad_saved_frames && !self->activating && self->using_saved_calibration &&
              self->image_state == FPI_IMAGE_DEVICE_STATE_CAPTURE &&
              (self->action != FPI_DEVICE_ACTION_ENROLL || self->stages >= 3))
            t->buffer[i] = index % 2 ? 0 : 255;
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
  g_assert_true (FPI_DEVICE_EGIS0575 (dev)->calibration.valid);
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
test_early_contact (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  wrong_finger = early_contact = FALSE;
  g_autoptr(FpPrint) print = enroll (dev, NULL, &error);
  g_assert_no_error (error);
  guint8 saved[EH575_CALIBRATION_SIZE];
  memcpy (saved, self->calibration.bytes, sizeof saved);
  early_contact = TRUE;
  release_after = calibration_frames = 0;
  gboolean match;
  g_assert_true (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  g_assert_cmpuint (calibration_frames, ==, 2);
  g_assert_cmpmem (saved, sizeof saved, self->calibration.bytes, sizeof saved);
  assert_clean (dev);
  wrong_finger = TRUE;
  g_assert_true (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (match);
  assert_clean (dev);
  wrong_finger = FALSE;
  memset (&self->calibration, 0, sizeof self->calibration);
  g_assert_false (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_REMOVE_FINGER);
  g_clear_error (&error);
  g_assert_false (self->poisoned);
  g_assert_true (self->early_notified);
  assert_clean (dev);
  /* Model fprintd's automatic restart of a non-terminal D-Bus retry. The
   * restarted operation waits through contact, then completes after lift.
   */
  release_after = 6;
  calibration_frames = 0;
  g_assert_true (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  g_assert_cmpuint (calibration_frames, >=, 10);
  g_assert_false (self->early_notified);
  g_assert_true (self->calibration.valid);
  assert_clean (dev);
  early_contact = FALSE;
  release_after = 0;
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
}

static gboolean
cancel_early_wait (gpointer data)
{
  if (calibration_frames < 3)
    return G_SOURCE_CONTINUE;
  g_cancellable_cancel (data);
  return G_SOURCE_REMOVE;
}

static void
test_cancel_early_contact (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  wrong_finger = early_contact = FALSE;
  g_autoptr(FpPrint) print = enroll (dev, NULL, &error);
  g_assert_no_error (error);
  memset (&FPI_DEVICE_EGIS0575 (dev)->calibration, 0, sizeof (Eh575Calibration));
  early_contact = TRUE;
  release_after = calibration_frames = 0;
  gboolean match;
  g_assert_false (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_REMOVE_FINGER);
  g_clear_error (&error);
  calibration_frames = 0;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_timeout_add (1, cancel_early_wait, cancel);
  g_assert_false (fp_device_verify_sync (dev, print, cancel, NULL, NULL, &match, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  assert_clean (dev);
  early_contact = FALSE;
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

static void
early_progress_cancel (FpDevice *dev, gint stage, FpPrint *print, gpointer data, GError *error)
{
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_REMOVE_FINGER);
  g_assert_cmpint (stage, ==, 0);
  g_cancellable_cancel (data);
}

static void
test_cancel_early_enrollment (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(FpPrint) template = fp_print_new (dev);
  g_object_ref_sink (template);
  early_contact = TRUE;
  release_after = calibration_frames = 0;
  g_autoptr(FpPrint) print = fp_device_enroll_sync (dev, template, cancel, early_progress_cancel, cancel, &error);
  g_assert_null (print);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  assert_clean (dev);
  early_contact = FALSE;
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
}

typedef struct
{
  FpDevice *dev;
  gboolean  done;
  GError   *error;
} SuspendTest;

static void
suspend_done (GObject *source, GAsyncResult *result, gpointer data)
{
  SuspendTest *test = data;

  g_assert_false (fp_device_suspend_finish (FP_DEVICE (source), result, &test->error));
  test->done = TRUE;
}

static gboolean
suspend_when_matching (gpointer data)
{
  SuspendTest *test = data;

  if (!FPI_DEVICE_EGIS0575 (test->dev)->matching)
    return G_SOURCE_CONTINUE;
  g_autoptr(GError) error = NULL;
  g_assert_false (fp_device_close_sync (test->dev, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);
  fp_device_suspend (test->dev, NULL, suspend_done, test);
  return G_SOURCE_REMOVE;
}

static void
test_suspend_worker (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  wrong_finger = FALSE;
  g_assert_true (fp_device_suspend_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_resume_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpPrint) print = enroll (dev, NULL, &error);
  g_assert_no_error (error);
  SuspendTest test = {dev, FALSE, NULL};
  g_timeout_add (1, suspend_when_matching, &test);
  gboolean match;
  g_assert_false (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);
  g_clear_error (&error);
  while (!test.done)
    g_main_context_iteration (NULL, TRUE);
  g_assert_error (test.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_clear_error (&test.error);
  assert_clean (dev);
  /* Stock fprintd clients release on the terminal suspend error, BEFORE
   * PrepareForSleep(false). The device must really close, not lose its claim
   * while retaining an open transport.
   */
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (fp_device_is_open (dev));
  g_assert_false (fp_device_open_sync (dev, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);
  g_clear_error (&error);
  g_assert_true (fp_device_resume_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_open_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  assert_clean (dev);
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
}

static void
test_persistent_profile (void)
{
  test_platform_id = "usb:02:00:03:01";
  wrong_finger = early_contact = FALSE;
  g_autoptr(FpDevice) first = new_device ();
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = enroll (first, NULL, &error);
  g_assert_no_error (error);
  FpDeviceEgis0575 *initial = FPI_DEVICE_EGIS0575 (first);
  guint8 saved[EH575_CALIBRATION_SIZE];
  memcpy (saved, initial->calibration.bytes, sizeof saved);
  g_assert_true (fp_device_close_sync (first, NULL, &error));
  g_clear_object (&first);
  /* New object and changed volatile register model a daemon restart/reset.
   * This test proves file reuse, not actual USB reboot/resume behavior.
   */
  g_autoptr(FpDevice) dev = new_device ();
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  g_assert_false (self->calibration.valid);
  early_contact = TRUE;
  release_after = calibration_frames = 0;
  register_dc = 53;
  gboolean match;
  g_assert_true (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  g_assert_cmpuint (calibration_frames, ==, 2);
  g_assert_cmpint (register_dc, ==, 32);
  g_assert_true (self->using_saved_calibration);
  g_assert_cmpmem (saved, sizeof saved, self->calibration.bytes, sizeof saved);
  assert_clean (dev);
  wrong_finger = TRUE;
  g_assert_true (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (match);
  g_assert_cmpmem (saved, sizeof saved, self->calibration.bytes, sizeof saved);
  assert_clean (dev);
  /* Cancellation must not erase a good profile needed after resume. */
  wrong_finger = FALSE;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  gpointer items[] = {dev, cancel};
  g_timeout_add (1, cancel_when_matching, items);
  g_assert_false (fp_device_verify_sync (dev, print, cancel, NULL, NULL, &match, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  Eh575Calibration loaded;
  g_assert_true (eh575_calibration_load (&loaded, &self->calibration_identity, test_cache_directory));
  g_assert_cmpmem (saved, sizeof saved, loaded.bytes, sizeof loaded.bytes);
  assert_clean (dev);
  early_contact = FALSE;
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
  test_platform_id = NULL;
}

static void
test_profile_quality_recovery (gconstpointer data)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  wrong_finger = early_contact = FALSE;
  g_autoptr(FpPrint) print = enroll (dev, NULL, &error);
  g_assert_no_error (error);
  early_contact = TRUE;
  release_after = calibration_frames = 0;
  bad_saved_frames = GPOINTER_TO_INT (data) == 0;
  poor_saved_contact = !bad_saved_frames;
  gboolean match;
  g_assert_false (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_REMOVE_FINGER);
  g_clear_error (&error);
  g_assert_false (self->calibration.valid);
  g_assert_false (self->poisoned);
  g_assert_true (self->early_notified);
  assert_clean (dev);
  bad_saved_frames = poor_saved_contact = FALSE;
  release_after = 6;
  calibration_frames = 0;
  g_assert_true (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  g_assert_true (self->calibration.valid);
  g_assert_false (self->using_saved_calibration);
  assert_clean (dev);
  early_contact = FALSE;
  release_after = 0;
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
}

static guint quality_retries;
static void
recovery_progress (FpDevice *dev, gint stage, FpPrint *print, gpointer data, GError *error)
{
  if (error)
    {
      g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_REMOVE_FINGER);
      g_assert_cmpint (stage, ==, 3);
      quality_retries++;
    }
  else
    {
      g_assert_cmpint (stage, >=, 1);
    }
}

static void
test_profile_enroll_recovery (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GError) error = NULL;
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  guint8 background[EH575_FRAME_SIZE];
  Eh575CalibrationIdentity id;
  g_assert_true (eh575_calibration_identity (&id, NULL));
  for (guint i = 0; i < sizeof background; i++)
    background[i] = 128 + (i % 7 < 3 ? 12 : -12);
  eh575_calibration_record (&self->calibration, &id, 32, background);
  wrong_finger = FALSE;
  early_contact = bad_saved_frames = TRUE;
  release_after = 6;
  calibration_frames = quality_retries = 0;
  g_autoptr(FpPrint) template = fp_print_new (dev);
  g_object_ref_sink (template);
  g_autoptr(FpPrint) print = fp_device_enroll_sync (dev, template, NULL, recovery_progress, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  g_assert_cmpuint (quality_retries, ==, 1);
  g_assert_cmpuint (self->stages, ==, EH575_RIDGE_STAGES);
  g_assert_true (self->calibration.valid);
  assert_clean (dev);
  early_contact = bad_saved_frames = FALSE;
  release_after = 0;
  gboolean match;
  g_assert_true (fp_device_verify_sync (dev, print, NULL, NULL, NULL, &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  assert_clean (dev);
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  test_cache_directory = g_dir_make_tmp ("eh575-profile-device-XXXXXX", NULL);
  g_assert_nonnull (test_cache_directory);
  g_setenv ("FP_EH575_CALIBRATION_DIR", test_cache_directory, TRUE);
  g_test_add_func ("/egis0575-ridge/public-roundtrip-match-reject-cancel-reactivate", test_roundtrip);
  g_test_add_func ("/egis0575-ridge/cancel-enrollment-progress", test_cancel_enroll);
  g_test_add_func ("/egis0575-ridge/malformed-template-no-usb", test_malformed);
  g_test_add_func ("/egis0575-ridge/suspend-worker-resume-reactivate", test_suspend_worker);
  g_test_add_func ("/egis0575-ridge/early-contact-warm-match-reject-cold-lift", test_early_contact);
  g_test_add_func ("/egis0575-ridge/cancel-cold-early-contact", test_cancel_early_contact);
  g_test_add_func ("/egis0575-ridge/cancel-cold-early-enrollment", test_cancel_early_enrollment);
  g_test_add_func ("/egis0575-ridge/persistent-profile-new-device-reset-match-reject-cancel", test_persistent_profile);
  g_test_add_data_func ("/egis0575-ridge/profile-raw-quality-lift-recovery", GINT_TO_POINTER (0), test_profile_quality_recovery);
  g_test_add_data_func ("/egis0575-ridge/profile-matcher-quality-lift-recovery", GINT_TO_POINTER (1), test_profile_quality_recovery);
  g_test_add_func ("/egis0575-ridge/profile-enrollment-quality-preserves-stages", test_profile_enroll_recovery);
  int status = g_test_run ();
  Eh575CalibrationIdentity id;
  g_assert_true (eh575_calibration_identity (&id, "usb:02:00:03:01"));
  g_autofree char *name = eh575_calibration_filename (&id);
  g_autofree char *path = g_build_filename (test_cache_directory, name, NULL);
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (g_rmdir (test_cache_directory), ==, 0);
  g_free (test_cache_directory);
  return status;
}
