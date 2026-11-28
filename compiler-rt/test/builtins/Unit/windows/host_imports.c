// RUN: %clang -shared -DMARKED %s -o %t-marked.dll
// RUN: %clang -shared -DDEEP %s -o %t-deep.dll
// RUN: %clang -shared -DRELAY --target=x86_64-pc-windows-msvc -fuse-ld=lld \
// RUN:   -nostdlib -Wl,-noentry,-implib:%t-relay.dll.lib %s %t-deep.dll.lib \
// RUN:   -o %t-relay.dll
// RUN: %clang -shared -DWEAK %s -o %t-weak.dll
// RUN: %clang -DHOST %s %t-weak.dll.lib %t-marked.dll.lib %t-relay.dll.lib \
// RUN:   -o %t.exe
// RUN: %clang -DHOST -DAPART %s -o %t-apart.exe
// RUN: %clang -shared %s %t.lib %t-apart.lib %t-weak.dll.lib -o %t.dll
// RUN: %clang -shared -DAPART %s %t-apart.lib -o %t-apart.dll
// RUN: %clang -shared -DLOOSE %s -Wl,-unresolved-host-imports -o %t-loose.dll
// RUN: cp %t.exe %t-renamed.exe && cp %t.dll %t-copy.dll
// RUN: %run %t-renamed.exe %t.exe %t.dll %t-apart.dll %t-weak.dll \
// RUN:   %t-copy.dll %t-loose.dll | FileCheck %s

// A DLL that imports from an executable binds what it imports in the
// executable that runs, whatever its file name: no import descriptor names
// the executable, and the DLL's fill writes each pointer by name before the
// DLL's code runs. A renamed executable binds the DLL to itself though the
// file that the import library names lies beside it, which is never loaded.
// A word of the DLL's constant data that holds a host function's address is
// written by the fill too. The DLL's guarded indirect call through such an
// address reaches the executable's function. Where the executable does not
// export a name that the DLL imports from it, the DLL fails to load, unless
// only weak references name it, which are then null. A DLL that the
// executable imports and that this toolchain made, whose header names an
// offer table, which is empty in a DLL with no vague entity, exports such a
// name in its place, and so does one that only a foreign DLL imports.
//
// An image whose only references to a DLL's function are weak does not load
// the DLL: the function's address is null until a load of the DLL precedes
// the image's. A weak reference that a DLL's link leaves undefined under
// -unresolved-host-imports is a weak host import: the executable's function
// where it exports the name, and null where nothing does.

// CHECK:      exe: weak 0, found 9, deep 5
// CHECK-NEXT: plugin: counter 2, same address 1, word 1, original loaded 0, weak 0, absent 1, found 9, deep 5
// CHECK-NEXT: apart: loaded 0, error 1114
// CHECK-NEXT: copy: weak 7
// CHECK-NEXT: loose: present 1, absent 1

#ifndef RELAY
#include <stdio.h>
#include <windows.h>
#endif

#ifdef WEAK
__declspec(dllexport) int weak_fn(void) { return 7; }
#elif defined(MARKED)
__declspec(dllexport) int host_found(void) { return 9; }
#elif defined(DEEP)
__declspec(dllexport) int host_deep(void) { return 5; }
#elif defined(RELAY)
int host_deep(void);
__declspec(dllexport) int relay(void) { return host_deep(); }
#elif defined(LOOSE)
__attribute__((weak)) void host_fn(void);
__attribute__((weak)) void host_none(void);
__declspec(dllexport) void *loose_run(int *absent) {
  *absent = !&host_none;
  return (void *)&host_fn;
}
#else
__attribute__((weak)) int weak_fn(void);
static int weak_state(void) { return &weak_fn ? weak_fn() : 0; }

#  ifdef HOST
__declspec(dllexport) int host_counter;
__declspec(dllexport) void host_fn(void) {}
// The executable never takes its address.
__declspec(dllexport) void host_target(void) { ++host_counter; }
#    ifdef APART
__declspec(dllexport) void host_apart(void) {}
__declspec(dllexport) int host_found(void) { return 0; }
__declspec(dllexport) int host_deep(void) { return 0; }
int relay(void) { return 0; }
#    else
int host_found(void);
int relay(void);
#    endif

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("exe: weak %d, found %d, deep %d\n", weak_state(), host_found(),
         relay());
  HMODULE plugin = LoadLibraryA(argv[2]);
  if (!plugin)
    return 1;
  void *(*run)(int *) = (void *(*)(int *))GetProcAddress(plugin, "plugin_run");
  int state[5];
  void *address = run(state);
  const char *original = argv[1];
  for (const char *c = argv[1]; *c; ++c)
    if (*c == '\\' || *c == '/')
      original = c + 1;
  printf("plugin: counter %d, same address %d, word %d, original loaded %d, "
         "weak %d, absent %d, found %d, deep %d\n",
         host_counter, address == (void *)host_fn, state[0],
         GetModuleHandleA(original) != NULL, state[1], state[2], state[3],
         state[4]);
  HMODULE apart = LoadLibraryA(argv[3]);
  printf("apart: loaded %d, error %lu\n", apart != NULL, GetLastError());
  LoadLibraryA(argv[4]);
  run = (void *(*)(int *))GetProcAddress(LoadLibraryA(argv[5]), "plugin_run");
  run(state);
  printf("copy: weak %d\n", state[1]);
  void *(*loose)(int *) =
      (void *(*)(int *))GetProcAddress(LoadLibraryA(argv[6]), "loose_run");
  address = loose(state);
  printf("loose: present %d, absent %d\n", address == (void *)host_fn,
         state[0]);
  return 0;
}
#  else
extern int host_counter;
void host_fn(void);
void host_target(void);
#    ifdef APART
void host_apart(void);
__declspec(dllexport) void plugin_apart(void) { host_apart(); }
#    else
__attribute__((weak)) void host_apart(void);
int host_found(void);
int host_deep(void);
#    endif

static void (*const hook)(void) = host_fn;
static void (*volatile call)(void) = host_target;

// The word of constant data, whether the weak import is there and what it
// returns, whether the weak host import that the executable lacks is null,
// and what the marked DLL's function returns.
__declspec(dllexport) void *plugin_run(int *state) {
  ++host_counter;
  call();
  state[0] = *(void (*const volatile *)(void))&hook == host_fn;
  state[1] = weak_state();
  state[2] = !&host_apart;
#    ifndef APART
  state[3] = host_found();
  state[4] = host_deep();
#    endif
  return (void *)host_fn;
}
#  endif
#endif
