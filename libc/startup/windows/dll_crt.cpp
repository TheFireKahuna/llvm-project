//===-- Complete DLL CRT for Windows Itanium ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single translation unit combining the bootstrap DLL CRT components:
//   - _DllMainCRTStartup (CRT section walking, DllMain dispatch)
//   - __security_cookie / __security_init_cookie (/GS stack protection)
//   - _tls_index / _tls_used (PE TLS directory for C++ thread_local)
//   - atexit / __cxa_atexit / __cxa_finalize (self-contained, SRW lock + NT VA)
//   - __cxa_thread_finalize / __cxa_thread_finalize_dso_unload (weak defaults — overridden by full libc)
//
// Compiled into each DLL that uses libc (libunwind, libc++, user DLLs).
// Each DLL gets its own atexit list, security cookie, and entry point.
//
// For libunwind/libc++ (built before libc in runtimes), cmake adds this
// file as a source. For user DLLs that link against libc, the full libc
// provides these symbols — this file is not used.
//
//===----------------------------------------------------------------------===//

// Order matters: support components before dll_startup which references them.
// security_cookie.cpp and tls_directory.cpp are linked as separate objects
// (crt_gs.obj, crt_tls.obj) by the driver — not included here.
#include "dll_atexit.cpp"                   // NOLINT(bugprone-suspicious-include)
#include "cxa_thread_finalize_default.cpp"  // NOLINT(bugprone-suspicious-include)
#include "dll_startup.cpp"                  // NOLINT(bugprone-suspicious-include)
