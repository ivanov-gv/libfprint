/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Offline extraction diagnostic. Reads one raw frame and its measured empty
 * reference from stdin, prints counts only, and never opens a USB device.
 * Experimental variants are NOT driver settings or authentication decisions.
 */
#include "fpi-image.h"
#include "drivers/egis0575.h"
#include <stdio.h>

typedef struct
{
  GMainLoop *loop;
  guint      count;
  gboolean   extracted;
} Extraction;

static void
extracted_cb (GObject *object, GAsyncResult *result, gpointer user_data)
{
  Extraction *extraction = user_data;

  g_autoptr(GError) error = NULL;
  extraction->extracted = fp_image_detect_minutiae_finish (FP_IMAGE (object), result, &error);
  extraction->count = extraction->extracted ? fp_image_get_minutiae (FP_IMAGE (object))->len : 0;
  g_main_loop_quit (extraction->loop);
}

static FpImage *
make_variant (const uint8_t *frame, const uint8_t *background, unsigned int gain,
              unsigned int scale, gboolean inverted)
{
  uint8_t normalized[EH575_FRAME_SIZE];
  double offset = eh575_mean (frame) - eh575_mean (background);
  FpImage *image = fp_image_new (EH575_WIDTH * scale, EH575_HEIGHT * scale);

  /* The baseline must call the same helpers as the actual driver. */
  if (gain == 2)
    eh575_normalize (normalized, frame, background);
  else
    for (size_t i = 0; i < EH575_FRAME_SIZE; i++)
      normalized[i] = (uint8_t) fmax (0, fmin (255, 128 + gain * (frame[i] - background[i] - offset)));
  if (scale == 2)
    {
      eh575_enlarge (image->data, normalized);
    }
  else
    {
      for (unsigned int y = 0; y < image->height; y++)
        for (unsigned int x = 0; x < image->width; x++)
          {
            unsigned int x0 = x / scale, y0 = y / scale;
            unsigned int x1 = MIN (x0 + 1, EH575_WIDTH - 1), y1 = MIN (y0 + 1, EH575_HEIGHT - 1);
            unsigned int dx = x % scale, dy = y % scale;
            image->data[y * image->width + x] =
              ((scale - dx) * (scale - dy) * normalized[y0 * EH575_WIDTH + x0] +
               dx * (scale - dy) * normalized[y0 * EH575_WIDTH + x1] +
               (scale - dx) * dy * normalized[y1 * EH575_WIDTH + x0] +
               dx * dy * normalized[y1 * EH575_WIDTH + x1]) / (scale * scale);
          }
    }
  /* Keep partial-image perimeter filtering for every variant. */
  image->flags = FPI_IMAGE_PARTIAL | (inverted ? FPI_IMAGE_COLORS_INVERTED : 0);
  memset (normalized, 0, sizeof normalized);
  return image;
}

int
main (void)
{
  uint8_t input[EH575_FRAME_SIZE * 2];
  Extraction extraction = { 0 };

  /* GLib debug output can include biometric coordinates; counts only here. */
  g_unsetenv ("G_MESSAGES_DEBUG");

  if (fread (input, 1, sizeof input, stdin) != sizeof input || fgetc (stdin) != EOF || ferror (stdin))
    {
      memset (input, 0, sizeof input);
      g_printerr ("Expected exactly one 103x52 uint8 frame followed by its empty reference.\n");
      return 2;
    }
  extraction.loop = g_main_loop_new (NULL, FALSE);
  puts ("gain,scale,inverted,minutiae,extracted");
  for (unsigned int gain = 1; gain <= 3; gain++)
    for (unsigned int scale = 1; scale <= 3; scale++)
      for (unsigned int inverted = 0; inverted <= 1; inverted++)
        {
          g_autoptr(FpImage) image = make_variant (input, input + EH575_FRAME_SIZE, gain, scale, inverted);
          fp_image_detect_minutiae (image, NULL, extracted_cb, &extraction);
          g_main_loop_run (extraction.loop);
          printf ("%u,%u,%u,%u,%u\n", gain, scale, inverted, extraction.count, extraction.extracted);
        }
  memset (input, 0, sizeof input);
  g_main_loop_unref (extraction.loop);
  return 0;
}
