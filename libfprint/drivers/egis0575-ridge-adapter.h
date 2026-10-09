/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Experimental FpDevice enrollment/verification adapter for shared EH575 USB
 * acquisition. Included privately by egis0575.c; never changes public ABI.
 */
typedef struct
{
  Eh575RidgeProbe probe;
  Eh575RidgeTouch touches[EH575_RIDGE_MAX_TOUCHES];
  guint           count;
} RidgeWork;

static void
ridge_touch_free (gpointer data)
{
  memset (data, 0, sizeof (Eh575RidgeTouch));
  g_free (data);
}

static void
ridge_work_free (gpointer data)
{
  memset (data, 0, sizeof (RidgeWork));
  g_free (data);
}

/* Untrusted host templates are bounded and versioned. Never import an NBIS
 * template or silently reinterpret Python enrollment files as native prints.
 */
static GPtrArray *
ridge_decode (FpPrint *print)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) array = NULL;
  const gchar *schema;
  GPtrArray *touches;
  if (fpi_print_get_type (print) != FPI_PRINT_RAW)
    return NULL;
  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(sa(ayay))")) ||
      g_variant_get_size (data) > 300000 || !g_variant_is_normal_form (data))
    return NULL;
  g_variant_get (data, "(&s@a(ayay))", &schema, &array);
  if (strcmp (schema, EH575_RIDGE_SCHEMA) || g_variant_n_children (array) < 6 ||
      g_variant_n_children (array) > EH575_RIDGE_MAX_TOUCHES)
    return NULL;
  touches = g_ptr_array_new_with_free_func (ridge_touch_free);
  for (gsize i = 0; i < g_variant_n_children (array); i++)
    {
      g_autoptr(GVariant) tuple = g_variant_get_child_value (array, i);
      g_autoptr(GVariant) image = g_variant_get_child_value (tuple, 0);
      g_autoptr(GVariant) background = g_variant_get_child_value (tuple, 1);
      gsize a, b;
      const guint8 *raw = g_variant_get_fixed_array (image, &a, 1);
      const guint8 *bg = g_variant_get_fixed_array (background, &b, 1);
      if (a != EH575_RIDGE_SIZE || b != EH575_RIDGE_SIZE)
        {
          g_ptr_array_unref (touches);
          return NULL;
        }
      Eh575RidgeTouch *touch = g_new (Eh575RidgeTouch, 1);
      memcpy (touch->image, raw, a);
      memcpy (touch->background, bg, b);
      g_ptr_array_add (touches, touch);
    }
  return touches;
}

static void
ridge_encode (FpDeviceEgis0575 *self)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ayay)"));
  for (guint i = 0; i < self->enrollment->len; i++)
    {
      Eh575RidgeTouch *touch = g_ptr_array_index (self->enrollment, i);
      g_variant_builder_add (&builder, "(@ay@ay)",
                             g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, touch->image, EH575_RIDGE_SIZE, 1),
                             g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, touch->background, EH575_RIDGE_SIZE, 1));
    }
  g_autoptr(GVariant) data = g_variant_ref_sink (g_variant_new ("(s@a(ayay))", EH575_RIDGE_SCHEMA,
                                                                g_variant_builder_end (&builder)));
  g_object_set (self->enroll_print, "fpi-data", data, NULL);
}

static void
ridge_complete (Eh575Device *dev, GError *error)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);
  FpiDeviceAction action = self->action;
  FpPrint *print = g_steal_pointer (&self->enroll_print);
  FpImage *image = g_steal_pointer (&self->capture_image);

  if (!error)
    error = g_steal_pointer (&self->action_error);
  else
    g_clear_error (&self->action_error);
  if (!error && g_cancellable_is_cancelled (fpi_device_get_cancellable (dev)))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "EH575 operation cancelled");
  g_clear_pointer (&self->enrollment, g_ptr_array_unref);
  self->image_state = FPI_IMAGE_DEVICE_STATE_INACTIVE;
  self->action = FPI_DEVICE_ACTION_NONE;
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
  switch (action)
    {
    case FPI_DEVICE_ACTION_ENROLL:
      if (!error && self->stages != EH575_RIDGE_STAGES)
        error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "Incomplete EH575 enrollment");
      if (error)
        g_clear_object (&print);
      fpi_device_enroll_complete (dev, print, error);
      break;

    case FPI_DEVICE_ACTION_VERIFY:
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, error);
          error = NULL;
        }
      else if (!error)
        {
          fpi_device_verify_report (dev, self->match.status == EH575_RIDGE_MATCH ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                    NULL, NULL);
        }
      fpi_device_verify_complete (dev, error);
      break;

    case FPI_DEVICE_ACTION_CAPTURE:
      if (error)
        g_clear_object (&image);
      fpi_device_capture_complete (dev, image, error);
      break;

    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
      g_clear_error (&error);
      g_clear_object (&print);
      g_clear_object (&image);
      g_assert_not_reached ();
    }
}

static void
ridge_activated (Eh575Device *dev, GError *error)
{
  if (error)
    {
      ridge_complete (dev, error);
      return;
    }
  FPI_DEVICE_EGIS0575 (dev)->image_state = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON;
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
}

static void
ridge_finger (Eh575Device *dev, gboolean present)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  if (present)
    {
      self->image_state = FPI_IMAGE_DEVICE_STATE_CAPTURE;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT);
    }
  else if (self->stages == EH575_RIDGE_STAGES && self->action == FPI_DEVICE_ACTION_ENROLL)
    {
      ridge_encode (self);
      dev_deactivate (dev);
    }
  else
    {
      self->image_state = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
    }
}

static void
ridge_retry (Eh575Device *dev, FpDeviceRetry reason)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  self->sample_count = 0;
  self->image_state = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF;
  if (self->action == FPI_DEVICE_ACTION_ENROLL)
    {
      fpi_device_enroll_progress (dev, self->stages, NULL, fpi_device_retry_new (reason));
    }
  else
    {
      self->action_error = fpi_device_retry_new (reason);
      dev_deactivate (dev);
    }
}

static void
ridge_compare_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
  RidgeWork *work = task_data;
  Eh575RidgeResult *result = g_new (Eh575RidgeResult, 1);

  *result = eh575_ridge_compare (work->touches, work->count, &work->probe, cancel);
  g_task_return_pointer (task, result, g_free);
}

static void
ridge_compare_done (GObject *source, GAsyncResult *task, gpointer data)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (source);

  g_autoptr(GError) error = NULL;
  g_autofree Eh575RidgeResult *result = g_task_propagate_pointer (G_TASK (task), &error);
  self->matching = FALSE;
  if (error)
    {
      self->action_error = g_steal_pointer (&error);
    }
  else if (result->status == EH575_RIDGE_POOR_IMAGE)
    {
      if (recover_saved_calibration (self))
        return;
      self->action_error = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
    }
  else if (result->status == EH575_RIDGE_INVALID || result->status == EH575_RIDGE_FAILED)
    {
      self->action_error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "Invalid EH575 ridge data or matcher failure");
    }
  else if (result->status == EH575_RIDGE_CANCELLED)
    {
      self->action_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "EH575 matching cancelled");
    }
  else if (g_get_monotonic_time () >= self->operation_deadline)
    {
      self->action_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "EH575 matching deadline exceeded");
    }
  else
    {
      self->match = *result;
      fp_dbg ("Native ridge result: accepted=%d matched_frames=%u correlation=%.4f margin=%.4f",
              result->status == EH575_RIDGE_MATCH, result->matched_frames, result->correlation, result->margin);
    }
  dev_deactivate (FP_DEVICE (self));
}

static void
ridge_burst (FpDeviceEgis0575 *self)
{
  self->sample_count = 0;
  if (self->action == FPI_DEVICE_ACTION_ENROLL)
    {
      Eh575RidgeTouch *touch = g_new0 (Eh575RidgeTouch, 1);
      eh575_median (touch->image, self->samples);
      memcpy (touch->background, self->background, EH575_RIDGE_SIZE);
      if (!eh575_ridge_touch_usable (touch))
        {
          ridge_touch_free (touch);
          if (!recover_saved_calibration (self))
            ridge_retry (FP_DEVICE (self), FP_DEVICE_RETRY_CENTER_FINGER);
          return;
        }
      g_ptr_array_add (self->enrollment, touch);
      self->stages++;
      self->clear_count = 0;
      self->image_state = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF;
      fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_PRESENT);
      fpi_device_enroll_progress (FP_DEVICE (self), self->stages, self->enroll_print, NULL);
    }
  else if (self->action == FPI_DEVICE_ACTION_CAPTURE)
    {
      self->capture_image = fp_image_new (EH575_WIDTH * 2, EH575_HEIGHT * 2);
      eh575_median (self->frame, self->samples);
      eh575_normalize (self->frame, self->frame, self->background);
      eh575_enlarge (self->capture_image->data, self->frame);
      self->capture_image->flags = FPI_IMAGE_PARTIAL;
      dev_deactivate (FP_DEVICE (self));
    }
  else
    {
      RidgeWork *work = g_new0 (RidgeWork, 1);
      work->count = self->enrollment->len;
      for (guint i = 0; i < work->count; i++)
        work->touches[i] = *(Eh575RidgeTouch *) g_ptr_array_index (self->enrollment, i);
      memcpy (work->probe.images, self->samples, sizeof work->probe.images);
      memcpy (work->probe.background, self->background, EH575_RIDGE_SIZE);
      self->matching = TRUE;
      self->image_state = FPI_IMAGE_DEVICE_STATE_IDLE;
      fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_PRESENT);
      g_autoptr(GTask) task = g_task_new (self, fpi_device_get_cancellable (FP_DEVICE (self)), ridge_compare_done, NULL);
      g_task_set_return_on_cancel (task, FALSE);
      g_task_set_task_data (task, work, ridge_work_free);
      g_task_run_in_thread (task, ridge_compare_thread);
    }
}

static void
ridge_start (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  self->action = fpi_device_get_current_action (dev);
  self->stages = 0;
  self->match = (Eh575RidgeResult){0};
  if (self->action == FPI_DEVICE_ACTION_VERIFY)
    {
      FpPrint *print;
      fpi_device_get_verify_data (dev, &print);
      self->enrollment = ridge_decode (print);
      if (!self->enrollment)
        {
          ridge_complete (dev, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID, "Enroll a fresh native EH575 ridge template"));
          return;
        }
    }
  else if (self->action == FPI_DEVICE_ACTION_ENROLL)
    {
      FpPrint *print;
      fpi_device_get_enroll_data (dev, &print);
      self->enroll_print = g_object_ref (print);
      fpi_print_set_type (print, FPI_PRINT_RAW);
      fpi_print_set_device_stored (print, FALSE);
      self->enrollment = g_ptr_array_new_with_free_func (ridge_touch_free);
    }
  dev_activate (dev);
}

static void
ridge_cancel (FpDevice *dev)
{
  FpDeviceEgis0575 *self = FPI_DEVICE_EGIS0575 (dev);

  if (self->matching)
    {
      /* Completion waits for the bounded worker; no early success or late
       * callback can leak into a subsequent operation.
       */
      self->stopping = TRUE;
      return;
    }
  dev_deactivate (dev);
}

#define fpi_image_device_open_complete fpi_device_open_complete
#define fpi_image_device_close_complete fpi_device_close_complete
#define fpi_image_device_activate_complete ridge_activated
#define fpi_image_device_deactivate_complete ridge_complete
#define fpi_image_device_session_error ridge_complete
#define fpi_image_device_report_finger_status ridge_finger
#define fpi_image_device_retry_scan ridge_retry
