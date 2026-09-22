// Exercise the production startup gate and the same inline observation from a
// DLL. Removing the manifest from a copy tests rejection without changing
// machine policy or writing to the PEB.
// RUN: %clangxx_crt_dll -DBUILD_DLL -O2 %s -o %t.dll
// RUN: %clangxx_crt_main -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe %t.dll
// RUN: %python %S/Inputs/segment_heap_without_manifest.py %t.exe %t.legacy.exe %t.dll
// REQUIRES: windows, crt

#include "../../../../lib/wincrt/heap.h"
#include <assert.h>
#include <malloc.h>
#include <windows.h>

#ifdef BUILD_DLL
static void *const EarlyHeap = wincrt::processHeap();
extern "C" __declspec(dllexport) void *heap_from_dll() {
  assert(EarlyHeap == wincrt::processHeap());
  return EarlyHeap;
}
#else
int main(int argc, char **argv) {
  assert(argc == 2);
  void *Heap = wincrt::processHeap();
  assert(Heap == GetProcessHeap());
  assert(Heap == reinterpret_cast<void *>(_get_heap_handle()));
  assert(wincrt::isSegmentHeap(Heap));
  HMODULE Dll = LoadLibraryA(argv[1]);
  assert(Dll);
  auto Query =
      reinterpret_cast<void *(*)()>(GetProcAddress(Dll, "heap_from_dll"));
  assert(Query && Query() == Heap);
  assert(FreeLibrary(Dll));
  return 0;
}
#endif
