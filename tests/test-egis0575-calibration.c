/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Calibration-cache tests use synthetic empty frames and private temp files.
 */
#include "egis0575-calibration.h"
#include <glib/gstdio.h>

static Eh575CalibrationIdentity
identity (void)
{
  Eh575CalibrationIdentity result;

  g_assert_true (eh575_calibration_identity (&result, "usb:02:00:03:01"));
  return result;
}

static void
record (Eh575Calibration *cache, const Eh575CalibrationIdentity *id)
{
  guint8 background[EH575_FRAME_SIZE];

  for (guint i = 0; i < sizeof background; i++)
    background[i] = 128 + (i % 7 < 3 ? 12 : -12);
  eh575_calibration_record (cache, id, 32, background);
  g_assert_true (eh575_calibration_compatible (cache, id));
}

static void
test_identity (void)
{
  Eh575CalibrationIdentity id = identity (), changed;
  Eh575Calibration cache;

  record (&cache, &id);
  /* A fresh identity instance (new daemon/boot) has no boot, sleep, USB
   * address or age dependency. The same physical port resolves identically.
   */
  g_assert_true (eh575_calibration_identity (&changed, "usb:02:00:03:01"));
  g_assert_true (eh575_calibration_compatible (&cache, &changed));
  g_assert_true (changed.persistent);
  g_assert_true (eh575_calibration_identity (&changed, "usb:02:00:03:02"));
  g_assert_false (eh575_calibration_compatible (&cache, &changed));
  g_assert_true (eh575_calibration_identity (&changed, NULL));
  g_assert_false (changed.persistent);
  g_assert_false (eh575_calibration_compatible (&cache, &changed));
  g_assert_false (eh575_calibration_identity (&changed, ""));
  char long_id[257];
  memset (long_id, 'a', 256);
  long_id[256] = 0;
  g_assert_false (eh575_calibration_identity (&changed, long_id));
}

static void
test_content (void)
{
  Eh575CalibrationIdentity id = identity ();
  Eh575Calibration cache;

  record (&cache, &id);
  cache.bytes[0] ^= 1;
  g_assert_false (eh575_calibration_compatible (&cache, &id));
  record (&cache, &id);
  cache.bytes[10] = 64;
  g_assert_false (eh575_calibration_compatible (&cache, &id));
  record (&cache, &id);
  cache.bytes[11] = 1;
  g_assert_false (eh575_calibration_compatible (&cache, &id));
  record (&cache, &id);
  cache.bytes[79] = 1;
  g_assert_false (eh575_calibration_compatible (&cache, &id));
  record (&cache, &id);
  cache.bytes[6] = '1'; /* Previous epoch-bound cache format is not imported. */
  g_assert_false (eh575_calibration_compatible (&cache, &id));
  record (&cache, &id);
  cache.bytes[80] ^= 1; /* Even an otherwise usable changed background fails checksum. */
  g_assert_false (eh575_calibration_compatible (&cache, &id));
  record (&cache, &id);
  memset (cache.bytes + 80, 0, EH575_FRAME_SIZE);
  g_assert_false (eh575_calibration_compatible (&cache, &id));
  record (&cache, &id);
  for (guint i = 0; i < EH575_FRAME_SIZE; i++)
    cache.bytes[i + 80] = i % 2 ? 80 : 180;
  g_assert_false (eh575_calibration_compatible (&cache, &id));
}

static void
test_files (void)
{
  Eh575CalibrationIdentity id = identity ();
  Eh575Calibration saved, loaded;
  g_autofree char *directory = g_dir_make_tmp ("eh575-calibration-XXXXXX", NULL);
  g_autofree char *name = eh575_calibration_filename (&id);
  g_autofree char *path = g_build_filename (directory, name, NULL);
  g_autofree char *other = g_build_filename (directory, "other", NULL);

  g_assert_nonnull (directory);
  g_assert_false (eh575_calibration_load (&loaded, &id, directory));
  record (&saved, &id);
  eh575_calibration_store (&saved, &id, directory);
  g_assert_true (eh575_calibration_load (&loaded, &id, directory));
  g_assert_cmpmem (saved.bytes, sizeof saved.bytes, loaded.bytes, sizeof loaded.bytes);
  /* New identity object/process resolves the same on-disk profile. */
  id = identity ();
  g_assert_true (eh575_calibration_load (&loaded, &id, directory));
  g_assert_cmpint (g_chmod (path, 0644), ==, 0);
  g_assert_false (eh575_calibration_load (&loaded, &id, directory));
  g_assert_cmpint (g_chmod (path, 0600), ==, 0);
  g_assert_cmpint (g_chmod (directory, 0755), ==, 0);
  g_assert_false (eh575_calibration_load (&loaded, &id, directory));
  g_assert_cmpint (g_chmod (directory, 0700), ==, 0);
  g_assert_cmpint (link (path, other), ==, 0);
  g_assert_false (eh575_calibration_load (&loaded, &id, directory));
  g_assert_cmpint (g_unlink (other), ==, 0);
  g_assert_cmpint (g_rename (path, other), ==, 0);
  g_assert_cmpint (symlink (other, path), ==, 0);
  g_assert_false (eh575_calibration_load (&loaded, &id, directory));
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (mkfifo (path, 0600), ==, 0);
  g_assert_false (eh575_calibration_load (&loaded, &id, directory));
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_true (g_file_set_contents (path, "short", 5, NULL));
  g_assert_cmpint (g_chmod (path, 0600), ==, 0);
  g_assert_false (eh575_calibration_load (&loaded, &id, directory));
  eh575_calibration_store (&saved, &id, directory);
  g_assert_true (eh575_calibration_load (&loaded, &id, directory));
  memset (&saved, 0, sizeof saved);
  eh575_calibration_store (&saved, &id, directory);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  g_assert_cmpint (g_unlink (other), ==, 0);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_frames (void)
{
  Eh575CalibrationIdentity id = identity ();
  Eh575Calibration cache;
  guint8 frame[EH575_FRAME_SIZE];

  record (&cache, &id);
  memcpy (frame, cache.bytes + 80, sizeof frame);
  g_assert_cmpint (eh575_calibration_evaluate (&cache, frame), ==, EH575_CALIBRATION_IDLE);
  for (guint i = 0; i < sizeof frame; i++)
    frame[i] += 5;
  g_assert_cmpint (eh575_calibration_evaluate (&cache, frame), ==, EH575_CALIBRATION_IDLE);
  for (guint i = 0; i < sizeof frame; i++)
    frame[i] += 20;
  g_assert_cmpint (eh575_calibration_evaluate (&cache, frame), ==, EH575_CALIBRATION_REMEASURE);
  for (guint i = 0; i < sizeof frame; i++)
    frame[i] = 128 + (i % 7 < 3 ? 6 : -6);
  g_assert_cmpint (eh575_calibration_evaluate (&cache, frame), ==, EH575_CALIBRATION_REMEASURE);
  for (guint i = 0; i < sizeof frame; i++)
    frame[i] = 128 + (i % 7 < 3 ? 40 : -40);
  g_assert_cmpint (eh575_calibration_evaluate (&cache, frame), ==, EH575_CALIBRATION_CONTACT);
  memset (frame, 0, sizeof frame);
  g_assert_cmpint (eh575_calibration_evaluate (&cache, frame), ==, EH575_CALIBRATION_REMEASURE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/egis0575-calibration/stable-port-new-process", test_identity);
  g_test_add_func ("/egis0575-calibration/schema-empty-quality", test_content);
  g_test_add_func ("/egis0575-calibration/protected-atomic-files", test_files);
  g_test_add_func ("/egis0575-calibration/idle-contact-unusable-readings", test_frames);
  return g_test_run ();
}
