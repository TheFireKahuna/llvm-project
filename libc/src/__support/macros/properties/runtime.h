//===-- Target runtime personality detection ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Detects the C runtime personality from compiler-provided target macros.
//
//   LIBC_TARGET_RUNTIME_IS_POSIX   - Full POSIX C runtime (llvm-libc is the
//                                    sole libc). Set for NTPOSIX, Linux, etc.
//   LIBC_TARGET_RUNTIME_IS_WIN32   - Win32/UCRT overlay mode. llvm-libc
//                                    coexists with the platform CRT.
//   LIBC_TARGET_RUNTIME_IS_NTPOSIX - NT kernel with POSIX runtime personality.
//                                    Always set together with _IS_POSIX.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_MACROS_PROPERTIES_RUNTIME_H
#define LLVM_LIBC_SRC___SUPPORT_MACROS_PROPERTIES_RUNTIME_H

#if defined(__NTPOSIX__)
// NTPOSIX: full POSIX runtime on NT kernel (no UCRT, no Win32 API).
#define LIBC_TARGET_RUNTIME_IS_NTPOSIX
#define LIBC_TARGET_RUNTIME_IS_POSIX
#elif defined(_WIN32)
// Win32: overlay mode — defer to platform CRT (UCRT) for most definitions.
// Covers MSVC, MinGW, and Windows Itanium environments.
#define LIBC_TARGET_RUNTIME_IS_WIN32
#else
// Everything else: full POSIX runtime (Linux, macOS, Fuchsia, etc.)
#define LIBC_TARGET_RUNTIME_IS_POSIX
#endif

#endif // LLVM_LIBC_SRC___SUPPORT_MACROS_PROPERTIES_RUNTIME_H
