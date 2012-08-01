/*
	DDS GIMP plugin

	Copyright (C) 2004-2012 Shawn Kirst <skirst@gmail.com>,
   with parts (C) 2003 Arne Reuter <homepage@arnereuter.de> where specified.

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; see the file COPYING.  If not, write to
	the Free Software Foundation, 51 Franklin Street, Fifth Floor
	Boston, MA 02110-1301, USA.
*/

/*
 * Parts of this code have been generously released in the public domain
 * by Fabian 'ryg' Giesen.  The original code can be found (at the time
 * of writing) here:  http://mollyrocket.com/forums/viewtopic.php?t=392
 *
 * For more information about this code, see the README.dxt file that
 * came with the source.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>

#include "dds.h"
#include "dxt.h"
#include "endian.h"
#include "mipmap.h"
#include "imath.h"
#include "vec.h"

#include "dxt_tables.h"

#define SWAP(a, b)  do { typeof(a) t; t = a; a = b; b = t; } while(0)

static const vec4_t V4ZERO      = VEC4_CONST1(0.0f);
static const vec4_t V4ONE       = VEC4_CONST1(1.0f);
static const vec4_t V4HALF      = VEC4_CONST1(0.5f);
static const vec4_t V4ONETHIRD  = VEC4_CONST3(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
static const vec4_t V4TWOTHIRDS = VEC4_CONST3(2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f);
static const vec4_t V4GRID      = VEC4_CONST3(31.0f, 63.0f, 31.0f);
static const vec4_t V4GRIDRCP   = VEC4_CONST3(1.0f / 31.0f, 1.0f / 63.0f, 1.0f / 31.0f);
static const vec4_t V4EPSILON   = VEC4_CONST1(1e-04f);

typedef struct
{
   unsigned int single;
   unsigned int alphamask;
   vec4_t points[16];
   vec4_t max;
   vec4_t min;
   vec4_t metric;
} dxtblock_t;

/* extract 4x4 BGRA block */
static void extract_block(const unsigned char *src, int x, int y,
                          int w, int h, unsigned char *block)
{
   int i, j;
   int bw = MIN(w - x, 4);
   int bh = MIN(h - y, 4);
   int bx, by;
   const int rem[] =
   {
      0, 0, 0, 0,
      0, 1, 0, 1,
      0, 1, 2, 0,
      0, 1, 2, 3
   };

   for(i = 0; i < 4; ++i)
   {
      by = rem[(bh - 1) * 4 + i] + y;
      for(j = 0; j < 4; ++j)
      {
         bx = rem[(bw - 1) * 4 + j] + x;
         block[(i * 4 * 4) + (j * 4) + 0] =
            src[(by * (w * 4)) + (bx * 4) + 0];
         block[(i * 4 * 4) + (j * 4) + 1] =
            src[(by * (w * 4)) + (bx * 4) + 1];
         block[(i * 4 * 4) + (j * 4) + 2] =
            src[(by * (w * 4)) + (bx * 4) + 2];
         block[(i * 4 * 4) + (j * 4) + 3] =
            src[(by * (w * 4)) + (bx * 4) + 3];
      }
   }
}

/* pack BGR8 to RGB565 */
static inline unsigned short pack_rgb565(const unsigned char *c)
{
   return((mul8bit(c[2], 31) << 11) |
          (mul8bit(c[1], 63) <<  5) |
          (mul8bit(c[0], 31)      ));
}

/* unpack RGB565 to BGR */
static void unpack_rgb565(unsigned char *dst, unsigned short v)
{
   int r = (v >> 11) & 0x1f;
   int g = (v >>  5) & 0x3f;
   int b = (v      ) & 0x1f;

   dst[0] = (b << 3) | (b >> 2);
   dst[1] = (g << 2) | (g >> 4);
   dst[2] = (r << 3) | (r >> 2);
}

/* linear interpolation at 1/3 point between a and b */
static void lerp_rgb13(unsigned char *dst, unsigned char *a, unsigned char *b)
{
#if 0
   dst[0] = blerp(a[0], b[0], 0x55);
   dst[1] = blerp(a[1], b[1], 0x55);
   dst[2] = blerp(a[2], b[2], 0x55);
#else
   /*
   * according to the S3TC/DX10 specs, this is the correct way to do the
   * interpolation (with no rounding bias)
   *
   * dst = (2 * a + b) / 3;
   */
   dst[0] = (2 * a[0] + b[0]) / 3;
   dst[1] = (2 * a[1] + b[1]) / 3;
   dst[2] = (2 * a[2] + b[2]) / 3;
#endif
}

static void vec4_endpoints_to_565(int *start, int *end, const vec4_t a, const vec4_t b)
{
   int c[8] __attribute__((aligned(16)));
   vec4_t ta = a * V4GRID + V4HALF;
   vec4_t tb = b * V4GRID + V4HALF;

#ifdef USE_SSE
# ifdef __SSE2__
   const __m128i C565 = _mm_setr_epi16(31, 63, 31, 0, 31, 63, 31, 0);
   __m128i ia = _mm_cvttps_epi32(ta);
   __m128i ib = _mm_cvttps_epi32(tb);
   __m128i zero = _mm_setzero_si128();
   __m128i s = _mm_packs_epi32(ia, ib);
   s = _mm_min_epi16(C565, _mm_max_epi16(zero, s));
   *((__m128i *)&c[0]) = _mm_unpacklo_epi16(s, zero);
   *((__m128i *)&c[4]) = _mm_unpackhi_epi16(s, zero);
# else
   const __m64 C565 = _mm_setr_pi16(31, 63, 31, 0);
   __m64 lo, hi, c0, c1;
   __m64 zero = _mm_setzero_si64();
   lo = _mm_cvttps_pi32(ta);
   hi = _mm_cvttps_pi32(_mm_movehl_ps(ta, ta));
   c0 = _mm_packs_pi32(lo, hi);
   lo = _mm_cvttps_pi32(tb);
   hi = _mm_cvttps_pi32(_mm_movehl_ps(tb, tb));
   c1 = _mm_packs_pi32(lo, hi);
   c0 = _mm_min_pi16(C565, _mm_max_pi16(zero, c0));
   c1 = _mm_min_pi16(C565, _mm_max_pi16(zero, c1));
   *((__m64 *)&c[0]) = _mm_unpacklo_pi16(c0, zero);
   *((__m64 *)&c[2]) = _mm_unpackhi_pi16(c0, zero);
   *((__m64 *)&c[4]) = _mm_unpacklo_pi16(c1, zero);
   *((__m64 *)&c[6]) = _mm_unpackhi_pi16(c1, zero);
   _mm_empty();
# endif
#else
   c[0] = (int)ta[0]; c[4] = (int)tb[0];
   c[1] = (int)ta[1]; c[5] = (int)tb[1];
   c[2] = (int)ta[2]; c[6] = (int)tb[2];
   c[0] = MIN(31, MAX(0, c[0]));
   c[1] = MIN(63, MAX(0, c[1]));
   c[2] = MIN(31, MAX(0, c[2]));
   c[4] = MIN(31, MAX(0, c[4]));
   c[5] = MIN(63, MAX(0, c[5]));
   c[6] = MIN(31, MAX(0, c[6]));
#endif

   *start = ((c[2] << 11) | (c[1] << 5) | c[0]);
   *end   = ((c[6] << 11) | (c[5] << 5) | c[4]);
}

static void dxtblock_init(dxtblock_t *dxtb, const unsigned char *block, int flags)
{
   int i, c0, c;
   int dxt1 = (flags & DXT_DXT1);
   float x, y, z;
   vec4_t min, max, center, t, cov, inset;

   dxtb->single = 1;
   dxtb->alphamask = 0;

   if(flags & DXT_PERCEPTUAL)
      dxtb->metric = vec4_set(0.2126f, 0.7152f, 0.0722f, 0.0f);
   else
      dxtb->metric = vec4_set(1.0f, 1.0f, 1.0f, 0.0f);

   c0 = GETL24(block);

   for(i = 0; i < 16; ++i)
   {
      if(dxt1 && block[4 * i + 3] < 128)
         dxtb->alphamask |= (3 << (2 * i));

      x = (float)block[4 * i + 0] / 255.0f;
      y = (float)block[4 * i + 1] / 255.0f;
      z = (float)block[4 * i + 2] / 255.0f;

      dxtb->points[i] = vec4_set(x, y, z, 0);

      c = GETL24(&block[4 * i]);
      dxtb->single = dxtb->single && (c == c0);
   }

   min = vec4_set1(1.0f);
   max = vec4_zero();

   // get bounding box extents
   for(i = 0; i < 16; ++i)
   {
      min = vec4_min(min, dxtb->points[i]);
      max = vec4_max(max, dxtb->points[i]);
   }

   // select diagonal
   center = (max + min) * V4HALF;
   cov = vec4_zero();
   for(i = 0; i < 16; ++i)
   {
      t = dxtb->points[i] - center;
      cov += t * vec4_splatz(t);
   }

#ifdef USE_SSE
   {
      __m128 mask, tmp;
      // get mask
      mask = _mm_cmplt_ps(cov, _mm_setzero_ps());
      // clear high bits (z, w)
      mask = _mm_movelh_ps(mask, _mm_setzero_ps());
      // mask and combine
      tmp = _mm_or_ps(_mm_and_ps(mask, min), _mm_andnot_ps(mask, max));
      min = _mm_or_ps(_mm_and_ps(mask, max), _mm_andnot_ps(mask, min));
      max = tmp;
   }
#else
   {
      float x0, x1, y0, y1;
      x0 = max[0];
      y0 = max[1];
      x1 = min[0];
      y1 = min[1];

      if(cov[0] < 0) SWAP(x0, x1);
      if(cov[1] < 0) SWAP(y0, y1);

      max[0] = x0;
      max[1] = y0;
      min[0] = x1;
      min[1] = y1;
   }
#endif

   // inset bounding box and clamp to [0,1]
   inset = (max - min) * vec4_set1(1.0f / 16.0f);
   max = vec4_min(V4ONE, vec4_max(V4ZERO, max - inset));
   min = vec4_min(V4ONE, vec4_max(V4ZERO, min + inset));

   // clamp to color space and save
   dxtb->max = vec4_trunc(V4GRID * max + V4HALF) * V4GRIDRCP;
   dxtb->min = vec4_trunc(V4GRID * min + V4HALF) * V4GRIDRCP;
}

static void optimize_endpoints3(dxtblock_t *dxtb, unsigned int indices,
                                vec4_t *max, vec4_t *min)
{
   float alpha, beta;
   vec4_t alpha2_sum, alphax_sum;
   vec4_t beta2_sum, betax_sum;
   vec4_t alphabeta_sum, a, b, factor;
   int i, bits;

   alpha2_sum = beta2_sum = alphabeta_sum = vec4_zero();
   alphax_sum = vec4_zero();
   betax_sum = vec4_zero();

   for(i = 0; i < 16; ++i)
   {
      bits = indices >> (2 * i);

      // skip alpha pixels
      if((bits & 3) == 3) continue;

      beta = (float)(bits & 1);
      if(bits & 2) beta = 0.5f;
      alpha = 1.0f - beta;

      a = vec4_set1(alpha);
      b = vec4_set1(beta);
      alpha2_sum += a * a;
      beta2_sum += b * b;
      alphabeta_sum += a * b;
      alphax_sum += dxtb->points[i] * a;
      betax_sum  += dxtb->points[i] * b;
   }

   factor = alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum;
   if(vec4_cmplt(factor, V4EPSILON)) return;
   factor = vec4_rcp(factor);

   a = (alphax_sum * beta2_sum  - betax_sum  * alphabeta_sum) * factor;
   b = (betax_sum  * alpha2_sum - alphax_sum * alphabeta_sum) * factor;

   // clamp to the color space
   a = vec4_min(V4ONE, vec4_max(V4ZERO, a));
   b = vec4_min(V4ONE, vec4_max(V4ZERO, b));
   a = vec4_trunc(V4GRID * a + V4HALF) * V4GRIDRCP;
   b = vec4_trunc(V4GRID * b + V4HALF) * V4GRIDRCP;

   *max = a;
   *min = b;
}

static void optimize_endpoints4(dxtblock_t *dxtb, unsigned int indices,
                                vec4_t *max, vec4_t *min)
{
   float alpha, beta;
   vec4_t alpha2_sum, alphax_sum;
   vec4_t beta2_sum, betax_sum;
   vec4_t alphabeta_sum, a, b, factor;
   int i, bits;

   alpha2_sum = beta2_sum = alphabeta_sum = vec4_zero();
   alphax_sum = vec4_zero();
   betax_sum = vec4_zero();

   for(i = 0; i < 16; ++i)
   {
      bits = indices >> (2 * i);

      beta = (float)(bits & 1);
      if(bits & 2) beta = (1.0f + beta) / 3.0f;
      alpha = 1.0f - beta;

      a = vec4_set1(alpha);
      b = vec4_set1(beta);
      alpha2_sum += a * a;
      beta2_sum += b * b;
      alphabeta_sum += a * b;
      alphax_sum += dxtb->points[i] * a;
      betax_sum  += dxtb->points[i] * b;
   }

   factor = alpha2_sum * beta2_sum - alphabeta_sum * alphabeta_sum;
   if(vec4_cmplt(factor, V4EPSILON)) return;
   factor = vec4_rcp(factor);

   a = (alphax_sum * beta2_sum  - betax_sum  * alphabeta_sum) * factor;
   b = (betax_sum  * alpha2_sum - alphax_sum * alphabeta_sum) * factor;

   // clamp to the color space
   a = vec4_min(V4ONE, vec4_max(V4ZERO, a));
   b = vec4_min(V4ONE, vec4_max(V4ZERO, b));
   a = vec4_trunc(V4GRID * a + V4HALF) * V4GRIDRCP;
   b = vec4_trunc(V4GRID * b + V4HALF) * V4GRIDRCP;

   *max = a;
   *min = b;
}

static unsigned int compress3(dxtblock_t *dxtb)
{
   const int MAX_ITERATIONS = 4;
   int i, iteration, bestiteration = 0, idx;
   unsigned int indices, bestindices = 0;
   vec4_t palette[3], max, min, t0, t1, t2;
   float error, besterror = FLT_MAX;
#ifdef USE_SSE
   vec4_t d, zero = _mm_setzero_ps();
#else
   float d[3];
#endif

   max = dxtb->max;
   min = dxtb->min;

   for(iteration = 0; ;)
   {
      // construct 3 color palette
      palette[0] = max;
      palette[1] = min;
      palette[2] = (max * V4HALF) + (min * V4HALF);

      indices = 0;
      error = 0;

      // match each point to the closest color
      for(i = 0; i < 16; ++i)
      {
         // skip alpha pixels
         if(((dxtb->alphamask >> (2 * i)) & 3) == 3)
         {
            indices |= (3 << (2 * i));
            continue;
         }

         t0 = (dxtb->points[i] - palette[0]) * dxtb->metric;
         t1 = (dxtb->points[i] - palette[1]) * dxtb->metric;
         t2 = (dxtb->points[i] - palette[2]) * dxtb->metric;

#ifdef USE_SSE
         _MM_TRANSPOSE4_PS(t0, t1, t2, zero);
         d = t0 * t0 + t1 * t1 + t2 * t2;
#else
         d[0] = vec4_dot(t0, t0);
         d[1] = vec4_dot(t1, t1);
         d[2] = vec4_dot(t2, t2);
#endif

         if((d[0] < d[1]) && (d[0] < d[2]))
            idx = 0;
         else if(d[1] < d[2])
            idx = 1;
         else
            idx = 2;

         indices |= (idx << (2 * i));

         error += d[idx];
      }

      if(error < besterror)
      {
         besterror = error;
         bestiteration = iteration;
         bestindices = indices;
         dxtb->max = max;
         dxtb->min = min;
      }

      if(bestiteration != iteration) break;

      ++iteration;
      if(iteration == MAX_ITERATIONS) break;

      // optimize endpoints
      optimize_endpoints3(dxtb, indices, &max, &min);
   }

   return(bestindices);
}

static unsigned int compress4(dxtblock_t *dxtb)
{
   const int MAX_ITERATIONS = 4;
   int i, iteration, bestiteration = 0;
   vec4_t palette[4], max, min, t0, t1, t2, t3;
   float error, besterror = FLT_MAX;
   unsigned int b0, b1, b2, b3, b4;
   unsigned int x0, x1, x2;
   unsigned int idx, indices, bestindices = 0;
#ifdef USE_SSE
   vec4_t d;
#else
   float d[4];
#endif

   max = dxtb->max;
   min = dxtb->min;

   for(iteration = 0; ;)
   {
      // construct 4 color palette
      palette[0] = max;
      palette[1] = min;
      palette[2] = (max * V4TWOTHIRDS) + (min * V4ONETHIRD );
      palette[3] = (max * V4ONETHIRD ) + (min * V4TWOTHIRDS);

      indices = 0;
      error = 0;

      // match each point to the closest color
      for(i = 0; i < 16; ++i)
      {
         t0 = (dxtb->points[i] - palette[0]) * dxtb->metric;
         t1 = (dxtb->points[i] - palette[1]) * dxtb->metric;
         t2 = (dxtb->points[i] - palette[2]) * dxtb->metric;
         t3 = (dxtb->points[i] - palette[3]) * dxtb->metric;

#ifdef USE_SSE
         _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
         d = t0 * t0 + t1 * t1 + t2 * t2;
#else
         d[0] = vec4_dot(t0, t0);
         d[1] = vec4_dot(t1, t1);
         d[2] = vec4_dot(t2, t2);
         d[3] = vec4_dot(t3, t3);
#endif

         b0 = d[0] > d[3];
         b1 = d[1] > d[2];
         b2 = d[0] > d[2];
         b3 = d[1] > d[3];
         b4 = d[2] > d[3];

         x0 = b1 & b2;
         x1 = b0 & b3;
         x2 = b0 & b4;

         idx = x2 | ((x0 | x1) << 1);

         indices |= (idx << (2 * i));

         error += d[idx];
      }

      if(error < besterror)
      {
         besterror = error;
         bestiteration = iteration;
         bestindices = indices;
         dxtb->max = max;
         dxtb->min = min;
      }

      if(bestiteration != iteration) break;

      ++iteration;
      if(iteration == MAX_ITERATIONS) break;

      // optimize endpoints
      optimize_endpoints4(dxtb, indices, &max, &min);
   }

   return(bestindices);
}

static void encode_color_block(unsigned char *dst, unsigned char *block, int flags)
{
   dxtblock_t dxtb;
   int i, max16, min16, bits;
   unsigned int indices, remapped;

   dxtblock_init(&dxtb, block, flags);

   if(dxtb.single)
   {
      max16 = (omatch5[block[2]][0] << 11) |
              (omatch6[block[1]][0] <<  5) |
              (omatch5[block[0]][0]      );
      min16 = (omatch5[block[2]][1] << 11) |
              (omatch6[block[1]][1] <<  5) |
              (omatch5[block[0]][1]      );

      indices = 0xaaaaaaaa; // 101010...

      if(dxtb.alphamask)
      {
         indices |= dxtb.alphamask;
         if(max16 > min16)
            SWAP(max16, min16);
      }
      else if(max16 < min16)
      {
         SWAP(max16, min16);
         indices ^= 0x55555555; // 010101...
      }
   }
   else if((flags & DXT_DXT1) && dxtb.alphamask)
   {
      indices = compress3(&dxtb);

      vec4_endpoints_to_565(&max16, &min16, dxtb.max, dxtb.min);

      if(max16 > min16)
      {
         SWAP(max16, min16);
         // remap indices 0 -> 1, 1 -> 0
         remapped = 0;
         for(i = 0; i < 16; ++i)
         {
            bits = (indices >> (2 * i)) & 3;
            if(!(bits & 2)) bits ^= 1;
            remapped |= (bits << (2 * i));
         }
         indices = remapped;
      }
   }
   else
   {
      indices = compress4(&dxtb);

      vec4_endpoints_to_565(&max16, &min16, dxtb.max, dxtb.min);

      if(max16 < min16)
      {
         SWAP(max16, min16);
         indices ^= 0x55555555; // 010101...
      }
   }

   PUTL16(dst + 0, max16);
   PUTL16(dst + 2, min16);
   PUTL32(dst + 4, indices);
}

static void get_min_max_YCoCg(const unsigned char *block,
                              unsigned char *mincolor, unsigned char *maxcolor)
{
   int i;

   mincolor[2] = mincolor[1] = 255;
   maxcolor[2] = maxcolor[1] = 0;

   for(i = 0; i < 16; ++i)
   {
      if(block[4 * i + 2] < mincolor[2]) mincolor[2] = block[4 * i + 2];
      if(block[4 * i + 1] < mincolor[1]) mincolor[1] = block[4 * i + 1];
      if(block[4 * i + 2] > maxcolor[2]) maxcolor[2] = block[4 * i + 2];
      if(block[4 * i + 1] > maxcolor[1]) maxcolor[1] = block[4 * i + 1];
   }
}

static void scale_YCoCg(unsigned char *block,
                        unsigned char *mincolor, unsigned char *maxcolor)
{
   const int s0 = 128 / 2 - 1;
   const int s1 = 128 / 4 - 1;
   int m0, m1, m2, m3;
   int mask0, mask1, scale;
   int i;

   m0 = abs(mincolor[2] - 128);
   m1 = abs(mincolor[1] - 128);
   m2 = abs(maxcolor[2] - 128);
   m3 = abs(maxcolor[1] - 128);

   if(m1 > m0) m0 = m1;
   if(m3 > m2) m2 = m3;
   if(m2 > m0) m0 = m2;

   mask0 = -(m0 <= s0);
   mask1 = -(m0 <= s1);
   scale = 1 + (1 & mask0) + (2 & mask1);

   mincolor[2] = (mincolor[2] - 128) * scale + 128;
   mincolor[1] = (mincolor[1] - 128) * scale + 128;
   mincolor[0] = (scale - 1) << 3;

   maxcolor[2] = (maxcolor[2] - 128) * scale + 128;
   maxcolor[1] = (maxcolor[1] - 128) * scale + 128;
   maxcolor[0] = (scale - 1) << 3;

   for(i = 0; i < 16; ++i)
   {
      block[i * 4 + 2] = (block[i * 4 + 2] - 128) * scale + 128;
      block[i * 4 + 1] = (block[i * 4 + 1] - 128) * scale + 128;
   }
}

#define INSET_SHIFT  4

static void inset_bbox_YCoCg(unsigned char *mincolor, unsigned char *maxcolor)
{
   int inset[4], mini[4], maxi[4];

   inset[2] = (maxcolor[2] - mincolor[2]) - ((1 << (INSET_SHIFT - 1)) - 1);
   inset[1] = (maxcolor[1] - mincolor[1]) - ((1 << (INSET_SHIFT - 1)) - 1);

   mini[2] = ((mincolor[2] << INSET_SHIFT) + inset[2]) >> INSET_SHIFT;
   mini[1] = ((mincolor[1] << INSET_SHIFT) + inset[1]) >> INSET_SHIFT;

   maxi[2] = ((maxcolor[2] << INSET_SHIFT) - inset[2]) >> INSET_SHIFT;
   maxi[1] = ((maxcolor[1] << INSET_SHIFT) - inset[1]) >> INSET_SHIFT;

   mini[2] = (mini[2] >= 0) ? mini[2] : 0;
   mini[1] = (mini[1] >= 0) ? mini[1] : 0;

   maxi[2] = (maxi[2] <= 255) ? maxi[2] : 255;
   maxi[1] = (maxi[1] <= 255) ? maxi[1] : 255;

   mincolor[2] = (mini[2] & 0xf8) | (mini[2] >> 5);
   mincolor[1] = (mini[1] & 0xfc) | (mini[1] >> 6);

   maxcolor[2] = (maxi[2] & 0xf8) | (maxi[2] >> 5);
   maxcolor[1] = (maxi[1] & 0xfc) | (maxi[1] >> 6);
}

static void select_diagonal_YCoCg(const unsigned char *block,
                                  unsigned char *mincolor,
                                  unsigned char *maxcolor)
{
   unsigned char mid0, mid1, side, mask, b0, b1, c0, c1;
   int i;

   mid0 = ((int)mincolor[2] + maxcolor[2] + 1) >> 1;
   mid1 = ((int)mincolor[1] + maxcolor[1] + 1) >> 1;

   side = 0;
   for(i = 0; i < 16; ++i)
   {
      b0 = block[i * 4 + 2] >= mid0;
      b1 = block[i * 4 + 1] >= mid1;
      side += (b0 ^ b1);
   }

   mask = -(side > 8);
   mask &= -(mincolor[2] != maxcolor[2]);

   c0 = mincolor[1];
   c1 = maxcolor[1];

   c0 ^= c1;
   c1 ^= c0 & mask;
   c0 ^= c1;

   mincolor[1] = c0;
   maxcolor[1] = c1;
}

static void encode_YCoCg_block(unsigned char *dst, unsigned char *block)
{
   unsigned char colors[4][3], *maxcolor, *mincolor;
   unsigned int mask;
   int c0, c1, d0, d1, d2, d3;
   int b0, b1, b2, b3, b4;
   int x0, x1, x2;
   int i, idx;

   maxcolor = &colors[0][0];
   mincolor = &colors[1][0];

   get_min_max_YCoCg(block, mincolor, maxcolor);
   scale_YCoCg(block, mincolor, maxcolor);
   inset_bbox_YCoCg(mincolor, maxcolor);
   select_diagonal_YCoCg(block, mincolor, maxcolor);

   lerp_rgb13(&colors[2][0], maxcolor, mincolor);
   lerp_rgb13(&colors[3][0], mincolor, maxcolor);

   mask = 0;

   for(i = 0; i < 16; ++i)
   {
      c0 = block[4 * i + 2];
      c1 = block[4 * i + 1];

      d0 = abs(colors[0][2] - c0) + abs(colors[0][1] - c1);
      d1 = abs(colors[1][2] - c0) + abs(colors[1][1] - c1);
      d2 = abs(colors[2][2] - c0) + abs(colors[2][1] - c1);
      d3 = abs(colors[3][2] - c0) + abs(colors[3][1] - c1);

      b0 = d0 > d3;
      b1 = d1 > d2;
      b2 = d0 > d2;
      b3 = d1 > d3;
      b4 = d2 > d3;

      x0 = b1 & b2;
      x1 = b0 & b3;
      x2 = b0 & b4;

      idx = (x2 | ((x0 | x1) << 1));

      mask |= idx << (2 * i);
   }

   PUTL16(dst + 0, pack_rgb565(maxcolor));
   PUTL16(dst + 2, pack_rgb565(mincolor));
   PUTL32(dst + 4, mask);
}

/* write DXT3 alpha block */
static void encode_alpha_block_DXT3(unsigned char *dst,
                                    const unsigned char *block)
{
   int i, a1, a2;

   block += 3;

   for(i = 0; i < 8; ++i)
   {
      a1 = block[8 * i + 0];
      a2 = block[8 * i + 4];
      *dst++ = ((a2 >> 4) << 4) | (a1 >> 4);
   }
}

/* Write DXT5 alpha block */
static void encode_alpha_block_DXT5(unsigned char *dst,
                                    const unsigned char *block,
                                    const int offset)
{
   int i, v, mn, mx;
   int dist, bias, dist2, dist4, bits, mask;
   int a, idx, t;

   block += offset;
   block += 3;

   /* find min/max alpha pair */
   mn = mx = block[0];
   for(i = 0; i < 16; ++i)
   {
      v = block[4 * i];
      if(v > mx) mx = v;
      if(v < mn) mn = v;
   }

   /* encode them */
   *dst++ = mx;
   *dst++ = mn;

   /*
    * determine bias and emit indices
    * given the choice of mx/mn, these indices are optimal:
    * http://fgiesen.wordpress.com/2009/12/15/dxt5-alpha-block-index-determination/
    */
   dist = mx - mn;
   dist4 = dist * 4;
   dist2 = dist * 2;
   bias = (dist < 8) ? (dist - 1) : (dist / 2 + 2);
   bias -= mn * 7;
   bits = 0;
   mask = 0;

   for(i = 0; i < 16; ++i)
   {
      a = block[4 * i] * 7 + bias;

      /* select index. this is a "linear scale" lerp factor between 0 (val=min) and 7 (val=max). */
      t = (a >= dist4) ? -1 : 0; idx =  t & 4; a -= dist4 & t;
      t = (a >= dist2) ? -1 : 0; idx += t & 2; a -= dist2 & t;
      idx += (a >= dist);

      /* turn linear scale into DXT index (0/1 are extremal pts) */
      idx = -idx & 7;
      idx ^= (2 > idx);

      /* write index */
      mask |= idx << bits;
      if((bits += 3) >= 8)
      {
         *dst++ = mask;
         mask >>= 8;
         bits -= 8;
      }
   }
}

#define BLOCK_OFFSET(x, y, w, bs)  (((y) >> 2) * ((bs) * (((w) + 3) >> 2)) + ((bs) * ((x) >> 2)))

static void compress_DXT1(unsigned char *dst, const unsigned char *src,
                          int w, int h, int flags)
{
   unsigned char block[64], *p;
   int x, y;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) private(block, p, x)
#endif
   for(y = 0; y < h; y += 4)
   {
      for(x = 0; x < w; x += 4)
      {
         p = dst + BLOCK_OFFSET(x, y, w, 8);
         extract_block(src, x, y, w, h, block);
         encode_color_block(p, block, DXT_DXT1 | flags);
      }
   }
}

static void compress_DXT3(unsigned char *dst, const unsigned char *src,
                          int w, int h, int flags)
{
   unsigned char block[64], *p;
   int x, y;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) private(block, p, x)
#endif
   for(y = 0; y < h; y += 4)
   {
      for(x = 0; x < w; x += 4)
      {
         p = dst + BLOCK_OFFSET(x, y, w, 16);
         extract_block(src, x, y, w, h, block);
         encode_alpha_block_DXT3(p, block);
         encode_color_block(p + 8, block, DXT_DXT3 | flags);
      }
   }
}

static void compress_DXT5(unsigned char *dst, const unsigned char *src,
                          int w, int h, int flags)
{
   unsigned char block[64], *p;
   int x, y;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) private(block, p, x)
#endif
   for(y = 0; y < h; y += 4)
   {
      for(x = 0; x < w; x += 4)
      {
         p = dst + BLOCK_OFFSET(x, y, w, 16);
         extract_block(src, x, y, w, h, block);
         encode_alpha_block_DXT5(p, block, 0);
         encode_color_block(p + 8, block, DXT_DXT5 | flags);
      }
   }
}

static void compress_BC4(unsigned char *dst, const unsigned char *src,
                         int w, int h)
{
   unsigned char block[64], *p;
   int x, y;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) private(block, p, x)
#endif
   for(y = 0; y < h; y += 4)
   {
      for(x = 0; x < w; x += 4)
      {
         p = dst + BLOCK_OFFSET(x, y, w, 8);
         extract_block(src, x, y, w, h, block);
         encode_alpha_block_DXT5(p, block, -1);
      }
   }
}

static void compress_BC5(unsigned char *dst, const unsigned char *src,
                         int w, int h)
{
   unsigned char block[64], *p;
   int x, y;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) private(block, p, x)
#endif
   for(y = 0; y < h; y += 4)
   {
      for(x = 0; x < w; x += 4)
      {
         p = dst + BLOCK_OFFSET(x, y, w, 16);
         extract_block(src, x, y, w, h, block);
         encode_alpha_block_DXT5(p, block, -2);
         encode_alpha_block_DXT5(p + 8, block, -1);
      }
   }
}

static void compress_YCoCg(unsigned char *dst, const unsigned char *src,
                           int w, int h)
{
   unsigned char block[64], *p;
   int x, y;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) private(block, p, x)
#endif
   for(y = 0; y < h; y += 4)
   {
      for(x = 0; x < w; x += 4)
      {
         p = dst + BLOCK_OFFSET(x, y, w, 16);
         extract_block(src, x, y, w, h, block);
         encode_alpha_block_DXT5(p, block, 0);
         encode_YCoCg_block(p + 8, block);
      }
   }
}

int dxt_compress(unsigned char *dst, unsigned char *src, int format,
                 unsigned int width, unsigned int height, int bpp,
                 int mipmaps, int flags)
{
   int i, size, w, h;
   unsigned int offset;
   unsigned char *tmp = NULL;
   int j;
   unsigned char *s;

   if(bpp == 1)
   {
      /* grayscale promoted to BGRA */

      size = get_mipmapped_size(width, height, 4, 0, mipmaps,
                                DDS_COMPRESS_NONE);
      tmp = g_malloc(size);

      for(i = j = 0; j < size; ++i, j += 4)
      {
         tmp[j + 0] = src[i];
         tmp[j + 1] = src[i];
         tmp[j + 2] = src[i];
         tmp[j + 3] = 255;
      }

      bpp = 4;
   }
   else if(bpp == 2)
   {
      /* gray-alpha promoted to BGRA */

      size = get_mipmapped_size(width, height, 4, 0, mipmaps,
                                DDS_COMPRESS_NONE);
      tmp = g_malloc(size);

      for(i = j = 0; j < size; i += 2, j += 4)
      {
         tmp[j + 0] = src[i];
         tmp[j + 1] = src[i];
         tmp[j + 2] = src[i];
         tmp[j + 3] = src[i + 1];
      }

      bpp = 4;
   }
   else if(bpp == 3)
   {
      size = get_mipmapped_size(width, height, 4, 0, mipmaps,
                                DDS_COMPRESS_NONE);
      tmp = g_malloc(size);

      for(i = j = 0; j < size; i += 3, j += 4)
      {
         tmp[j + 0] = src[i + 0];
         tmp[j + 1] = src[i + 1];
         tmp[j + 2] = src[i + 2];
         tmp[j + 3] = 255;
      }

      bpp = 4;
   }

   offset = 0;
   w = width;
   h = height;
   s = tmp ? tmp : src;

   for(i = 0; i < mipmaps; ++i)
   {
      switch(format)
      {
         case DDS_COMPRESS_BC1:
            compress_DXT1(dst + offset, s, w, h, flags);
            break;
         case DDS_COMPRESS_BC2:
            compress_DXT3(dst + offset, s, w, h, flags);
            break;
         case DDS_COMPRESS_BC3:
         case DDS_COMPRESS_BC3N:
         case DDS_COMPRESS_RXGB:
         case DDS_COMPRESS_AEXP:
         case DDS_COMPRESS_YCOCG:
            compress_DXT5(dst + offset, s, w, h, flags);
            break;
         case DDS_COMPRESS_BC4:
            compress_BC4(dst + offset, s, w, h);
            break;
         case DDS_COMPRESS_BC5:
            compress_BC5(dst + offset, s, w, h);
            break;
         case DDS_COMPRESS_YCOCGS:
            compress_YCoCg(dst + offset, s, w, h);
            break;
         default:
            compress_DXT5(dst + offset, s, w, h, flags);
            break;
      }
      s += (w * h * bpp);
      offset += get_mipmapped_size(w, h, 0, 0, 1, format);
      w = MAX(1, w >> 1);
      h = MAX(1, h >> 1);
   }

   if(tmp) g_free(tmp);

   return(1);
}

static void decode_color_block(unsigned char *block, unsigned char *src,
                               int format)
{
   int i, x, y;
   unsigned char *d = block;
   unsigned int indexes, idx;
   unsigned char colors[4][3];
   unsigned short c0, c1;

   c0 = GETL16(&src[0]);
   c1 = GETL16(&src[2]);

   unpack_rgb565(colors[0], c0);
   unpack_rgb565(colors[1], c1);

   if((c0 > c1) || (format == DDS_COMPRESS_BC3))
   {
      lerp_rgb13(colors[2], colors[0], colors[1]);
      lerp_rgb13(colors[3], colors[1], colors[0]);
   }
   else
   {
      for(i = 0; i < 3; ++i)
      {
         colors[2][i] = (colors[0][i] + colors[1][i] + 1) >> 1;
         colors[3][i] = 255;
      }
   }

   src += 4;
   for(y = 0; y < 4; ++y)
   {
      indexes = src[y];
      for(x = 0; x < 4; ++x)
      {
         idx = indexes & 0x03;
         d[0] = colors[idx][2];
         d[1] = colors[idx][1];
         d[2] = colors[idx][0];
         if(format == DDS_COMPRESS_BC1)
            d[3] = ((c0 <= c1) && idx == 3) ? 0 : 255;
         indexes >>= 2;
         d += 4;
      }
   }
}

static void decode_alpha_block_DXT3(unsigned char *block, unsigned char *src)
{
   int x, y;
   unsigned char *d = block;
   unsigned int bits;

   for(y = 0; y < 4; ++y)
   {
      bits = GETL16(&src[2 * y]);
      for(x = 0; x < 4; ++x)
      {
         d[0] = (bits & 0x0f) * 17;
         bits >>= 4;
         d += 4;
      }
   }
}

static void decode_alpha_block_DXT5(unsigned char *block, unsigned char *src, int w)
{
   int x, y, code;
   unsigned char *d = block;
   unsigned char a0 = src[0];
   unsigned char a1 = src[1];
   unsigned long long bits = GETL64(src) >> 16;

   for(y = 0; y < 4; ++y)
   {
      for(x = 0; x < 4; ++x)
      {
         code = ((unsigned int)bits) & 0x07;
         if(code == 0)
            d[0] = a0;
         else if(code == 1)
            d[0] = a1;
         else if(a0 > a1)
            d[0] = ((8 - code) * a0 + (code - 1) * a1) / 7;
         else if(code >= 6)
            d[0] = (code == 6) ? 0 : 255;
         else
            d[0] = ((6 - code) * a0 + (code - 1) * a1) / 5;
         bits >>= 3;
         d += 4;
      }
      if(w < 4) bits >>= (3 * (4 - w));
   }
}

static void make_normal(unsigned char *dst, unsigned char x, unsigned char y)
{
   float nx = 2.0f * ((float)x / 255.0f) - 1.0f;
   float ny = 2.0f * ((float)y / 255.0f) - 1.0f;
   float nz = 0.0f;
   float d = 1.0f - nx * nx + ny * ny;
   int z;

   if(d > 0) nz = sqrtf(d);

   z = (int)(255.0f * (nz + 1) / 2.0f);
   z = MAX(0, MIN(255, z));

   dst[0] = x;
   dst[1] = y;
   dst[2] = z;
}

static void normalize_block(unsigned char *block, int format)
{
   int x, y, tmp;

   for(y = 0; y < 4; ++y)
   {
      for(x = 0; x < 4; ++x)
      {
         if(format == DDS_COMPRESS_BC3)
         {
            tmp = block[y * 16 + (x * 4)];
            make_normal(&block[y * 16 + (x * 4)],
                        block[y * 16 + (x * 4) + 3],
                        block[y * 16 + (x * 4) + 1]);
            block[y * 16 + (x * 4) + 3] = tmp;
         }
         else if(format == DDS_COMPRESS_BC5)
         {
            make_normal(&block[y * 16 + (x * 4)],
                        block[y * 16 + (x * 4)],
                        block[y * 16 + (x * 4) + 1]);
         }
      }
   }
}

static void put_block(unsigned char *dst, unsigned char *block,
                      unsigned int bx, unsigned int by,
                      unsigned int width, unsigned height,
                      int bpp)
{
   int x, y, i;
   unsigned char *d;

   for(y = 0; y < 4 && ((by + y) < height); ++y)
   {
      d = dst + ((y + by) * width + bx) * bpp;
      for(x = 0; x < 4 && ((bx + x) < width); ++x)
      {
         for(i = 0; i < bpp; ++ i)
            *d++ = block[y * 16 + (x * 4) + i];
      }
   }
}

int dxt_decompress(unsigned char *dst, unsigned char *src, int format,
                   unsigned int size, unsigned int width, unsigned int height,
                   int bpp, int normals)
{
   unsigned char *s;
   unsigned int x, y;
   unsigned char block[16 * 4];

   s = src;

   for(y = 0; y < height; y += 4)
   {
      for(x = 0; x < width; x += 4)
      {
         memset(block, 255, 16 * 4);

         if(format == DDS_COMPRESS_BC1)
         {
            decode_color_block(block, s, format);
            s += 8;
         }
         else if(format == DDS_COMPRESS_BC2)
         {
            decode_alpha_block_DXT3(block + 3, s);
            s += 8;
            decode_color_block(block, s, format);
            s += 8;
         }
         else if(format == DDS_COMPRESS_BC3)
         {
            decode_alpha_block_DXT5(block + 3, s, width);
            s += 8;
            decode_color_block(block, s, format);
            s += 8;
         }
         else if(format == DDS_COMPRESS_BC4)
         {
            decode_alpha_block_DXT5(block, s, width);
            s += 8;
         }
         else if(format == DDS_COMPRESS_BC5)
         {
            decode_alpha_block_DXT5(block, s + 8, width);
            decode_alpha_block_DXT5(block + 1, s, width);
            s += 16;
         }

         if(normals)
            normalize_block(block, format);

         put_block(dst, block, x, y, width, height, bpp);
      }
   }

   return(1);
}
