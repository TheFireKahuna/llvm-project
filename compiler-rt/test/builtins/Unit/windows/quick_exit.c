// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt -shared -DFOREIGN_DLL -nostartfiles -fno-stack-protector \
// RUN:     -mguard=none -Wl,-entry:foreignEntry %s -o %t-foreign.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe
// RUN: %run %t.exe quick_exit | FileCheck %s --check-prefixes=QUICK,MODULES
// RUN: %run %t.exe dll | FileCheck %s --check-prefixes=QUICK,MODULES
// RUN: %run %t.exe foreign %t-foreign.dll | FileCheck %s --check-prefix=QUICK
// RUN: %run %t.exe _Exit | FileCheck %s --check-prefix=EXIT
// RUN: %run %t.exe _exit | FileCheck %s --check-prefix=EXIT
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: quick_exit '
// RUN: llvm-readobj --coff-imports %t.dll | FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: quick_exit '
// RUN: %clang_wincrt -static %s %t.dll.lib -o %t-static.exe
// RUN: %run %t-static.exe quick_exit | FileCheck %s \
// RUN:     --check-prefixes=QUICK,MODULES
// RUN: %run %t-static.exe dll | FileCheck %s --check-prefixes=QUICK,MODULES
// RUN: %clang_wincrt -DHOST -nostartfiles -fno-stack-protector -mguard=none \
// RUN:     -Wl,-entry:hostEntry -Wl,-subsystem:console %s %t.dll.lib -o %t-host.exe
// RUN: %run %t-host.exe | FileCheck %s --check-prefix=HOST

// quick_exit runs the at_quick_exit registrations of every image in reverse
// order and no atexit one; _Exit and _exit run neither. Every image's
// quick_exit is wincrt's, which under a Windows Itanium executable ends the
// process without loading kernel.appcore.dll or msvcrt.dll, as the Universal
// CRT's does. A foreign DLL's call to the Universal CRT's quick_exit still
// runs them, through the executable's token in the Universal CRT's table.
// Under a foreign executable, quick_exit is the Universal CRT's, which runs
// the executable's registrations with the DLL's.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static void report(const char *Text) {
  printf("%s\n", Text);
  fflush(stdout);
}

#if defined(DLL)

static void dllQuick(void) { report("at_quick_exit dll"); }

__declspec(dllexport) void dllRegister(void) { at_quick_exit(dllQuick); }
__declspec(dllexport) void dllCallsQuickExit(void) { quick_exit(0); }

// The detach runs after quick_exit has ended the process.
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH) {
    printf("kernel.appcore.dll %d, msvcrt.dll %d\n",
           GetModuleHandleW(L"kernel.appcore.dll") != NULL,
           GetModuleHandleW(L"msvcrt.dll") != NULL);
    fflush(stdout);
  }
  return TRUE;
}

#elif defined(FOREIGN_DLL)

// A DLL with no wincrt start-up, which calls the Universal CRT's quick_exit.
__declspec(dllexport) void foreignCallsQuickExit(void) { quick_exit(0); }

BOOL WINAPI foreignEntry(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  return TRUE;
}

#elif defined(HOST)

#include <corecrt_startup.h>

// An executable with no wincrt start-up, which reserves no exit list. It
// registers as a Visual C++ executable's start-up does.
__declspec(dllimport) void dllRegister(void);
__declspec(dllimport) void dllCallsQuickExit(void);

static void hostQuick(void) { report("at_quick_exit host"); }

int hostEntry(void) {
  _crt_at_quick_exit(hostQuick);
  dllRegister();
  dllCallsQuickExit();
  return 1;
}

#else

__declspec(dllimport) void dllRegister(void);
__declspec(dllimport) void dllCallsQuickExit(void);

static void atexitFunction(void) { report("atexit"); }
static void quick1(void) { report("at_quick_exit 1"); }
static void quick2(void) { report("at_quick_exit 2"); }

int main(int argc, char **argv) {
  atexit(atexitFunction);
  at_quick_exit(quick2);
  dllRegister();
  at_quick_exit(quick1);
  if (!strcmp(argv[1], "_Exit"))
    _Exit(0);
  if (!strcmp(argv[1], "_exit"))
    _exit(0);
  if (!strcmp(argv[1], "dll"))
    dllCallsQuickExit();
  if (!strcmp(argv[1], "foreign")) {
    HMODULE Foreign = LoadLibraryA(argv[2]);
    void (*Call)(void) =
        (void (*)(void))GetProcAddress(Foreign, "foreignCallsQuickExit");
    Call();
  }
  quick_exit(0);
}

#endif

// QUICK:        at_quick_exit 1
// QUICK-NEXT:   at_quick_exit dll
// QUICK-NEXT:   at_quick_exit 2
// QUICK-NOT:    atexit
// MODULES:      kernel.appcore.dll 0, msvcrt.dll 0

// EXIT-NOT: at_quick_exit
// EXIT-NOT: atexit

// IMPORT: Name: clang_rt.wincrt_dynamic.dll
// IMPORT: Symbol: __wrap_quick_exit

// HOST:      at_quick_exit dll
// HOST-NEXT: at_quick_exit host
