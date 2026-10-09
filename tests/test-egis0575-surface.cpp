/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Synthetic-only check of cached FFT surfaces against the original six
 * OpenCV TM_CCORR calls. Include the private implementation, not a public API.
 */
#include "egis0575-ridge.cpp"

static cv::Mat
reference_surface (const cv::Mat &probe, const cv::Mat &reference,
                   const cv::Mat &source_mask, const cv::Mat &reference_mask)
{
  cv::Mat rm, sm;
  reference_mask.convertTo (rm, CV_32F, 1. / 255);
  source_mask.convertTo (sm, CV_32F, 1. / 255);
  cv::Mat targets[3] = {rm, reference.mul (rm), reference.mul (reference).mul (rm)};
  cv::Mat parts[3] = {sm, probe.mul (sm), probe.mul (probe).mul (sm)};
  for (cv::Mat &target : targets)
    cv::copyMakeBorder (target, target, 30, 30, 50, 50, cv::BORDER_CONSTANT, 0);
  auto corr = [&] (int target, int part) {
                cv::Mat result;
                cv::matchTemplate (targets[target], parts[part], result, cv::TM_CCORR);
                return result;
              };
  cv::Mat count = corr (0, 0), safe, sa = corr (0, 1), sb = corr (1, 0);
  cv::max (count, 1, safe);
  cv::Mat va = corr (0, 2) - sa.mul (sa) / safe;
  cv::Mat vb = corr (2, 0) - sb.mul (sb) / safe;
  cv::max (va, 0, va);
  cv::max (vb, 0, vb);
  cv::Mat denominator;
  cv::sqrt (va.mul (vb), denominator);
  cv::max (denominator, 1e-6, denominator);
  cv::Mat result = (corr (1, 1) - sa.mul (sb) / safe) / denominator;
  result.setTo (-1, count < .4 * AREA);
  return result;
}

static void
test_surfaces (void)
{
  cv::RNG rng (0x575);
  cv::Mat reference (H, W, CV_32F);
  cv::Mat mask = cv::Mat::zeros (H, W, CV_8U);
  mask (cv::Rect (5, 5, W - 10, H - 10)).setTo (255);
  for (int pattern = 0; pattern < 3; pattern++)
    {
      if (pattern == 0)
        rng.fill (reference, cv::RNG::NORMAL, 0, 1);
      else
        for (int y = 0; y < H; y++)
          for (int x = 0; x < W; x++)
            reference.at<float> (y, x) = pattern == 1 ? std::sin (y * .8 + x * .1) : 0;
      const CorrelationParts target = correlation_parts (reference, mask, true);
      for (double scale : {.85, 1., 1.15})
        for (int angle : {-35, -10, 0, 15, 35})
          {
            cv::Mat transform = cv::getRotationMatrix2D (cv::Point2f (51, 25.5), angle, scale);
            transform.at<double> (0, 2) += angle == -35 ? 50 : (angle == 35 ? -50 : 0);
            transform.at<double> (1, 2) += angle == -35 ? 30 : (angle == 35 ? -30 : 0);
            cv::Mat probe, contact;
            cv::warpAffine (reference, probe, transform, cv::Size (W, H));
            cv::warpAffine (mask, contact, transform, cv::Size (W, H), cv::INTER_NEAREST);
            if (pattern == 0 && angle == -10)
              rng.fill (probe, cv::RNG::NORMAL, 0, 1);
            /* Irregular contact and clipped borders, not only full rectangles. */
            if (angle == 15)
              contact (cv::Rect (8, 7, 20, 14)).setTo (0);
            const CorrelationParts source = correlation_parts (probe, contact, false);
            cv::Mat expected = reference_surface (probe, reference, contact, mask);
            cv::Mat actual = surface (source, target);
            g_assert_cmpint (actual.rows, ==, 61);
            g_assert_cmpint (actual.cols, ==, 101);
            g_assert_true (cv::checkRange (actual));
            g_assert_cmpfloat (cv::norm (expected, actual, cv::NORM_INF), <, 2e-5);
            /* Neither surface may invent valid overlap where the other has none. */
            g_assert_cmpint (cv::countNonZero ((expected == -1) != (actual == -1)), ==, 0);
            g_assert_cmpfloat (cv::norm (surface (source, target), actual, cv::NORM_INF), ==, 0);
          }
    }
  mask.setTo (0);
  cv::Mat empty = surface (correlation_parts (reference, mask, false),
                          correlation_parts (reference, mask, true));
  g_assert_cmpint (cv::countNonZero (empty != -1), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/egis0575/cached-surface-equivalence", test_surfaces);
  return g_test_run ();
}
