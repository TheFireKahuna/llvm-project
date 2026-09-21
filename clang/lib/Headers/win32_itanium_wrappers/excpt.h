/*===---- excpt.h - Structured exception handling declarations -------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

#ifndef __EXCPT_H
#define __EXCPT_H

/* On MSVC or when VC Tools headers are available, defer to the system header.
 * On Windows Itanium without VC Tools, provide minimal SEH declarations
 * required by Windows SDK headers (windows.h). */
#if defined(_MSC_VER) && !defined(_WIN32_ITANIUM)
#  if __has_include_next(<excpt.h>)
#    include_next <excpt.h>
#  endif
#else
#define _INC_EXCPT

#include <vcruntime.h>

_CRT_BEGIN_C_HEADER

/* Exception disposition return values. */
typedef enum _EXCEPTION_DISPOSITION {
  ExceptionContinueExecution,
  ExceptionContinueSearch,
  ExceptionNestedException,
  ExceptionCollidedUnwind
} EXCEPTION_DISPOSITION;

// SEH handler
#if defined(_M_IX86) && !defined(_CHPE_X86_ARM64_EH_)

    struct _EXCEPTION_RECORD;
    struct _CONTEXT;

    EXCEPTION_DISPOSITION __cdecl _except_handler(
        _In_ struct _EXCEPTION_RECORD* _ExceptionRecord,
        _In_ void*                     _EstablisherFrame,
        _Inout_ struct _CONTEXT*       _ContextRecord,
        _Inout_ void*                  _DispatcherContext
        );

#elif defined(_M_X64) || defined(_M_ARM64) || defined(_CHPE_X86_ARM64_EH_)
    #ifndef _M_CEE_PURE

        struct _EXCEPTION_RECORD;
        struct _CONTEXT;
        struct _DISPATCHER_CONTEXT;

        _VCRTIMP EXCEPTION_DISPOSITION __cdecl __C_specific_handler(
            _In_    struct _EXCEPTION_RECORD*   ExceptionRecord,
            _In_    void*                       EstablisherFrame,
            _Inout_ struct _CONTEXT*            ContextRecord,
            _Inout_ struct _DISPATCHER_CONTEXT* DispatcherContext
            );

    #endif
#endif


#define GetExceptionCode _exception_code
#define exception_code _exception_code
#define GetExceptionInformation() ((struct _EXCEPTION_POINTERS *)_exception_info())
#define exception_info() ((struct _EXCEPTION_POINTERS *)_exception_info())
#define AbnormalTermination _abnormal_termination
#define abnormal_termination _abnormal_termination

/* SEH intrinsics - compiler builtins. */
unsigned long __cdecl _exception_code(void);
void *        __cdecl _exception_info(void);
int           __cdecl _abnormal_termination(void);



/* Exception filter expression values. */
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

_CRT_END_C_HEADER

#endif /* !_MSC_VER || _WIN32_ITANIUM */

#endif /* __EXCPT_H */
