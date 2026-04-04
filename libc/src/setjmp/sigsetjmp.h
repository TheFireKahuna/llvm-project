//===-- Implementation header for sigsetjmp ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SETJMP_SIGSETJMP_H
#define LLVM_LIBC_SRC_SETJMP_SIGSETJMP_H

#include "hdr/types/sigjmp_buf.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/compiler.h"
#include "src/__support/macros/properties/os.h"

namespace LIBC_NAMESPACE_DECL {

#ifdef LIBC_COMPILER_IS_GCC
[[gnu::nothrow]]
#endif
[[gnu::returns_twice]] int
#ifdef LIBC_TARGET_OS_IS_WINDOWS
// Like setjmp on Windows, sigsetjmp needs the caller's establisher frame.
// Keep the public/internal sigsetjmp spelling as a macro so the frame is
// captured at the call site. Under NT-POSIX SysV AMD64 ABI: buf in rdi,
// savesigs in esi, frame in rdx (aarch64: x0/x1/x2).
__llvm_libc_sigsetjmp(sigjmp_buf buf, int savesigs, void *frame);
#else
sigsetjmp(sigjmp_buf buf, int savesigs);
#endif

} // namespace LIBC_NAMESPACE_DECL

#if defined(LIBC_TARGET_OS_IS_WINDOWS) && !defined(LLVM_LIBC_SIGSETJMP_DONT_DEFINE_MACRO)
#define sigsetjmp(buf, savesigs)                                               \
  __llvm_libc_sigsetjmp((buf), (savesigs), __builtin_frame_address(0))
#endif

#endif // LLVM_LIBC_SRC_SETJMP_SIGSETJMP_H
