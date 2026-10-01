//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-threads, c++03

// [basic.start.term] across images: a thread_local object that a DLL's code
// constructs on the main thread is destroyed at exit before the executable's
// objects of static storage duration, since both images register with the
// registries of the shared C++ runtime.

// RUN: %{cxx} %{flags} %{compile_flags} -shared -DDLL %s %{link_flags} -o %{temp}/local.dll
// RUN: %{cxx} %{flags} %{compile_flags} %s %{link_flags} %{temp}/local.dll.lib -o %{temp}/main.exe
// RUN: %{exec} %{temp}/main.exe

#include <cstdlib>

#ifdef DLL

static const bool* staticAlive;
static bool destroyed = false;

struct ThreadLocal {
  int value = 1;
  ~ThreadLocal() {
    if (!*staticAlive)
      std::abort();
    destroyed = true;
  }
};

static thread_local ThreadLocal local;

__attribute__((visibility("default"))) int useThreadLocal(const bool* alive) {
  staticAlive = alive;
  return local.value;
}

__attribute__((visibility("default"))) bool threadLocalDestroyed() { return destroyed; }

#else

int useThreadLocal(const bool* alive);
bool threadLocalDestroyed();

static bool alive = true;

struct Static {
  ~Static() {
    if (!threadLocalDestroyed())
      std::abort();
    alive = false;
  }
};

static Static object;

int main(int, char**) { return useThreadLocal(&alive) - 1; }

#endif
