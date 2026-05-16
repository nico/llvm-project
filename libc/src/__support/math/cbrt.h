//===-- Correctly rounded cbrt for double precision ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_MATH_CBRT_H
#define LLVM_LIBC_SRC___SUPPORT_MATH_CBRT_H

#include "src/__support/FPUtil/FEnvImpl.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/FPUtil/multiply_add.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h" // LIBC_UNLIKELY

namespace LIBC_NAMESPACE_DECL {

namespace math {

// Correctly rounded cbrt algorithm, adapted from the CORE-MATH project
// (https://core-math.gitlabpages.inria.fr/).
//
// The algorithm has 4 stages:
//   1. Range reduction: decompose x = sign * 2^(3q + r) * m, where
//      1 <= m < 2 and 0 <= r <= 2.  Then cbrt(x) = sign * 2^q * cbrt(2^r * m).
//   2. Degree-3 polynomial approximation of m^(1/3) on [1, 2].
//   3. Two Newton-Raphson refinements to get ~106 bits of accuracy.
//   4. Two hardcoded exceptional cases for round-to-nearest.
LIBC_INLINE constexpr double cbrt(double x) {
  using FPBits = fputil::FPBits<double>;

  FPBits x_bits(x);
  uint64_t x_u = x_bits.uintval();
  uint64_t x_abs = x_u & 0x7FFF'FFFF'FFFF'FFFF;
  uint64_t x_sign = x_u >> 63;

  unsigned e = (x_u >> 52) & 0x7FF;

  // Handle special cases: 0, inf, nan, subnormal.
  if (LIBC_UNLIKELY(((e + 1) & 0x7FF) < 2)) {
    if (e == 0x7FF || x_abs == 0)
      return static_cast<double>(x + x);

    // Subnormal: normalize by counting leading zeros.
    int nz = cpp::countl_zero(x_abs) - 11;
    uint64_t mant = (x_abs << nz) & 0x000F'FFFF'FFFF'FFFF;
    e -= static_cast<unsigned>(nz - 1);
    x_u = mant | (static_cast<uint64_t>(e) << 52) | (x_sign << 63);
  }

  uint64_t mant = x_u & 0x000F'FFFF'FFFF'FFFF;

  // Add 3072 to the exponent so that e + 3072 is always positive and
  // the division/modulo by 3 works correctly.
  e += 3072;
  unsigned et = e / 3;   // exponent of result / 3
  unsigned it = e % 3;   // 0, 1, or 2

  // z = 1.mantissa (reduced argument in [1, 2))
  double z = FPBits(mant | (static_cast<uint64_t>(0x3FF) << 52)).get_val();

  // zz = sign * 2^it * z, so that cbrt(x) = 2^(et-1365) * cbrt(zz)
  // where 1 <= |zz| < 8.
  double zz =
      FPBits(mant | (static_cast<uint64_t>(0x3FF + it) << 52) | (x_sign << 63))
          .get_val();

  // escale[it] = 2^(it/3), with sign applied.
  constexpr uint64_t ESCALE_BITS[3] = {
      0x3FF0'0000'0000'0000, // 1.0
      0x3FF4'28A2'F98D'728B, // 2^(1/3)
      0x3FF9'65FE'A53D'6E3D, // 2^(2/3)
  };
  double escale =
      FPBits(ESCALE_BITS[it] | (x_sign << 63)).get_val();

  // Degree-3 polynomial approximation of z^(1/3) on [1, 2].
  // Max error < 9.2e-5.
  constexpr double C0 = 0x1.1b0babccfef9cp-1;
  constexpr double C1 = 0x1.2c9a3e94d1da5p-1;
  constexpr double C2 = -0x1.4dc30b1a1ddbap-3;
  constexpr double C3 = 0x1.7a8d3e4ec9b07p-6;

  double z2 = z * z;
  double p01 = fputil::multiply_add(z, C1, C0);
  double p23 = fputil::multiply_add(z, C3, C2);
  double y = fputil::multiply_add(z2, p23, p01);

  // First Newton-Raphson refinement (cubic variant).
  // f(y) = 1 - z/y^3, so h = y^3/z - 1 measures the error.
  // Correction: y -= (h*y) * (1/3 - 2/9 * h)
  constexpr double ONE_THIRD = 0x1.5555555555555p-2;
  constexpr double TWO_NINTHS = 0x1.c71c71c71c71cp-3;

  double r = 1.0 / z;
  double y2 = y * y;
  double h = y2 * (y * r) - 1.0;
  y -= (h * y) * (ONE_THIRD - TWO_NINTHS * h);

  // Multiply by 2^(it/3) to get approximation of zz^(1/3).
  y *= escale;

  // Second Newton-Raphson refinement (linear, using FMA for exact squares).
  // Compute y^3 as (y2 + y2l) * y where y2 + y2l = y*y exactly via FMA.
  double rr = 1.0 / zz;  // Note: rr has the sign of zz baked in.
  y2 = y * y;
  double y2l = fputil::multiply_add(y, y, -y2);
  double y3 = y2 * y;
  double y3l = fputil::multiply_add(y, y2, -y3) + y * y2l;
  // h approximates y^3/zz - 1 with ~106-bit accuracy.
  h = ((y3 - zz) + y3l) * rr;
  double dy = h * (y * ONE_THIRD);
  // y1 + dy_rem approximates zz^(1/3) with ~106-bit accuracy.
  double y1 = y - dy;
  double dy_rem = (y - y1) - dy;

#ifndef LIBC_MATH_CBRT_SKIP_ACCURATE_PASS
  // Check if we're near a rounding boundary.
  double ady = FPBits(dy_rem).abs().get_val();
  // For round-to-nearest, check distance from 1/2 ULP boundary.
  double ady0 = FPBits(ady - 0x1p-53).abs().get_val();
  double ady1 = FPBits(ady - 0x1p-52 - 0x1p-53).abs().get_val();

  if (LIBC_UNLIKELY(ady0 < 0x1p-75 || ady1 < 0x1p-75)) {
    // Near a rounding boundary: redo the Newton step with y1 for more accuracy.
    y2 = y1 * y1;
    y2l = fputil::multiply_add(y1, y1, -y2);
    y3 = y2 * y1;
    y3l = fputil::multiply_add(y1, y2, -y3) + y1 * y2l;
    h = ((y3 - zz) + y3l) * rr;
    dy = h * (y1 * ONE_THIRD);
    y = y1 - dy;
    dy_rem = (y1 - y) - dy;
    y1 = y;
    ady = FPBits(dy_rem).abs().get_val();
    ady0 = FPBits(ady - 0x1p-53).abs().get_val();
    ady1 = FPBits(ady - 0x1p-52 - 0x1p-53).abs().get_val();

    if (LIBC_UNLIKELY(ady0 < 0x1p-98 || ady1 < 0x1p-98)) {
      // Hardcoded exceptional cases for round-to-nearest.
      double azz = FPBits(zz).abs().get_val();
      if (azz == 0x1.9b78223aa307cp+1)
        y1 = FPBits(0x3FF79D15'0E8D59BCull | (x_sign << 63)).get_val();
      if (azz == 0x1.a202bfc89ddffp+2)
        y1 = FPBits(0x3FFDE87A'A837820Full | (x_sign << 63)).get_val();
    }
  }
#endif // LIBC_MATH_CBRT_SKIP_ACCURATE_PASS

  // Apply the exponent: multiply by 2^(et - 342 - 1023).
  FPBits y1_bits(y1);
  uint64_t y1_u = y1_bits.uintval();
  y1_u += static_cast<uint64_t>(et - 342 - 1023) << 52;

#ifndef LIBC_MATH_CBRT_SKIP_ACCURATE_PASS
  // Check for exact outputs (to clear inexact flag).
  int64_t m0 = static_cast<int64_t>(y1_u << 30);
  int64_t m1 = m0 >> 63;
  if (LIBC_UNLIKELY(static_cast<uint64_t>(m0 ^ m1) <= (1ull << 30))) {
    FPBits y1r_bits(y1);
    uint64_t y1r_u = (y1r_bits.uintval() + (1ull << 15)) &
                     0xFFFF'FFFF'FFFF'0000ull;
    double y1r = FPBits(y1r_u).get_val();
    if (FPBits((y1r - y1) - dy_rem).abs().get_val() < 0x1p-60 ||
        FPBits(zz).abs().get_val() == 1.0) {
      y1_u = (y1_u + (1ull << 15)) & 0xFFFF'FFFF'FFFF'0000ull;
      fputil::clear_except_if_required(FE_INEXACT);
    }
  }
#endif // LIBC_MATH_CBRT_SKIP_ACCURATE_PASS

  return FPBits(y1_u).get_val();
}

} // namespace math

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_MATH_CBRT_H
