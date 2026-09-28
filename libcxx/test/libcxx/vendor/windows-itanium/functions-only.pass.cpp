//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-threads, c++03

// A program that reaches the C++ runtime only through functions: the global
// allocation functions, a thread, and a thread_local object whose destructor
// runs when the thread exits. It needs the runtime's exports and its image
// start-up, registries and TLS callbacks, and none of its data.

#include <cassert>
#include <cstddef>
#include <new>
#include <thread>

struct Tracked {
  int value;
  explicit Tracked(int v) : value(v) {}
};

static int destroyed = 0;

struct ThreadLocal {
  int value = 7;
  ~ThreadLocal() { destroyed += value; }
};

static thread_local ThreadLocal tls;

int main(int, char**) {
  Tracked* single = new Tracked(42);
  assert(single->value == 42);
  delete single;

  Tracked* array = new Tracked[3]{Tracked(1), Tracked(2), Tracked(3)};
  assert(array[2].value == 3);
  delete[] array;

  int* nothrow = new (std::nothrow_t{}) int(123);
  assert(nothrow != nullptr && *nothrow == 123);
  delete nothrow;

  struct alignas(64) Aligned {
    char bytes[64];
  };
  Aligned* aligned = new Aligned;
  assert(reinterpret_cast<std::size_t>(aligned) % 64 == 0);
  delete aligned;

  std::thread t([] { assert(tls.value == 7); });
  t.join();
  assert(destroyed == 7);

  return 0;
}
