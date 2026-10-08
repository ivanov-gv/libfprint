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
  gboolean *prompted = data;
  gboolean ready = (status & FP_FINGER_STATUS_NEEDED) && !(status & FP_FINGER_STATUS_PRESENT);

  if (ready && !*prompted)
    g_print ("Reader calibrated. Place your finger flat and hold still. Do not slide; lift fully after capture.\n");
  *prompted = ready;
}

static void
enroll_progress (FpDevice *dev, gint stage, FpPrint *print, gpointer data, GError *error)
{
  if (error)
    g_print ("Retry: %s\n", error->message);
  else
    g_print ("Enrollment stage %d/%d captured. Lift your finger.\n", stage, fp_device_get_nr_enroll_stages (dev));
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
  gboolean prompted = FALSE;
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
  g_signal_connect (dev, "notify::finger-status", G_CALLBACK (finger_status), &prompted);
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
          guchar *bytes = NULL;
          gsize length = 0;
          fp_print_set_finger (template, FP_FINGER_RIGHT_INDEX);
          g_print ("Keep empty until calibrated. Enroll your right index finger with stationary touches. Vary placement slightly only after lifting, never slide during a touch.\n");
          print = fp_device_enroll_sync (dev, template, cancel, enroll_progress, NULL, &error);
          if (print && fp_print_serialize (print, &bytes, &length, &error))
            ok = g_file_set_contents ("native-print.bin", (const char *) bytes, length, &error);
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
