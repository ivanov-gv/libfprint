/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Optional empty-reader calibration cache, never a fingerprint template.
 * Linux boot/suspend identity and strict local-file permissions fail closed.
 */
#pragma once
#include "egis0575.h"
#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EH575_CALIBRATION_SIZE (80 + EH575_FRAME_SIZE)
#define EH575_CALIBRATION_MAX_AGE G_GINT64_CONSTANT (3600000000)

typedef struct
{
  guint8 bus, address;
  char   boot[37];
  gint64 monotonic, suspended;
} Eh575CalibrationIdentity;

typedef struct
{
  guint8   bytes[EH575_CALIBRATION_SIZE];
  gboolean valid;
} Eh575Calibration;

static gboolean G_GNUC_UNUSED
eh575_calibration_identity (Eh575CalibrationIdentity *identity)
{
#ifdef CLOCK_BOOTTIME
  struct timespec boot, mono;
  g_autofree char *id = NULL;
  gsize length;
  if (!g_file_get_contents ("/proc/sys/kernel/random/boot_id", &id, &length, NULL) ||
      length != 37 || id[36] != '\n' ||
      clock_gettime (CLOCK_MONOTONIC, &mono) || clock_gettime (CLOCK_BOOTTIME, &boot))
    return FALSE;
  id[36] = 0;
  if (!g_uuid_string_is_valid (id))
    return FALSE;
  memcpy (identity->boot, id, 37);
  identity->monotonic = (gint64) mono.tv_sec * 1000000 + mono.tv_nsec / 1000;
  identity->suspended = (gint64) boot.tv_sec * 1000000 + boot.tv_nsec / 1000 - identity->monotonic;
  return TRUE;
#else
  (void) identity;
  return FALSE;
#endif
}

static guint64
eh575_calibration_read_time (const guint8 *bytes)
{
  guint64 value;

  memcpy (&value, bytes, sizeof value);
  return GUINT64_FROM_LE (value);
}

static gboolean
eh575_calibration_empty (const guint8 *background)
{
  double mean = eh575_mean (background);

  return mean >= 96 && mean <= 160 && eh575_deviation (background, NULL) <= 18 &&
         eh575_clipped (background) <= .005;
}

static gboolean
eh575_calibration_compatible (const Eh575Calibration         *cache,
                              const Eh575CalibrationIdentity *identity)
{
  const guint8 *b = cache->bytes;
  guint64 stamp = eh575_calibration_read_time (b + 16);
  guint64 suspended = eh575_calibration_read_time (b + 24);

  if (!cache->valid || memcmp (b, "EH575C1", 8) || b[8] != identity->bus ||
      b[9] != identity->address || b[10] > 63 || b[11] != (EH575_REVISION & 255) ||
      b[12] != (EH575_REVISION >> 8) || memcmp (b + 32, identity->boot, 36) ||
      identity->monotonic < 0 || identity->suspended < 0 || stamp > (guint64) identity->monotonic ||
                            (guint64) identity->monotonic - stamp > EH575_CALIBRATION_MAX_AGE ||
      suspended > G_MAXINT64 || llabs ((gint64) suspended - identity->suspended) > 50000)
    return FALSE;
  for (guint i = 13; i < 16; i++)
    if (b[i])
      return FALSE;
  for (guint i = 68; i < 80; i++)
    if (b[i])
      return FALSE;
  return eh575_calibration_empty (b + 80);
}

static void G_GNUC_UNUSED
eh575_calibration_record (Eh575Calibration *cache, const Eh575CalibrationIdentity *identity,
                          int dc, const guint8 *background)
{
  guint64 stamp = GUINT64_TO_LE ((guint64) identity->monotonic);
  guint64 suspended = GUINT64_TO_LE ((guint64) identity->suspended);

  memset (cache, 0, sizeof *cache);
  if (dc < 0 || dc > 63 || !eh575_calibration_empty (background) || identity->suspended < 0)
    return;
  memcpy (cache->bytes, "EH575C1", 8);
  cache->bytes[8] = identity->bus;
  cache->bytes[9] = identity->address;
  cache->bytes[10] = dc;
  cache->bytes[11] = EH575_REVISION & 255;
  cache->bytes[12] = EH575_REVISION >> 8;
  memcpy (cache->bytes + 16, &stamp, 8);
  memcpy (cache->bytes + 24, &suspended, 8);
  memcpy (cache->bytes + 32, identity->boot, 36);
  memcpy (cache->bytes + 80, background, EH575_FRAME_SIZE);
  cache->valid = TRUE;
}

static int
eh575_calibration_directory (const char *directory)
{
  struct stat st;

  if (!directory || directory[0] != '/' || strchr (directory, ':'))
    return -1;
  int fd = open (directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd >= 0 && (fstat (fd, &st) || st.st_uid != geteuid () || (st.st_mode & 0777) != 0700))
    {
      close (fd);
      return -1;
    }
  return fd;
}

static char *
eh575_calibration_filename (const Eh575CalibrationIdentity *identity)
{
  return g_strdup_printf (".eh575-calibration-v1-%u-%u", identity->bus, identity->address);
}

static gboolean
eh575_calibration_io (int fd, guint8 *bytes, gsize size, gboolean writing)
{
  while (size)
    {
      ssize_t count = writing ? write (fd, bytes, size) : read (fd, bytes, size);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        return FALSE;
      bytes += count;
      size -= count;
    }
  return TRUE;
}

static gboolean G_GNUC_UNUSED
eh575_calibration_load (Eh575Calibration *cache, const Eh575CalibrationIdentity *identity,
                        const char *directory)
{
  struct stat st;
  gboolean valid = FALSE;
  int dir = eh575_calibration_directory (directory);
  g_autofree char *name = eh575_calibration_filename (identity);

  memset (cache, 0, sizeof *cache);
  if (dir < 0)
    return FALSE;
  int fd = openat (dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd >= 0)
    {
      if (!fstat (fd, &st) && S_ISREG (st.st_mode) && st.st_uid == geteuid () &&
          (st.st_mode & 0777) == 0600 && st.st_nlink == 1 && st.st_size == sizeof cache->bytes &&
          eh575_calibration_io (fd, cache->bytes, sizeof cache->bytes, FALSE))
        {
          cache->valid = TRUE;
          valid = eh575_calibration_compatible (cache, identity);
        }
      close (fd);
    }
  close (dir);
  if (!valid)
    memset (cache, 0, sizeof *cache);
  return valid;
}

static void G_GNUC_UNUSED
eh575_calibration_store (Eh575Calibration *cache, const Eh575CalibrationIdentity *identity,
                         const char *directory)
{
  int dir = eh575_calibration_directory (directory);
  g_autofree char *name = eh575_calibration_filename (identity);
  g_autofree char *temp = g_strdup_printf ("%s.%08x.tmp", name, g_random_int ());

  if (dir < 0)
    return;
  if (eh575_calibration_compatible (cache, identity))
    {
      int fd = openat (dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd >= 0)
        {
          gboolean written = eh575_calibration_io (fd, cache->bytes, sizeof cache->bytes, TRUE) && !fsync (fd);
          close (fd);
          if (written && !renameat (dir, temp, dir, name))
            {
              int ignored = fsync (dir);
              (void) ignored;
            }
          unlinkat (dir, temp, 0);
        }
    }
  else
    {
      /* Only remove our own regular cache file, never a foreign entry. */
      struct stat st;
      if (!fstatat (dir, name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG (st.st_mode) &&
          st.st_uid == geteuid () && st.st_nlink == 1)
        unlinkat (dir, name, 0);
    }
  close (dir);
}
