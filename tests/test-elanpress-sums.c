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
#else
/* with one copy of the sums, a set FP_ELANPRESS_SIMD is logged once */
static void
test_simd_env_unused (void)
{
  const char *caps[] = { NULL, "", "avx2" };

  if (g_test_subprocess ())
    {
      g_autofree guint8 *img = g_malloc0 (32 * 32);
      ElanpressMatchParams p;

      elanpress_match_params_default (&p);
      for (int i = 0; i < 2; i++)
        elanpress_probe_free (elanpress_probe_new (img, 32, 32, &p));
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
      if (caps[i] && *caps[i])
        g_test_trap_assert_stderr ("*FP_ELANPRESS_SIMD=avx2 has no effect*");
      else
        g_test_trap_assert_stderr_unmatched ("*has no effect*");
      g_test_trap_assert_stderr_unmatched ("*has no effect*has no effect*");
    }
  g_unsetenv ("FP_ELANPRESS_SIMD");
}
#endif

#if defined(__x86_64__) && defined(__GLIBC__) && !defined(__FMA__)
/* Whole matches with the default settings, against what the matcher found
 * before the sums were rewritten: two pairs cut from one noise image with
 * noise added to the template, and two unrelated ones. Other rounding, as
 * from FMA, could change the last bits, so this only runs on x86-64 */
static void
test_match_unchanged (void)
{
  const ElanpressMatchResult want[] = {
    { 0x1.65a04ee56748bp-1, 0x1.65a04ee56748bp-1, -0x1p+1, 0x1.3688601441729p-1, 0x1.656a2574ebc54p-1, -18, -2, 0x0p+0, 3348 },
    { 0x1.b7397aebcf748p-5, 0x1.b7397aebcf748p-5, -0x1p+1, -0x1.ff60803c79be1p-7, 0x1.d748b991295fbp-5, -23, -17, 0x1.4p+4, 2098 },
    { 0x1.f14c27efabb98p-5, 0x1.f14c27efabb98p-5, -0x1p+1, -0x1.27378879e74e6p-6, 0x1.d1d5585c38699p-5, 34, -12, -0x1.8p+3, 1949 },
    { 0x1.2416154099d2ep-4, 0x1.2416154099d2ep-4, -0x1p+1, -0x1.210e939b8bb79p-5, 0x1.1f9b2d9934d8ep-4, -33, 10, 0x1.8p+4, 1860 },
  };
  GRand *r = g_rand_new_with_seed (21);
  const int W = 100, H = 70, w = 80, h = 56;

  for (guint c = 0; c < G_N_ELEMENTS (want); c++)
    {
      g_autofree guint8 *big = random_image (r, W, H);
      g_autofree guint8 *other = random_image (r, w, h);
      g_autofree guint8 *probe = g_new (guint8, w * h);
      g_autofree guint8 *tmpl = g_new (guint8, w * h);
      int sx = g_rand_int_range (r, 0, 20), sy = g_rand_int_range (r, 0, 14);
      ElanpressMatchParams p;
      ElanpressMatchResult res;
      ElanpressProbe *pr;

      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          {
            probe[y * w + x] = big[(y + sy) * W + x + sx];
            tmpl[y * w + x] = c < 2 ?
                              CLAMP (big[y * W + x] + g_rand_int_range (r, -40, 41), 0, 255) :
                              other[y * w + x];
          }
      elanpress_match_params_default (&p);
      pr = elanpress_probe_new (probe, w, h, &p);
      elanpress_probe_match (pr, tmpl, &res);
      elanpress_probe_free (pr);
      g_assert_cmpfloat (res.score, ==, want[c].score);
      g_assert_cmpfloat (res.peak, ==, want[c].peak);
      g_assert_cmpfloat (res.sidelobe, ==, want[c].sidelobe);
      g_assert_cmpfloat (res.block_min, ==, want[c].block_min);
      g_assert_cmpfloat (res.block_mean, ==, want[c].block_mean);
      g_assert_cmpint (res.dx, ==, want[c].dx);
      g_assert_cmpint (res.dy, ==, want[c].dy);
      g_assert_cmpfloat (res.rot, ==, want[c].rot);
      g_assert_cmpint (res.overlap, ==, want[c].overlap);
    }
  g_rand_free (r);
}
#endif

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/elanpress/sums/reference", test_sums_match_reference);
  g_test_add_func ("/elanpress/sums/table-limit", test_sums_at_table_limit);
  g_test_add_func ("/elanpress/sums/without-tables", test_sums_without_tables);
  g_test_add_func ("/elanpress/sums/layer-half", test_layer_half);
  g_test_add_func ("/elanpress/sums/canvas-paste", test_canvas_paste);
#if defined(__x86_64__) && defined(__GLIBC__) && !defined(__FMA__)
  g_test_add_func ("/elanpress/match/unchanged", test_match_unchanged);
#endif
#ifdef ELANPRESS_DISPATCH
  for (guint k = 0; k < G_N_ELEMENTS (copies); k++)
    {
      g_autofree char *path = g_strdup_printf ("/elanpress/sums/cross-sums/%s", copies[k].name);
      g_autofree char *limit = g_strdup_printf ("/elanpress/sums/lane-limit/%s", copies[k].name);

      g_test_add_data_func (path, GUINT_TO_POINTER (k), test_cross_sums_copy);
      g_test_add_data_func (limit, GUINT_TO_POINTER (k), test_cross_sums_copy_at_lane_limit);
    }
  g_test_add_func ("/elanpress/sums/simd-env", test_simd_env);
#else
  g_test_add_func ("/elanpress/sums/simd-env-unused", test_simd_env_unused);
#endif

  return g_test_run ();
}
