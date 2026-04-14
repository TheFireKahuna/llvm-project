//===-- Definition of type which can represent a futex word ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES___FUTEX_WORD_H
#define LLVM_LIBC_TYPES___FUTEX_WORD_H

#if defined(__NTPOSIX__)
typedef struct {
  // Windows uses a 64-bit futex word: [value:32 | generation:8 | head:24].
  // The extra 32 bits embed a lock-free Treiber wait queue directly in the
  // futex, enabling store-is-wake (value change + waiter pop in one CAS).
  // Linux/Darwin use the kernel's 32-bit futex interface.
  _Alignas(sizeof(__UINT64_TYPE__) > _Alignof(__UINT64_TYPE__)
               ? sizeof(__UINT64_TYPE__)
               : _Alignof(__UINT64_TYPE__)) __UINT64_TYPE__ __word;
} __futex_word;
#else
typedef struct {
  // Futex word should be aligned appropriately to allow target atomic
  // instructions. This declaration mimics the internal setup.
  _Alignas(sizeof(__UINT32_TYPE__) > _Alignof(__UINT32_TYPE__)
               ? sizeof(__UINT32_TYPE__)
               : _Alignof(__UINT32_TYPE__)) __UINT32_TYPE__ __word;
} __futex_word;
#endif

#endif // LLVM_LIBC_TYPES___FUTEX_WORD_H
