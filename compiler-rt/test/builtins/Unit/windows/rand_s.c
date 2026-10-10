// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %clang_wincrt -static %s -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck %s

// rand_s draws varying values without loading advapi32.dll or msvcrt.dll,
// which the Universal CRT's rand_s loads, and a null argument goes to the
// invalid parameter handler and fails with EINVAL.

#define _CRT_RAND_S
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static void handler(const wchar_t *Expression, const wchar_t *Function,
                    const wchar_t *File, unsigned Line, uintptr_t Reserved) {
  (void)Expression, (void)Function, (void)File, (void)Line, (void)Reserved;
  printf("invalid parameter\n");
}

int main(void) {
  unsigned First, Next;
  int Varied = 0;
  if (rand_s(&First))
    return 1;
  for (int I = 0; I < 16 && !Varied; ++I)
    Varied = !rand_s(&Next) && Next != First;
  printf("varied %d\n", Varied);
  _set_invalid_parameter_handler(handler);
  errno = 0;
  int Error = rand_s(NULL);
  printf("null %d, errno %d\n", Error == EINVAL, errno == EINVAL);
  printf("advapi32.dll %d, msvcrt.dll %d\n",
         GetModuleHandleW(L"advapi32.dll") != NULL,
         GetModuleHandleW(L"msvcrt.dll") != NULL);
  return 0;
}

// CHECK:      varied 1
// CHECK-NEXT: invalid parameter
// CHECK-NEXT: null 1, errno 1
// CHECK-NEXT: advapi32.dll 0, msvcrt.dll 0
