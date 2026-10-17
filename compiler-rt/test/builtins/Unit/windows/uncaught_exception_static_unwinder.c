// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// An Itanium exception that no frame handles resumes its raise in the
// unhandled-exception filter, so that _Unwind_RaiseException returns and
// __cxa_throw can call std::terminate, also when the unwinder is linked into
// the executable and exports nothing. The unwinder here is a stand-in that
// raises, and names its raise functions, as libunwind does.

#include <stdio.h>
#include <windows.h>

__attribute__((visibility("hidden"), noinline)) int
_Unwind_RaiseException(void *Object) {
  EXCEPTION_RECORD Record = {0};
  Record.ExceptionCode = 0x20474343; // "GCC"
  Record.NumberParameters = 1;
  Record.ExceptionInformation[0] = (ULONG_PTR)Object;
  RtlRaiseException(&Record);
  return 5; // _URC_END_OF_STACK
}

__attribute__((visibility("hidden"))) const void *const
    __unw_seh_raise_functions[2] = {_Unwind_RaiseException,
                                    _Unwind_RaiseException};

int main(void) {
  static int Exception;
  printf("raise returned %d\n", _Unwind_RaiseException(&Exception));
  return 0;
}

// CHECK: raise returned 5
