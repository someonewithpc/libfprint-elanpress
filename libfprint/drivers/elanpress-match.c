/*
 * Image processing and correlation matching for ELAN press-type sensors
 *
 * Copyright (C) 2026 Mohammad Nauman
 * Derived from work by Filip Spanne (elanpress, 04f3:0c6e) and ideas from
 * dragosol/fpmatch (04f3:0c3d, 144x64 variant).
 *
 * The imaged area of these sensors (64x88 px, ~3.2x4.4 mm on the 0c3d pad
 * this was written for) is far too small for minutiae matching: NBIS
 * bozorth3 refuses to score prints with fewer than 10 minutiae, and a patch
 * this size yields 1-5. Prints therefore store the enrolled images
 * themselves and matching is done by zero-mean normalised cross-correlation
 * of locally-normalised ridge images, searched over translation and a
 * small rotation range, coarse-to-fine.
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
 */

#include "config.h"

#include "elanpress-match.h"

#include <math.h>
#ifdef __SSE2__
#include <immintrin.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* the unit test counts the windowed searches that fall back */
#ifndef ELANPRESS_COUNT_FALLBACK
#define ELANPRESS_COUNT_FALLBACK()
#endif

/* the unit test also builds the matcher without intrinsics, so the
 * portable loops run on x86 too */
#if defined(__SSE2__) && !defined(ELANPRESS_NO_INTRINSICS)
#define ELANPRESS_SSE2 1
/* meson checks that the compiler can build the AVX2 and AVX-512 copies */
#if HAVE_ELANPRESS_AVX
#define ELANPRESS_DISPATCH 1
#endif
#endif

/* Two pixels of different values correlate at exactly +1 or -1, so with a
 * tiny FP_ELANPRESS_MIN_OVERLAP a placement that barely touches the template
 * could win. Whatever the setting, a placement has to overlap by this many
 * pixels at full resolution, as layer_ncc_rect() asks of its blocks, and by
 * the same area at half and quarter resolution */
#define MIN_OVERLAP_PIXELS 64

/* ------------------------------------------------------------ params */

void
elanpress_match_params_default (ElanpressMatchParams *p)
{
  /* +/-30: on an 80x80 pad a second subject's genuine presses pinned at
   * the old +/-20 limit, all on one side */
  p->rot_range = 30.0;
  p->rot_step = 4.0;
  p->min_overlap = 0.40;
  p->threshold = 0.76;
  p->norm_win = 11;
  p->candidates = 4;
  p->score_mode = 0;
  p->sidelobe_r = 4;
  p->block_weight = 0.0;
  p->rot_prefilter = 3;
  p->coarse_peaks = 4;
  p->reg_thresh = 0.75;
  p->max_side = 256;
}

/* d as the parser reads it, with a '.' whatever the locale */
static const char *
env_format (char buf[G_ASCII_DTOSTR_BUF_SIZE], double d)
{
  return g_ascii_formatd (buf, G_ASCII_DTOSTR_BUF_SIZE, "%g", d);
}

double
elanpress_env_double (const char *name, double def)
{
  const char *v = g_getenv (name);
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  char *end;
  double d;

  if (!v || !*v)
    return def;
  d = g_ascii_strtod (v, &end);
  /* g_ascii_strtod () accepts "nan" and "inf", and overflows to inf */
  if (end == v || *end || !isfinite (d))
    {
      g_message ("Ignoring %s=\"%s\", expected a number, using %s",
                 name, v, env_format (buf, def));
      return def;
    }
  return d;
}

/* d, read from name, clamped to [lo, hi]; logged if it was outside */
static double
env_clamp (const char *name, double d, double lo, double hi)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  if (d < lo || d > hi)
    {
      d = CLAMP (d, lo, hi);
      g_message ("%s=\"%s\" is out of range, using %s",
                 name, g_getenv (name), env_format (buf, d));
    }
  return d;
}

/* d, read from name, clamped to [lo, hi] while still a double, since a
 * double past G_MAXINT does not fit an int, then cut to an integer and,
 * if odd is set, made odd; logged if any of these changed it */
static int
env_clamp_int (const char *name, double d, int lo, int hi, gboolean odd)
{
  double c = CLAMP (d, lo, hi);
  int n = (int) c;

  if (odd)
    n |= 1;
  if (n != d)
    g_message ("%s=\"%s\" is %s, using %d", name, g_getenv (name),
               c != d ? "out of range" :
               n != c && (int) c == c ? "not odd" : "not a whole number", n);
  return n;
}

/* an integer setting; values under min are ignored, as before, and the rest
 * are clamped to max */
static int
env_int (const char *name, int min, int max, gboolean odd, int def)
{
  double d = elanpress_env_double (name, def);

  if (d < min)
    {
      g_message ("Ignoring %s=\"%s\", expected at least %d, using %d",
                 name, g_getenv (name), min, def);
      return def;
    }
  return env_clamp_int (name, d, min, max, odd);
}

void
elanpress_match_params_from_env (ElanpressMatchParams *p)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  double d;

  elanpress_match_params_default (p);
  /* range / step sets how many rotated copies of the probe are made */
  d = elanpress_env_double ("FP_ELANPRESS_ROT_RANGE", p->rot_range);
  p->rot_range = env_clamp ("FP_ELANPRESS_ROT_RANGE", d,
                            0, ELANPRESS_MAX_ROT_RANGE);
  d = elanpress_env_double ("FP_ELANPRESS_ROT_STEP", p->rot_step);
  if (d > 0)
    p->rot_step = env_clamp ("FP_ELANPRESS_ROT_STEP", d,
                             ELANPRESS_MIN_ROT_STEP, G_MAXDOUBLE);
  else
    g_message ("Ignoring FP_ELANPRESS_ROT_STEP=\"%s\", expected a number above 0, using %s",
               g_getenv ("FP_ELANPRESS_ROT_STEP"), env_format (buf, p->rot_step));
  /* a fraction of the frame, multiplied by its area and cast to int. At 2
   * the threshold is already twice the frame, which no overlap reaches */
  d = elanpress_env_double ("FP_ELANPRESS_MIN_OVERLAP", p->min_overlap);
  p->min_overlap = env_clamp ("FP_ELANPRESS_MIN_OVERLAP", d, 0, 2);
  p->norm_win = env_int ("FP_ELANPRESS_NORM_WIN", 3, G_MAXINT, TRUE, p->norm_win);
  p->candidates = env_int ("FP_ELANPRESS_CANDIDATES", 1, ELANPRESS_MAX_CANDIDATES, FALSE,
                           p->candidates);
  d = elanpress_env_double ("FP_ELANPRESS_SCORE_MODE", p->score_mode);
  p->score_mode = env_clamp_int ("FP_ELANPRESS_SCORE_MODE", d, 0, 2, FALSE);
  p->sidelobe_r = env_int ("FP_ELANPRESS_SIDELOBE_R", 1, G_MAXINT, FALSE, p->sidelobe_r);
  d = elanpress_env_double ("FP_ELANPRESS_BLOCK_WEIGHT", p->block_weight);
  p->block_weight = env_clamp ("FP_ELANPRESS_BLOCK_WEIGHT", d, 0, 1);
  p->rot_prefilter = env_int ("FP_ELANPRESS_ROT_PREFILTER", 0, G_MAXINT, FALSE,
                              p->rot_prefilter);
  p->coarse_peaks = env_int ("FP_ELANPRESS_COARSE_PEAKS", 0, 64, FALSE,
                             p->coarse_peaks);
  p->reg_thresh = elanpress_env_double ("FP_ELANPRESS_REG_THRESH", p->reg_thresh);
  p->max_side = env_int ("FP_ELANPRESS_MAX_SIDE", 64, G_MAXINT, FALSE, p->max_side);
}

void
elanpress_device_config_from_env (ElanpressDeviceConfig *cfg)
{
  double d;

  cfg->threshold = elanpress_env_double ("FP_ELANPRESS_THRESHOLD", cfg->threshold);
  d = elanpress_env_double ("FP_ELANPRESS_ENROLL_STAGES", cfg->enroll_stages);
  cfg->enroll_stages = env_clamp_int ("FP_ELANPRESS_ENROLL_STAGES", d,
                                      1, ELANPRESS_MAX_ENROLL_STAGES, FALSE);
  d = elanpress_env_double ("FP_ELANPRESS_IMAGES_PER_TOUCH", cfg->images_per_touch);
  cfg->images_per_touch = env_clamp_int ("FP_ELANPRESS_IMAGES_PER_TOUCH", d,
                                         1, ELANPRESS_MAX_FRAMES, FALSE);
  d = elanpress_env_double ("FP_ELANPRESS_DUP_SIMILARITY", cfg->dup_similarity);
  cfg->dup_similarity = env_clamp ("FP_ELANPRESS_DUP_SIMILARITY", d, 0, 1);
}

void
elanpress_gates_from_env (ElanpressGates *g)
{
  g->min_mean = elanpress_env_double ("FP_ELANPRESS_MIN_MEAN", ELANPRESS_MIN_MEAN);
  g->min_contrast = elanpress_env_double ("FP_ELANPRESS_MIN_CONTRAST", ELANPRESS_MIN_CONTRAST);
  g->min_coverage = elanpress_env_double ("FP_ELANPRESS_MIN_COVERAGE", ELANPRESS_MIN_COVERAGE);
}

/* ------------------------------------------------------- raw frames */

/* the wire format is column-major with frame-height-tall columns (see
 * elan_save_frame in the elan driver); rotate to row-major */
void
elanpress_rotate_frame (const guint8 *raw, guint16 *out, int w, int h)
{
  const guint16 *in = (const guint16 *) raw;

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      out[y * w + x] = GUINT16_FROM_LE (in[y + x * h]);
}

void
elanpress_frame_stats (const guint16 *frame, int w, int h,
                       double *mean, double *stddev)
{
  int n = w * h;
  double s = 0, ss = 0;

  for (int i = 0; i < n; i++)
    {
      s += frame[i];
      ss += (double) frame[i] * frame[i];
    }
  *mean = s / n;
  *stddev = sqrt (MAX (ss / n - (s / n) * (s / n), 0.0));
}

/* Written as the negation of the rejects, so it matches the reject test it
 * replaced exactly, NaN included */
gboolean
elanpress_quality_passes (const ElanpressQuality *q, const ElanpressGates *g)
{
  return !(q->mean < g->min_mean || q->contrast < g->min_contrast ||
           q->coverage < g->min_coverage);
}

/* A frame taken while the finger lands or lifts fails the coverage gate,
 * but it is mostly ridge edges, so its contrast can be twice that of a full
 * press. Letting it set the bar would drop every good frame of the touch */
double
elanpress_contrast_bar (const ElanpressQuality *q, guint n,
                        const ElanpressGates *g)
{
  double best = 0;

  for (guint i = 0; i < n; i++)
    if (elanpress_quality_passes (&q[i], g))
      best = MAX (best, q[i].contrast);
  return 0.5 * best;
}

void
elanpress_frames_pass (const ElanpressQuality *q, guint n,
                       const ElanpressGates *g, gboolean *pass)
{
  double bar = elanpress_contrast_bar (q, n, g);

  for (guint i = 0; i < n; i++)
    pass[i] = elanpress_quality_passes (&q[i], g) && !(q[i].contrast < bar);
}

/* ---------------------------------------------------- preprocessing */

/* Background subtraction, then local mean/std normalisation over a
 * norm_win x norm_win box. This flattens uneven contact pressure and
 * equalises ridge contrast so that the correlation sees ridge *shape*,
 * not brightness. Regions with no texture (finger not touching) collapse
 * towards 128 and therefore contribute nothing to the correlation. */
guint8 *
elanpress_process_frame (const guint16 *frame, const guint16 *bg,
                         int w, int h, int norm_win, ElanpressQuality *q)
{
  int n = w * h;
  int sw = w + 1;
  int r = MAX (norm_win, 3) / 2;
  float *d = calloc ((size_t) n, sizeof (float));
  /* calloc rather than g_new0: static analysers model calloc as zeroing
   * memory, which lets them follow the integral-image recurrence below */
  double *S = calloc ((size_t) sw * (h + 1), sizeof (double));
  double *SS = calloc ((size_t) sw * (h + 1), sizeof (double));
  guint8 *out = g_new (guint8, n);
  double sum = 0, sq = 0, mean, gstd, eps;
  int textured = 0;

  for (int i = 0; i < n; i++)
    {
      d[i] = (float) frame[i] - (float) (bg ? bg[i] : 0);
      sum += d[i];
      sq += (double) d[i] * d[i];
    }
  mean = sum / n;
  gstd = sqrt (MAX (sq / n - mean * mean, 0.0));

  /* integral images with a zero first row and column, built from running
   * row sums so every value read was written earlier in these loops */
  for (int x = 0; x < sw; x++)
    {
      S[x] = 0;
      SS[x] = 0;
    }
  for (int y = 0; y < h; y++)
    {
      double rs = 0, rss = 0;
      size_t row = (size_t) (y + 1) * sw, prev = (size_t) y * sw;

      S[row] = 0;
      SS[row] = 0;
      for (int x = 0; x < w; x++)
        {
          double p = d[y * w + x];

          rs += p;
          rss += p * p;
          S[row + x + 1] = rs + S[prev + x + 1];
          SS[row + x + 1] = rss + SS[prev + x + 1];
        }
    }

  /* eps keeps flat, finger-free regions from being amplified into noise */
  eps = 0.25 * gstd + 1.0;

  for (int y = 0; y < h; y++)
    {
      int y0 = MAX (y - r, 0), y1 = MIN (y + r + 1, h);
      for (int x = 0; x < w; x++)
        {
          int x0 = MAX (x - r, 0), x1 = MIN (x + r + 1, w);
          double cnt = (double) (y1 - y0) * (x1 - x0);
          double s = S[(size_t) y1 * sw + x1] - S[(size_t) y0 * sw + x1] -
                     S[(size_t) y1 * sw + x0] + S[(size_t) y0 * sw + x0];
          double ss = SS[(size_t) y1 * sw + x1] - SS[(size_t) y0 * sw + x1] -
                      SS[(size_t) y1 * sw + x0] + SS[(size_t) y0 * sw + x0];
          double m = s / cnt;
          double sd = sqrt (MAX (ss / cnt - m * m, 0.0));
          double z = (d[y * w + x] - m) / (sd + eps);
          double v = 128.0 + 40.0 * z;

          if (sd > 0.35 * gstd)
            textured++;
          out[y * w + x] = (guint8) CLAMP (v, 0.0, 255.0);
        }
    }

  if (q)
    {
      q->mean = mean;
      q->contrast = gstd;
      q->coverage = (double) textured / n;
    }

  free (d);
  free (S);
  free (SS);
  return out;
}

double
elanpress_similarity (const guint8 *a, const guint8 *b, int w, int h)
{
  int n = w * h;
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, ma, mb, va, vb, cov;

  for (int i = 0; i < n; i++)
    {
      sa += a[i];
      sb += b[i];
      saa += (double) a[i] * a[i];
      sbb += (double) b[i] * b[i];
      sab += (double) a[i] * b[i];
    }
  ma = sa / n;
  mb = sb / n;
  va = saa / n - ma * ma;
  vb = sbb / n - mb * mb;
  cov = sab / n - ma * mb;
  if (va <= 0 || vb <= 0)
    return 0.0;
  return cov / sqrt (va * vb);
}

/* ---------------------------------------------------------- layers */

/* Pixels per step of the general NCC loop. Each row carries PAD zero
 * pixels on both sides, so a full-overlap sum can round its span out to
 * whole steps instead of finishing every short row with a scalar tail. The
 * cross-term loop of layer_sums_fullb() rounds to BLOCK instead. */
#ifdef __AVX2__
#define VEC 32
#else
#define VEC 16
#endif
#define PAD 32
G_STATIC_ASSERT (PAD >= VEC / 2);

/* Masks and squares are 16-bit like the values, so every NCC term is a
 * 16 x 16 -> 32-bit multiply-add (pmaddwd). */
typedef struct
{
  int      w, h;
  int      stride;
  gint16  *buf[3];
  gint16  *v;  /* value - 128, 0 where invalid */
  gint16  *m;  /* validity mask, 0 or 1 */
  gint16  *q;  /* v * v */
  gboolean full;  /* every pixel valid */
  gint32  *sat;   /* NULL, or summed-area tables of v, q and m, (w + 1) x (h + 1) each */
} Layer;

static void
layer_init (Layer *l, int w, int h)
{
  l->w = w;
  l->h = h;
  l->stride = w + 2 * PAD;
  /* + 2 * PAD keeps the pointers valid when h is 0 */
  for (int i = 0; i < 3; i++)
    l->buf[i] = g_new0 (gint16, (size_t) l->stride * h + 2 * PAD);
  l->v = l->buf[0] + PAD;
  l->m = l->buf[1] + PAD;
  l->q = l->buf[2] + PAD;
  l->full = FALSE;
  l->sat = NULL;
}

/* fill in the squares once v and m are final */
static void
layer_finish (Layer *l)
{
  l->full = TRUE;
  for (int y = 0; y < l->h; y++)
    for (int x = 0; x < l->w; x++)
      {
        int i = y * l->stride + x;

        l->q[i] = l->v[i] * l->v[i];
        if (!l->m[i])
          l->full = FALSE;
      }
}

/* q is at most 128 * 128, so a table of q over this many pixels stays
 * below 2^31 */
#define TABLE_MAX_PIXELS (G_MAXINT32 / (128 * 128))

/* Summed-area tables for layer_sums_fullb(), which reads them without
 * checking: every probe layer needs them, and a template needs them
 * whenever it is full. A layer too large for 32-bit tables gets none and
 * takes the general path. */
static void
layer_build_tables (Layer *l)
{
  int sw, sn;

  if ((gint64) l->w * l->h > TABLE_MAX_PIXELS)
    return;
  sw = l->w + 1;
  sn = sw * (l->h + 1);
  l->sat = g_new0 (gint32, (size_t) 3 * sn);
  for (int y = 0; y < l->h; y++)
    for (int x = 0; x < l->w; x++)
      {
        int i = y * l->stride + x, j = (y + 1) * sw + x + 1;
        const gint32 px[3] = { l->v[i], l->q[i], l->m[i] };

        for (int t = 0; t < 3; t++)
          {
            gint32 *T = l->sat + t * sn;

            /* column sum first, so no partial sum of q passes T[j] */
            T[j] = px[t] + T[j - 1] + (T[j - sw] - T[j - sw - 1]);
          }
      }
}

/* sum of table t over the rectangle [x0,x1)x[y0,y1) */
static inline gint64
layer_rect_sum (const Layer *l, int t, int x0, int y0, int x1, int y1)
{
  int sw = l->w + 1;
  const gint32 *T = l->sat + (size_t) t * sw * (l->h + 1);

  return (gint64) T[y1 * sw + x1] - T[y0 * sw + x1] - T[y1 * sw + x0] + T[y0 * sw + x0];
}

static void
layer_clear (Layer *l)
{
  for (int i = 0; i < 3; i++)
    g_clear_pointer (&l->buf[i], g_free);
  g_clear_pointer (&l->sat, g_free);
  l->v = l->m = l->q = NULL;
}

static void
layer_from_image (Layer *l, const guint8 *img, const guint8 *mask, int w, int h)
{
  layer_init (l, w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        if (mask && !mask[y * w + x])
          continue;
        l->v[y * l->stride + x] = (gint16) img[y * w + x] - 128;
        l->m[y * l->stride + x] = 1;
      }
  layer_finish (l);
}

/* rotate img by deg around its centre into a same-size layer; pixels whose
 * source falls outside the image are masked out */
static void
layer_rotated (Layer *l, const guint8 *img, int w, int h, double deg)
{
  double th = deg * M_PI / 180.0;
  double c = cos (th), s = sin (th);
  double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;

  layer_init (l, w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        double dx = x - cx, dy = y - cy;
        double sx = c * dx + s * dy + cx;
        double sy = -s * dx + c * dy + cy;
        int x0 = (int) floor (sx), y0 = (int) floor (sy);
        double fx = sx - x0, fy = sy - y0;
        double v;

        if (x0 < 0 || y0 < 0 || x0 + 1 >= w || y0 + 1 >= h)
          continue;
        v = (1 - fx) * (1 - fy) * img[y0 * w + x0] +
            fx * (1 - fy) * img[y0 * w + x0 + 1] +
            (1 - fx) * fy * img[(y0 + 1) * w + x0] +
            fx * fy * img[(y0 + 1) * w + x0 + 1];
        l->v[y * l->stride + x] = (gint16) lrint (v - 128.0);
        l->m[y * l->stride + x] = 1;
      }
  layer_finish (l);
}

/* 2x2 box downsample; a coarse pixel is valid only if all four are */
static void
layer_half (Layer *l, const Layer *src)
{
  int w = src->w / 2, h = src->h / 2;

  layer_init (l, w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        const gint16 *p = src->v + (2 * y) * src->stride + 2 * x;
        const gint16 *m = src->m + (2 * y) * src->stride + 2 * x;

        if (m[0] && m[1] && m[src->stride] && m[src->stride + 1])
          {
            l->v[y * l->stride + x] = (p[0] + p[1] + p[src->stride] + p[src->stride + 1]) / 4;
            l->m[y * l->stride + x] = 1;
          }
      }
  layer_finish (l);
}

/* The NCC sums of a against b shifted by (dx, dy) over the rectangle
 * [x0,x1)x[y0,y1) of a. Invalid pixels hold 0, so each one-sided sum only
 * needs the other side's mask, and the loop has no branches. A row is at
 * most a few hundred pixels, so its sums fit in 32 bits.
 *
 * With step > 1 the span is widened to whole steps. That is only exact when
 * [x0,x1) is the full overlap of a and b: every pixel outside it is invalid
 * on one side or the other, padding included. */
static inline int
layer_sums_general (const Layer *a, const Layer *b, int dx, int dy,
                    int x0, int y0, int x1, int y1, const int step, gint64 s[5])
{
  int n = 0, extra;

  s[0] = s[1] = s[2] = s[3] = s[4] = 0;
  if (x1 <= x0)
    return 0;
  extra = (step - (x1 - x0) % step) % step;
  x0 -= extra / 2;
  x1 += extra - extra / 2;
  for (int y = y0; y < y1; y++)
    {
      const gint16 *av = a->v + y * a->stride + x0;
      const gint16 *am = a->m + y * a->stride + x0;
      const gint16 *aq = a->q + y * a->stride + x0;
      const gint16 *bv = b->v + (y - dy) * b->stride + (x0 - dx);
      const gint16 *bm = b->m + (y - dy) * b->stride + (x0 - dx);
      const gint16 *bq = b->q + (y - dy) * b->stride + (x0 - dx);
      gint32 sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, rn = 0;

      for (int xs = 0; xs < x1 - x0; xs += step)
        for (int x = xs; x < xs + step; x++)
          {
            sa += av[x] * bm[x];
            sb += bv[x] * am[x];
            saa += aq[x] * bm[x];
            sbb += bq[x] * am[x];
            sab += av[x] * bv[x];
            rn += am[x] * bm[x];
          }
      s[0] += sa;
      s[1] += sb;
      s[2] += saa;
      s[3] += sbb;
      s[4] += sab;
      n += rn;
    }
  return n;
}

/* The fast path pads rows to whole blocks; a literal block length lets the
 * compiler unroll the inner loop at any vector width. */
#define BLOCK 32
G_STATIC_ASSERT (PAD >= BLOCK / 2);

#ifdef ELANPRESS_SSE2
/* GCC at -O2 adds every block of the loop below down to one number before
 * starting the next, which costs about as much as the block itself. These
 * copies keep per-lane sums in registers instead. Each product is at most
 * 128 * 128 and both layers have tables, so a window holds at most
 * TABLE_MAX_PIXELS pixels, and neither a 32-bit lane nor the 32-bit total
 * SUMS_ADD returns can pass 2^31. */

static inline gint32
hsum128 (__m128i x)
{
  x = _mm_add_epi32 (x, _mm_shuffle_epi32 (x, 0x4e));
  x = _mm_add_epi32 (x, _mm_shuffle_epi32 (x, 0xb1));
  return _mm_cvtsi128_si32 (x);
}

#define SUMS_NAME cross_sums_sse2
#define SUMS_ATTR
#define SUMS_VEC __m128i
#define SUMS_W 8
#define SUMS_ZERO _mm_setzero_si128 ()
#define SUMS_LOAD(p) _mm_loadu_si128 ((const void *) (p))
#define SUMS_DOT(acc, x, y) _mm_add_epi32 (acc, _mm_madd_epi16 (x, y))
#define SUMS_ADD(acc) hsum128 (acc)
#include "elanpress-match-sums.h"

#ifdef ELANPRESS_DISPATCH
/* Distributions build for baseline x86-64, so the loop also comes in AVX2
 * and AVX-512 copies, picked by the CPU at run time. */
#define SUMS_NAME cross_sums_avx2
#define SUMS_ATTR __attribute__((target ("avx2")))
#define SUMS_VEC __m256i
#define SUMS_W 16
#define SUMS_ZERO _mm256_setzero_si256 ()
#define SUMS_LOAD(p) _mm256_loadu_si256 ((const void *) (p))
#define SUMS_DOT(acc, x, y) _mm256_add_epi32 (acc, _mm256_madd_epi16 (x, y))
#define SUMS_ADD(acc) \
  hsum128 (_mm_add_epi32 (_mm256_castsi256_si128 (acc), \
                          _mm256_extracti128_si256 (acc, 1)))
#include "elanpress-match-sums.h"

#define SUMS_NAME cross_sums_avx512
#define SUMS_ATTR __attribute__((target ("avx512bw")))
#define SUMS_VEC __m512i
#define SUMS_W 32
#define SUMS_ZERO _mm512_setzero_si512 ()
#define SUMS_LOAD(p) _mm512_loadu_si512 ((const void *) (p))
#define SUMS_DOT(acc, x, y) _mm512_add_epi32 (acc, _mm512_madd_epi16 (x, y))
#define SUMS_ADD(acc) _mm512_reduce_add_epi32 (acc)
#include "elanpress-match-sums.h"

/* 1 for SSE2, 2 for AVX2, 3 for AVX-512. FP_ELANPRESS_SIMD=sse2, avx2 or
 * avx512 caps it, so every copy can run on a newer CPU. */
static int
cross_sums_level (void)
{
  static gsize level = 0;

  if (g_once_init_enter (&level))
    {
      const char *cap = g_getenv ("FP_ELANPRESS_SIMD");
      gsize l = 1;

      if (__builtin_cpu_supports ("avx512bw"))
        l = 3;
      else if (__builtin_cpu_supports ("avx2"))
        l = 2;
      if (g_ascii_strcasecmp (cap ? cap : "", "sse2") == 0)
        l = 1;
      else if (g_ascii_strcasecmp (cap ? cap : "", "avx2") == 0)
        l = MIN (l, 2);
      else if (cap && *cap && g_ascii_strcasecmp (cap, "avx512") != 0)
        g_message ("FP_ELANPRESS_SIMD=%s not recognised, expected sse2, avx2 or avx512", cap);
      g_once_init_leave (&level, l);
    }
  return (int) level;
}

static void
cross_sums (const Layer *a, const Layer *b, int dx, int dy,
            int x0, int y0, int x1, int y1, const gboolean fulla, gint64 s[5])
{
  switch (cross_sums_level ())
    {
    case 3:
      cross_sums_avx512 (a, b, dx, dy, x0, y0, x1, y1, fulla, s);
      break;

    case 2:
      cross_sums_avx2 (a, b, dx, dy, x0, y0, x1, y1, fulla, s);
      break;

    default:
      cross_sums_sse2 (a, b, dx, dy, x0, y0, x1, y1, fulla, s);
    }
}
#else
static void
cross_sums (const Layer *a, const Layer *b, int dx, int dy,
            int x0, int y0, int x1, int y1, const gboolean fulla, gint64 s[5])
{
  cross_sums_sse2 (a, b, dx, dy, x0, y0, x1, y1, fulla, s);
}
#endif
#endif

/* When b is valid everywhere, the sums of a alone and the pixel count are
 * rectangle sums over a, and only the three terms with b stay in the loop.
 * When a is valid everywhere too, only the cross term does. */
static inline int
layer_sums_fullb (const Layer *a, const Layer *b, int dx, int dy,
                  int x0, int y0, int x1, int y1, const gboolean fulla,
                  gint64 s[5])
{
  int extra, n;

  s[0] = layer_rect_sum (a, 0, x0, y0, x1, y1);
  s[2] = layer_rect_sum (a, 1, x0, y0, x1, y1);
  n = (int) layer_rect_sum (a, 2, x0, y0, x1, y1);
  s[1] = s[3] = s[4] = 0;
  if (fulla)
    {
      s[1] = layer_rect_sum (b, 0, x0 - dx, y0 - dy, x1 - dx, y1 - dy);
      s[3] = layer_rect_sum (b, 1, x0 - dx, y0 - dy, x1 - dx, y1 - dy);
    }
  extra = (BLOCK - (x1 - x0) % BLOCK) % BLOCK;
  x0 -= extra / 2;
  x1 += extra - extra / 2;
#ifdef ELANPRESS_SSE2
  cross_sums (a, b, dx, dy, x0, y0, x1, y1, fulla, s);
#else
  for (int y = y0; y < y1; y++)
    {
      const gint16 *av = a->v + y * a->stride + x0;
      const gint16 *am = a->m + y * a->stride + x0;
      const gint16 *bv = b->v + (y - dy) * b->stride + (x0 - dx);
      const gint16 *bq = b->q + (y - dy) * b->stride + (x0 - dx);
      gint32 sb = 0, sbb = 0, sab = 0;

      if (fulla)
        {
          for (int xs = 0; xs < x1 - x0; xs += BLOCK)
            for (int x = xs; x < xs + BLOCK; x++)
              sab += av[x] * bv[x];
        }
      else
        {
          for (int xs = 0; xs < x1 - x0; xs += BLOCK)
            for (int x = xs; x < xs + BLOCK; x++)
              {
                sb += bv[x] * am[x];
                sbb += bq[x] * am[x];
                sab += av[x] * bv[x];
              }
        }
      s[1] += sb;
      s[3] += sbb;
      s[4] += sab;
    }
#endif
  return n;
}

/* Takes the table path when both layers have tables and b is full:
 * layer_sums_fullb() assumes that every pixel of b is valid. */
static inline int
layer_sums (const Layer *a, const Layer *b, int dx, int dy,
            int x0, int y0, int x1, int y1, const int step, gint64 s[5])
{
  if (step == 1 || !a->sat || !b->sat || !b->full)
    return layer_sums_general (a, b, dx, dy, x0, y0, x1, y1, step, s);
  if (x1 <= x0 || y1 <= y0)
    {
      s[0] = s[1] = s[2] = s[3] = s[4] = 0;
      return 0;
    }
  return layer_sums_fullb (a, b, dx, dy, x0, y0, x1, y1,
                           a->full, s);
}

static double
ncc_from_sums (const gint64 s[5], int n)
{
  double ma = (double) s[0] / n, mb = (double) s[1] / n;
  double va = (double) s[2] / n - ma * ma;
  double vb = (double) s[3] / n - mb * mb;
  double cov = (double) s[4] / n - ma * mb;

  if (va <= 1e-9 || vb <= 1e-9)
    return -2.0;
  return cov / sqrt (va * vb);
}

/* NCC of a (fixed) against b shifted by (dx, dy): a(x, y) pairs with
 * b(x - dx, y - dy). Returns -2 when the overlap is below min_n. */
static double
layer_ncc (const Layer *a, const Layer *b, int dx, int dy, int min_n,
           int *n_out)
{
  int x0 = MAX (0, dx), x1 = MIN (a->w, b->w + dx);
  int y0 = MAX (0, dy), y1 = MIN (a->h, b->h + dy);
  gint64 s[5];
  int n;

  if (x1 - x0 <= 0 || y1 - y0 <= 0)
    return -2.0;
  if ((x1 - x0) * (y1 - y0) < min_n)
    return -2.0;

  n = layer_sums (a, b, dx, dy, x0, y0, x1, y1, VEC, s);
  if (n_out)
    *n_out = n;
  if (n < min_n)
    return -2.0;
  return ncc_from_sums (s, n);
}

/* NCC of a against shifted b restricted to the a-rectangle [x0,x1)x[y0,y1) */
static double
layer_ncc_rect (const Layer *a, const Layer *b, int dx, int dy,
                int x0, int y0, int x1, int y1, int min_n)
{
  gint64 s[5];
  int n;

  x0 = MAX (x0, MAX (0, dx));
  x1 = MIN (x1, MIN (a->w, b->w + dx));
  y0 = MAX (y0, MAX (0, dy));
  y1 = MIN (y1, MIN (a->h, b->h + dy));
  n = layer_sums (a, b, dx, dy, x0, y0, x1, y1, 1, s);
  if (n < min_n)
    return -2.0;
  return ncc_from_sums (s, n);
}

/* consistency of the peak alignment over a 3x3 grid of the overlap */
static void
layer_block_stats (const Layer *a, const Layer *b, int dx, int dy,
                   double *bmin, double *bmean)
{
  int x0 = MAX (0, dx), x1 = MIN (a->w, b->w + dx);
  int y0 = MAX (0, dy), y1 = MIN (a->h, b->h + dy);
  int n = 0;
  double sum = 0, mn = 2.0;

  for (int by = 0; by < 3; by++)
    for (int bx = 0; bx < 3; bx++)
      {
        int bx0 = x0 + (x1 - x0) * bx / 3, bx1 = x0 + (x1 - x0) * (bx + 1) / 3;
        int by0 = y0 + (y1 - y0) * by / 3, by1 = y0 + (y1 - y0) * (by + 1) / 3;
        double s = layer_ncc_rect (a, b, dx, dy, bx0, by0, bx1, by1, 64);

        if (s <= -2.0)
          continue;
        sum += s;
        mn = MIN (mn, s);
        n++;
      }
  *bmin = n ? mn : -1.0;
  *bmean = n ? sum / n : -1.0;
}

/* ----------------------------------------------------------- probe */

struct _ElanpressProbe
{
  int                  w, h, n_rot;
  double              *angles;
  Layer               *full;    /* per rotation, full resolution */
  Layer               *half;    /* per rotation, half resolution */
  Layer               *quarter; /* per rotation, quarter resolution */
  ElanpressMatchParams p;
};

ElanpressProbe *
elanpress_probe_new (const guint8 *img, int w, int h,
                     const ElanpressMatchParams *p)
{
  ElanpressProbe *pr = g_new0 (ElanpressProbe, 1);
  int n_rot;

  pr->w = w;
  pr->h = h;
  pr->p = *p;
  if (pr->p.rot_step <= 0)
    pr->p.rot_step = 4.0;
  n_rot = (int) floor (pr->p.rot_range / pr->p.rot_step + 1e-9);
  pr->n_rot = 2 * n_rot + 1;
  pr->angles = g_new (double, pr->n_rot);
  pr->full = g_new0 (Layer, pr->n_rot);
  pr->half = g_new0 (Layer, pr->n_rot);
  pr->quarter = g_new0 (Layer, pr->n_rot);
  for (int i = 0; i < pr->n_rot; i++)
    {
      pr->angles[i] = (i - n_rot) * pr->p.rot_step;
      if (pr->angles[i] == 0.0)
        layer_from_image (&pr->full[i], img, NULL, w, h);
      else
        layer_rotated (&pr->full[i], img, w, h, pr->angles[i]);
      layer_half (&pr->half[i], &pr->full[i]);
      layer_half (&pr->quarter[i], &pr->half[i]);
      layer_build_tables (&pr->full[i]);
      layer_build_tables (&pr->half[i]);
      layer_build_tables (&pr->quarter[i]);
    }
#ifdef ELANPRESS_DISPATCH
  /* read FP_ELANPRESS_SIMD here rather than in a match worker */
  cross_sums_level ();
#endif
  return pr;
}

void
elanpress_probe_free (ElanpressProbe *pr)
{
  if (!pr)
    return;
  for (int i = 0; i < pr->n_rot; i++)
    {
      layer_clear (&pr->full[i]);
      layer_clear (&pr->half[i]);
      layer_clear (&pr->quarter[i]);
    }
  g_free (pr->full);
  g_free (pr->half);
  g_free (pr->quarter);
  g_free (pr->angles);
  g_free (pr);
}

int
elanpress_probe_n_rotations (const ElanpressProbe *pr)
{
  return pr->n_rot;
}

typedef struct
{
  double score;
  int    rot, dx, dy;
} Cand;

static void
cand_insert (Cand *c, int k, double score, int rot, int dx, int dy)
{
  /* replace a nearby candidate if this one is better, else the worst */
  int worst = 0;

  for (int i = 0; i < k; i++)
    {
      if (c[i].rot == rot && abs (c[i].dx - dx) <= 1 && abs (c[i].dy - dy) <= 1)
        {
          if (score > c[i].score)
            {
              c[i].score = score;
              c[i].dx = dx;
              c[i].dy = dy;
            }
          return;
        }
      if (c[i].score < c[worst].score)
        worst = i;
    }
  if (score > c[worst].score)
    {
      c[worst].score = score;
      c[worst].rot = rot;
      c[worst].dx = dx;
      c[worst].dy = dy;
    }
}

/* Keeps up to k quarter-resolution peaks of one rotation more than 2
 * pixels apart, so one broad peak can't take every window: a better offset
 * replaces every slot within 2 pixels, a worse one is dropped. What is kept
 * depends on scan order, since a dropped peak doesn't come back when the
 * one that beat it is replaced. */
static void
peak_insert (Cand *c, int k, double score, int rot, int dx, int dy)
{
  int slot = -1;

  for (int i = 0; i < k; i++)
    if (c[i].rot >= 0 && abs (c[i].dx - dx) <= 2 && abs (c[i].dy - dy) <= 2 &&
        c[i].score >= score)
      return;
  for (int i = 0; i < k; i++)
    if (c[i].rot >= 0 && abs (c[i].dx - dx) <= 2 && abs (c[i].dy - dy) <= 2)
      {
        c[i] = (Cand){ -3.0, -1, 0, 0 };
        slot = i;
      }
  if (slot < 0)
    {
      slot = 0;
      for (int i = 1; i < k; i++)
        if (c[i].score < c[slot].score)
          slot = i;
      if (c[slot].score >= score)
        return;
    }
  c[slot] = (Cand){ score, rot, dx, dy };
}

double
elanpress_probe_match_canvas (const ElanpressProbe *pr, const guint8 *px,
                              const guint8 *mask, int tw, int th,
                              ElanpressMatchResult *res)
{
  Layer tfull, thalf;
  int k = MAX (pr->p.candidates, 1);
  Cand *cands = g_new (Cand, k);
  int pw = pr->half[0].w, ph = pr->half[0].h;
  int hw, hh, mw, mh, min_n_half, min_n_full, best_cand = -1;
  float *maps = NULL;
  ElanpressMatchResult best = { -2.0, -2.0, -2.0, -1.0, -1.0, 0, 0, 0.0, 0 };
  int best_rot_idx = -1;
  guint8 *visit = NULL;

  layer_from_image (&tfull, px, mask, tw, th);
  layer_half (&thalf, &tfull);
  if (tfull.full)
    layer_build_tables (&tfull);
  if (thalf.full)
    layer_build_tables (&thalf);
  hw = thalf.w;
  hh = thalf.h;
  /* offsets range over every placement with any overlap */
  mw = pw + hw - 1;
  mh = ph + hh - 1;
  min_n_half = MAX ((int) (pr->p.min_overlap * pw * ph), MIN_OVERLAP_PIXELS / 4);
  min_n_full = MAX ((int) (pr->p.min_overlap * pr->w * pr->h), MIN_OVERLAP_PIXELS);
  if (pr->p.score_mode != 0)
    {
      maps = g_new (float, (size_t) pr->n_rot * mw * mh);
      for (size_t i = 0; i < (size_t) pr->n_rot * mw * mh; i++)
        maps[i] = -2.0f;
    }

  for (int i = 0; i < k; i++)
    {
      cands[i].score = -3.0;
      cands[i].rot = -1;
      cands[i].dx = cands[i].dy = 0;
    }

  /* Rotation pre-selection: score every rotation at quarter resolution
   * (about 16x cheaper than the half-resolution pass) and keep only the
   * best few for the real search. Ranking is all that is needed here. */
  gboolean *use_rot = g_new (gboolean, pr->n_rot);
  int keep = pr->p.rot_prefilter;
  /* The sidelobe modes need the half-resolution map at every offset. A
   * masked canvas loses too many valid pixels at quarter scale for its
   * peaks to point at the right half-resolution offsets. */
  int n_peaks = maps || mask ? 0 : pr->p.coarse_peaks;
  Cand *peaks = NULL;

  for (int r = 0; r < pr->n_rot; r++)
    use_rot[r] = TRUE;
  if (keep > 0 && keep < pr->n_rot)
    {
      Layer tquarter;
      double *qbest = g_new (double, pr->n_rot);
      int qw, qh, min_n_quarter, min_n_peak;

      if (n_peaks > 0)
        {
          peaks = g_new (Cand, (size_t) pr->n_rot * n_peaks);
          for (int i = 0; i < pr->n_rot * n_peaks; i++)
            peaks[i] = (Cand){ -3.0, -1, 0, 0 };
        }

      layer_half (&tquarter, &thalf);
      if (tquarter.full)
        layer_build_tables (&tquarter);
      qw = tquarter.w;
      qh = tquarter.h;
      min_n_quarter = MAX ((int) (pr->p.min_overlap * pr->quarter[0].w * pr->quarter[0].h),
                           MIN_OVERLAP_PIXELS / 16);
      /* The masked corners of a rotated probe grow at each halving, so an
       * offset just above min_overlap at half resolution can fall below it
       * at quarter resolution. Peaks get a 10% margin. */
      min_n_peak = peaks ? min_n_quarter * 9 / 10 : min_n_quarter;
      for (int r = 0; r < pr->n_rot; r++)
        {
          const Layer *p_q = &pr->quarter[r];

          qbest[r] = -2.0;
          for (int dy = -(qh - 1); dy <= p_q->h - 1; dy++)
            for (int dx = -(qw - 1); dx <= p_q->w - 1; dx++)
              {
                int n = 0;
                double s = layer_ncc (p_q, &tquarter, dx, dy, min_n_peak, &n);

                if (n >= min_n_quarter && s > qbest[r])
                  qbest[r] = s;
                if (peaks && s > -2.0)
                  peak_insert (peaks + r * n_peaks, n_peaks, s, r, dx, dy);
              }
          use_rot[r] = FALSE;
        }
      for (int n = 0; n < keep; n++)
        {
          int bi = -1;

          for (int r = 0; r < pr->n_rot; r++)
            if (!use_rot[r] && (bi < 0 || qbest[r] > qbest[bi]))
              bi = r;
          if (bi >= 0)
            use_rot[bi] = TRUE;
        }
      g_free (qbest);
      layer_clear (&tquarter);
    }

  /* coarse: selected rotations at half resolution, every translation or,
   * with quarter-resolution peaks, only the offsets around them. A real
   * match has a strong peak that survives the quarter scale; this mostly
   * skips chance alignments. probe(x, y) pairs with canvas(x - dx, y - dy). */
  if (peaks)
    visit = g_new (guint8, (size_t) mw * mh);

  for (int r = 0; r < pr->n_rot; r++)
    {
      const Layer *p_h = &pr->half[r];
      gboolean windowed = FALSE;

      if (!use_rot[r])
        continue;

      if (visit)
        {
          /* a quarter offset q is a half offset within 2q +/- 1; two more
           * pixels of margin for the peak moving between scales */
          const int R = 3;

          memset (visit, 0, (size_t) mw * mh);
          for (int i = 0; i < n_peaks; i++)
            {
              const Cand *c = &peaks[r * n_peaks + i];

              if (c->rot < 0)
                continue;
              windowed = TRUE;
              for (int dy = MAX (2 * c->dy - R, -(hh - 1)); dy <= MIN (2 * c->dy + R, ph - 1); dy++)
                for (int dx = MAX (2 * c->dx - R, -(hw - 1)); dx <= MIN (2 * c->dx + R, pw - 1); dx++)
                  visit[(dy + hh - 1) * mw + (dx + hw - 1)] = 1;
            }
        }

      for (;;)
        {
          gboolean found = FALSE;

          for (int dy = -(hh - 1); dy <= ph - 1; dy++)
            for (int dx = -(hw - 1); dx <= pw - 1; dx++)
              {
                double s;

                if (windowed && !visit[(dy + hh - 1) * mw + (dx + hw - 1)])
                  continue;
                s = layer_ncc (p_h, &thalf, dx, dy, min_n_half, NULL);
                if (s > -2.0)
                  {
                    found = TRUE;
                    if (maps)
                      maps[((size_t) r * mh + (dy + hh - 1)) * mw + (dx + hw - 1)] = (float) s;
                    cand_insert (cands, k, s, r, dx, dy);
                  }
              }
          /* Peaks at a marginal quarter overlap can leave every offset in
           * the windows short of min_n_half or flat (zero variance), and a
           * rotation can have no quarter peak at all; search the whole
           * rotation then. */
          if (found || !windowed)
            break;
          ELANPRESS_COUNT_FALLBACK ();
          windowed = FALSE;
        }
    }

  /* fine: refine each candidate at full resolution */
  for (int i = 0; i < k; i++)
    {
      const Layer *pf;

      if (cands[i].rot < 0)
        continue;
      pf = &pr->full[cands[i].rot];
      for (int dy = 2 * cands[i].dy - 2; dy <= 2 * cands[i].dy + 2; dy++)
        for (int dx = 2 * cands[i].dx - 2; dx <= 2 * cands[i].dx + 2; dx++)
          {
            int n = 0;
            double s = layer_ncc (pf, &tfull, dx, dy, min_n_full, &n);

            if (s > best.peak)
              {
                best.peak = s;
                best.dx = dx;
                best.dy = dy;
                best.rot = pr->angles[cands[i].rot];
                best.overlap = n;
                best_cand = i;
                best_rot_idx = cands[i].rot;
              }
          }
    }

  if (best_rot_idx >= 0)
    layer_block_stats (&pr->full[best_rot_idx], &tfull, best.dx, best.dy,
                       &best.block_min, &best.block_mean);

  /* Sidelobe: the best coarse score anywhere except a window around the
   * peak, over every rotation (experimental, score_mode 1 / 2). */
  if (maps && best_cand >= 0)
    {
      int cdx = cands[best_cand].dx, cdy = cands[best_cand].dy;
      int R = MAX (pr->p.sidelobe_r, 1);

      for (int r = 0; r < pr->n_rot; r++)
        for (int y = 0; y < mh; y++)
          for (int x = 0; x < mw; x++)
            {
              int dx = x - (hw - 1), dy = y - (hh - 1);
              float s;

              if (abs (dx - cdx) <= R && abs (dy - cdy) <= R)
                continue;
              s = maps[((size_t) r * mh + y) * mw + x];
              if (s > best.sidelobe)
                best.sidelobe = s;
            }
    }

  switch (pr->p.score_mode)
    {
    case 1:
      best.score = best.sidelobe > -2.0 ? best.peak - best.sidelobe : best.peak;
      break;

    case 2:
      best.score = best.sidelobe > 0.05 ? best.peak / best.sidelobe - 1.0 : best.peak;
      break;

    default:
      best.score = best.peak;
    }
  if (pr->p.block_weight > 0 && best.block_min > -1.0)
    best.score = (1.0 - pr->p.block_weight) * best.score +
                 pr->p.block_weight * best.block_min;

  g_free (use_rot);
  g_free (peaks);
  g_free (visit);
  g_free (maps);
  g_free (cands);
  layer_clear (&tfull);
  layer_clear (&thalf);

  if (res)
    *res = best;
  return best.score;
}

double
elanpress_probe_match (const ElanpressProbe *pr, const guint8 *tmpl,
                       ElanpressMatchResult *res)
{
  return elanpress_probe_match_canvas (pr, tmpl, NULL, pr->w, pr->h, res);
}

/* ---------------------------------------------------------- mosaic */

ElanpressCanvas *
elanpress_canvas_new_from_image (const guint8 *img, int w, int h)
{
  ElanpressCanvas *c = g_new0 (ElanpressCanvas, 1);

  c->w = w;
  c->h = h;
  c->px = g_memdup2 (img, (gsize) w * h);
  c->mask = g_new (guint8, (gsize) w * h);
  memset (c->mask, 1, (gsize) w * h);
  c->n_images = 1;
  return c;
}

void
elanpress_canvas_free (ElanpressCanvas *c)
{
  if (!c)
    return;
  g_free (c->px);
  g_free (c->mask);
  g_free (c);
}

/* Paste img, rotated by rot degrees, so that rotated-probe pixel (x, y)
 * lands on canvas pixel (x - dx, y - dy) -- the geometry layer_ncc scores.
 * The canvas grows as needed up to max_side; pixels already holding data
 * are kept (no blending, so ridges stay sharp). */
static gboolean
canvas_paste (ElanpressCanvas *c, const guint8 *img, int w, int h,
              int dx, int dy, double rot, int max_side)
{
  Layer l;
  int x0 = -dx, y0 = -dy;              /* canvas position of probe pixel (0,0) */
  int nx0 = MIN (0, x0), ny0 = MIN (0, y0);
  int nx1 = MAX (c->w, x0 + w), ny1 = MAX (c->h, y0 + h);
  int nw = nx1 - nx0, nh = ny1 - ny0;

  if (nw > max_side || nh > max_side)
    return FALSE;

  if (nw != c->w || nh != c->h)
    {
      guint8 *px = g_new0 (guint8, (gsize) nw * nh);
      guint8 *mask = g_new0 (guint8, (gsize) nw * nh);

      for (int y = 0; y < c->h; y++)
        {
          memcpy (px + (gsize) (y - ny0) * nw - nx0, c->px + (gsize) y * c->w, c->w);
          memcpy (mask + (gsize) (y - ny0) * nw - nx0, c->mask + (gsize) y * c->w, c->w);
        }
      g_free (c->px);
      g_free (c->mask);
      c->px = px;
      c->mask = mask;
      c->w = nw;
      c->h = nh;
      x0 -= nx0;
      y0 -= ny0;
    }

  if (rot == 0.0)
    layer_from_image (&l, img, NULL, w, h);
  else
    layer_rotated (&l, img, w, h, rot);

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        int cx = x0 + x, cy = y0 + y;

        if (!l.m[y * l.stride + x])
          continue;
        if (c->mask[cy * c->w + cx])
          continue;
        c->px[cy * c->w + cx] = (guint8) (l.v[y * l.stride + x] + 128);
        c->mask[cy * c->w + cx] = 1;
      }
  layer_clear (&l);
  c->n_images++;
  return TRUE;
}

GPtrArray *
elanpress_mosaic_build (GPtrArray *images, int w, int h,
                        const ElanpressMatchParams *p,
                        double reg_thresh, int max_side)
{
  GPtrArray *frags = g_ptr_array_new_with_free_func ((GDestroyNotify) elanpress_canvas_free);
  GPtrArray *pending = g_ptr_array_new ();

  for (guint i = 0; i < images->len; i++)
    g_ptr_array_add (pending, g_ptr_array_index (images, i));

  /* two passes: an image that fails to register at first may register
   * once its neighbours have grown a fragment */
  for (int pass = 0; pass < 2 && pending->len > 0; pass++)
    {
      GPtrArray *still = g_ptr_array_new ();

      for (guint i = 0; i < pending->len; i++)
        {
          const guint8 *img = g_ptr_array_index (pending, i);
          ElanpressProbe *probe = elanpress_probe_new (img, w, h, p);
          ElanpressMatchResult best = { -2.0, -2.0, -2.0, -1.0, -1.0, 0, 0, 0.0, 0 };
          int best_f = -1;

          for (guint f = 0; f < frags->len; f++)
            {
              ElanpressCanvas *c = g_ptr_array_index (frags, f);
              ElanpressMatchResult r;

              elanpress_probe_match_canvas (probe, c->px, c->mask, c->w, c->h, &r);
              if (r.peak > best.peak)
                {
                  best = r;
                  best_f = f;
                }
            }
          elanpress_probe_free (probe);

          if (best_f >= 0 && best.peak >= reg_thresh &&
              canvas_paste (g_ptr_array_index (frags, best_f), img, w, h,
                            best.dx, best.dy, best.rot, max_side))
            continue;

          if (pass == 0 && frags->len > 0)
            g_ptr_array_add (still, (gpointer) img);     /* retry later */
          else
            g_ptr_array_add (frags, elanpress_canvas_new_from_image (img, w, h));
        }
      g_ptr_array_unref (pending);
      pending = still;
    }
  g_ptr_array_unref (pending);

  return frags;
}

double
elanpress_match (const guint8 *probe, const guint8 *tmpl, int w, int h,
                 const ElanpressMatchParams *p, ElanpressMatchResult *res)
{
  ElanpressProbe *pr = elanpress_probe_new (probe, w, h, p);
  double s = elanpress_probe_match (pr, tmpl, res);

  elanpress_probe_free (pr);
  return s;
}

/* ------------------------------------------------------------- PGM */

gboolean
elanpress_pgm_write8 (const char *path, const guint8 *px, int w, int h)
{
  FILE *f = fopen (path, "wb");

  if (!f)
    return FALSE;
  fprintf (f, "P5\n%d %d\n255\n", w, h);
  fwrite (px, 1, (size_t) w * h, f);
  return fclose (f) == 0;
}

gboolean
elanpress_pgm_write16 (const char *path, const guint16 *px, int w, int h)
{
  FILE *f = fopen (path, "wb");

  if (!f)
    return FALSE;
  fprintf (f, "P5\n%d %d\n65535\n", w, h);
  for (int i = 0; i < w * h; i++)
    {
      guint8 b[2] = { px[i] >> 8, px[i] & 0xff };
      fwrite (b, 1, 2, f);
    }
  return fclose (f) == 0;
}

static gboolean
pgm_header (FILE *f, int *w, int *h, int *maxval)
{
  char magic[3] = { 0 };
  int c;

  if (fscanf (f, "%2s", magic) != 1 || strcmp (magic, "P5") != 0)
    return FALSE;
  /* skip whitespace and comments */
  for (int i = 0; i < 3; i++)
    {
      int *dst = i == 0 ? w : i == 1 ? h : maxval;
      do
        {
          c = fgetc (f);
          if (c == '#')
            while (c != '\n' && c != EOF)
              c = fgetc (f);
        }
      while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
      if (c == EOF)
        return FALSE;
      ungetc (c, f);
      if (fscanf (f, "%d", dst) != 1)
        return FALSE;
    }
  /* exactly one whitespace byte separates the header from the pixels */
  if (fgetc (f) == EOF)
    return FALSE;
  return *w > 0 && *h > 0;
}

guint8 *
elanpress_pgm_read8 (const char *path, int *w, int *h)
{
  FILE *f = fopen (path, "rb");
  int maxval;
  guint8 *px;

  if (!f)
    return NULL;
  if (!pgm_header (f, w, h, &maxval) || maxval != 255)
    {
      fclose (f);
      return NULL;
    }
  px = g_new (guint8, (size_t) *w * *h);
  if (fread (px, 1, (size_t) *w * *h, f) != (size_t) *w * *h)
    {
      g_free (px);
      px = NULL;
    }
  fclose (f);
  return px;
}

guint16 *
elanpress_pgm_read16 (const char *path, int *w, int *h)
{
  FILE *f = fopen (path, "rb");
  int maxval;
  guint16 *px;

  if (!f)
    return NULL;
  if (!pgm_header (f, w, h, &maxval) || maxval <= 255)
    {
      fclose (f);
      return NULL;
    }
  px = g_new (guint16, (size_t) *w * *h);
  for (int i = 0; i < *w * *h; i++)
    {
      int hi = fgetc (f);
      int lo = hi == EOF ? EOF : fgetc (f);
      if (hi == EOF || lo == EOF)
        {
          g_free (px);
          fclose (f);
          return NULL;
        }
      px[i] = (guint16) ((hi << 8) | lo);
    }
  fclose (f);
  return px;
}
