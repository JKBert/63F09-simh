/* m63f09_fpu.c: arithmetic unit of the 63F09 FPU (63F39.vhd, a RISC-V style unit: IEEE 754 single and double, five rounding modes,
   accrued-style flags NV DZ OF UF NX in bits 4-0, canonical NaN).  The host FPU does the arithmetic; fenv gives the rounding mode and
   the flags.  Single values live in the low 32 bits of the 64-bit stack registers (the upper half of a result is zero).

   Copyright (c) 2026, Dr. BERTRAND Joël and Claude (Anthropic)

       Permission is hereby granted, free of charge, to any person obtaining a
       copy of this software and associated documentation files (the "Software"),
       to deal in the Software without restriction, including without limitation
       the rights to use, copy, modify, merge, publish, distribute, sublicense,
       and/or sell copies of the Software, and to permit persons to whom the
       Software is furnished to do so, subject to the following conditions:

       The above copyright notice and this permission notice shall be included in
       all copies or substantial portions of the Software.

       THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
       IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
       FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
       THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
       IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
       CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <fenv.h>
#include <math.h>
#include <string.h>
#include "m63f09_defs.h"

/* 1 : an invalid float -> integer conversion gives the "integer indefinite" (signed 0x80..0, unsigned all ones, negative included), what the RTL of
   2026-11-11 does ; 0 : saturation (owner's choice B of 2026-11-11, proposal in regress/proto/dmcnew/fpub/63F39.vhd): NaN and too large -> the largest
   integer, too small -> the smallest, negative to unsigned -> 0.  Set to 0 once soc63f09 is regenerated from the new 63F39.vhd. */
#ifndef M63_RTL_F2I_INDEFINITE
#define M63_RTL_F2I_INDEFINITE 0
#endif

#pragma STDC FENV_ACCESS ON

#define F_NX 0x01
#define F_UF 0x02
#define F_OF 0x04
#define F_DZ 0x08
#define F_NV 0x10

#define CAN_S 0x7FC00000u
#define CAN_D 0x7FF8000000000000ULL

static float  bits_f (uint32 u) { float f; memcpy (&f, &u, 4); return f; }
static uint32 f_bits (float f) { uint32 u; memcpy (&u, &f, 4); return u; }
static double bits_d (t_uint64 u) { double d; memcpy (&d, &u, 8); return d; }
static t_uint64 d_bits (double d) { t_uint64 u; memcpy (&u, &d, 8); return u; }

static int is_snan_s (uint32 u) { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) && !(u & 0x00400000u); }
static int is_snan_d (t_uint64 u) { return (u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (u & 0x000FFFFFFFFFFFFFULL) && !(u & 0x0008000000000000ULL); }

static int set_rm (int rm)                              /* RNE RTZ RDN RUP; RMM and DYN are done by the callers (RNE here) */
{
    int old = fegetround ();

    switch (rm) {
        case 1: fesetround (FE_TOWARDZERO); break;
        case 2: fesetround (FE_DOWNWARD); break;
        case 3: fesetround (FE_UPWARD); break;
        case 7: fesetround (FE_TOWARDZERO); break;      /* DYN behaves as RTZ in the RTL (checked) */
        default: fesetround (FE_TONEAREST); break;
    }
    return old;
}

static uint8 get_flags (void)
{
    uint8 f = 0;
    int e = fetestexcept (FE_ALL_EXCEPT);

    if (e & FE_INEXACT) f |= F_NX;
    if (e & FE_UNDERFLOW) f |= F_UF;
    if (e & FE_OVERFLOW) f |= F_OF;
    if (e & FE_DIVBYZERO) f |= F_DZ;
    if (e & FE_INVALID) f |= F_NV;
    return f;
}


/* Round to nearest, ties away from zero (RMM): the host has no such mode, so the result is computed to nearest even and a tie, the
   only case where the two modes differ, is detected from the exact value and moved to the neighbour of larger magnitude. */
static float rmm_s (int op, float a, float b, float c, float r)
{
    double ed = 0, mid;
    float nb;
    int dir;

    if (isnan (r) || isinf (r))
        return r;
    switch (op) {
        case 0: ed = fma ((double) a, (double) b, (double) c); break;
        case 1: ed = fma ((double) a, (double) b, -(double) c); break;
        case 2: ed = fma (-(double) a, (double) b, -(double) c); break;
        case 3: ed = fma (-(double) a, (double) b, (double) c); break;
        case 4: ed = (double) a + (double) b; break;
        case 5: ed = (double) a - (double) b; break;
        case 6: ed = (double) a * (double) b; break;
        case 7: {
            double rem = fma (-(double) r, (double) b, (double) a);

            if (rem == 0)
                return r;
            dir = ((rem > 0) == (b > 0)) ? 1 : -1;
            nb = nextafterf (r, dir > 0 ? INFINITY : -INFINITY);
            if (isinf (nb))
                return r;
            mid = ((double) r + (double) nb) / 2;
            if (fma (mid, (double) b, -(double) a) == 0)
                return fabsf (nb) > fabsf (r) ? nb : r;
            return r;
        }
        default: return r;
    }
    if (ed == (double) r)
        return r;
    dir = ed > (double) r ? 1 : -1;
    nb = nextafterf (r, dir > 0 ? INFINITY : -INFINITY);
    if (isinf (nb))
        return r;
    mid = ((double) r + (double) nb) / 2;
    if (ed == mid)
        return fabsf (nb) > fabsf (r) ? nb : r;
    return r;
}

static double rmm_d (int op, double a, double b, double r)
{
    double e = 0, nb;
    int dir;

    if (isnan (r) || isinf (r))
        return r;
    switch (op) {
        case 4: case 5: {
            double bb = op == 5 ? -b : b, t = r - a;

            e = (a - (r - t)) + (bb - t);               /* TwoSum: a + bb = r + e exactly */
            break;
        }
        case 6: e = fma (a, b, -r); break;
        case 7: {
            double rem = fma (-r, b, a);
            long double midl;

            if (rem == 0)
                return r;
            dir = ((rem > 0) == (b > 0)) ? 1 : -1;
            nb = nextafter (r, dir > 0 ? INFINITY : -INFINITY);
            if (isinf (nb))
                return r;
            midl = ((long double) r + (long double) nb) / 2;
            if (fmal (midl, (long double) b, -(long double) a) == 0)
                return fabs (nb) > fabs (r) ? nb : r;
            return r;
        }
        default: return r;
    }
    if (e == 0)
        return r;
    nb = nextafter (r, e > 0 ? INFINITY : -INFINITY);
    if (isinf (nb))
        return r;
    if (fabs (e) * 2 == fabs (nb - r))
        return fabs (nb) > fabs (r) ? nb : r;
    return r;
}

/* Magnitude of an integer rounded to p bits with ties away (RMM): returns the exact magnitude to convert, ties moved up */
static t_uint64 rmm_int (t_uint64 m, int p)
{
    int n = 64 - __builtin_clzll (m), sh;
    t_uint64 low, half;

    if (m == 0 || n <= p)
        return m;
    sh = n - p;
    low = m & ((1ULL << sh) - 1);
    half = 1ULL << (sh - 1);
    return low == half ? ((m >> sh) + 1) << sh : m;
}

/* Arithmetic: op 0 FMADD 1 FMSUB 2 FNMADD 3 FNMSUB 4 FADD 5 FSUB 6 FMUL 7 FDIV 8 FSQRT 9 FSGNJ 10 FCMP 11 FMAX 12 FCLASS.
   d1 d2 d3 are the unit inputs (FP2 FP1 FP0 for the FMA family, FP1 FP0 for the binary ones, FP0 for FSQRT and FCLASS). */
void m63f09_fpu_exec (int op, int s32, int rm, t_uint64 d1, t_uint64 d2, t_uint64 d3, t_uint64 *res, uint8 *flags)
{
    int old = set_rm (rm);
    uint8 fl = 0;
    t_uint64 r = 0;

    feclearexcept (FE_ALL_EXCEPT);
    if (s32) {
        uint32 u1 = (uint32) d1, u2 = (uint32) d2, u3 = (uint32) d3;
        volatile float a = bits_f (u1), b = bits_f (u2), c = bits_f (u3), z = 0;
        int nan_in = isnan (a) || isnan (b) || isnan (c);
        int snan = (op <= 3 ? (is_snan_s (u1) || is_snan_s (u2) || is_snan_s (u3))
                  : op == 8 || op == 12 ? is_snan_s (u1) : (is_snan_s (u1) || is_snan_s (u2)));

        switch (op) {
            case 0: z = fmaf (a, b, c); break;
            case 1: z = fmaf (a, b, -c); break;
            case 2: z = fmaf (-a, b, -c); break;
            case 3: z = fmaf (-a, b, c); break;
            case 4: z = a + b; break;
            case 5: z = a - b; break;
            case 6: z = a * b; break;
            case 7: z = a / b; break;
            case 8: z = sqrtf (a); break;
        }
        if (op <= 8) {
            if (op <= 3 && (isinf (a) || isinf (b)) && (a == 0 || b == 0))
                fl |= F_NV;
            fl |= get_flags ();                         /* before the helpers below, which raise flags of their own */
            if (rm == 4)
                z = rmm_s (op, a, b, c, z);
            else if (rm == 7 && (fl & F_OF))            /* DYN: truncation of the mantissa but the overflow gives an infinity (RTL) */
                z = copysignf (INFINITY, z);
            r = f_bits (z);
            if (isnan (z)) {
                r = CAN_S;
                if (snan || !nan_in)
                    fl |= F_NV;
            }
        }
    } else {
        volatile double a = bits_d (d1), b = bits_d (d2), c = bits_d (d3), z = 0;
        int nan_in = isnan (a) || isnan (b) || isnan (c);
        int snan = (op <= 3 ? (is_snan_d (d1) || is_snan_d (d2) || is_snan_d (d3))
                  : op == 8 || op == 12 ? is_snan_d (d1) : (is_snan_d (d1) || is_snan_d (d2)));

        switch (op) {
            case 0: z = fma (a, b, c); break;
            case 1: z = fma (a, b, -c); break;
            case 2: z = fma (-a, b, -c); break;
            case 3: z = fma (-a, b, c); break;
            case 4: z = a + b; break;
            case 5: z = a - b; break;
            case 6: z = a * b; break;
            case 7: z = a / b; break;
            case 8: z = sqrt (a); break;
        }
        if (op <= 8) {
            if (op <= 3 && (isinf (a) || isinf (b)) && (a == 0 || b == 0))
                fl |= F_NV;
            fl |= get_flags ();
            if (rm == 4)
                z = rmm_d (op, a, b, z);
            else if (rm == 7 && (fl & F_OF))
                z = copysign (INFINITY, z);
            r = d_bits (z);
            if (isnan (z)) {
                r = CAN_D;
                if (snan || !nan_in)
                    fl |= F_NV;
            }
        }
    }
    if (op <= 8) {
        *res = r;
        *flags = fl;
        fesetround (old);
        return;
    }
    fesetround (old);
    fl = 0;
    {
        int width = s32 ? 32 : 64;
        t_uint64 sgnmask = 1ULL << (width - 1);
        t_uint64 exp_mask = s32 ? 0x7F800000ULL : 0x7FF0000000000000ULL;
        t_uint64 man_mask = s32 ? 0x007FFFFFULL : 0x000FFFFFFFFFFFFFULL;
        t_uint64 qbit = s32 ? 0x00400000ULL : 0x0008000000000000ULL;
        t_uint64 a = s32 ? (d1 & 0xFFFFFFFFULL) : d1, b = s32 ? (d2 & 0xFFFFFFFFULL) : d2;
        t_uint64 can = s32 ? CAN_S : CAN_D;
        int a_nan = (a & exp_mask) == exp_mask && (a & man_mask), b_nan = (b & exp_mask) == exp_mask && (b & man_mask);
        int a_snan = a_nan && !(a & qbit), b_snan = b_nan && !(b & qbit);

        switch (op) {
            case 9: {                                   /* FSGNJ: a with the sign of b (rm 0), its opposite (1), or xor (2) */
                t_uint64 sb = b & sgnmask, sa = a & sgnmask;
                t_uint64 s = rm == 1 ? (sb ^ sgnmask) : rm == 2 ? (sa ^ sb) : sb;

                r = (a & ~sgnmask) | s;
                break;
            }
            case 10: {                                  /* FCMP a ? b (a = FP1, b = FP0): rm 2 equal, 1 less than, 0 less or equal */
                if (a_nan || b_nan) {
                    r = 0;
                    if (rm == 2 ? (a_snan || b_snan) : 1)
                        fl |= F_NV;
                } else {
                    double x = s32 ? (double) bits_f ((uint32) a) : bits_d (a), y = s32 ? (double) bits_f ((uint32) b) : bits_d (b);

                    r = rm == 2 ? (x == y) : rm == 1 ? (x < y) : rm == 0 ? (x <= y) : 0;
                }
                break;
            }
            case 11: {                                  /* FMAX: rm 0 minimum, rm 1 maximum (RISC-V FMIN/FMAX) */
                if ((a_snan || b_snan) && rm <= 1)
                    fl |= F_NV;
                if (rm > 1)
                    r = 0;
                else if (a_nan && b_nan)
                    r = can;
                else if (a_nan)
                    r = d2;                             /* the operand comes back as it is in the register */
                else if (b_nan)
                    r = d1;
                else {
                    double x = s32 ? (double) bits_f ((uint32) a) : bits_d (a), y = s32 ? (double) bits_f ((uint32) b) : bits_d (b);
                    int ismax = (rm == 1);
                    int a_less;

                    if (rm > 1) { r = 0; break; }

                    if (x == y)                         /* -0 < +0 */
                        a_less = (a & sgnmask) != 0;
                    else
                        a_less = x < y;
                    r = (ismax ? !a_less : a_less) ? d1 : d2;
                }
                break;
            }
            case 12: {                                  /* FCLASS */
                int sign = (a & sgnmask) != 0;
                t_uint64 e = a & exp_mask, m = a & man_mask;

                if (e == exp_mask)
                    r = m ? (a_snan ? 0x100 : 0x200) : (sign ? 0x001 : 0x080);
                else if (e == 0)
                    r = m ? (sign ? 0x004 : 0x020) : (sign ? 0x008 : 0x010);
                else
                    r = sign ? 0x002 : 0x040;
                break;
            }
        }
    }
    *res = r;
    *flags = fl;
}

/* Conversions.  kind: 0 F2F (s32 = the destination is single) 1 I2F 2 F2I.  cop: 0 32S 1 32U 2 64S 3 64U (the integer side). */
void m63f09_fpu_cvt (int kind, int cop, int s32, int rm, t_uint64 d1, t_uint64 *res, uint8 *flags)
{
    int old;
    uint8 fl = 0;
    int tie_nx = 0;
    t_uint64 r = 0;

    if (kind == 0) {
        old = set_rm (rm);
        feclearexcept (FE_ALL_EXCEPT);
        if (s32) {                                      /* double -> single */
            volatile double a = bits_d (d1);
            volatile float z = (float) a;
            uint8 hf = get_flags ();

            if (rm == 7 && (hf & F_OF))
                z = copysignf (INFINITY, z);
            if (rm == 4 && !isnan (a) && !isinf (z)) {
                float nbf = nextafterf (z, (a > (double) z) ? INFINITY : -INFINITY);

                if (!isinf (nbf) && a == ((double) z + (double) nbf) / 2 && fabsf (nbf) > fabsf (z))
                    z = nbf;
            }
            r = isnan (a) ? CAN_S : f_bits (z);
            if (is_snan_d (d1))
                fl |= F_NV;
            feclearexcept (FE_ALL_EXCEPT);
            fl |= hf;
        } else {                                        /* single -> double */
            volatile float a = bits_f ((uint32) d1);
            volatile double z = (double) a;

            r = isnan (a) ? CAN_D : d_bits (z);
            if (is_snan_s ((uint32) d1))
                fl |= F_NV;
        }
        fl |= get_flags ();
        fesetround (old);
    } else if (kind == 1) {                             /* integer -> float */
        int sgn = !(cop & 1);
        int w64 = cop >= 2;
        t_int64 sv = w64 ? (t_int64) d1 : (t_int64) (int32) (uint32) d1;
        t_uint64 uv = w64 ? d1 : (d1 & 0xFFFFFFFFULL);

        if (rm == 4) {                                  /* ties away: round the magnitude here, the conversion below is then exact */
            t_uint64 mag = sgn ? (sv < 0 ? (t_uint64) 0 - (t_uint64) sv : (t_uint64) sv) : uv;
            t_uint64 mr = rmm_int (mag, s32 ? 24 : 53);

            if (mr != mag) {
                tie_nx = 1;
                if (sgn) {
                    sv = sv < 0 ? -(t_int64) mr : (t_int64) mr;
                    uv = (t_uint64) sv;
                } else
                    uv = mr;
            }
        }
        old = set_rm (rm);
        feclearexcept (FE_ALL_EXCEPT);
        if (s32) {
            volatile float z;

            if (sgn)
                z = (float) sv;
            else if (uv >> 63) {                        /* top bit set: halve with a sticky bit so that one rounding is done */
                volatile float h = (float) ((uv >> 1) | (uv & 1));

                z = h * 2.0f;
            } else
                z = (float) (t_int64) uv;
            r = f_bits (z);
        } else {
            volatile double z;

            if (sgn)
                z = (double) sv;
            else if (uv >> 63) {
                volatile double h = (double) ((uv >> 1) | (uv & 1));

                z = h * 2.0;
            } else
                z = (double) (t_int64) uv;
            r = d_bits (z);
        }
        fl |= (get_flags () & F_NX) | (tie_nx ? F_NX : 0);
        fesetround (old);
    } else {                                            /* float -> integer, RISC-V: saturates, NaN is the largest value, NV */
        int sgn = !(cop & 1);
        int w64 = cop >= 2;
        double x = s32 ? (double) bits_f ((uint32) d1) : bits_d (d1);
        int nan = isnan (x);
        double ri;
        t_int64 imax = w64 ? INT64_MAX : INT32_MAX, imin = w64 ? INT64_MIN : INT32_MIN;
        t_uint64 umax = w64 ? UINT64_MAX : UINT32_MAX;

        if (nan) {
            r = (sgn && M63_RTL_F2I_INDEFINITE) ? (t_uint64) imin : sgn ? (t_uint64) imax : umax;   /* invalid: indefinite (RTL) or saturation (B) */
            fl |= F_NV;
        } else {
            switch (rm) {
                case 1: case 7: ri = trunc (x); break;
                case 2: ri = floor (x); break;
                case 3: ri = ceil (x); break;
                case 4: ri = round (x); break;
                default: ri = nearbyint (x); break;      /* the host is in round-to-nearest-even here */
            }
            if (sgn) {
                if (ri >= (w64 ? 9223372036854775808.0 : 2147483648.0) || ri < (w64 ? -9223372036854775808.0 : -2147483648.0)) {
                    r = (M63_RTL_F2I_INDEFINITE || ri < 0) ? (t_uint64) imin : (t_uint64) imax;
                    fl |= F_NV;
                } else {
                    r = (t_uint64) (t_int64) ri;
                    if (ri != x)
                        fl |= F_NX;
                }
            } else {
                if (ri >= (w64 ? 18446744073709551616.0 : 4294967296.0) || ri < 0) {
                    r = (!M63_RTL_F2I_INDEFINITE && ri < 0) ? 0 : umax;
                    fl |= F_NV;
                } else {
                    r = w64 ? (t_uint64) ri : (t_uint64) (uint32) ri;
                    if (ri != x)
                        fl |= F_NX;
                }
            }
        }
        if (!w64)
            r &= 0xFFFFFFFFULL;                         /* the 32-bit results are zero extended (checked against the RTL) */
    }
    *res = r;
    *flags = fl;
}
