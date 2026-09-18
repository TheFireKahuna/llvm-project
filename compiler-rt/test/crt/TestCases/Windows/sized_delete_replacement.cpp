// An image that replaces only the unsized operator delete has every sized
// delete the compiler emits forward to that replacement, as the standard
// requires, although the shared C++ runtime defines the sized forms too.
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <malloc.h>
#include <new>
#include <stdlib.h>

static int Replaced;

void *operator new(size_t Size) { return malloc(Size); }
void *operator new[](size_t Size) { return malloc(Size); }
void *operator new(size_t Size, std::align_val_t Align) {
  return _aligned_malloc(Size, static_cast<size_t>(Align));
}
void *operator new[](size_t Size, std::align_val_t Align) {
  return _aligned_malloc(Size, static_cast<size_t>(Align));
}
void operator delete(void *Pointer) noexcept {
  ++Replaced;
  free(Pointer);
}
void operator delete[](void *Pointer) noexcept {
  ++Replaced;
  free(Pointer);
}
void operator delete(void *Pointer, std::align_val_t) noexcept {
  ++Replaced;
  _aligned_free(Pointer);
}
void operator delete[](void *Pointer, std::align_val_t) noexcept {
  ++Replaced;
  _aligned_free(Pointer);
}

// Non-trivial destructors give the arrays a cookie, so the compiler calls
// the sized array deletes.
struct Object {
  ~Object() {}
  int Value = 0;
};
struct alignas(64) Aligned {
  ~Aligned() {}
  int Value = 0;
};

int main() {
  delete new Object;
  assert(Replaced == 1);
  delete[] new Object[3];
  assert(Replaced == 2);
  delete new Aligned;
  assert(Replaced == 3);
  delete[] new Aligned[3];
  assert(Replaced == 4);
  return 0;
}
