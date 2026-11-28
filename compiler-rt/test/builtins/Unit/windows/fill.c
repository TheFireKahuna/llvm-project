// RUN: %clang -shared -DOTHER %s -o %t-other.dll
// RUN: %clang -shared -DDLL %s %t-other.dll.lib -o %t.dll
// RUN: %clang -shared -DDLL -DPLAIN %s %t-other.dll.lib -o %t-plain.dll
// RUN: %clang %s %t-other.dll.lib -o %t.exe
// RUN: %run %t.exe %t.dll %t-plain.dll | FileCheck %s

// A constant that holds an address inside another DLL's array, which that
// DLL exports no name for, is written by the image's fill before any
// initializer of the image runs, in a section that is read-only before and
// after. The loader reaches the fill through the image's entry point, which
// passes every notification on to the DLL's own entry; for the reason 'FILL'
// it runs the fill again, which finds every word written and changes none.
// An image with no such constant has no sealed section, and an executable
// has its constant written before main. The section begins with a word that
// the fill sets once it has written.
//
// The fill also writes an image's sealed values: __imp_X$val with the word
// that import X holds, and __imp_X$init with what X returns for the image's
// base, calling it once to write and once more to check what it wrote. Where
// X returns zero the image does not start: a DLL fails to load, its entry
// seeing neither the attach nor the detach that the loader sends after it,
// and an executable exits with the fill's status.
//
// A thread-local pointer to such an address is right in the thread that
// loads the DLL, whose copy the fill writes, and in a thread that starts
// later, which copies the template.

// CHECK:      exe: 6 at offset 5, sealed 1, protection 2, value 42, init 1, calls 2
// CHECK-NEXT: fails: 1, error 1114, calls 3
// CHECK-NEXT: exits: c0000142
// CHECK-NEXT: process attach, constructed with 1
// CHECK-NEXT: plain: sealed -1
// CHECK-NEXT: process attach, constructed with 4, value 42, init 1
// CHECK-NEXT: dll: 4 at offset 3, sealed 1, protection 2, calls 5
// CHECK-NEXT: thread-local: 3 here, 3 in a new thread
// CHECK-NEXT: again: 1, 4 at offset 3, protection 2, calls 6
// CHECK-NEXT: process detach
// CHECK-NEXT: process detach

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifdef OTHER
__declspec(dllexport) int other_dll_array[8] = {1, 2, 3, 4, 5, 6, 7, 8};
__declspec(dllexport) uintptr_t other_value = 42;
__declspec(dllexport) int other_calls;
__declspec(dllexport) uintptr_t other_init(void *image) {
  ++other_calls;
  return GetEnvironmentVariableA("FILL_FAIL", NULL, 0) ? 0
                                                       : (uintptr_t)image + 1;
}
#else
extern int other_dll_array[];
// The words are the image's own, which code reads where they are.
#pragma GCC visibility push(hidden)
extern const uintptr_t sealed_value __asm__("__imp_other_value$val");
extern const uintptr_t sealed_init __asm__("__imp_other_init$init");
#pragma GCC visibility pop
#endif

#ifdef DLL
#ifdef PLAIN
const int *const p = other_dll_array;
#else
const int *const p = &other_dll_array[3];
#endif
__declspec(dllexport) const int *const *get(void) { return &p; }
#ifndef PLAIN
static __thread const int *thread_word = &other_dll_array[2];
__declspec(dllexport) DWORD WINAPI thread_value(void *unused) {
  return *thread_word;
}
#endif

// The word is read where it is, not folded into the code that reads it.
static const int *const *volatile word = &p;
static int constructed;
__attribute__((constructor)) static void construct(void) {
  constructed = **word;
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, void *reserved) {
  if (reason == DLL_PROCESS_ATTACH)
    printf("process attach, constructed with %d", constructed);
#ifndef PLAIN
  if (reason == DLL_PROCESS_ATTACH)
    printf(", value %d, init %d", (int)sealed_value,
           sealed_init == (uintptr_t)dll + 1);
#endif
  if (reason == DLL_PROCESS_ATTACH)
    printf("\n");
  if (reason == DLL_PROCESS_DETACH)
    printf("process detach\n");
  fflush(stdout);
  return TRUE;
}
#elif !defined(OTHER)
__declspec(dllimport) extern int other_calls;
const int *const q = &other_dll_array[5];
static const int *const *volatile word = &q;

static IMAGE_NT_HEADERS *headers(HMODULE module) {
  char *base = (char *)module;
  return (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
}

// The word that the sealed section begins with, which the fill sets once
// it has written, or -1 for an image that has no such section.
static int sealed(HMODULE module) {
  IMAGE_NT_HEADERS *nt = headers(module);
  IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i)
    if (!memcmp(section[i].Name, ".sealed", 8))
      return *(const int *)((char *)module + section[i].VirtualAddress);
  return -1;
}

static DWORD protection(const void *address) {
  MEMORY_BASIC_INFORMATION info;
  VirtualQuery(address, &info, sizeof(info));
  return info.Protect;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const int *const *p = word;
  HMODULE exe = GetModuleHandleA(NULL);
  printf("exe: %d at offset %d, sealed %d, protection %lu, value %d, init %d, "
         "calls %d\n",
         **p, (int)(*p - other_dll_array), sealed(exe), protection(p),
         (int)sealed_value, sealed_init == (uintptr_t)exe + 1, other_calls);
  SetEnvironmentVariableA("FILL_FAIL", "1");
  HMODULE failed = LoadLibraryA(argv[1]);
  printf("fails: %d, error %lu, calls %d\n", !failed, GetLastError(),
         other_calls);
  STARTUPINFOA startup = {sizeof(startup)};
  PROCESS_INFORMATION child;
  DWORD status = 0;
  if (CreateProcessA(argv[0], NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup,
                     &child)) {
    WaitForSingleObject(child.hProcess, INFINITE);
    GetExitCodeProcess(child.hProcess, &status);
  }
  printf("exits: %lx\n", status);
  SetEnvironmentVariableA("FILL_FAIL", NULL);
  HMODULE plain = LoadLibraryA(argv[2]);
  printf("plain: sealed %d\n", sealed(plain));
  HMODULE dll = LoadLibraryA(argv[1]);
  if (!dll)
    return 1;
  p = ((const int *const *(*)(void))GetProcAddress(dll, "get"))();
  printf("dll: %d at offset %d, sealed %d, protection %lu, calls %d\n", **p,
         (int)(*p - other_dll_array), sealed(dll), protection(p), other_calls);
  LPTHREAD_START_ROUTINE thread_value =
      (LPTHREAD_START_ROUTINE)GetProcAddress(dll, "thread_value");
  HANDLE thread = CreateThread(NULL, 0, thread_value, NULL, 0, NULL);
  WaitForSingleObject(thread, INFINITE);
  GetExitCodeThread(thread, &status);
  printf("thread-local: %lu here, %lu in a new thread\n", thread_value(NULL),
         status);
  BOOL(WINAPI * entry)(HMODULE, DWORD, void *) =
      (BOOL(WINAPI *)(HMODULE, DWORD, void *))(
          (char *)dll + headers(dll)->OptionalHeader.AddressOfEntryPoint);
  BOOL again = entry(dll, 0x4C4C4946, NULL);
  printf("again: %d, %d at offset %d, protection %lu, calls %d\n", again, **p,
         (int)(*p - other_dll_array), protection(p), other_calls);
  FreeLibrary(dll);
  return 0;
}
#endif
