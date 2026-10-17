//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: no-exceptions, c++03

// [basic.start.term]: a destructor of an object of static storage duration
// that exits by an exception calls std::terminate, and so calls the
// program's terminate handler. The process's termination registries, which
// call the destructor, are in a DLL that links no C++ runtime.

#include <cstdlib>
#include <exception>

struct Throws {
  ~Throws() noexcept(false) { throw 1; }
};

static Throws object;

int main(int, char**) {
  std::set_terminate([] { std::_Exit(0); });
  return 1;
}
