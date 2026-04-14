//===-- Common header for FMA implementations -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_FPUTIL_FMA_H
#define LLVM_LIBC_SRC___SUPPORT_FPUTIL_FMA_H

#include "src/__support/CPP/type_traits.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/FPUtil/generic/FMA.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h" // LIBC_UNLIKELY
#include "src/__support/macros/properties/architectures.h"
#include "src/__support/macros/properties/cpu_features.h" // LIBC_TARGET_CPU_HAS_FMA

namespace LIBC_NAMESPACE_DECL {
namespace fputil {

template <typename OutType, typename InType>
LIBC_INLINE OutType fma(InType x, InType y, InType z) {
  return generic::fma<OutType>(x, y, z);
}

#ifdef LIBC_TARGET_CPU_HAS_FMA

#ifdef LIBC_TARGET_CPU_HAS_FMA_FLOAT
template <> LIBC_INLINE float fma(float x, float y, float z) {
#ifndef LIBC_MATH_HAS_NO_EXCEPT
  // IEEE 754 / C23: fma(±inf, 0, qNaN) and fma(0, ±inf, qNaN) must raise
  // INVALID.  Hardware FMA may suppress it when a NaN absorbs the result.
  // Hot path cost: one never-taken branch on z's exponent bits.
  if (LIBC_UNLIKELY(FPBits<float>(z).is_nan())) {
    uint32_t x_abs = FPBits<float>(x).abs().uintval();
    uint32_t y_abs = FPBits<float>(y).abs().uintval();
    constexpr uint32_t INF = FPBits<float>::inf().uintval();
    if ((x_abs == INF && y_abs == 0) || (y_abs == INF && x_abs == 0))
      raise_except_if_required(FE_INVALID);
  }
#endif
#if __has_builtin(__builtin_elementwise_fma)
  return __builtin_elementwise_fma(x, y, z);
#else
  return __builtin_fmaf(x, y, z);
#endif
}
#endif // LIBC_TARGET_CPU_HAS_FMA_FLOAT

#ifdef LIBC_TARGET_CPU_HAS_FMA_DOUBLE
template <> LIBC_INLINE double fma(double x, double y, double z) {
#ifndef LIBC_MATH_HAS_NO_EXCEPT
  // IEEE 754 / C23: fma(±inf, 0, qNaN) and fma(0, ±inf, qNaN) must raise
  // INVALID.  Hardware FMA may suppress it when a NaN absorbs the result.
  // Hot path cost: one never-taken branch on z's exponent bits.
  if (LIBC_UNLIKELY(FPBits<double>(z).is_nan())) {
    uint64_t x_abs = FPBits<double>(x).abs().uintval();
    uint64_t y_abs = FPBits<double>(y).abs().uintval();
    constexpr uint64_t INF = FPBits<double>::inf().uintval();
    if ((x_abs == INF && y_abs == 0) || (y_abs == INF && x_abs == 0))
      raise_except_if_required(FE_INVALID);
  }
#endif
#if __has_builtin(__builtin_elementwise_fma)
  return __builtin_elementwise_fma(x, y, z);
#else
  return __builtin_fma(x, y, z);
#endif
}
#endif // LIBC_TARGET_CPU_HAS_FMA_DOUBLE
#endif // LIBC_TARGET_CPU_HAS_FMA

} // namespace fputil
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_FPUTIL_FMA_H
