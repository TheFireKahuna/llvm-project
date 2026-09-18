// Unloading an image releases everything it registered. Thousands of
// load/register/unload cycles must leave the process's memory flat, and a
// live image that finalizes itself and registers again must reuse its slots.
// RUN: %clangxx_crt_dll -std=c++17 -O2 -UNDEBUG -DBUILD_DLL %s -o %t.dll
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe %t.dll
// REQUIRES: windows, crt

#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <windows.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void __cxa_finalize(void *);
extern "C" void *__dso_handle;

#if defined(BUILD_DLL)
static long destroyed;
static void destroy(void *) { ++destroyed; }
struct Object {
  ~Object() { ++destroyed; }
};
extern "C" __declspec(dllexport) long touch(int count) {
  static Object object;
  for (int i = 0; i < count; ++i)
    assert(__cxa_atexit(destroy, nullptr, __dso_handle) == 0);
  return destroyed;
}
#else
#  include <psapi.h>

static SIZE_T privateBytes() {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  assert(K32GetProcessMemoryInfo(
      GetCurrentProcess(),
      reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
      sizeof(counters)));
  return counters.PrivateUsage;
}

static long unloadCycles(const char *path, int cycles, int perCycle) {
  long total = 0;
  for (int i = 0; i < cycles; ++i) {
    HMODULE module = LoadLibraryA(path);
    assert(module);
    auto touch =
        reinterpret_cast<long (*)(int)>(GetProcAddress(module, "touch"));
    assert(touch);
    touch(perCycle);
    assert(FreeLibrary(module));
    assert(!GetModuleHandleA(path));
    total += perCycle + 1;
  }
  return total;
}

static void noop(void *) {}

int main(int argc, char **argv) {
  assert(argc == 2);
  // The first phase absorbs one-time growth in the loader and heap; the
  // second, identical phase must then be flat. 250,000 registrations each.
  unloadCycles(argv[1], 2500, 100);
  SIZE_T afterFirst = privateBytes();
  unloadCycles(argv[1], 2500, 100);
  SIZE_T afterSecond = privateBytes();
  assert(afterSecond < afterFirst + 64 * 1024);
  // A live image reusing its own slots: 200,000 registrations.
  SIZE_T beforeReuse = privateBytes();
  for (int i = 0; i < 2000; ++i) {
    for (int j = 0; j < 100; ++j)
      assert(__cxa_atexit(noop, nullptr, __dso_handle) == 0);
    __cxa_finalize(__dso_handle);
  }
  assert(privateBytes() < beforeReuse + 64 * 1024);
  return 0;
}
#endif
