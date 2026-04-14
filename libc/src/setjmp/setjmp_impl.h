//===-- Implementation header for setjmp ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SETJMP_SETJMP_IMPL_H
#define LLVM_LIBC_SRC_SETJMP_SETJMP_IMPL_H

// This header has the _impl prefix in its name to avoid conflict with the
// public header setjmp.h which is also included. here.
#include "hdr/types/jmp_buf.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/compiler.h"
#include "src/__support/macros/properties/os.h"

namespace LIBC_NAMESPACE_DECL {

// TODO(https://github.com/llvm/llvm-project/issues/112427)
// Some of the architecture-specific definitions are marked `naked`, which in
// GCC implies `nothrow`.
//
// Right now, our aliases aren't marked `nothrow`, so we wind up in a situation
// where clang will emit -Wmissing-exception-spec if we add `nothrow` here, but
// GCC will emit -Wmissing-attributes here without `nothrow`. We need to update
// LLVM_LIBC_FUNCTION to denote when a function throws or not.

#ifdef LIBC_COMPILER_IS_GCC
[[gnu::nothrow]]
#endif
[[gnu::returns_twice]] int
#ifdef LIBC_TARGET_OS_IS_WINDOWS
// On Windows, the real entrypoint takes a second argument: the caller's
// establisher frame for RtlUnwindEx (longjmp). The public/internal setjmp
// spelling remains a macro so the caller's frame is captured at the call site.
__llvm_libc_setjmp(jmp_buf buf, void *frame);
#else
setjmp(jmp_buf buf);
#endif

} // namespace LIBC_NAMESPACE_DECL

#if defined(LIBC_TARGET_OS_IS_WINDOWS) && !defined(LLVM_LIBC_SETJMP_DONT_DEFINE_MACRO)
#define setjmp(buf) __llvm_libc_setjmp((buf), __builtin_frame_address(0))
#endif

#endif // LLVM_LIBC_SRC_SETJMP_SETJMP_IMPL_H
