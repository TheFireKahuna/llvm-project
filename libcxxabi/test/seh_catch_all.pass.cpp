//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.*-windows-itanium}}
// UNSUPPORTED: no-exceptions

// A structured exception raised in a called function is taken by catch (...),
// the frames between run their cleanups, and a structured exception that an
// __except frame takes also runs the cleanups of the C++ frames it unwinds.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <windows.h>

// The call sites below must be invokes, so the faulting function is reached
// through a pointer the compiler cannot see through.
static void fault_impl() {
  int *volatile p = nullptr;
  *p = 1;
}
static void raise_impl() { RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
static void (*volatile fault)() = fault_impl;
static void (*volatile raise_custom)() = raise_impl;

static std::string trace;
struct Tracer {
  const char *name;
  explicit Tracer(const char *n) : name(n) {}
  ~Tracer() { trace += name; trace += ';'; }
};

__attribute__((noinline)) static void between() {
  Tracer t("between");
  fault();
}

static int filter(unsigned code) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "filter%08x;", code);
  trace += buf;
  return EXCEPTION_EXECUTE_HANDLER;
}

__attribute__((noinline)) static int guard(void (*f)()) {
  __try {
    f();
  } __except (filter(GetExceptionCode())) {
    trace += "except;";
    return 1;
  }
  return 0;
}

static void reset() { trace.clear(); }

// catch (...) takes a fault from a callee; the frame between runs its
// destructor first.
static void test_catch_all() {
  reset();
  try {
    between();
  } catch (...) {
    trace += "catch;";
  }
  assert(trace == "between;catch;");
}

// A RaiseException code is taken the same way.
static void test_raise() {
  reset();
  try {
    Tracer t("t");
    raise_custom();
  } catch (...) {
    trace += "catch;";
  }
  assert(trace == "t;catch;");
}

// The innermost handler wins: an __except inside the try takes it before the
// catch (...) is asked, and a catch (...) inside a __try takes it before the
// filter runs.
static void test_inner_except() {
  reset();
  try {
    guard(between);
  } catch (...) {
    trace += "wrong;";
  }
  assert(trace == "filterc0000005;between;except;");
}

__attribute__((noinline)) static void inner_catch() {
  try {
    between();
  } catch (...) {
    trace += "catch;";
  }
}
static void test_inner_catch() {
  reset();
  guard(inner_catch);
  assert(trace == "between;catch;");
}

// throw; from the catch raises the original exception again: an outer
// catch (...) takes it, and an outer filter sees the original code.
static void test_rethrow() {
  reset();
  try {
    try {
      between();
    } catch (...) {
      trace += "inner;";
      throw;
    }
  } catch (...) {
    trace += "outer;";
  }
  assert(trace == "between;inner;outer;");
}

__attribute__((noinline)) static void rethrow_to_filter() {
  try {
    between();
  } catch (...) {
    trace += "inner;";
    throw;
  }
}
static void test_rethrow_to_except() {
  reset();
  guard(rethrow_to_filter);
  assert(trace == "between;inner;filterc0000005;except;");
}

// An __except frame takes the exception: the C++ frames it unwinds run
// their destructors, in order, interleaved with a __finally.
__attribute__((noinline)) static void finally_frame(void (*f)()) {
  __try {
    f();
  } __finally {
    trace += AbnormalTermination() ? "finally;" : "finally-normal;";
  }
}
__attribute__((noinline)) static void two_frames() {
  Tracer t("outer-frame");
  finally_frame(between);
}
static void test_except_runs_cleanups() {
  reset();
  int r = guard(two_frames);
  assert(r == 1);
  assert(trace == "filterc0000005;between;finally;outer-frame;except;");
}

// A destructor running during such an unwind raises its own structured
// exception, which an __except inside the destructor takes; the outer unwind
// then continues.
__attribute__((noinline)) static void guarded_raise() {
  __try {
    raise_custom();
  } __except (filter(GetExceptionCode())) {
    trace += "inner-except;";
  }
}
struct NestingTracer {
  ~NestingTracer() {
    guarded_raise();
    trace += "dtor-done;";
  }
};
__attribute__((noinline)) static void nesting_frame() {
  NestingTracer t;
  fault();
}
static void test_nested_unwind() {
  reset();
  guard(nesting_frame);
  assert(trace == "filterc0000005;filtere0000001;inner-except;dtor-done;except;");
}

// When the function that takes the exception has several activations on the
// stack, the frame the unwind is heading for cannot be told apart from the
// others, so the cleanups in between are skipped rather than run against the
// wrong activation. The handler itself is unaffected.
__attribute__((noinline)) static void leaf() {
  Tracer t("leaf");
  fault();
}
static int rec_filter(int n) { return n == 2 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }
__attribute__((noinline)) static int recurse(int n) {
  __try {
    if (n == 0)
      leaf();
    else
      return recurse(n - 1) + 1;
  } __except (rec_filter(n)) {
    trace += "except2;";
    return 100;
  }
  return 0;
}
static void test_recursion() {
  reset();
  int r = recurse(4);
  assert(r == 102);
  assert(trace == "except2;");
}

// A frame whose return address follows a call the compiler took as not
// throwing has no call-site entry there; it is passed by, its destructor
// skipped, and the exception still reaches the catch (...).
static void fault_nothrow_impl() noexcept { fault_impl(); }
static void (*volatile fault_nounwind)() noexcept = fault_nothrow_impl;
__attribute__((noinline)) static void nothrow_fault() noexcept { fault_nounwind(); }
__attribute__((noinline)) static void gap_frame() {
  Tracer t("gap");
  nothrow_fault();
  fault(); // gives the frame a landing pad elsewhere
}
static void test_gap() {
  reset();
  try {
    gap_frame();
  } catch (...) {
    trace += "catch;";
  }
  assert(trace == "catch;");
}

// Caught while another exception is being handled, and carried by an
// exception_ptr: the object is a C++ exception of the runtime's own type.
static void test_nested_catch() {
  trace.clear();
  try {
    throw 1;
  } catch (int) {
    try {
      between();
    } catch (...) {
      trace += "nested;";
      assert(std::current_exception());
    }
    trace += "still-in-catch;";
  }
  assert(trace == "between;nested;still-in-catch;");
  assert(std::uncaught_exceptions() == 0);
}

static void test_exception_ptr() {
  trace.clear();
  std::exception_ptr ep;
  try {
    between();
  } catch (...) {
    ep = std::current_exception();
  }
  assert(ep);
  try {
    std::rethrow_exception(ep);
  } catch (...) {
    trace += "rethrown;";
  }
  assert(trace == "between;rethrown;");
}

// A typed catch does not name the runtime's type; the exception passes on.
__attribute__((noinline)) static void typed_only() {
  try {
    between();
  } catch (const std::exception &) {
    trace += "wrong;";
  }
}
static void test_typed_only() {
  trace.clear();
  guard(typed_only);
  assert(trace == "filterc0000005;between;except;");
}

int main(int, char **) {
  test_nested_catch();
  test_exception_ptr();
  test_typed_only();
  assert(std::uncaught_exceptions() == 0);
  test_catch_all();
  test_raise();
  test_inner_except();
  test_inner_catch();
  test_rethrow();
  test_rethrow_to_except();
  test_except_runs_cleanups();
  test_nested_unwind();
  test_recursion();
  test_gap();
  assert(std::uncaught_exceptions() == 0);
  return 0;
}
