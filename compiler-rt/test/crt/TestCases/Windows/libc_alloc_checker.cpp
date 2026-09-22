// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG -DLIBC_NAMESPACE=__llvm_libc -I%S/../../../../../libc %s %S/../../../../../libc/src/__support/CPP/new.cpp -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include "src/__support/alloc-checker.h"
#include <assert.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>

int main() {
  constexpr size_t Size = 1024;
  constexpr std::align_val_t Align = std::align_val_t(64);
  for (unsigned kind = 0; kind != 4; ++kind) {
    __llvm_libc::AllocChecker ac;
    void *p = __llvm_libc::AllocChecker::aligned_alloc(Size, Align, ac);
    assert(ac && p && reinterpret_cast<uintptr_t>(p) % 64 == 0);
    assert(_msize(p) >= Size);
    memset(p, 0x5a, Size);
    switch (kind) {
    case 0:
      ::operator delete(p, Align);
      break;
    case 1:
      ::operator delete(p, Size, Align);
      break;
    case 2:
      ::operator delete[](p, Align);
      break;
    case 3:
      ::operator delete[](p, Size, Align);
      break;
    }
  }
}
