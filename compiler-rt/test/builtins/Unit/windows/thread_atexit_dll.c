// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe %t.dll | FileCheck %s

// A thread-local destructor keeps the DLL that registered it loaded until it
// has run, even after the DLL's last FreeLibrary.

#include <stdio.h>
#include <windows.h>

#ifdef DLL

int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern void *__dso_handle;

static void destroy(void *Object) {
  (void)Object;
  printf("destructor in the DLL\n");
  fflush(stdout);
}

__declspec(dllexport) void registerInDll(void) {
  __cxa_thread_atexit_impl(destroy, NULL, &__dso_handle);
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH) {
    printf("dll detach\n");
    fflush(stdout);
  }
  return TRUE;
}

#else

static HANDLE Registered, Unloaded;

static DWORD WINAPI thread(LPVOID Module) {
  void (*Register)(void) =
      (void (*)(void))GetProcAddress((HMODULE)Module, "registerInDll");
  Register();
  SetEvent(Registered);
  WaitForSingleObject(Unloaded, INFINITE);
  return 0;
}

int main(int argc, char **argv) {
  HMODULE Module = LoadLibraryA(argv[1]);
  Registered = CreateEventW(NULL, TRUE, FALSE, NULL);
  Unloaded = CreateEventW(NULL, TRUE, FALSE, NULL);
  HANDLE Thread = CreateThread(NULL, 0, thread, Module, 0, NULL);
  WaitForSingleObject(Registered, INFINITE);
  FreeLibrary(Module);
  printf("freed, loaded %d\n", GetModuleHandleA(argv[1]) != NULL);
  fflush(stdout);
  SetEvent(Unloaded);
  WaitForSingleObject(Thread, INFINITE);
  // The thread pool drops the last reference.
  for (int I = 0; I < 500 && GetModuleHandleA(argv[1]); ++I)
    Sleep(10);
  printf("unloaded %d\n", GetModuleHandleA(argv[1]) == NULL);
  return 0;
}

#endif

// CHECK:      freed, loaded 1
// CHECK-NEXT: destructor in the DLL
// CHECK-NEXT: dll detach
// CHECK-NEXT: unloaded 1
