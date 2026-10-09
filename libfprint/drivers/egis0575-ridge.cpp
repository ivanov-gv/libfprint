/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 fingerprint contributors
 * Prototype attribution/license: egis0575-prototype-MIT.txt.
 * Native development port of the private prototype's ridge registration.
 * All decisions require actual measured overlap, detailed corroboration and
 * an unambiguous registration. No minutia synthesis or NBIS threshold change.
 */
#include "egis0575-ridge.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/video/tracking.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace {
constexpr int W = 103, H = 52, AREA = (W - 10) * (H - 10);
struct Prepared
{
  cv::Mat ridge, mask;
};
struct Score
{
  double overlap, correlation, edge, regional;
};
struct Candidate
{
  Score   score;
  cv::Mat transform;
};
bool
cancelled (GCancellable *c)
{
  return c && g_cancellable_is_cancelled (c);
}

bool
prepare (const uint8_t *image, const uint8_t *background, Prepared &out)
{
  cv::Mat raw (H, W, CV_8U, const_cast<uint8_t *> (image));
  cv::Mat bg (H, W, CV_8U, const_cast<uint8_t *> (background));
  cv::Scalar mean, deviation;

  cv::meanStdDev (raw, mean, deviation);
  if (deviation[0] < 20 || double (cv::countNonZero ((raw <= 2) | (raw >= 253))) / (W * H) > .05)
    return false;
  cv::Mat difference, smooth, low, local, variance;
  raw.convertTo (difference, CV_32F);
  cv::Mat bgfloat;
  bg.convertTo (bgfloat, CV_32F);
  difference -= bgfloat;
  cv::GaussianBlur (difference, smooth, cv::Size (3, 3), .6);
  cv::GaussianBlur (smooth, low, cv::Size (), 4);
  out.ridge = smooth - low;
  cv::meanStdDev (out.ridge, mean, deviation);
  out.ridge /= std::max (deviation[0], 1.0);
  cv::GaussianBlur (difference, local, cv::Size (), 3);
  cv::GaussianBlur (difference.mul (difference), variance, cv::Size (), 3);
  out.mask = (variance - local.mul (local)) >= 64;
  cv::erode (out.mask, out.mask, cv::Mat::ones (3, 3, CV_8U));
  out.mask.rowRange (0, 5).setTo (0);
  out.mask.rowRange (H - 5, H).setTo (0);
  out.mask.colRange (0, 5).setTo (0);
  out.mask.colRange (W - 5, W).setTo (0);
  return cv::countNonZero (out.mask) >= .65 * AREA;
}

double
ncc (const cv::Mat &a, const cv::Mat &b, const cv::Mat &mask)
{
  const int count = cv::countNonZero (mask);

  if (!count)
    return 0;
  cv::Scalar ma, mb, da, db;
  cv::meanStdDev (a, ma, da, mask);
  cv::meanStdDev (b, mb, db, mask);
  if (da[0] < 1e-6 || db[0] < 1e-6)
    return 0;
  cv::Mat product = (a - ma[0]).mul (b - mb[0]);
  return cv::mean (product, mask)[0] / (da[0] * db[0]);
}

Score
scores (const Prepared &probe, const Prepared &reference, const cv::Mat &transform)
{
  cv::Mat aligned, mask;

  cv::warpAffine (probe.ridge, aligned, transform, cv::Size (W, H));
  cv::warpAffine (probe.mask, mask, transform, cv::Size (W, H), cv::INTER_NEAREST);
  mask &= reference.mask;
  Score s { double (cv::countNonZero (mask)) / AREA, ncc (aligned, reference.ridge, mask), 1, 0 };
  for (int axis = 0; axis < 2; axis++)
    {
      cv::Mat a, b;
      cv::Sobel (aligned, a, CV_32F, axis == 0, axis == 1);
      cv::Sobel (reference.ridge, b, CV_32F, axis == 0, axis == 1);
      s.edge = std::min (s.edge, ncc (a, b, mask));
    }
  std::vector<cv::Point> points;
  cv::findNonZero (mask, points);
  if (points.size () < .4 * AREA)
    return s;
  std::stable_sort (points.begin (), points.end (), [] (const cv::Point &a, const cv::Point &b) {
      return a.x < b.x;
    });
  s.regional = 1;
  size_t start = 0;
  for (size_t region = 0; region < 3; region++)
    {
      const size_t length = points.size () / 3 + (region < points.size () % 3);
      cv::Mat part = cv::Mat::zeros (H, W, CV_8U);
      for (size_t i = start; i < start + length; i++)
        part.at<uint8_t> (points[i]) = 255;
      s.regional = std::min (s.regional, ncc (aligned, reference.ridge, part));
      start += length;
    }
  return s;
}

double
coarse_correlation (const Prepared &probe, const Prepared &reference, const cv::Mat &transform)
{
  cv::Mat aligned, mask;

  cv::warpAffine (probe.ridge, aligned, transform, cv::Size (W, H));
  cv::warpAffine (probe.mask, mask, transform, cv::Size (W, H), cv::INTER_NEAREST);
  mask &= reference.mask;
  /* Coarse candidates are ranked ONLY by NCC. Edge/region corroboration is
   * still computed for every refined candidate and all five final frames.
   */
  return ncc (aligned, reference.ridge, mask);
}

bool
geometry (const cv::Mat &transform)
{
  if (transform.rows != 2 || transform.cols != 3 || !cv::checkRange (transform))
    return false;
  cv::Mat linear = transform (cv::Rect (0, 0, 2, 2));
  cv::Mat values;
  cv::SVD::compute (linear, values);
  double large = values.at<double> (0), small = values.at<double> (1);
  double angle = std::atan2 (transform.at<double> (1, 0), transform.at<double> (0, 0)) * 180 / CV_PI;
  return cv::determinant (linear) > 0 && small >= .80 && large <= 1.20 &&
         large / small <= 1.25 && std::abs (angle) <= 35;
}

double
distance (const cv::Mat &a, const cv::Mat &b)
{
  double largest = 0;

  for (double y : {5., H - 6.})
    for (double x : {5., W - 6.})
      {
        double dx = (a.at<double> (0, 0) - b.at<double> (0, 0)) * x +
                    (a.at<double> (0, 1) - b.at<double> (0, 1)) * y + a.at<double> (0, 2) - b.at<double> (0, 2);
        double dy = (a.at<double> (1, 0) - b.at<double> (1, 0)) * x +
                    (a.at<double> (1, 1) - b.at<double> (1, 1)) * y + a.at<double> (1, 2) - b.at<double> (1, 2);
        largest = std::max (largest, std::hypot (dx, dy));
      }
  return largest;
}

cv::Mat
surface (const cv::Mat &probe, const cv::Mat &reference, const cv::Mat &source_mask, const cv::Mat &reference_mask)
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

cv::Mat
feature_seed (const Prepared &probe, const Prepared &reference)
{
  std::vector<cv::KeyPoint> keys[2];
  cv::Mat descriptors[2];
  auto sift = cv::SIFT::create (200, 3, .02, 10, 1.2);
  const Prepared *images[] = {&probe, &reference};

  for (int i = 0; i < 2; i++)
    {
      cv::Mat image, mask;
      images[i]->ridge.convertTo (image, CV_8U, 35, 128);
      cv::resize (image, image, cv::Size (), 2, 2, cv::INTER_CUBIC);
      cv::resize (images[i]->mask, mask, cv::Size (), 2, 2, cv::INTER_NEAREST);
      sift->detectAndCompute (image, mask, keys[i], descriptors[i]);
    }
  if (descriptors[0].empty () || descriptors[1].rows < 2)
    return cv::Mat ();
  std::vector<std::vector<cv::DMatch> > pairs;
  cv::BFMatcher ().knnMatch (descriptors[0], descriptors[1], pairs, 2);
  std::map<int, cv::DMatch> unique;
  for (const auto &pair : pairs)
    if (pair.size () == 2 && pair[0].distance < .8 * pair[1].distance)
      {
        auto previous = unique.find (pair[0].trainIdx);
        if (previous == unique.end () || pair[0].distance < previous->second.distance)
          unique[pair[0].trainIdx] = pair[0];
      }
  if (unique.size () < 4)
    return cv::Mat ();
  std::vector<cv::Point2f> a, b;
  for (const auto &entry : unique)
    {
      a.push_back (keys[0][entry.second.queryIdx].pt * .5);
      b.push_back (keys[1][entry.second.trainIdx].pt * .5);
    }
  cv::Mat inliers;
  cv::Mat result = cv::estimateAffine2D (a, b, inliers, cv::RANSAC, 1.5, 1000, .995);
  return geometry (result) ? result : cv::Mat ();
}

bool
registration (const Prepared &probe, const Prepared &reference, Candidate &best, double &margin, GCancellable *cancel)
{
  std::vector<Candidate> candidates, refined;

  for (double scale : {.85, 1., 1.15})
    for (int angle = -35; angle <= 35; angle += 5)
      {
        if (cancelled (cancel))
          return false;
        cv::Mat transform = cv::getRotationMatrix2D (cv::Point2f ((W - 1) / 2., (H - 1) / 2.), angle, scale);
        cv::Mat rotated, mask;
        cv::warpAffine (probe.ridge, rotated, transform, cv::Size (W, H));
        cv::warpAffine (probe.mask, mask, transform, cv::Size (W, H), cv::INTER_NEAREST);
        cv::Mat peaks = surface (rotated, reference.ridge, mask, reference.mask);
        for (int i = 0; i < 3; i++)
          {
            double peak;
            cv::Point position;
            cv::minMaxLoc (peaks, nullptr, &peak, nullptr, &position);
            if (peak < 0)
              break;
            cv::Mat shifted = transform.clone ();
            shifted.at<double> (0, 2) += position.x - 50;
            shifted.at<double> (1, 2) += position.y - 30;
            candidates.push_back ({{0, coarse_correlation (probe, reference, shifted), 0, 0}, shifted});
            int left = std::max (0, position.x - 2), top = std::max (0, position.y - 2);
            cv::Rect region (left, top, std::min (peaks.cols, position.x + 3) - left,
                             std::min (peaks.rows, position.y + 3) - top);
            peaks (region).setTo (-1);
          }
      }
  std::sort (candidates.begin (), candidates.end (), [] (const Candidate &a, const Candidate &b) {
      return a.score.correlation > b.score.correlation;
    });
  std::vector<cv::Mat> selected;
  if (cancelled (cancel))
    return false;
  cv::Mat seed = feature_seed (probe, reference);
  if (!seed.empty ())
    selected.push_back (seed);
  for (const Candidate &candidate : candidates)
    {
      bool distinct = true;
      for (const cv::Mat &other : selected)
        if (distance (candidate.transform, other) <= 3)
          distinct = false;
      if (distinct)
        selected.push_back (candidate.transform);
      if (selected.size () >= 10)
        break;
    }
  for (cv::Mat transform : selected)
    {
      if (cancelled (cancel))
        return false;
      cv::Mat warp;
      cv::invertAffineTransform (transform, warp);
      warp.convertTo (warp, CV_32F);
      try {
          cv::findTransformECC (reference.ridge, probe.ridge, warp, cv::MOTION_AFFINE,
                                cv::TermCriteria (cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 40, 1e-4), probe.mask, 3);
          cv::invertAffineTransform (warp, transform);
          transform.convertTo (transform, CV_64F);
        } catch (const cv::Exception &) { /* Keep seed, never accept refinement failure alone. */ }
      if (geometry (transform))
        refined.push_back ({scores (probe, reference, transform), transform});
    }
  if (refined.empty ())
    return false;
  std::sort (refined.begin (), refined.end (), [] (const Candidate &a, const Candidate &b) {
      return a.score.correlation > b.score.correlation;
    });
  best = refined[0];
  double competing = 0;
  for (size_t i = 1; i < refined.size (); i++)
    if (distance (refined[i].transform, best.transform) > 3)
      competing = std::max (competing, refined[i].score.correlation);
  margin = best.score.correlation - competing;
  return true;
}

bool
sufficient (const Score &s, double margin)
{
  const bool area = s.overlap >= .55 || (s.overlap >= .40 && s.correlation >= .90 && s.edge >= .75 && s.regional >= .80);

  return area && s.correlation >= .85 && s.edge >= .70 && s.regional >= .70 && margin >= .04;
}
} // namespace

extern "C" gboolean
eh575_ridge_touch_usable (const Eh575RidgeTouch *touch)
{
  if (!touch)
    return FALSE;
  try { Prepared prepared;
        return prepare (touch->image, touch->background, prepared);
    }
  catch (const std::exception &) { return FALSE;
    }
}

extern "C" Eh575RidgeResult
eh575_ridge_compare (const Eh575RidgeTouch *touches, unsigned int count, const Eh575RidgeProbe *probe, GCancellable *cancel)
{
  Eh575RidgeResult result {};

  if (!touches || !probe || count < 6 || count > EH575_RIDGE_MAX_TOUCHES)
    {
      result.status = EH575_RIDGE_INVALID;
      return result;
    }
  try {
      Prepared frames[5], representative;
      uint8_t median[EH575_RIDGE_SIZE];
      for (int i = 0; i < 5; i++)
        if (!prepare (probe->images[i], probe->background, frames[i]))
          {
            result.status = EH575_RIDGE_POOR_IMAGE;
            return result;
          }
      for (int i = 0; i < EH575_RIDGE_SIZE; i++)
        {
          uint8_t values[5];
          for (int j = 0; j < 5; j++)
            values[j] = probe->images[j][i];
          std::nth_element (values, values + 2, values + 5);
          median[i] = values[2];
        }
      if (!prepare (median, probe->background, representative))
        {
          result.status = EH575_RIDGE_POOR_IMAGE;
          return result;
        }
      /* Validate the ENTIRE gallery before the early match exit. Otherwise a
       * valid first area could hide a malformed/poor-quality later area that
       * the exhaustive matcher would reject. No template/schema policy changes.
       */
      std::vector<Prepared> references (count);
      struct OrderedArea { double priority; unsigned int index; };
      std::vector<OrderedArea> order;
      for (unsigned int i = 0; i < count; i++)
        {
          if (cancelled (cancel))
            {
              result.status = EH575_RIDGE_CANCELLED;
              return result;
            }
          if (!prepare (touches[i].image, touches[i].background, references[i]))
            {
              result.status = EH575_RIDGE_INVALID;
              return result;
            }
          /* Translation-only NCC is an ordering hint, NEVER evidence for
           * acceptance or a filter. Every attempted area still gets the full
           * rotation/scale/affine search and ambiguity/corroboration checks.
           */
          cv::Mat hint = surface (representative.ridge, references[i].ridge,
                                  representative.mask, references[i].mask);
          double peak;
          cv::minMaxLoc (hint, nullptr, &peak);
          order.push_back ({peak, i});
        }
      std::stable_sort (order.begin (), order.end (), [] (const OrderedArea &a, const OrderedArea &b) {
          return a.priority > b.priority;
        });
      for (const OrderedArea &area : order)
        {
          if (cancelled (cancel))
            {
              result.status = EH575_RIDGE_CANCELLED;
              return result;
            }
          const unsigned int i = area.index;
          const Prepared &reference = references[i];
          Candidate best;
          double margin;
          if (!registration (representative, reference, best, margin, cancel))
            continue;
          unsigned int matched = 0;
          for (const Prepared &frame : frames)
            matched += sufficient (scores (frame, reference, best.transform), margin);
          if (matched > result.matched_frames || (matched == result.matched_frames && best.score.correlation > result.correlation))
            {
              result.matched_frames = matched;
              result.template_touch = i + 1;
              result.correlation = best.score.correlation;
              result.overlap = best.score.overlap;
              result.edge = best.score.edge;
              result.regional = best.score.regional;
              result.margin = margin;
              result.status = matched >= 3 ? EH575_RIDGE_MATCH : EH575_RIDGE_NO_MATCH;
            }
          /* Acceptance is an OR over enrolled areas, not a best-area policy.
           * Once one fully registered area passes the unchanged shared-transform
           * five-frame quorum and ambiguity gates, remaining areas cannot undo
           * that match. Cancellation is checked again below before returning.
           */
          if (result.status == EH575_RIDGE_MATCH)
            break;
        }
      if (cancelled (cancel))
        result.status = EH575_RIDGE_CANCELLED;
    } catch (const cv::Exception &) { result.status = EH575_RIDGE_FAILED;
    }
  catch (const std::exception &) { result.status = EH575_RIDGE_FAILED;
    }
  return result;
}
