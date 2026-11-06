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
#include <locale.h>
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
    "abc", "nan", "NAN", "inf", "-inf", "1e999", "-1e999", "0.5x", "  ",
    "0.5 x",
  };
  const char *accepted[] = { "0.85", " 0.85", "0.85 ", "0.85\n", "\t0.85\t" };

  /* unset and empty log nothing: anything before the sentinel fails */
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, "sentinel");
  g_unsetenv ("FP_ELANPRESS_TEST");
  g_assert_cmpfloat (elanpress_env_double ("FP_ELANPRESS_TEST", 7), ==, 7);
  g_setenv ("FP_ELANPRESS_TEST", "", TRUE);
  g_assert_cmpfloat (elanpress_env_double ("FP_ELANPRESS_TEST", 7), ==, 7);
  g_message ("sentinel");
  g_test_assert_expected_messages ();
  for (guint i = 0; i < G_N_ELEMENTS (rejected); i++)
    {
      g_autofree char *message = g_strdup_printf (
        "Ignoring FP_ELANPRESS_TEST=\"%s\", expected a number, using 7", rejected[i]);

      g_setenv ("FP_ELANPRESS_TEST", rejected[i], TRUE);
      g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message);
      g_assert_cmpfloat (elanpress_env_double ("FP_ELANPRESS_TEST", 7), ==, 7);
      g_test_assert_expected_messages ();
    }

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, "sentinel");
  for (guint i = 0; i < G_N_ELEMENTS (accepted); i++)
    {
      g_setenv ("FP_ELANPRESS_TEST", accepted[i], TRUE);
      g_assert_cmpfloat (elanpress_env_double ("FP_ELANPRESS_TEST", 7), ==, 0.85);
    }
  g_message ("sentinel");
  g_test_assert_expected_messages ();
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

typedef void (*EnvReader) (void);

static void
read_params (void)
{
  ElanpressMatchParams p;

  elanpress_match_params_from_env (&p);
}

static void
read_config (void)
{
  ElanpressDeviceConfig cfg = { 0.76, 20, 3, 0.85 };

  elanpress_device_config_from_env (&cfg);
}

static void
read_gates (void)
{
  ElanpressGates g;

  elanpress_gates_from_env (&g);
}

/* Runs read with name=value and checks that it logs message and nothing
 * else. A message that doesn't match the head of the expected list aborts
 * the test, so a stray one before or after fails on the sentinel. */
static void
expect_logged (EnvReader read, const char *name, const char *value,
               const char *message)
{
  if (name)
    g_setenv (name, value, TRUE);
  if (message)
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message);
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, "sentinel");
  read ();
  g_message ("sentinel");
  g_test_assert_expected_messages ();
  if (name)
    g_unsetenv (name);
}

/* the messages print numbers the way the parser reads them, with a '.',
 * also where the locale writes a ','. Both readers' messages that name a
 * default are checked; env_clamp () only prints whole-number bounds */
static void
test_env_logged_locale (void)
{
  const char *locales[] = { "de_DE.UTF-8", "fr_FR.UTF-8", "pt_PT.UTF-8" };
  g_autofree char *old = g_strdup (setlocale (LC_NUMERIC, NULL));
  const char *set = NULL;
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  ElanpressMatchParams def;
  char *message;

  for (guint i = 0; i < G_N_ELEMENTS (locales) && !set; i++)
    set = setlocale (LC_NUMERIC, locales[i]);
  if (!set)
    {
      g_test_skip ("no locale with a decimal comma installed");
      return;
    }
  g_snprintf (buf, sizeof buf, "%g", 0.5);
  g_assert_cmpstr (buf, ==, "0,5");

  elanpress_match_params_default (&def);
  message = g_strdup_printf ("Ignoring FP_ELANPRESS_MIN_OVERLAP=\"3\", expected a number in (0, 1], using %s",
                             g_ascii_formatd (buf, sizeof buf, "%g", def.min_overlap));
  expect_logged (read_params, "FP_ELANPRESS_MIN_OVERLAP", "3", message);
  g_free (message);
  message = g_strdup_printf ("Ignoring FP_ELANPRESS_REG_THRESH=\"0,7\", expected a number, using %s",
                             g_ascii_formatd (buf, sizeof buf, "%g", def.reg_thresh));
  expect_logged (read_params, "FP_ELANPRESS_REG_THRESH", "0,7", message);
  g_free (message);
  setlocale (LC_NUMERIC, old);
}

/* every value that is ignored or changed says so */
static void
test_env_logged (void)
{
  expect_logged (read_params, "FP_ELANPRESS_REG_THRESH", "0.7x",
                 "Ignoring FP_ELANPRESS_REG_THRESH=\"0.7x\", expected a number, using 0.75");
  /* the value is escaped, so it can't break the log line */
  expect_logged (read_params, "FP_ELANPRESS_REG_THRESH", "0.7\nx",
                 "Ignoring FP_ELANPRESS_REG_THRESH=\"0.7\\nx\", expected a number, using 0.75");
  expect_logged (read_params, "FP_ELANPRESS_ROT_STEP", "-1\n",
                 "Ignoring FP_ELANPRESS_ROT_STEP=\"-1\\n\", expected a number above 0, using 4");
  expect_logged (read_params, "FP_ELANPRESS_REG_THRESH", "nan",
                 "Ignoring FP_ELANPRESS_REG_THRESH=\"nan\", expected a number, using 0.75");
  /* the threshold is the device's, read with the device config */
  expect_logged (read_params, "FP_ELANPRESS_THRESHOLD", "x", NULL);
  expect_logged (read_params, "FP_ELANPRESS_NORM_WIN", "2",
                 "Ignoring FP_ELANPRESS_NORM_WIN=\"2\", expected at least 3, using 11");
  expect_logged (read_params, "FP_ELANPRESS_NORM_WIN", "8",
                 "FP_ELANPRESS_NORM_WIN=\"8\" is not odd, using 9");
  expect_logged (read_params, "FP_ELANPRESS_NORM_WIN", "8.5",
                 "FP_ELANPRESS_NORM_WIN=\"8.5\" is not a whole number, using 9");
  expect_logged (read_params, "FP_ELANPRESS_NORM_WIN", "x",
                 "Ignoring FP_ELANPRESS_NORM_WIN=\"x\", expected a number, using 11");
  expect_logged (read_params, "FP_ELANPRESS_ROT_STEP", "x",
                 "Ignoring FP_ELANPRESS_ROT_STEP=\"x\", expected a number, using 4");
  expect_logged (read_params, "FP_ELANPRESS_SIDELOBE_R", "0",
                 "Ignoring FP_ELANPRESS_SIDELOBE_R=\"0\", expected at least 1, using 4");
  expect_logged (read_params, "FP_ELANPRESS_ROT_PREFILTER", "-1",
                 "Ignoring FP_ELANPRESS_ROT_PREFILTER=\"-1\", expected at least 0, using 3");
  expect_logged (read_params, "FP_ELANPRESS_MAX_SIDE", "10",
                 "Ignoring FP_ELANPRESS_MAX_SIDE=\"10\", expected at least 64, using 256");
  expect_logged (read_params, "FP_ELANPRESS_NORM_WIN", "1e12",
                 "FP_ELANPRESS_NORM_WIN=\"1e12\" is out of range, using 2147483647");
  expect_logged (read_params, "FP_ELANPRESS_NORM_WIN", "9.5",
                 "FP_ELANPRESS_NORM_WIN=\"9.5\" is not a whole number, using 9");
  expect_logged (read_params, "FP_ELANPRESS_CANDIDATES", "2.9",
                 "FP_ELANPRESS_CANDIDATES=\"2.9\" is not a whole number, using 2");
  expect_logged (read_params, "FP_ELANPRESS_SCORE_MODE", "1.7",
                 "FP_ELANPRESS_SCORE_MODE=\"1.7\" is not a whole number, using 1");
  expect_logged (read_params, "FP_ELANPRESS_CANDIDATES", "100",
                 "FP_ELANPRESS_CANDIDATES=\"100\" is out of range, using 64");
  expect_logged (read_params, "FP_ELANPRESS_ROT_STEP", "0",
                 "Ignoring FP_ELANPRESS_ROT_STEP=\"0\", expected a number above 0, using 4");
  expect_logged (read_params, "FP_ELANPRESS_ROT_STEP", "0.1",
                 "FP_ELANPRESS_ROT_STEP=\"0.1\" is out of range, using 1");
  expect_logged (read_params, "FP_ELANPRESS_ROT_RANGE", "-1",
                 "FP_ELANPRESS_ROT_RANGE=\"-1\" is out of range, using 0");
  expect_logged (read_params, "FP_ELANPRESS_MIN_OVERLAP", "3",
                 "Ignoring FP_ELANPRESS_MIN_OVERLAP=\"3\", expected a number in (0, 1], using 0.4");
  expect_logged (read_params, "FP_ELANPRESS_MIN_OVERLAP", "0",
                 "Ignoring FP_ELANPRESS_MIN_OVERLAP=\"0\", expected a number in (0, 1], using 0.4");
  expect_logged (read_params, "FP_ELANPRESS_SCORE_MODE", "5",
                 "FP_ELANPRESS_SCORE_MODE=\"5\" is out of range, using 2");
  expect_logged (read_params, "FP_ELANPRESS_BLOCK_WEIGHT", "-0.5",
                 "FP_ELANPRESS_BLOCK_WEIGHT=\"-0.5\" is out of range, using 0");

  expect_logged (read_config, "FP_ELANPRESS_THRESHOLD", "x",
                 "Ignoring FP_ELANPRESS_THRESHOLD=\"x\", expected a number, using 0.76");
  expect_logged (read_config, "FP_ELANPRESS_ENROLL_STAGES", "100",
                 "FP_ELANPRESS_ENROLL_STAGES=\"100\" is out of range, using 40");
  expect_logged (read_config, "FP_ELANPRESS_ENROLL_STAGES", "12.9",
                 "FP_ELANPRESS_ENROLL_STAGES=\"12.9\" is not a whole number, using 12");
  expect_logged (read_config, "FP_ELANPRESS_ENROLL_STAGES", "x",
                 "Ignoring FP_ELANPRESS_ENROLL_STAGES=\"x\", expected a number, using 20");
  expect_logged (read_config, "FP_ELANPRESS_IMAGES_PER_TOUCH", "20",
                 "FP_ELANPRESS_IMAGES_PER_TOUCH=\"20\" is out of range, using 14");
  expect_logged (read_config, "FP_ELANPRESS_IMAGES_PER_TOUCH", "0",
                 "FP_ELANPRESS_IMAGES_PER_TOUCH=\"0\" is out of range, using 1");
  expect_logged (read_config, "FP_ELANPRESS_DUP_SIMILARITY", "1.5",
                 "FP_ELANPRESS_DUP_SIMILARITY=\"1.5\" is out of range, using 1");

  expect_logged (read_gates, "FP_ELANPRESS_MIN_MEAN", "inf",
                 "Ignoring FP_ELANPRESS_MIN_MEAN=\"inf\", expected a number, using 150");
  expect_logged (read_gates, "FP_ELANPRESS_MIN_CONTRAST", "4o",
                 "Ignoring FP_ELANPRESS_MIN_CONTRAST=\"4o\", expected a number, using 40");
  expect_logged (read_gates, "FP_ELANPRESS_MIN_COVERAGE", "",
                 NULL);

  /* values in range and unset or empty variables log nothing */
  g_setenv ("FP_ELANPRESS_CANDIDATES", "", TRUE);
  expect_logged (read_params, "FP_ELANPRESS_MIN_OVERLAP", "0.3", NULL);
  expect_logged (read_config, "FP_ELANPRESS_ENROLL_STAGES", "40", NULL);
  expect_logged (read_gates, "FP_ELANPRESS_MIN_COVERAGE", "0.5", NULL);
  g_unsetenv ("FP_ELANPRESS_CANDIDATES");
}

/* the readers take each setting from its variable */
static void
test_env_config_and_gates (void)
{
  ElanpressDeviceConfig cfg = { 0.76, 20, 3, 0.85 };
  ElanpressMatchParams p;
  ElanpressGates g;

  /* the threshold is the device's; the matcher's reader keeps the global
   * default for elanpress_load_config () to replace */
  g_setenv ("FP_ELANPRESS_THRESHOLD", "0.9", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpfloat (p.threshold, ==, 0.76);

  g_setenv ("FP_ELANPRESS_THRESHOLD", "0.8", TRUE);
  g_setenv ("FP_ELANPRESS_ENROLL_STAGES", "12", TRUE);
  g_setenv ("FP_ELANPRESS_IMAGES_PER_TOUCH", "5", TRUE);
  g_setenv ("FP_ELANPRESS_DUP_SIMILARITY", "0.9", TRUE);
  elanpress_device_config_from_env (&cfg);
  g_assert_cmpfloat (cfg.threshold, ==, 0.8);
  g_assert_cmpint (cfg.enroll_stages, ==, 12);
  g_assert_cmpint (cfg.images_per_touch, ==, 5);
  g_assert_cmpfloat (cfg.dup_similarity, ==, 0.9);

  /* the values used, as the messages in test_env_logged () name them */
  g_setenv ("FP_ELANPRESS_ENROLL_STAGES", "100", TRUE);
  g_setenv ("FP_ELANPRESS_IMAGES_PER_TOUCH", "0", TRUE);
  g_setenv ("FP_ELANPRESS_DUP_SIMILARITY", "1.5", TRUE);
  elanpress_device_config_from_env (&cfg);
  g_assert_cmpint (cfg.enroll_stages, ==, ELANPRESS_MAX_ENROLL_STAGES);
  g_assert_cmpint (cfg.images_per_touch, ==, 1);
  g_assert_cmpfloat (cfg.dup_similarity, ==, 1);
  g_setenv ("FP_ELANPRESS_ENROLL_STAGES", "12.9", TRUE);
  elanpress_device_config_from_env (&cfg);
  g_assert_cmpint (cfg.enroll_stages, ==, 12);

  g_unsetenv ("FP_ELANPRESS_THRESHOLD");
  g_unsetenv ("FP_ELANPRESS_ENROLL_STAGES");
  g_unsetenv ("FP_ELANPRESS_IMAGES_PER_TOUCH");
  g_unsetenv ("FP_ELANPRESS_DUP_SIMILARITY");

  elanpress_gates_from_env (&g);
  g_assert_cmpfloat (g.min_mean, ==, ELANPRESS_MIN_MEAN);
  g_assert_cmpfloat (g.min_contrast, ==, ELANPRESS_MIN_CONTRAST);
  g_assert_cmpfloat (g.min_coverage, ==, ELANPRESS_MIN_COVERAGE);
  g_setenv ("FP_ELANPRESS_MIN_MEAN", "200", TRUE);
  g_setenv ("FP_ELANPRESS_MIN_CONTRAST", "60", TRUE);
  g_setenv ("FP_ELANPRESS_MIN_COVERAGE", "0.5", TRUE);
  elanpress_gates_from_env (&g);
  g_assert_cmpfloat (g.min_mean, ==, 200);
  g_assert_cmpfloat (g.min_contrast, ==, 60);
  g_assert_cmpfloat (g.min_coverage, ==, 0.5);
  g_unsetenv ("FP_ELANPRESS_MIN_MEAN");
  g_unsetenv ("FP_ELANPRESS_MIN_CONTRAST");
  g_unsetenv ("FP_ELANPRESS_MIN_COVERAGE");
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

/* the probe of a random image matched against the image itself */
static ElanpressMatchResult
match_self (double min_overlap)
{
  g_autoptr(GRand) r = g_rand_new_with_seed (1);
  g_autofree guint8 *img = g_malloc (64 * 88);
  ElanpressMatchParams p;
  ElanpressMatchResult res;
  ElanpressProbe *pr;

  for (int i = 0; i < 64 * 88; i++)
    img[i] = g_rand_int_range (r, 0, 256);
  elanpress_match_params_default (&p);
  p.min_overlap = min_overlap;
  pr = elanpress_probe_new (img, 64, 88, &p);
  elanpress_probe_match (pr, img, &res);
  elanpress_probe_free (pr);
  return res;
}

static void
test_env_min_overlap_fraction (void)
{
  const char *ignored[] = { "1e30", "1.5", "1.0001", "0", "-1e30" };
  ElanpressMatchParams def, p;
  ElanpressMatchResult res;

  elanpress_match_params_default (&def);
  for (guint i = 0; i < G_N_ELEMENTS (ignored); i++)
    {
      g_setenv ("FP_ELANPRESS_MIN_OVERLAP", ignored[i], TRUE);
      elanpress_match_params_from_env (&p);
      g_assert_cmpfloat (p.min_overlap, ==, def.min_overlap);
    }

  g_setenv ("FP_ELANPRESS_MIN_OVERLAP", "1", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpfloat (p.min_overlap, ==, 1.0);

  g_setenv ("FP_ELANPRESS_MIN_OVERLAP", "0.3", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpfloat (p.min_overlap, ==, 0.3);

  g_setenv ("FP_ELANPRESS_MIN_OVERLAP", "1e-6", TRUE);
  elanpress_match_params_from_env (&p);
  g_assert_cmpfloat (p.min_overlap, ==, 1e-6);

  /* the value that overflowed the cast reaches the matcher as the default */
  g_setenv ("FP_ELANPRESS_MIN_OVERLAP", "1e30", TRUE);
  elanpress_match_params_from_env (&p);
  res = match_self (p.min_overlap);
  g_assert_cmpint (res.overlap, ==, 64 * 88);

  g_unsetenv ("FP_ELANPRESS_MIN_OVERLAP");

  /* at the top of the range only the full overlap counts, and it is found */
  res = match_self (1.0);
  g_assert_cmpint (res.overlap, ==, 64 * 88);
  g_assert_cmpfloat (res.score, >, 0.99);
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
  g_test_add_func ("/elanpress/env/logged", test_env_logged);
  g_test_add_func ("/elanpress/env/logged-locale", test_env_logged_locale);
  g_test_add_func ("/elanpress/env/config-and-gates", test_env_config_and_gates);
  g_test_add_func ("/elanpress/env/int-settings-fit",
                   test_env_int_settings_fit);
  g_test_add_func ("/elanpress/env/rotation-bounded",
                   test_env_rotation_bounded);
  g_test_add_func ("/elanpress/probe/rotation-count",
                   test_probe_rotation_count);
  g_test_add_func ("/elanpress/env/min-overlap-fraction",
                   test_env_min_overlap_fraction);

  return g_test_run ();
}
