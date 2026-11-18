//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// make_shared and allocate_shared of an unbounded array cast storage to a
// polymorphic type before an object of that type is constructed there. Under
// -fsanitize=cfi-unrelated-cast, such a cast would read a vtable pointer from
// storage that holds none, so it must not be checked.

// REQUIRES: clang, has-filecheck
// UNSUPPORTED: c++03, c++11, c++14, c++17

// RUN: %{cxx} %s %{flags} %{compile_flags} -flto -fvisibility=hidden -fsanitize=cfi-unrelated-cast \
// RUN:   -fno-sanitize-ignorelist -S -emit-llvm -o %t.ll
// RUN: FileCheck %s --input-file=%t.ll --implicit-check-not="call i1 @llvm.type.test"

#include <memory>

struct Poly {
  virtual ~Poly() = default;
};

// The one checked cast, which shows that the sanitizer is in effect.
// CHECK: define {{.*}}from_void
// CHECK: call i1 @llvm.type.test
Poly* from_void(void* p) { return static_cast<Poly*>(p); }

void test() {
  (void)std::make_shared<Poly>();
  (void)std::allocate_shared<Poly[]>(std::allocator<Poly>(), 2);
}
