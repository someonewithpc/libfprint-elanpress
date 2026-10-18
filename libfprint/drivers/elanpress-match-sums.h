/*
 * Cross-term loop of the ELAN press-type matcher, one copy per instruction set
 *
 * Copyright (C) 2026 Hugo Sales
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

/* The cross-term loop of layer_sums_fullb(), included once per instruction
 * set by elanpress-match.c. The includer defines SUMS_NAME and SUMS_ATTR for
 * the function, the vector type SUMS_VEC holding SUMS_W pixels, SUMS_ZERO,
 * SUMS_LOAD (p), SUMS_DOT (acc, x, y), which adds the pairwise products of x
 * and y to the 32-bit lanes of acc, and SUMS_ADD (acc), which adds them up. */

G_STATIC_ASSERT (BLOCK % SUMS_W == 0);

SUMS_ATTR static void
SUMS_NAME (const Layer *a, const Layer *b, int dx, int dy,
           int x0, int y0, int x1, int y1, const gboolean fulla,
           gint64 s[5])
{
  SUMS_VEC vsb = SUMS_ZERO, vsbb = SUMS_ZERO, vsab = SUMS_ZERO;

  for (int y = y0; y < y1; y++)
    {
      const gint16 *av = a->v + y * a->stride + x0;
      const gint16 *am = a->m + y * a->stride + x0;
      const gint16 *bv = b->v + (y - dy) * b->stride + (x0 - dx);
      const gint16 *bq = b->q + (y - dy) * b->stride + (x0 - dx);

      if (fulla)
        {
          for (int x = 0; x < x1 - x0; x += SUMS_W)
            vsab = SUMS_DOT (vsab, SUMS_LOAD (av + x), SUMS_LOAD (bv + x));
        }
      else
        {
          for (int x = 0; x < x1 - x0; x += SUMS_W)
            {
              SUMS_VEC m = SUMS_LOAD (am + x), v = SUMS_LOAD (bv + x);

              vsb = SUMS_DOT (vsb, v, m);
              vsbb = SUMS_DOT (vsbb, SUMS_LOAD (bq + x), m);
              vsab = SUMS_DOT (vsab, SUMS_LOAD (av + x), v);
            }
        }
    }
  s[1] += SUMS_ADD (vsb);
  s[3] += SUMS_ADD (vsbb);
  s[4] += SUMS_ADD (vsab);
}

#undef SUMS_NAME
#undef SUMS_ATTR
#undef SUMS_VEC
#undef SUMS_W
#undef SUMS_ZERO
#undef SUMS_LOAD
#undef SUMS_DOT
#undef SUMS_ADD
