/*===---- vcruntime_startup.h - VCRuntime startup declarations --------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __clang_vcruntime_startup_h
#define __clang_vcruntime_startup_h

#if !defined(LLVM_CRT_UCRT)
/* Plain clang-cl / MSVC-compat build: defer to the real VCRuntime header. */
#if __has_include_next(<vcruntime_startup.h>)
#include_next <vcruntime_startup.h>
#endif
#else

/*
 * Zero-Visual-Studio-headers targets (Windows Itanium): the VCRuntime is not
 * on the include path, but UCRT's <corecrt_startup.h> includes this header
 * unconditionally for the startup argv/exit modes and __vcrt_* entry hooks.
 * Only depends on our own <vcruntime.h>.
 */
#include <vcruntime.h>

_CRT_BEGIN_C_HEADER

typedef enum _crt_argv_mode {
  _crt_argv_no_arguments,
  _crt_argv_unexpanded_arguments,
  _crt_argv_expanded_arguments,
} _crt_argv_mode;

typedef enum _crt_exit_return_mode {
  _crt_exit_terminate_process,
  _crt_exit_return_to_caller
} _crt_exit_return_mode;

typedef enum _crt_exit_cleanup_mode {
  _crt_exit_full_cleanup,
  _crt_exit_quick_cleanup,
  _crt_exit_no_cleanup
} _crt_exit_cleanup_mode;

extern _crt_exit_return_mode __current_exit_return_mode;

__vcrt_bool __cdecl __vcrt_initialize(void);
__vcrt_bool __cdecl __vcrt_uninitialize(__vcrt_bool _Terminating);
__vcrt_bool __cdecl __vcrt_uninitialize_critical(void);
__vcrt_bool __cdecl __vcrt_thread_attach(void);
__vcrt_bool __cdecl __vcrt_thread_detach(void);

int __cdecl __isa_available_init(void);
_crt_argv_mode __CRTDECL _get_startup_argv_mode(void);

_CRT_END_C_HEADER

#endif /* LLVM_CRT_UCRT */

#endif /* __clang_vcruntime_startup_h */
