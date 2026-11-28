// RUN: %clang -shared -DDLL %s -o %t.dll
// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe atexit | FileCheck %s --check-prefix=ATEXIT
// RUN: %run %t.exe dll %t.dll | FileCheck %s --check-prefixes=DLL,ATEXIT
// RUN: env WINCRT_RAISE_IN_DLLMAIN=1 %run %t.exe dll %t.dll | FileCheck %s --check-prefixes=DLLMAIN,DLL,ATEXIT
// RUN: %run %t.exe filter | FileCheck %s --check-prefix=FILTER
// RUN: %clang -DTABLE %s -o %t-table.exe
// RUN: %run %t-table.exe filter | FileCheck %s --check-prefix=FILTER
// RUN: %clang -static %s -o %t-static.exe
// RUN: %run %t-static.exe atexit | FileCheck %s --check-prefix=ATEXIT

// An Itanium exception must not pass DllMain, whose caller the loader makes
// take every exception, nor a function registered to run at termination.
// [ATEXIT] [DLL] [DLLMAIN] The frame around each, in an image and in the
// registries' DLL alike, resumes the unwinder's raise, whose return makes the
// thrower's runtime terminate with nothing unwound. [FILTER] The executable's
// unhandled-exception filter resumes a raise no frame took, which it knows by
// the function that made it, one the unwinder's table names or one the image
// exports; it passes every other exception to the program's own filter. The
// unwinder is a stand-in that raises as libunwind does.

// DLLMAIN:     raise returned 5 to DllMain
// DLL:         loaded
// ATEXIT:      raise returned 5
// DLL-NEXT:    unloaded
// FILTER:      raise returned 5
// FILTER-NEXT: the program's filter takes 0x20474343

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef TABLE
__attribute__((visibility("hidden")))
#else
__declspec(dllexport)
#endif
__attribute__((noinline)) int _Unwind_RaiseException(void *Object) {
  EXCEPTION_RECORD Record = {0};
  Record.ExceptionCode = 0x20474343; // "GCC"
  Record.NumberParameters = 1;
  Record.ExceptionInformation[0] = (ULONG_PTR)Object;
  RtlRaiseException(&Record);
  return 5; // _URC_END_OF_STACK
}

#ifdef TABLE
// libunwind's table names the bodies that raise, the entries passed the
// thread pointer.
int raiseBody(void *) __asm__("_Unwind_RaiseException$tp");
__attribute__((visibility("hidden"))) const void *const __unw_seh_raise_functions[2] = {
    raiseBody, raiseBody};
#endif

static int Exception;
static void destructor(void) {
  printf("raise returned %d\n", _Unwind_RaiseException(&Exception));
}

#ifdef DLL

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_ATTACH) {
    if (getenv("WINCRT_RAISE_IN_DLLMAIN"))
      printf("raise returned %d to DllMain\n", _Unwind_RaiseException(&Exception));
    atexit(destructor);
  }
  return TRUE;
}

#else

static LONG WINAPI filter(EXCEPTION_POINTERS *Pointers) {
  printf("the program's filter takes %#lx\n", Pointers->ExceptionRecord->ExceptionCode);
  exit(0);
}

int main(int argc, char **argv) {
  if (!strcmp(argv[1], "atexit"))
    return atexit(destructor);
  if (!strcmp(argv[1], "dll")) {
    HMODULE Module = LoadLibraryA(argv[2]);
    printf(Module ? "loaded\n" : "load failed\n");
    FreeLibrary(Module);
    printf("unloaded\n");
    return 0;
  }
  SetUnhandledExceptionFilter(filter);
  destructor();
  // The unwinder's code, raised by another function.
  ULONG_PTR Argument = (ULONG_PTR)&Exception;
  RaiseException(0x20474343, 0, 1, &Argument);
  return 1;
}

#endif
