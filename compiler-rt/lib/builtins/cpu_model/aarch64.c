//===-- cpu_model/aarch64.c - Support for __cpu_model builtin  ----*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file is based on LLVM's lib/Support/Host.cpp.
//  It implements __aarch64_have_lse_atomics, __aarch64_cpu_features for
//  AArch64.
//
//===----------------------------------------------------------------------===//

#include "aarch64.h"

#if !defined(__aarch64__) && !defined(__arm64__) && !defined(_M_ARM64) &&      \
    !defined(__arm64ec__) && !defined(_M_ARM64EC)
#error This file is intended only for aarch64-based targets
#endif

#if __has_include(<sys/ifunc.h>)
#include <sys/ifunc.h>
#else
typedef struct __ifunc_arg_t {
  unsigned long _size;
  unsigned long _hwcap;
  unsigned long _hwcap2;
} __ifunc_arg_t;
#endif // __has_include(<sys/ifunc.h>)

#if defined(__NTPOSIX__)
// NT-POSIX has no Windows headers. Below 64, an AArch64 feature is the byte of
// KUSER_SHARED_DATA.ProcessorFeatures, mapped at 0x7ffe0274 in every process,
// that ntdll's predicate reads; above, only ntdll's predicate knows it, through
// the import library of ntdll the fill names in every image.
#define PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE 30
#define PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE 31
#define PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE 34
__declspec(dllimport) unsigned char RtlIsProcessorFeaturePresent(unsigned F);
#define IsProcessorFeaturePresent(F)                                           \
  ((F) < 64 ? ((const unsigned char *)0x7ffe0274)[F]                           \
            : RtlIsProcessorFeaturePresent(F))
#endif

// LSE support detection for out-of-line atomics
// using HWCAP and Auxiliary vector
_Bool __aarch64_have_lse_atomics
    __attribute__((visibility("hidden"), nocommon)) = false;

// The formatter wants to re-order these includes, but doing so is incorrect:
// clang-format off
#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/auxv.h>
#include "aarch64/hwcap.inc"
#include "aarch64/lse_atomics/elf_aux_info.inc"
#elif defined(__Fuchsia__)
#include "aarch64/hwcap.inc"
#include "aarch64/lse_atomics/fuchsia.inc"
#elif defined(__ANDROID__)
#include <sys/auxv.h>
#include "aarch64/hwcap.inc"
#include "aarch64/lse_atomics/android.inc"
#elif defined(__linux__)
#include <sys/auxv.h>
#include "aarch64/hwcap.inc"
#include "aarch64/lse_atomics/getauxval.inc"
#elif defined(_WIN32) || defined(__NTPOSIX__)
#include "aarch64/lse_atomics/windows.inc"
#else
// When unimplemented, we leave __aarch64_have_lse_atomics initialized to false.
#endif
// clang-format on

#if !defined(DISABLE_AARCH64_FMV)

// Architecture features used
// in Function Multi Versioning
struct {
  unsigned long long features;
  // As features grows new fields could be added
} __aarch64_cpu_features __attribute__((visibility("hidden"), nocommon));

// The formatter wants to re-order these includes, but doing so is incorrect:
// clang-format off
#if defined(TARGET_BAREMETAL_AARCH64)
#include "aarch64/fmv/baremetal.inc"
#elif defined(__APPLE__)
#include "aarch64/fmv/apple.inc"
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include "aarch64/fmv/hwcap.inc"
#include "aarch64/fmv/elf_aux_info.inc"
#elif defined(__Fuchsia__)
#include "aarch64/fmv/fuchsia.inc"
#elif defined(__ANDROID__)
#include "aarch64/fmv/hwcap.inc"
#include "aarch64/fmv/android.inc"
#elif defined(__linux__)
#include "aarch64/fmv/hwcap.inc"
#include "aarch64/fmv/getauxval.inc"
#elif defined(_WIN32) || defined(__NTPOSIX__)
#include "aarch64/fmv/windows.inc"
#else
#include "aarch64/fmv/unimplemented.inc"
#endif
// clang-format on

#endif // !defined(DISABLE_AARCH64_FMV)
