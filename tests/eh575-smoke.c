/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Unprivileged hardware smoke test using only the public libfprint API.
 * No fprintd/PAM/system changes. Private template file in the current directory.
 */
#include <libfprint/fprint.h>
#include <glib-unix.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

typedef struct
{
  gboolean prompted, ridge, enrolling, verifying, comparing;
  gint     next_stage;
} Prompt;

static gboolean
save_template (const guchar *bytes, gsize length, GError **error)
{
  int fd = open ("native-print.bin", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  gsize offset = 0;

  if (fd < 0)
    {
      g_set_error_literal (error, G_FILE_ERROR, g_file_error_from_errno (errno), "Cannot create a new private native template; existing files are never overwritten");
      return FALSE;
    }
  while (offset < length)
    {
      ssize_t written = write (fd, bytes + offset, length - offset);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        break;
      offset += written;
    }
  gboolean ok = offset == length && fsync (fd) == 0;
  if (close (fd) != 0)
    ok = FALSE;
  if (!ok)
    g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Private template write failed; the incomplete file is preserved, use a fresh test directory");
  return ok;
}

static gboolean
cancel_scan (gpointer data)
{
  g_cancellable_cancel (data);
  return G_SOURCE_CONTINUE;
}

static void
finger_status (FpDevice *dev, GParamSpec *spec, gpointer data)
{
  FpFingerStatusFlags status = fp_device_get_finger_status (dev);
  Prompt *prompt = data;
  gboolean ready = (status & FP_FINGER_STATUS_NEEDED) && !(status & FP_FINGER_STATUS_PRESENT);

  if (ready && !prompt->prompted)
    {
      if (prompt->ridge && prompt->enrolling)
        {
          const char *areas[] = {"center", "slightly tip-side", "slightly base-side", "slightly left-side", "slightly right-side"};
          gint index = CLAMP (prompt->next_stage / 3, 0, 4);
          g_print ("Touch %d/15 [%s]: place your pad flat and hold still. Do not slide; lift fully after capture.\n",
                   prompt->next_stage + 1, areas[index]);
        }
      else
        {
          g_print ("Reader calibrated. Place your finger flat and hold still. Do not slide; lift fully after capture.\n");
        }
    }
  if (prompt->ridge && prompt->verifying && !prompt->comparing &&
      (status & FP_FINGER_STATUS_PRESENT) && !(status & FP_FINGER_STATUS_NEEDED))
    {
      prompt->comparing = TRUE;
      g_print ("Captured. You may lift; comparing stationary ridge images.\n");
    }
  prompt->prompted = ready;
}

static void
enroll_progress (FpDevice *dev, gint stage, FpPrint *print, gpointer data, GError *error)
{
  if (error)
    {
      g_print ("Retry: %s\n", error->message);
    }
  else
    {
      ((Prompt *) data)->next_stage = stage;
      g_print ("Enrollment stage %d/%d captured. Lift your finger.\n", stage, fp_device_get_nr_enroll_stages (dev));
    }
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) context = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = NULL;
  FpDevice *dev = NULL;
  GPtrArray *devices;
  guint signal_id, timer_id = 0;
  gboolean ok = FALSE;
  Prompt prompt = {0};
  const char *command;
  umask (0077);
  if (geteuid () == 0 || argc != 2)
    {
      g_printerr ("Run without sudo: eh575-smoke open|idle|capture|enroll|verify\n");
      return 2;
    }
  command = argv[1];
  if (strcmp (command, "open") && strcmp (command, "idle") && strcmp (command, "capture") &&
      strcmp (command, "enroll") && strcmp (command, "verify"))
    return 2;
  context = fp_context_new ();
  devices = fp_context_get_devices (context);
  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *candidate = g_ptr_array_index (devices, i);
      if (!strcmp (fp_device_get_driver (candidate), "egis0575"))
        {
          if (dev)
            {
              g_printerr ("More than one EH575 found\n");
              return 1;
            }
          dev = candidate;
        }
    }
  if (!dev)
    {
      g_printerr ("No native EH575 device found\n");
      return 1;
    }
  if (!fp_device_open_sync (dev, cancel, &error))
    {
      g_printerr ("Open failed: %s\n", error->message);
      return 1;
    }
  g_print ("Opened %s using %s.\n", fp_device_get_name (dev), fp_device_get_driver (dev));
  signal_id = g_unix_signal_add (SIGINT, cancel_scan, cancel);
  prompt.ridge = strstr (fp_device_get_name (dev), "stationary ridge") != NULL;
  prompt.enrolling = !strcmp (command, "enroll");
  prompt.verifying = !strcmp (command, "verify");
  g_signal_connect (dev, "notify::finger-status", G_CALLBACK (finger_status), &prompt);
  if (prompt.ridge)
    g_print ("Experimental native stationary matching. This test does not enable system login.\n");
  if (!strcmp (command, "open"))
    {
      ok = TRUE;
    }
  else if (!strcmp (command, "idle") || !strcmp (command, "capture"))
    {
      g_autoptr(FpImage) image = NULL;
      if (!strcmp (command, "idle"))
        {
          g_print ("Keep the reader EMPTY throughout this cancellation test.\n");
          timer_id = g_timeout_add_seconds (3, cancel_scan, cancel);
        }
      else
        {
          g_print ("Keep the reader empty until 'Reader calibrated'. No image will be saved.\n");
        }
      image = fp_device_capture_sync (dev, TRUE, cancel, &error);
      if (!strcmp (command, "idle"))
        {
          ok = !image && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          if (ok)
            {
              g_clear_error (&error);
              g_print ("Idle cancellation completed.\n");
            }
        }
      else if (image)
        {
          GPtrArray *minutiae = fp_image_get_minutiae (image);
          if (prompt.ridge)
            g_print ("Captured %u x %u native image. Ridge matching does not use minutiae; image discarded, not saved.\n",
                     fp_image_get_width (image), fp_image_get_height (image));
          else
            g_print ("Captured %u x %u native image; %u minutiae. Image discarded, not saved.\n",
                     fp_image_get_width (image), fp_image_get_height (image), minutiae ? minutiae->len : 0);
          ok = TRUE;
        }
    }
  else if (!strcmp (command, "enroll"))
    {
      if (g_file_test ("native-print.bin", G_FILE_TEST_EXISTS))
        {
          g_printerr ("native-print.bin exists; use a new private test directory to preserve it.\n");
        }
      else
        {
          g_autoptr(FpPrint) template = fp_print_new (dev);
          g_object_ref_sink (template);
          guchar *bytes = NULL;
          gsize length = 0;
          fp_print_set_finger (template, FP_FINGER_RIGHT_INDEX);
          g_print ("Keep empty until calibrated. Enroll your right index finger with stationary touches. Vary placement slightly only after lifting, never slide during a touch.\n");
          if (prompt.ridge)
            g_print ("Three touches each at center, tip-side, base-side, left-side and right-side. Use small overlapping pad areas; directions refer to your finger, not the laptop.\n");
          print = fp_device_enroll_sync (dev, template, cancel, enroll_progress, &prompt, &error);
          if (print && fp_print_serialize (print, &bytes, &length, &error))
            ok = save_template (bytes, length, &error);
          if (bytes)
            memset (bytes, 0, length);
          g_free (bytes);
          if (ok)
            g_print ("Enrolled. Private native template saved; Python enrollment unchanged.\n");
        }
    }
  else
    {
      gchar *bytes = NULL;
      gsize length = 0;
      gboolean match = FALSE;
      if (g_file_get_contents ("native-print.bin", &bytes, &length, &error))
        print = fp_print_deserialize ((guchar *) bytes, length, &error);
      if (bytes)
        memset (bytes, 0, length);
      g_free (bytes);
      if (print)
        {
          g_print ("Keep empty until calibrated, then place the finger you want to test.\n");
          ok = fp_device_verify_sync (dev, print, cancel, NULL, NULL, &match, NULL, &error);
          if (ok)
            {
              g_print ("Native result: %s\n", match ? "MATCH" : "NO MATCH");
              ok = match;
            }
        }
    }
  if (timer_id)
    g_source_remove (timer_id);
  g_source_remove (signal_id);
  if (error)
    {
      g_printerr ("Scan failed: %s\n", error->message);
      g_clear_error (&error);
    }
  if (!fp_device_close_sync (dev, NULL, &error))
    {
      g_printerr ("Close failed: %s\n", error->message);
      ok = FALSE;
    }
  return ok ? 0 : 1;
}
