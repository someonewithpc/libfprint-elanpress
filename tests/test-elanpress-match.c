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
#include <string.h>
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

/* the matcher settings themselves keep their defaults for values that are
 * not finite, which the old parser passed on to casts and comparisons */
static void
test_env_params_not_finite (void)
{
  const char *names[] = {
    "FP_ELANPRESS_THRESHOLD", "FP_ELANPRESS_ROT_RANGE", "FP_ELANPRESS_ROT_STEP",
    "FP_ELANPRESS_MIN_OVERLAP", "FP_ELANPRESS_NORM_WIN", "FP_ELANPRESS_CANDIDATES",
    "FP_ELANPRESS_SCORE_MODE", "FP_ELANPRESS_SIDELOBE_R", "FP_ELANPRESS_BLOCK_WEIGHT",
    "FP_ELANPRESS_ROT_PREFILTER", "FP_ELANPRESS_REG_THRESH", "FP_ELANPRESS_MAX_SIDE",
  };
  const char *values[] = { "nan", "inf", "-inf", "1e999" };

  for (guint v = 0; v < G_N_ELEMENTS (values); v++)
    {
      ElanpressMatchParams def, p;

      /* zeroed first so the padding compares equal too */
      memset (&def, 0, sizeof def);
      memset (&p, 0, sizeof p);
      elanpress_match_params_default (&def);
      for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        g_setenv (names[i], values[v], TRUE);
      elanpress_match_params_from_env (&p);
      for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        g_unsetenv (names[i]);
      g_assert_cmpmem (&p, sizeof p, &def, sizeof def);
    }
}

static const char *int_settings[] = {
  "FP_ELANPRESS_NORM_WIN", "FP_ELANPRESS_CANDIDATES",
  "FP_ELANPRESS_SCORE_MODE", "FP_ELANPRESS_SIDELOBE_R",
  "FP_ELANPRESS_ROT_PREFILTER", "FP_ELANPRESS_MAX_SIDE",
};

static void
set_int_settings (const char *values[])
{
  for (guint i = 0; i < G_N_ELEMENTS (int_settings); i++)
    g_setenv (int_settings[i], values[i], TRUE);
}

static void
test_env_int_settings_fit (void)
{
  const char *huge[] = { "1e30", "1e30", "1e30", "1e30", "1e30", "1e30" };
  const char *at_min[] = { "3", "1", "0", "1", "0", "64" };
  const char *under_min[] = { "2", "0", "-1", "0", "-1", "63" };
  ElanpressMatchParams def, p;

  elanpress_match_params_default (&def);

  set_int_settings (huge);
  elanpress_match_params_from_env (&p);
  g_assert_cmpint (p.norm_win, ==, G_MAXINT);
  g_assert_cmpint (p.candidates, ==, ELANPRESS_MAX_CANDIDATES);
  g_assert_cmpint (p.score_mode, ==, 2);
  g_assert_cmpint (p.sidelobe_r, ==, G_MAXINT);
  g_assert_cmpint (p.rot_prefilter, ==, G_MAXINT);
  g_assert_cmpint (p.max_side, ==, G_MAXINT);

  set_int_settings (at_min);
  elanpress_match_params_from_env (&p);
  g_assert_cmpint (p.norm_win, ==, 3);
  g_assert_cmpint (p.candidates, ==, 1);
  g_assert_cmpint (p.score_mode, ==, 0);
  g_assert_cmpint (p.sidelobe_r, ==, 1);
  g_assert_cmpint (p.rot_prefilter, ==, 0);
  g_assert_cmpint (p.max_side, ==, 64);

  /* SCORE_MODE has no minimum; it clamps to 0 */
  set_int_settings (under_min);
  elanpress_match_params_from_env (&p);
  g_assert_cmpint (p.norm_win, ==, def.norm_win);
  g_assert_cmpint (p.candidates, ==, def.candidates);
  g_assert_cmpint (p.score_mode, ==, 0);
  g_assert_cmpint (p.sidelobe_r, ==, def.sidelobe_r);
  g_assert_cmpint (p.rot_prefilter, ==, def.rot_prefilter);
  g_assert_cmpint (p.max_side, ==, def.max_side);

  g_setenv ("FP_ELANPRESS_NORM_WIN", "8", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpint (p.norm_win, ==, 9);

  /* SCORE_MODE has no minimum, so a huge negative value reaches the cast.
   * Its default is 0, so the checks for 0 can't tell a clamp from an
   * ignored value; they only catch the cast under a float-cast-overflow
   * sanitizer, which -fsanitize=undefined leaves out in GCC */
  g_setenv ("FP_ELANPRESS_SCORE_MODE", "1", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpint (p.score_mode, ==, 1);
  g_setenv ("FP_ELANPRESS_SCORE_MODE", "-1e30", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpint (p.score_mode, ==, 0);

  /* the candidate cap on its own, with values that fit an int */
  g_setenv ("FP_ELANPRESS_CANDIDATES", "2e9", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpint (p.candidates, ==, ELANPRESS_MAX_CANDIDATES);
  g_setenv ("FP_ELANPRESS_CANDIDATES", "64", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpint (p.candidates, ==, 64);

  for (guint i = 0; i < G_N_ELEMENTS (int_settings); i++)
    g_unsetenv (int_settings[i]);
}

static void
test_env_rotation_bounded (void)
{
  ElanpressMatchParams def, p;

  elanpress_match_params_default (&def);

  g_setenv ("FP_ELANPRESS_ROT_RANGE", "-1", TRUE);
  g_setenv ("FP_ELANPRESS_ROT_STEP", "1e-30", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpfloat (p.rot_range, ==, 0);
  g_assert_cmpfloat (p.rot_step, ==, ELANPRESS_MIN_ROT_STEP);

  g_setenv ("FP_ELANPRESS_ROT_RANGE", "1e30", TRUE);
  g_setenv ("FP_ELANPRESS_ROT_STEP", "0", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpfloat (p.rot_range, ==, ELANPRESS_MAX_ROT_RANGE);
  g_assert_cmpfloat (p.rot_step, ==, def.rot_step);

  /* a negative step is ignored too, not raised to the minimum */
  g_setenv ("FP_ELANPRESS_ROT_STEP", "-5", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpfloat (p.rot_step, ==, def.rot_step);

  g_setenv ("FP_ELANPRESS_ROT_RANGE", "20", TRUE);
  g_setenv ("FP_ELANPRESS_ROT_STEP", "2.5", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpfloat (p.rot_range, ==, 20);
  g_assert_cmpfloat (p.rot_step, ==, 2.5);

  g_unsetenv ("FP_ELANPRESS_ROT_RANGE");
  g_unsetenv ("FP_ELANPRESS_ROT_STEP");
}

/* the bounds above are what limit the probe's rotated copies, when the
 * settings come from the environment */
static void
test_probe_rotation_count (void)
{
  g_autofree guint8 *img = g_malloc0 (64 * 88);
  ElanpressMatchParams p;
  ElanpressProbe *pr;

  elanpress_match_params_default (&p);
  pr = elanpress_probe_new (img, 64, 88, &p);
  g_assert_cmpint (elanpress_probe_n_rotations (pr), ==, 15);
  elanpress_probe_free (pr);

  g_setenv ("FP_ELANPRESS_ROT_RANGE", "1e9", TRUE);
  g_setenv ("FP_ELANPRESS_ROT_STEP", "1e-9", TRUE);
  elanpress_match_params_from_env (&p);
  g_unsetenv ("FP_ELANPRESS_ROT_RANGE");
  g_unsetenv ("FP_ELANPRESS_ROT_STEP");
  pr = elanpress_probe_new (img, 64, 88, &p);
  g_assert_cmpint (elanpress_probe_n_rotations (pr), ==, 181);
  elanpress_probe_free (pr);

  p.rot_range = ELANPRESS_MAX_ROT_RANGE;
  p.rot_step = ELANPRESS_MIN_ROT_STEP;
  pr = elanpress_probe_new (img, 64, 88, &p);
  g_assert_cmpint (elanpress_probe_n_rotations (pr), ==, 181);
  elanpress_probe_free (pr);

  p.rot_range = 0;
  pr = elanpress_probe_new (img, 64, 88, &p);
  g_assert_cmpint (elanpress_probe_n_rotations (pr), ==, 1);
  elanpress_probe_free (pr);
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
  g_test_add_func ("/elanpress/env/params-not-finite", test_env_params_not_finite);
  g_test_add_func ("/elanpress/env/int-settings-fit",
                   test_env_int_settings_fit);
  g_test_add_func ("/elanpress/env/rotation-bounded",
                   test_env_rotation_bounded);
  g_test_add_func ("/elanpress/probe/rotation-count",
                   test_probe_rotation_count);

  return g_test_run ();
}
