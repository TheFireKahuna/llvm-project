//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-threads, c++03

// [basic.start.term]: the objects of thread storage duration of the thread
// that calls exit are destroyed before any object of static storage duration.
// Check it for the main thread of an executable that registers both with the
// C++ runtime.

#include <cstdlib>

static bool threadLocalDestroyed = false;
static bool staticDestroyed      = false;

struct Static {
  ~Static() {
    if (!threadLocalDestroyed)
      std::abort();
    staticDestroyed = true;
  }
};

static Static object;

struct ThreadLocal {
  int value = 1;
  ~ThreadLocal() {
    if (staticDestroyed)
      std::abort();
    threadLocalDestroyed = true;
  }
};

static thread_local ThreadLocal local;

int main(int, char**) { return local.value - 1; }
