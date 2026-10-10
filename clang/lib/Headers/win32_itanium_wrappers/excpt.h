/*===---- excpt.h - Structured exception handling --------------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 */

/* The Windows SDK's windows.h and winnt.h take these declarations from the
 * Visual C++ runtime's excpt.h. */

#ifndef __CLANG_EXCPT_H
#define __CLANG_EXCPT_H

#include <vcruntime.h>

_CRT_BEGIN_C_HEADER

typedef enum _EXCEPTION_DISPOSITION {
  ExceptionContinueExecution,
  ExceptionContinueSearch,
  ExceptionNestedException,
  ExceptionCollidedUnwind
} EXCEPTION_DISPOSITION;

struct _EXCEPTION_RECORD;
struct _CONTEXT;
struct _DISPATCHER_CONTEXT;

/* The language-specific handler of functions that use __try. */
_VCRTIMP EXCEPTION_DISPOSITION __cdecl
__C_specific_handler(struct _EXCEPTION_RECORD *ExceptionRecord,
                     void *EstablisherFrame, struct _CONTEXT *ContextRecord,
                     struct _DISPATCHER_CONTEXT *DispatcherContext);

unsigned long __cdecl _exception_code(void);
void *__cdecl _exception_info(void);
int __cdecl _abnormal_termination(void);

#define GetExceptionCode _exception_code
#define exception_code _exception_code
#define GetExceptionInformation()                                              \
  ((struct _EXCEPTION_POINTERS *)_exception_info())
#define exception_info() ((struct _EXCEPTION_POINTERS *)_exception_info())
#define AbnormalTermination _abnormal_termination
#define abnormal_termination _abnormal_termination

#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

_CRT_END_C_HEADER

#endif /* __CLANG_EXCPT_H */
