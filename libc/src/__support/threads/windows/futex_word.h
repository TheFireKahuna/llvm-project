//===--- Definition of a type for a futex word ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_WORD_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_WORD_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// 32-bit futex value — matches Linux FutexWordType for upstream compatibility.
// The Futex class pairs this with a separate Atomic<uint32_t> wait_list member
// for an embedded per-Futex waiter chain (no generation counter needed — the
// Dekker protocol between live_count/value handles lost-wakeup prevention).
using FutexWordType = uint32_t;

// The caller-visible value type. Same as FutexWordType on all platforms.
using FutexValueType = uint32_t;

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_FUTEX_WORD_H
