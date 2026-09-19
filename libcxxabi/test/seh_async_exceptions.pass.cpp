//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.*-windows-itanium}}
// UNSUPPORTED: no-exceptions
// ADDITIONAL_COMPILE_FLAGS: -fasync-exceptions

// With -fasync-exceptions the call-site table covers the instructions of a
// try block and of an object's lifetime, so a structured exception raised by
// one of them, not just by a callee, reaches the catch (...) and runs the
// destructors of the objects alive at that point.

#include <cassert>
#include <string>
#include <windows.h>

static std::string trace;
struct Tracer {
  const char *name;
  explicit Tracer(const char *n) : name(n) {}
  ~Tracer() { trace += name; trace += ';'; }
};

int *volatile null_pointer = nullptr;
volatile int zero = 0;

// The fault is at a plain store, with no call in the try block.
__attribute__((noinline)) static void test_store() {
  trace.clear();
  try {
    Tracer t("live");
    *null_pointer = 1;
    trace += "not-reached;";
  } catch (...) {
    trace += "catch;";
  }
  assert(trace == "live;catch;");
}

// An integer division by zero.
__attribute__((noinline)) static int divide(int d) {
  try {
    return 100 / d;
  } catch (...) {
    return -1;
  }
}
static void test_divide() { assert(divide(zero) == -1); }

// Objects alive at the fault are destroyed, an object outside the try is not
// touched until its own scope ends.
__attribute__((noinline)) static void test_objects() {
  trace.clear();
  {
    Tracer outer("outer");
    try {
      std::string s(64, 'x');
      Tracer inner("inner");
      *null_pointer = (int)s.size();
    } catch (...) {
      trace += "catch;";
    }
    trace += "after;";
  }
  assert(trace == "inner;catch;after;outer;");
}

// A fault after the try block is not the try's business.
__attribute__((noinline)) static void after_try() {
  try {
    zero = 1;
  } catch (...) {
    trace += "wrong;";
  }
  *null_pointer = 2;
}
static void test_after_try() {
  trace.clear();
  __try {
    after_try();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    trace += "except;";
  }
  assert(trace == "except;");
}

// A typed catch does not take a structured exception.
__attribute__((noinline)) static void typed_only() {
  try {
    *null_pointer = 3;
  } catch (int) {
    trace += "wrong;";
  }
}
static void test_typed_only() {
  trace.clear();
  __try {
    typed_only();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    trace += "except;";
  }
  assert(trace == "except;");
}

// Nothing changes for a try block that is left normally.
__attribute__((noinline)) static int normal(int *p) {
  try {
    Tracer t("t");
    *p = 4;
  } catch (...) {
    return -1;
  }
  return *p;
}
static void test_normal() {
  trace.clear();
  int v = 0;
  assert(normal(&v) == 4);
  assert(trace == "t;");
}

int main(int, char **) {
  test_store();
  test_divide();
  test_objects();
  test_after_try();
  test_typed_only();
  test_normal();
  return 0;
}
