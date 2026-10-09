/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Optional empty-reader calibration cache, never a fingerprint template.
 * Persistent physical-port binding and strict local-file permissions.
 */
#pragma once
#include "egis0575.h"
#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define EH575_CALIBRATION_SIZE (80 + EH575_FRAME_SIZE)

typedef struct
{
  guint8   device[32];
  gboolean persistent;
} Eh575CalibrationIdentity;

typedef struct
{
  guint8   bytes[EH575_CALIBRATION_SIZE];
  gboolean valid;
} Eh575Calibration;

static gboolean G_GNUC_UNUSED
eh575_calibration_identity (Eh575CalibrationIdentity *identity, const char *platform_id)
{
  memset (identity, 0, sizeof *identity);
  /* Without a physical ID, reuse is limited to this object's memory. */
  if (!platform_id)
    return TRUE;
  if (!*platform_id || strnlen (platform_id, 256) >= 256)
    return FALSE;
  g_autofree char *key = g_strdup_printf ("EH575:1c7a:0575:%04x:%s", EH575_REVISION, platform_id);
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  gsize length = sizeof identity->device;
  g_checksum_update (checksum, (const guint8 *) key, strlen (key));
  g_checksum_get_digest (checksum, identity->device, &length);
  identity->persistent = TRUE;
  return TRUE;
}

static void
eh575_calibration_digest (const guint8 *bytes, guint8 digest[32])
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  gsize length = 32;
  g_checksum_update (checksum, bytes, 48);
  g_checksum_update (checksum, bytes + 80, EH575_FRAME_SIZE);
  g_checksum_get_digest (checksum, digest, &length);
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
  guint8 digest[32];

  if (!cache->valid || memcmp (b, "EH575C2", 8) || b[8] || b[9] || b[10] > 63 ||
      b[11] != (EH575_REVISION & 255) || b[12] != (EH575_REVISION >> 8) ||
      memcmp (b + 16, identity->device, 32))
    return FALSE;
  for (guint i = 13; i < 16; i++)
    if (b[i])
      return FALSE;
  eh575_calibration_digest (b, digest);
  return !memcmp (b + 48, digest, 32) && eh575_calibration_empty (b + 80);
}

typedef enum { EH575_CALIBRATION_IDLE, EH575_CALIBRATION_CONTACT, EH575_CALIBRATION_REMEASURE } Eh575CalibrationFrame;

static Eh575CalibrationFrame G_GNUC_UNUSED
eh575_calibration_evaluate (const Eh575Calibration *cache, const guint8 *frame)
{
  const guint8 *background = cache->bytes + 80;
  double change = eh575_deviation (frame, background);

  if (eh575_deviation (frame, NULL) >= 20 && eh575_clipped (frame) <= .05 && change >= 8)
    return EH575_CALIBRATION_CONTACT;
  if (eh575_calibration_empty (frame) && change <= 4 &&
      fabs (eh575_mean (frame) - eh575_mean (background)) <= 16)
    return EH575_CALIBRATION_IDLE;
  return EH575_CALIBRATION_REMEASURE;
}

static void G_GNUC_UNUSED
eh575_calibration_record (Eh575Calibration *cache, const Eh575CalibrationIdentity *identity,
                          int dc, const guint8 *background)
{
  memset (cache, 0, sizeof *cache);
  if (dc < 0 || dc > 63 || !eh575_calibration_empty (background))
    return;
  memcpy (cache->bytes, "EH575C2", 8);
  cache->bytes[10] = dc;
  cache->bytes[11] = EH575_REVISION & 255;
  cache->bytes[12] = EH575_REVISION >> 8;
  memcpy (cache->bytes + 16, identity->device, 32);
  memcpy (cache->bytes + 80, background, EH575_FRAME_SIZE);
  eh575_calibration_digest (cache->bytes, cache->bytes + 48);
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
  char hex[65];
  const char *digits = "0123456789abcdef";

  for (guint i = 0; i < 32; i++)
    {
      hex[2 * i] = digits[identity->device[i] >> 4];
      hex[2 * i + 1] = digits[identity->device[i] & 15];
    }
  hex[64] = 0;
  return g_strdup_printf (".eh575-calibration-v2-%s", hex);
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
  int dir = eh575_calibration_directory (identity->persistent ? directory : NULL);
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
  int dir = eh575_calibration_directory (identity->persistent ? directory : NULL);
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
