//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: c++03, c++11, c++14, no-exceptions

// A DLL may be loaded by an executable that the segment heap's manifest did
// not start, here rundll32.exe on the NT heap, which aligns no block beyond
// 16 bytes by its size. Aligned operator new still returns a block of every
// alignment there, rather than throwing bad_alloc.

// RUN: %{cxx} %{flags} %{compile_flags} -shared %s -o %t.dll %{link_flags}
// RUN: %{exec} rundll32.exe %t.dll,probe

#include <cstddef>
#include <cstdint>
#include <new>
#include <windows.h>

extern "C" __attribute__((visibility("default"))) void CALLBACK
probe(HWND, HINSTANCE, LPSTR, int) {
  for (std::size_t alignment = 32; alignment <= 4096; alignment *= 2) {
    try {
      void* p = ::operator new(alignment * 3, std::align_val_t{alignment});
      bool aligned = reinterpret_cast<std::uintptr_t>(p) % alignment == 0;
      ::operator delete(p, std::align_val_t{alignment});
      if (!aligned)
        ExitProcess(2);
    } catch (const std::bad_alloc&) {
      ExitProcess(1);
    }
  }
  ExitProcess(0);
}
