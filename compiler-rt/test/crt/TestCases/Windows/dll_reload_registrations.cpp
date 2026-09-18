// Repeated unload/reload must not confuse registrations at reused image addresses.
// RUN: %clangxx_crt_dll -std=c++17 -O2 -UNDEBUG -DBUILD_DLL %s -o %t.dll
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe %t.dll normal
// RUN: %run %t.exe %t.dll quick
// RUN: %clangxx_crt_dll -std=c++17 -O2 -UNDEBUG -DBUILD_DLL -DREJECT_ATTACH %s -o %t.reject.dll
// RUN: %run %t.exe %t.reject.dll reject 2>&1 | FileCheck %s --check-prefix=REJECT
// REQUIRES: windows, crt

#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

struct State {
  int constructed, destroyed, normal, quick;
};
#ifdef BUILD_DLL
static State *state;
static void normal() { ++state->normal; }
static void quick() { ++state->quick; }
struct Object {
  Object() { ++state->constructed; }
  ~Object() { ++state->destroyed; }
};
extern "C" __declspec(dllexport) void install(State *value) {
  state = value;
  static Object object;
  assert(atexit(normal) == 0 && at_quick_exit(quick) == 0);
}
#  ifdef REJECT_ATTACH
static struct Startup {
  Startup() { fprintf(stderr, "rejected DLL constructed\n"); }
  ~Startup() { fprintf(stderr, "rejected DLL destroyed\n"); }
} startup;
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void *) {
  return reason != DLL_PROCESS_ATTACH;
}
#  endif

// REJECT: rejected DLL constructed
// REJECT-NEXT: rejected DLL destroyed
// REJECT-NOT: rejected DLL
#else
static State states[64];
static void verify() {
  for (const auto &state : states)
    assert(state.constructed == 1 && state.destroyed == 1 &&
           state.normal == 1 && state.quick == 0);
}
int main(int argc, char **argv) {
  assert(argc == 3);
  if (argv[2][0] == 'r') {
    assert(!LoadLibraryA(argv[1]));
    assert(!GetModuleHandleA(argv[1]));
    return 0;
  }
  assert(atexit(verify) == 0 && at_quick_exit(verify) == 0);
  for (auto &state : states) {
    HMODULE module = LoadLibraryA(argv[1]);
    assert(module);
    auto install =
        reinterpret_cast<void (*)(State *)>(GetProcAddress(module, "install"));
    assert(install);
    install(&state);
    assert(FreeLibrary(module));
    assert(!GetModuleHandleA(argv[1]));
    assert(state.constructed == 1 && state.destroyed == 1 && state.normal == 1);
  }
  if (argv[2][0] == 'q')
    quick_exit(0);
}
#endif
