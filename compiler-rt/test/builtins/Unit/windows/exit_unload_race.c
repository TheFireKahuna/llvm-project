// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe %t.dll | FileCheck %s

// The exit drain takes its references to a callback's image and its DSO's
// before it claims the entry, and leaves the entries of an image that is
// unloading to that image's detach: two threads draining every registration
// while a third loads and unloads a DLL whose initializer registers
// destructors never call into an unloaded image.

// CHECK: done

#include <stdio.h>
#include <windows.h>

void __cxa_finalize(void *);

#ifdef DLL
extern void *__dso_handle;
int __cxa_atexit(void (*)(void *), void *, void *);
static volatile long Counter;
static void bump(void *Unused) { InterlockedIncrement(&Counter); }

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_ATTACH)
    for (int I = 0; I != 64; ++I)
      __cxa_atexit(bump, NULL, &__dso_handle);
  return TRUE;
}
#else
static volatile long Stop;

static DWORD WINAPI drain(LPVOID Unused) {
  while (!Stop)
    __cxa_finalize(NULL);
  return 0;
}

int main(int argc, char **argv) {
  HANDLE Threads[2];
  for (int I = 0; I != 2; ++I)
    Threads[I] = CreateThread(NULL, 0, drain, NULL, 0, NULL);
  for (int I = 0; I != 2000; ++I) {
    HMODULE Module = LoadLibraryA(argv[1]);
    if (!Module)
      return 1;
    FreeLibrary(Module);
  }
  Stop = 1;
  WaitForMultipleObjects(2, Threads, TRUE, INFINITE);
  printf("done\n");
  return 0;
}
#endif
