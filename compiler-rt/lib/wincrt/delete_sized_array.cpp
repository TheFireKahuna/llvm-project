//===-- delete_sized_array.cpp - Sized delete forwarder -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The default sized operator delete forwards to the unsized one of the same
// image, so an image that replaces operator delete(void*) has every sized
// deletion the compiler emits reach its replacement. As in Microsoft's CRT the
// forwarders belong to the C runtime, one per translation unit so that an
// image defining a sized form itself never links the fallback; the shared C++
// runtime defines none of them on this target.
//
//===----------------------------------------------------------------------===//

#include <stddef.h>

void operator delete[](void *Pointer, size_t) noexcept {
  ::operator delete[](Pointer);
}
