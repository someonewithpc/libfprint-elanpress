/*
 * Unit tests for the elanpress NCC sums
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

static int fallbacks;
#define ELANPRESS_COUNT_FALLBACK() (fallbacks++)

/* The sums are static, so the matcher is built into this test */
#include "elanpress-match.c"

/* The six sums of a against b shifted by (dx, dy) over [x0,x1)x[y0,y1),
 * one pixel at a time, without the padding and squaring v itself */
static int
reference_sums (const Layer *a, const Layer *b, int dx, int dy,
                int x0, int y0, int x1, int y1, gint64 s[5])
{
  int n = 0;

  s[0] = s[1] = s[2] = s[3] = s[4] = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++)
      {
        int i = y * a->stride + x;
        int j = (y - dy) * b->stride + (x - dx);

        s[0] += a->v[i] * b->m[j];
        s[1] += b->v[j] * a->m[i];
        s[2] += a->v[i] * a->v[i] * b->m[j];
        s[3] += b->v[j] * b->v[j] * a->m[i];
        s[4] += a->v[i] * b->v[j];
        n += a->m[i] * b->m[j];
      }
  return n;
}

static guint8 *
random_image (GRand *r, int w, int h)
{
  guint8 *img = g_new0 (guint8, (size_t) w * h);

  for (int i = 0; i < w * h; i++)
    img[i] = g_rand_int_range (r, 0, 256);
  return img;
}

/* smoothed noise: texture at about ridge scale that, unlike a periodic
 * pattern, lines up with itself at one shift only */
static guint8 *
texture_image (GRand *r, int w, int h)
{
  g_autofree double *a = g_new (double, (size_t) w * h);
  g_autofree double *b = g_new (double, (size_t) w * h);
  guint8 *img = g_new (guint8, (size_t) w * h);
  double lo = G_MAXDOUBLE, hi = -G_MAXDOUBLE;

  for (int i = 0; i < w * h; i++)
    a[i] = g_rand_double (r);
  for (int pass = 0; pass < 2; pass++)
    {
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          {
            double sum = 0;
            int n = 0;

            for (int v = MAX (0, y - 1); v <= MIN (h - 1, y + 1); v++)
              for (int u = MAX (0, x - 1); u <= MIN (w - 1, x + 1); u++, n++)
                sum += a[v * w + u];
            b[y * w + x] = sum / n;
          }
      memcpy (a, b, sizeof (double) * w * h);
    }
  for (int i = 0; i < w * h; i++)
    {
      lo = MIN (lo, a[i]);
      hi = MAX (hi, a[i]);
    }
  for (int i = 0; i < w * h; i++)
    img[i] = (guint8) lrint (255 * (a[i] - lo) / (hi - lo));
  return img;
}

static void
assert_sums_equal (const gint64 s[5], int n, const gint64 ref[5], int ref_n)
{
  g_assert_cmpint (n, ==, ref_n);
  for (int k = 0; k < 5; k++)
    g_assert_cmpint (s[k], ==, ref[k]);
}

/* every offset with any overlap, as layer_ncc() and layer_ncc_rect() call
 * layer_sums(); covers the table path, the general loop and step 1 */
static void
check_all_offsets (GRand *r, const Layer *a, const Layer *b)
{
  for (int dy = -(b->h - 1); dy < a->h; dy++)
    for (int dx = -(b->w - 1); dx < a->w; dx++)
      {
        int x0 = MAX (0, dx), x1 = MIN (a->w, b->w + dx);
        int y0 = MAX (0, dy), y1 = MIN (a->h, b->h + dy);
        int rx0, rx1, ry0, ry1, n, ref_n;
        gint64 s[5], ref[5];

        n = layer_sums (a, b, dx, dy, x0, y0, x1, y1, VEC, s);
        ref_n = reference_sums (a, b, dx, dy, x0, y0, x1, y1, ref);
        assert_sums_equal (s, n, ref, ref_n);

        rx0 = g_rand_int_range (r, x0, x1);
        rx1 = g_rand_int_range (r, rx0, x1) + 1;
        ry0 = g_rand_int_range (r, y0, y1);
        ry1 = g_rand_int_range (r, ry0, y1) + 1;
        n = layer_sums (a, b, dx, dy, rx0, ry0, rx1, ry1, 1, s);
        ref_n = reference_sums (a, b, dx, dy, rx0, ry0, rx1, ry1, ref);
        assert_sums_equal (s, n, ref, ref_n);
      }
}

static void
test_sums_match_reference (void)
{
  GRand *r = g_rand_new_with_seed (1);

  for (int c = 0; c < 12; c++)
    {
      int w = g_rand_int_range (r, 8, 40), h = g_rand_int_range (r, 8, 30);
      int bw = g_rand_int_range (r, 8, 40), bh = g_rand_int_range (r, 8, 30);
      g_autofree guint8 *pimg = random_image (r, w, h);
      g_autofree guint8 *timg = random_image (r, bw, bh);
      g_autofree guint8 *mask = g_new (guint8, (size_t) bw * bh);
      Layer a, b;

      /* a rotated probe has invalid corners; a template, of its own size
       * like a canvas, is full, masked without holes, or masked with holes
       * and then gets no tables */
      if (c % 2)
        layer_rotated (&a, pimg, w, h, g_rand_double_range (r, -30, 30));
      else
        layer_from_image (&a, pimg, NULL, w, h);
      layer_build_tables (&a);
      for (int i = 0; i < bw * bh; i++)
        mask[i] = (c % 3 == 2 && g_rand_int_range (r, 0, 10) == 0) ? 0 : 255;
      layer_from_image (&b, timg, c % 3 ? mask : NULL, bw, bh);
      if (b.full)
        layer_build_tables (&b);
      check_all_offsets (r, &a, &b);
      layer_clear (&a);
      layer_clear (&b);
    }
  g_rand_free (r);
}

/* The largest square layers that still get tables, all at the pixel value
 * with the largest square: the last entry of the q table comes within
 * 0.03% of G_MAXINT32 */
static void
test_sums_at_table_limit (void)
{
  const int w = 362, h = 362;
  g_autofree guint8 *img = g_new0 (guint8, w * h);
  gint64 s[5], ref[5];
  Layer a, b;
  int n;

  g_assert_cmpint (w * h, <=, TABLE_MAX_PIXELS);
  g_assert_cmpint ((w + 1) * (h + 1), >, TABLE_MAX_PIXELS);
  layer_from_image (&a, img, NULL, w, h);
  layer_from_image (&b, img, NULL, w, h);
  layer_build_tables (&a);
  layer_build_tables (&b);
  g_assert_nonnull (a.sat);
  g_assert_nonnull (b.sat);
  n = layer_sums (&a, &b, 0, 0, 0, 0, w, h, VEC, s);
  assert_sums_equal (s, n, ref, reference_sums (&a, &b, 0, 0, 0, 0, w, h, ref));
  layer_clear (&a);
  layer_clear (&b);
}

/* past TABLE_MAX_PIXELS a layer gets no tables and the general loop runs */
static void
test_sums_without_tables (void)
{
  GRand *r = g_rand_new_with_seed (2);
  const int w = 400, h = 400;
  g_autofree guint8 *pimg = random_image (r, w, h);
  g_autofree guint8 *timg = random_image (r, w, h);
  Layer a, b;

  g_assert_cmpint (w * h, >, TABLE_MAX_PIXELS);
  layer_from_image (&a, pimg, NULL, w, h);
  layer_from_image (&b, timg, NULL, w, h);
  layer_build_tables (&a);
  layer_build_tables (&b);
  g_assert_null (a.sat);
  g_assert_null (b.sat);
  for (int i = 0; i < 8; i++)
    {
      int dx = g_rand_int_range (r, -(w - 1), w), dy = g_rand_int_range (r, -(h - 1), h);
      int x0 = MAX (0, dx), x1 = MIN (a.w, b.w + dx);
      int y0 = MAX (0, dy), y1 = MIN (a.h, b.h + dy);
      gint64 s[5], ref[5];
      int n = layer_sums (&a, &b, dx, dy, x0, y0, x1, y1, VEC, s);

      assert_sums_equal (s, n, ref, reference_sums (&a, &b, dx, dy, x0, y0, x1, y1, ref));
    }
  layer_clear (&a);
  layer_clear (&b);
  g_rand_free (r);
}

/* The sums reference reads layers through the same row stride as the
 * matcher, so these compare the layers with plain w x h arrays */
static void
assert_layer_equal (const Layer *l, const gint16 *v, const guint8 *m, int w, int h)
{
  g_assert_cmpint (l->w, ==, w);
  g_assert_cmpint (l->h, ==, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        g_assert_cmpint (l->m[y * l->stride + x], ==, m[y * w + x]);
        g_assert_cmpint (l->v[y * l->stride + x], ==, m[y * w + x] ? v[y * w + x] : 0);
      }
}

/* one halving of plain arrays: the mean of each 2x2 block, valid only when
 * all four pixels are */
static void
half_arrays (const gint16 *v, const guint8 *m, int w, int h, gint16 *hv, guint8 *hm)
{
  for (int y = 0; y < h / 2; y++)
    for (int x = 0; x < w / 2; x++)
      {
        int i = 2 * y * w + 2 * x;

        hm[y * (w / 2) + x] = m[i] && m[i + 1] && m[i + w] && m[i + w + 1];
        hv[y * (w / 2) + x] = hm[y * (w / 2) + x] ?
                              (v[i] + v[i + 1] + v[i + w] + v[i + w + 1]) / 4 : 0;
      }
}

/* odd sizes, so a halving drops the last row and column */
static void
test_layer_half (void)
{
  GRand *r = g_rand_new_with_seed (5);
  const int w = 75, h = 53;
  g_autofree guint8 *img = random_image (r, w, h);
  g_autofree guint8 *mask = g_new (guint8, (size_t) w * h);
  g_autofree gint16 *v = g_new (gint16, (size_t) w * h);
  g_autofree gint16 *hv = g_new (gint16, (size_t) w * h);
  g_autofree gint16 *qv = g_new (gint16, (size_t) w * h);
  g_autofree guint8 *hm = g_new (guint8, (size_t) w * h);
  g_autofree guint8 *qm = g_new (guint8, (size_t) w * h);
  Layer full, half, quarter;

  for (int i = 0; i < w * h; i++)
    {
      mask[i] = g_rand_int_range (r, 0, 10) != 0;
      v[i] = (gint16) img[i] - 128;
    }
  layer_from_image (&full, img, mask, w, h);
  layer_half (&half, &full);
  layer_half (&quarter, &half);
  assert_layer_equal (&full, v, mask, w, h);
  half_arrays (v, mask, w, h, hv, hm);
  assert_layer_equal (&half, hv, hm, w / 2, h / 2);
  half_arrays (hv, hm, w / 2, h / 2, qv, qm);
  assert_layer_equal (&quarter, qv, qm, w / 4, h / 4);
  layer_clear (&full);
  layer_clear (&half);
  layer_clear (&quarter);
  g_rand_free (r);
}

/* a paste up and to the left grows the canvas on those sides, keeps the
 * pixels already there and fills the rest from the new image */
static void
test_canvas_paste (void)
{
  GRand *r = g_rand_new_with_seed (6);
  const int w = 41, h = 29, dx = 7, dy = 5;
  g_autofree guint8 *a = random_image (r, w, h);
  g_autofree guint8 *b = random_image (r, w, h);
  ElanpressCanvas *c = elanpress_canvas_new_from_image (a, w, h);

  g_assert_true (canvas_paste (c, b, w, h, dx, dy, 0.0, 1000));
  g_assert_cmpint (c->w, ==, w + dx);
  g_assert_cmpint (c->h, ==, h + dy);
  g_assert_cmpint (c->n_images, ==, 2);
  for (int y = 0; y < c->h; y++)
    for (int x = 0; x < c->w; x++)
      {
        gboolean in_a = x >= dx && y >= dy;
        gboolean in_b = x < w && y < h;
        guint8 px = c->px[y * c->w + x];

        g_assert_cmpint (c->mask[y * c->w + x], ==, in_a || in_b);
        if (in_a)
          g_assert_cmpint (px, ==, a[(y - dy) * w + (x - dx)]);
        else if (in_b)
          g_assert_cmpint (px, ==, b[y * w + x]);
      }
  elanpress_canvas_free (c);
  g_rand_free (r);
}

#ifdef ELANPRESS_DISPATCH
typedef void (*CrossSums) (const Layer   *a,
                           const Layer   *b,
                           int            dx,
                           int            dy,
                           int            x0,
                           int            y0,
                           int            x1,
                           int            y1,
                           const gboolean fulla,
                           gint64         s[5]);

/* numbered as cross_sums_level() numbers them */
static const struct
{
  const char *name;
  CrossSums   fn;
  int         level;
} copies[] = {
  { "sse2", cross_sums_sse2, 1 },
  { "avx2", cross_sums_avx2, 2 },
  { "avx512", cross_sums_avx512, 3 },
};

static int
cpu_level (void)
{
  if (__builtin_cpu_supports ("avx512bw"))
    return 3;
  if (__builtin_cpu_supports ("avx2"))
    return 2;
  return 1;
}

static gboolean
skip_unsupported (guint k)
{
  if (copies[k].level <= cpu_level ())
    return FALSE;
  g_autofree char *msg = g_strdup_printf ("no %s on this CPU", copies[k].name);

  g_test_skip (msg);
  return TRUE;
}

/* x0 and x1 widened to whole blocks, as layer_sums_fullb() calls them */
static void
check_copy (guint k, const Layer *a, const Layer *b, int dx, int dy,
            const gboolean fulla)
{
  int x0 = MAX (0, dx), x1 = MIN (a->w, b->w + dx);
  int y0 = MAX (0, dy), y1 = MIN (a->h, b->h + dy);
  int extra = (BLOCK - (x1 - x0) % BLOCK) % BLOCK;
  gint64 s[5] = { 0 }, ref[5];

  reference_sums (a, b, dx, dy, x0, y0, x1, y1, ref);
  copies[k].fn (a, b, dx, dy, x0 - extra / 2, y0, x1 + extra - extra / 2, y1,
                fulla, s);
  g_assert_cmpint (s[4], ==, ref[4]);
  if (!fulla)
    {
      g_assert_cmpint (s[1], ==, ref[1]);
      g_assert_cmpint (s[3], ==, ref[3]);
    }
}

static void
test_cross_sums_copy (gconstpointer data)
{
  guint k = GPOINTER_TO_UINT (data);
  GRand *r;

  if (skip_unsupported (k))
    return;
  r = g_rand_new_with_seed (3);
  for (int c = 0; c < 200; c++)
    {
      int w = g_rand_int_range (r, 8, 160), h = g_rand_int_range (r, 8, 100);
      g_autofree guint8 *pimg = random_image (r, w, h);
      g_autofree guint8 *timg = random_image (r, w, h);
      int dx = g_rand_int_range (r, -(w - 1), w), dy = g_rand_int_range (r, -(h - 1), h);
      Layer a, b;

      if (c % 2)
        layer_rotated (&a, pimg, w, h, g_rand_double_range (r, -30, 30));
      else
        layer_from_image (&a, pimg, NULL, w, h);
      layer_from_image (&b, timg, NULL, w, h);
      check_copy (k, &a, &b, dx, dy, a.full);
      layer_clear (&a);
      layer_clear (&b);
    }
  g_rand_free (r);
}

/* The largest square layers that get tables, at the pixel values with the
 * largest products: the 32-bit total of the lanes comes within 0.03% of
 * 2^31 */
static void
test_cross_sums_copy_at_lane_limit (gconstpointer data)
{
  const int w = 362, h = 362;
  const guint8 values[][2] = { { 0, 0 }, { 0, 255 }, { 255, 255 } };
  guint k = GPOINTER_TO_UINT (data);
  g_autofree guint8 *pimg = g_new (guint8, w * h);
  g_autofree guint8 *timg = g_new (guint8, w * h);

  g_assert_cmpint (w * h, <=, TABLE_MAX_PIXELS);
  g_assert_cmpint ((w + 1) * (h + 1), >, TABLE_MAX_PIXELS);
  if (skip_unsupported (k))
    return;
  for (guint v = 0; v < G_N_ELEMENTS (values); v++)
    {
      Layer a, b;

      memset (pimg, values[v][0], w * h);
      memset (timg, values[v][1], w * h);
      layer_from_image (&a, pimg, NULL, w, h);
      layer_from_image (&b, timg, NULL, w, h);
      check_copy (k, &a, &b, 0, 0, FALSE);
      check_copy (k, &a, &b, 0, 0, TRUE);
      layer_clear (&a);
      layer_clear (&b);
    }
}

/* /proc/cpuinfo as a second opinion on cpu_level(), 0 when it can't be
 * read */
static int
cpuinfo_level (void)
{
  g_autofree char *info = NULL;

  g_autoptr(GRegex) avx512 = g_regex_new ("^flags\\b.*\\bavx512bw\\b", G_REGEX_MULTILINE, 0, NULL);
  g_autoptr(GRegex) avx2 = g_regex_new ("^flags\\b.*\\bavx2\\b", G_REGEX_MULTILINE, 0, NULL);

  if (!g_file_get_contents ("/proc/cpuinfo", &info, NULL, NULL))
    return 0;
  if (g_regex_match (avx512, info, 0, NULL))
    return 3;
  if (g_regex_match (avx2, info, 0, NULL))
    return 2;
  return 1;
}

/* The level is fixed on first use, so each value of FP_ELANPRESS_SIMD
 * gets its own process. Each process also runs the reference sums, so
 * every copy the CPU has goes through cross_sums(). */
static void
test_simd_env (void)
{
  const char *caps[] = { NULL, "", "sse2", "AVX2", "avx512", "neon" };
  int cpu = cpu_level ();

  /* a kernel booted with clearcpuid= or some hypervisors hide flags that
   * CPUID still reports */
  if (cpuinfo_level () != cpu)
    {
      g_test_skip ("/proc/cpuinfo and CPUID disagree on AVX2 or AVX-512BW");
      return;
    }
  if (g_test_subprocess ())
    {
      const char *cap = g_getenv ("FP_ELANPRESS_SIMD");
      int want = cpu;

      if (g_strcmp0 (cap, "sse2") == 0)
        want = 1;
      else if (g_strcmp0 (cap, "AVX2") == 0)
        want = MIN (cpu, 2);
      g_assert_cmpint (cross_sums_level (), ==, want);
      test_sums_match_reference ();
      return;
    }
  for (guint i = 0; i < G_N_ELEMENTS (caps); i++)
    {
      if (caps[i])
        g_setenv ("FP_ELANPRESS_SIMD", caps[i], TRUE);
      else
        g_unsetenv ("FP_ELANPRESS_SIMD");
      g_test_trap_subprocess (NULL, 0, 0);
      g_test_trap_assert_passed ();
      if (g_strcmp0 (caps[i], "neon") == 0)
        g_test_trap_assert_stderr ("*FP_ELANPRESS_SIMD=neon not recognised*");
      else
        g_test_trap_assert_stderr_unmatched ("*not recognised*");
    }
  g_unsetenv ("FP_ELANPRESS_SIMD");
}
#endif

/* The windowed half-resolution search finds the same alignment as the
 * full search for a crop of a larger print */
static void
test_coarse_peaks_find_the_shift (void)
{
  GRand *r = g_rand_new_with_seed (4);
  const int W = 100, H = 70, w = 80, h = 56;

  for (int c = 0; c < 40; c++)
    {
      g_autofree guint8 *big = texture_image (r, W, H);
      g_autofree guint8 *probe = g_new (guint8, w * h);
      g_autofree guint8 *tmpl = g_new (guint8, w * h);
      int sx = g_rand_int_range (r, 0, 20), sy = g_rand_int_range (r, 0, 14);
      ElanpressMatchResult res[2];

      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          {
            probe[y * w + x] = big[(y + sy) * W + x + sx];
            tmpl[y * w + x] = big[y * W + x];
          }
      for (int peaks = 0; peaks < 2; peaks++)
        {
          ElanpressMatchParams p;
          ElanpressProbe *pr;

          elanpress_match_params_default (&p);
          p.rot_prefilter = 3;
          p.coarse_peaks = peaks ? 4 : 0;
          pr = elanpress_probe_new (probe, w, h, &p);
          fallbacks = 0;
          elanpress_probe_match (pr, tmpl, &res[peaks]);
          elanpress_probe_free (pr);
          /* equal results alone would also come from falling back to the
           * full search every time */
          g_assert_cmpint (fallbacks, ==, 0);
        }
      g_assert_cmpfloat (res[0].peak, >, 0.99);
      g_assert_cmpint (res[0].dx, ==, -sx);
      g_assert_cmpint (res[0].dy, ==, -sy);
      g_assert_cmpfloat (fabs (res[0].rot), <, 1e-9);
      g_assert_cmpint (res[1].dx, ==, res[0].dx);
      g_assert_cmpint (res[1].dy, ==, res[0].dy);
      g_assert_cmpfloat (res[1].rot, ==, res[0].rot);
      g_assert_cmpfloat (res[1].score, ==, res[0].score);
    }
  g_rand_free (r);
}

static const Cand *
find_peak (const Cand *c, int k, int dx, int dy)
{
  for (int i = 0; i < k; i++)
    if (c[i].rot >= 0 && c[i].dx == dx && c[i].dy == dy)
      return &c[i];
  return NULL;
}

static int
count_peaks (const Cand *c, int k)
{
  int n = 0;

  for (int i = 0; i < k; i++)
    n += c[i].rot >= 0;
  return n;
}

static void
test_peak_insert (void)
{
  Cand c[3];

  for (int i = 0; i < 3; i++)
    c[i] = (Cand){ -3.0, -1, 0, 0 };

  peak_insert (c, 3, 0.5, 0, 0, 0);
  /* within 2 pixels: a worse peak is dropped, a better one replaces it */
  peak_insert (c, 3, 0.4, 0, 1, 1);
  g_assert_cmpint (count_peaks (c, 3), ==, 1);
  g_assert_nonnull (find_peak (c, 3, 0, 0));
  peak_insert (c, 3, 0.6, 0, 2, -2);
  g_assert_cmpint (count_peaks (c, 3), ==, 1);
  g_assert_nonnull (find_peak (c, 3, 2, -2));
  g_assert_cmpfloat (find_peak (c, 3, 2, -2)->score, ==, 0.6);

  /* 3 pixels apart is far enough to keep both */
  peak_insert (c, 3, 0.3, 0, 5, -2);
  peak_insert (c, 3, 0.2, 0, 20, 20);
  g_assert_cmpint (count_peaks (c, 3), ==, 3);

  /* when full, a far peak replaces the worst only if it beats it */
  peak_insert (c, 3, 0.1, 0, 40, 40);
  g_assert_null (find_peak (c, 3, 40, 40));
  peak_insert (c, 3, 0.25, 0, 40, 40);
  g_assert_null (find_peak (c, 3, 20, 20));
  g_assert_nonnull (find_peak (c, 3, 40, 40));

  /* a peak within 2 pixels of two others that beats both takes one slot
   * and frees the other */
  peak_insert (c, 3, 0.9, 0, 3, -2);
  g_assert_cmpint (count_peaks (c, 3), ==, 2);
  g_assert_nonnull (find_peak (c, 3, 3, -2));
  g_assert_cmpfloat (find_peak (c, 3, 3, -2)->score, ==, 0.9);
  g_assert_nonnull (find_peak (c, 3, 40, 40));

  /* within 2 pixels of a better one: dropped even with a free slot */
  peak_insert (c, 3, 0.8, 0, 4, -1);
  g_assert_cmpint (count_peaks (c, 3), ==, 2);
}

/* This test and test_coarse_peaks_window_radius() use pairs of random
 * images picked on x86-64 with glibc's libm and no FMA, where other
 * rounding can change which offsets they hit */
#if defined(__x86_64__) && defined(__GLIBC__) && !defined(__FMA__)
/* Random pairs whose one quarter-resolution peak sits at an overlap that
 * is too small at half resolution everywhere in its window, so the
 * windowed search finds nothing and has to search the whole rotation */
static void
test_coarse_peaks_fall_back (void)
{
  const guint32 seeds[] = { 14, 1291, 1626 };

  for (guint i = 0; i < G_N_ELEMENTS (seeds); i++)
    {
      GRand *r = g_rand_new_with_seed (seeds[i]);
      int w = g_rand_int_range (r, 24, 64), h = g_rand_int_range (r, 24, 64);
      int tw = g_rand_int_range (r, 24, 64), th = g_rand_int_range (r, 24, 64);
      double min_overlap = g_rand_double_range (r, 0.3, 0.7);
      g_autofree guint8 *probe = random_image (r, w, h);
      g_autofree guint8 *tmpl = random_image (r, tw, th);
      ElanpressMatchResult res[2];

      for (int peaks = 0; peaks < 2; peaks++)
        {
          ElanpressMatchParams p;
          ElanpressProbe *pr;

          elanpress_match_params_default (&p);
          p.min_overlap = min_overlap;
          p.rot_prefilter = 1;
          p.coarse_peaks = peaks;
          pr = elanpress_probe_new (probe, w, h, &p);
          fallbacks = 0;
          elanpress_probe_match_canvas (pr, tmpl, NULL, tw, th, &res[peaks]);
          elanpress_probe_free (pr);
        }
      g_assert_cmpint (fallbacks, ==, 1);
      g_assert_cmpfloat (res[0].peak, >, -2.0);
      g_assert_cmpint (res[1].dx, ==, res[0].dx);
      g_assert_cmpint (res[1].dy, ==, res[0].dy);
      g_assert_cmpfloat (res[1].rot, ==, res[0].rot);
      g_assert_cmpfloat (res[1].score, ==, res[0].score);
      g_rand_free (r);
    }
}

#endif

/* Two unrelated random images, matched with and without the
 * quarter-resolution peaks. Their best alignment is weak and can lie
 * anywhere, so it often sits off the centre of a window. */
static void
match_noise_both_ways (guint32 seed, const guint8 *mask, int score_mode)
{
  const int w = 80, h = 56;
  GRand *r = g_rand_new_with_seed (seed);
  g_autofree guint8 *probe = random_image (r, w, h);
  g_autofree guint8 *tmpl = random_image (r, w, h);
  ElanpressMatchResult res[2];

  for (int peaks = 0; peaks < 2; peaks++)
    {
      ElanpressMatchParams p;
      ElanpressProbe *pr;

      elanpress_match_params_default (&p);
      p.score_mode = score_mode;
      p.coarse_peaks = peaks ? 4 : 0;
      pr = elanpress_probe_new (probe, w, h, &p);
      fallbacks = 0;
      elanpress_probe_match_canvas (pr, tmpl, mask, w, h, &res[peaks]);
      elanpress_probe_free (pr);
      g_assert_cmpint (fallbacks, ==, 0);
    }
  g_assert_cmpfloat (res[0].peak, >, -2.0);
  g_assert_cmpint (res[1].dx, ==, res[0].dx);
  g_assert_cmpint (res[1].dy, ==, res[0].dy);
  g_assert_cmpfloat (res[1].rot, ==, res[0].rot);
  g_assert_cmpfloat (res[1].sidelobe, ==, res[0].sidelobe);
  g_assert_cmpfloat (res[1].score, ==, res[0].score);
  g_rand_free (r);
}

#if defined(__x86_64__) && defined(__GLIBC__) && !defined(__FMA__)
/* pairs whose windowed search finds the full search's offset with the
 * window radius of 3 and misses it with 2 */
static void
test_coarse_peaks_window_radius (void)
{
  const guint32 seeds[] = { 3, 11, 12 };

  for (guint i = 0; i < G_N_ELEMENTS (seeds); i++)
    match_noise_both_ways (seeds[i], NULL, 0);
}
#endif

/* A masked canvas and the sidelobe score modes keep the full search. For
 * these pairs the peaks would change the result. */
static void
test_coarse_peaks_off_for_mask (void)
{
  const guint32 seeds[] = { 0, 2, 3 };
  guint8 mask[80 * 56];

  for (int y = 0; y < 56; y++)
    for (int x = 0; x < 80; x++)
      mask[y * 80 + x] = (x / 8 + y / 8) % 3 ? 255 : 0;
  for (guint i = 0; i < G_N_ELEMENTS (seeds); i++)
    match_noise_both_ways (seeds[i], mask, 0);
}

static void
test_coarse_peaks_off_for_sidelobe (void)
{
  const guint32 seeds[] = { 0, 2, 3 };

  for (guint i = 0; i < G_N_ELEMENTS (seeds); i++)
    match_noise_both_ways (seeds[i], NULL, 1);
}

/* with FP_ELANPRESS_MIN_OVERLAP next to 0, the corners where two noise
 * images barely touch would score 1.0 */
static void
test_min_overlap_floor (void)
{
  const int w = 80, h = 56;
  GRand *r = g_rand_new_with_seed (7);

  for (int c = 0; c < 4; c++)
    {
      g_autofree guint8 *probe = random_image (r, w, h);
      g_autofree guint8 *tmpl = random_image (r, w, h);
      ElanpressMatchParams p;
      ElanpressMatchResult res;
      ElanpressProbe *pr;

      elanpress_match_params_default (&p);
      p.min_overlap = 1e-9;
      pr = elanpress_probe_new (probe, w, h, &p);
      elanpress_probe_match (pr, tmpl, &res);
      elanpress_probe_free (pr);
      g_assert_cmpint (res.overlap, >=, MIN_OVERLAP_PIXELS);
      g_assert_cmpfloat (res.peak, <, 0.9);
    }
  g_rand_free (r);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/elanpress/sums/reference", test_sums_match_reference);
  g_test_add_func ("/elanpress/sums/table-limit", test_sums_at_table_limit);
  g_test_add_func ("/elanpress/sums/without-tables", test_sums_without_tables);
  g_test_add_func ("/elanpress/sums/layer-half", test_layer_half);
  g_test_add_func ("/elanpress/sums/canvas-paste", test_canvas_paste);
#ifdef ELANPRESS_DISPATCH
  for (guint k = 0; k < G_N_ELEMENTS (copies); k++)
    {
      g_autofree char *path = g_strdup_printf ("/elanpress/sums/cross-sums/%s", copies[k].name);
      g_autofree char *limit = g_strdup_printf ("/elanpress/sums/lane-limit/%s", copies[k].name);

      g_test_add_data_func (path, GUINT_TO_POINTER (k), test_cross_sums_copy);
      g_test_add_data_func (limit, GUINT_TO_POINTER (k), test_cross_sums_copy_at_lane_limit);
    }
  g_test_add_func ("/elanpress/sums/simd-env", test_simd_env);
#endif
  g_test_add_func ("/elanpress/match/peak-insert", test_peak_insert);
  g_test_add_func ("/elanpress/match/coarse-peaks", test_coarse_peaks_find_the_shift);
#if defined(__x86_64__) && defined(__GLIBC__) && !defined(__FMA__)
  g_test_add_func ("/elanpress/match/coarse-peaks-fall-back", test_coarse_peaks_fall_back);
  g_test_add_func ("/elanpress/match/coarse-peaks-window", test_coarse_peaks_window_radius);
#endif
  g_test_add_func ("/elanpress/match/coarse-peaks-masked", test_coarse_peaks_off_for_mask);
  g_test_add_func ("/elanpress/match/coarse-peaks-sidelobe", test_coarse_peaks_off_for_sidelobe);
  g_test_add_func ("/elanpress/match/min-overlap-floor", test_min_overlap_floor);

  return g_test_run ();
}
