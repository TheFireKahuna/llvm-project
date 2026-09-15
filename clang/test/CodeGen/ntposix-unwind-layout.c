// RUN: %clang --target=x86_64-pc-windows-ntposix -ffreestanding -fexceptions -fsyntax-only %s
// RUN: %clang --target=aarch64-pc-windows-ntposix -ffreestanding -fexceptions -fsyntax-only %s

#ifndef __SEH__
#error ntposix must select the SEH exception ABI by default
#endif

typedef __UINT64_TYPE__ uint64_t;
typedef __UINTPTR_TYPE__ uintptr_t;
#define offsetof(T, F) __builtin_offsetof(T, F)
typedef int _Unwind_Reason_Code;
typedef int _Unwind_Action;
#include "../../../libunwind/include/unwind_itanium.h"

_Static_assert(sizeof(_Unwind_Exception) == 64, "SEH exception extent");
_Static_assert(_Alignof(_Unwind_Exception) == 16, "exception alignment");
_Static_assert(offsetof(_Unwind_Exception, exception_class) == 0, "class");
_Static_assert(offsetof(_Unwind_Exception, exception_cleanup) == 8, "cleanup");
_Static_assert(offsetof(_Unwind_Exception, private_) == 16, "private words");
_Static_assert(sizeof(((_Unwind_Exception *)0)->private_) == 48, "six words");
