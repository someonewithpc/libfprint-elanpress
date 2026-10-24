/*
 * Unit tests for the elanpress frame selection helpers
 * Copyright (C) 2026 Hugo Sales <hugo@hsal.es>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <glib.h>
#include <math.h>
#include "elanpress-match.h"

static const ElanpressGates gates = {
  ELANPRESS_MIN_MEAN, ELANPRESS_MIN_CONTRAST, ELANPRESS_MIN_COVERAGE
};

/* A touch recorded on 04f3:0c6e: one frame taken while the finger landed,
 * then eight full-contact frames with half its contrast */
static void
test_landing_frame_sets_no_bar (void)
{
  ElanpressQuality q[9];
  gboolean pass[9];

  q[0] = (ElanpressQuality){ 2000, 1514, 0.34 };
  for (int i = 1; i < 9; i++)
    q[i] = (ElanpressQuality){ 2000, 670, 1.00 };

  g_assert_cmpfloat_with_epsilon (elanpress_contrast_bar (q, 9, &gates),
                                  335, 1e-9);
  elanpress_frames_pass (q, 9, &gates, pass);
  g_assert_false (pass[0]);
  for (int i = 1; i < 9; i++)
    g_assert_true (pass[i]);
}

static void
test_frames_under_the_bar_fail (void)
{
  ElanpressQuality q[] = {
    { 2000, 600, 0.90 },
    { 2000, 299, 0.90 },   /* under half of 600 */
    { 2000, 300, 0.90 },
  };
  gboolean pass[3];

  elanpress_frames_pass (q, 3, &gates, pass);
  g_assert_true (pass[0]);
  g_assert_false (pass[1]);
  g_assert_true (pass[2]);
}

static void
test_bar_is_half_the_best_passing_frame (void)
{
  ElanpressQuality q[] = {
    { 2000, 200, 0.80 },
    { 2000, 600, 0.90 },
    { 100, 900, 0.90 },   /* fails the mean gate */
  };

  g_assert_cmpfloat_with_epsilon (elanpress_contrast_bar (q, 3, &gates),
                                  300, 1e-9);
}

static void
test_no_frame_passes (void)
{
  ElanpressQuality q[] = {
    { 2000, 1500, 0.20 },
    { 100, 800, 1.00 },
  };
  gboolean pass[2];

  g_assert_cmpfloat (elanpress_contrast_bar (q, 2, &gates), ==, 0);
  g_assert_cmpfloat (elanpress_contrast_bar (NULL, 0, &gates), ==, 0);
  elanpress_frames_pass (q, 2, &gates, pass);
  g_assert_false (pass[0]);
  g_assert_false (pass[1]);
}

static void
test_nan_gate_rejects_nothing (void)
{
  const ElanpressGates nan_gates = { NAN, NAN, NAN };
  ElanpressQuality q = { 0, 0, 0 };

  g_assert_true (elanpress_quality_passes (&q, &nan_gates));
}

static void
test_env_double (void)
{
  const char *rejected[] = {
    "", "abc", "nan", "NAN", "inf", "-inf", "1e999", "-1e999",
  };
  double d = 7;

  g_unsetenv ("FP_ELANPRESS_TEST");
  g_assert_false (elanpress_env_double ("FP_ELANPRESS_TEST", &d));
  for (guint i = 0; i < G_N_ELEMENTS (rejected); i++)
    {
      g_setenv ("FP_ELANPRESS_TEST", rejected[i], TRUE);
      g_assert_false (elanpress_env_double ("FP_ELANPRESS_TEST", &d));
    }
  g_assert_cmpfloat (d, ==, 7);

  g_setenv ("FP_ELANPRESS_TEST", "0.85", TRUE);
  g_assert_true (elanpress_env_double ("FP_ELANPRESS_TEST", &d));
  g_assert_cmpfloat (d, ==, 0.85);

  /* text after the number has always been ignored; keep it that way */
  g_setenv ("FP_ELANPRESS_TEST", "0.5x", TRUE);
  g_assert_true (elanpress_env_double ("FP_ELANPRESS_TEST", &d));
  g_assert_cmpfloat (d, ==, 0.5);
  g_unsetenv ("FP_ELANPRESS_TEST");
}

int
main (int argc, char *argv[])
{
  g_auto(GStrv) names = g_listenv ();

  g_test_init (&argc, &argv, NULL);

  /* each test sets the FP_ELANPRESS_* variables it needs, and one exported
   * by whoever runs the tests would change what the others read */
  for (guint i = 0; names[i]; i++)
    if (g_str_has_prefix (names[i], "FP_ELANPRESS_"))
      g_unsetenv (names[i]);

  g_test_add_func ("/elanpress/contrast-bar/landing-frame",
                   test_landing_frame_sets_no_bar);
  g_test_add_func ("/elanpress/contrast-bar/best-passing",
                   test_bar_is_half_the_best_passing_frame);
  g_test_add_func ("/elanpress/contrast-bar/under-the-bar",
                   test_frames_under_the_bar_fail);
  g_test_add_func ("/elanpress/contrast-bar/none-pass",
                   test_no_frame_passes);
  g_test_add_func ("/elanpress/quality/nan-gate",
                   test_nan_gate_rejects_nothing);

  g_test_add_func ("/elanpress/env/double", test_env_double);

  return g_test_run ();
}
