// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe %t.dll | FileCheck %s

// Unloading a DLL runs only its own registrations, in reverse order, after
// its DllMain and before its terminators; the executable's run at exit.

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#ifdef DLL

static void dll1(void) { printf("dll atexit 1\n"); }
static void dll2(void) { printf("dll atexit 2\n"); }
static void terminate(void) { printf("dll terminator\n"); }

#  pragma section(".CRT$XTU", read)
__attribute__((used))
__declspec(allocate(".CRT$XTU")) static void (*const Terminator)(void) =
    terminate;

__declspec(dllexport) void registerInDll(void) {
  atexit(dll1);
  atexit(dll2);
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH)
    printf("dll detach\n");
  return TRUE;
}

#else

static void exe1(void) { printf("exe atexit 1\n"); }
static void exe2(void) { printf("exe atexit 2\n"); }

int main(int argc, char **argv) {
  atexit(exe1);
  HMODULE Module = LoadLibraryA(argv[1]);
  void (*Register)(void) =
      (void (*)(void))GetProcAddress(Module, "registerInDll");
  Register();
  atexit(exe2);
  FreeLibrary(Module);
  printf("unloaded\n");
  return 0;
}

#endif

// CHECK:      dll detach
// CHECK-NEXT: dll atexit 2
// CHECK-NEXT: dll atexit 1
// CHECK-NEXT: dll terminator
// CHECK-NEXT: unloaded
// CHECK-NEXT: exe atexit 2
// CHECK-NEXT: exe atexit 1
