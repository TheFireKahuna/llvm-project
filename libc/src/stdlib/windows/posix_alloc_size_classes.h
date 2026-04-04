//===-- posix_alloc size class table (internal) -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single source of truth for the slab allocator's size class table. Used by
// posix_alloc.cpp for runtime dispatch and by allocator tests for exhaustive
// coverage — adding or removing a class updates both in lockstep.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDLIB_WINDOWS_POSIX_ALLOC_SIZE_CLASSES_H
#define LLVM_LIBC_SRC_STDLIB_WINDOWS_POSIX_ALLOC_SIZE_CLASSES_H

#include "src/__support/macros/config.h"

#include "hdr/stdint_proxy.h"

namespace LIBC_NAMESPACE_DECL {

inline constexpr int POSIX_ALLOC_NUM_CLASSES = 40;

inline constexpr uint16_t POSIX_ALLOC_SIZE_CLASSES[POSIX_ALLOC_NUM_CLASSES] = {
    // 16–64: step 16 (alignment-constrained)
    16,    32,    48,    64,
    // 64–128: step 16 (4 per doubling begins)
    80,    96,    112,   128,
    // 128–256: step 32
    160,   192,   224,   256,
    // 256–512: step 64
    320,   384,   448,   512,
    // 512–1024: step 128
    640,   768,   896,   1024,
    // 1024–2048: step 256
    1280,  1536,  1792,  2048,
    // 2048–4096: step 512
    2560,  3072,  3584,  4096,
    // 4096–8192: step 1024
    5120,  6144,  7168,  8192,
    // 8192–16384: step 2048
    10240, 12288, 14336, 16384,
    // 16384–32768: step ~4096
    20480, 24576, 28672, 32768,
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDLIB_WINDOWS_POSIX_ALLOC_SIZE_CLASSES_H
