//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-threads, c++03

// The C runtime's free releases a block of the over-aligned operator new, and a
// thread_local object's destructor runs when its thread exits.

#include <cassert>
#include <cstdlib>
#include <new>
#include <thread>

static int destroyed = 0;

struct ThreadLocal {
  ~ThreadLocal() { ++destroyed; }
};
static thread_local ThreadLocal tls;

int main(int, char**) {
  void* aligned = ::operator new(1, std::align_val_t(64));
  assert(reinterpret_cast<std::size_t>(aligned) % 64 == 0);
  std::free(aligned);
  std::thread([] { (void)&tls; }).join();
  assert(destroyed == 1);
  return 0;
}
