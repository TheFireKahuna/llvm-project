//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.*-windows-itanium}}
// UNSUPPORTED: no-exceptions

// _set_se_translator turns a structured exception into a C++ exception of the
// translator's choosing while a handler is being looked for, so that it can
// be caught by type, nested inside another handler, and carried by an
// exception_ptr.

#include <cassert>
#include <exception>
#include <stdexcept>
#include <string>
#include <windows.h>
#include <eh.h>

static void fault_impl() {
  int *volatile p = nullptr;
  *p = 1;
}
static void (*volatile fault)() = fault_impl;

struct seh_error : std::runtime_error {
  unsigned code;
  void *address;
  seh_error(unsigned c, void *a) : std::runtime_error("structured exception"), code(c), address(a) {}
};

static int translations = 0;
static void translator(unsigned code, EXCEPTION_POINTERS *pointers) {
  ++translations;
  throw seh_error(code, pointers->ExceptionRecord->ExceptionAddress);
}
static void declining_translator(unsigned, EXCEPTION_POINTERS *) { ++translations; }

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

__attribute__((noinline)) static int guard(void (*f)()) {
  __try {
    f();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    trace += "except;";
    return 1;
  }
  return 0;
}

static void test_typed() {
  trace.clear();
  try {
    between();
  } catch (const seh_error &e) {
    assert(e.code == EXCEPTION_ACCESS_VIOLATION);
    assert(e.address != nullptr);
    trace += "typed;";
  }
  assert(trace == "between;typed;");
}

static void test_base_class() {
  trace.clear();
  try {
    between();
  } catch (const std::exception &e) {
    assert(std::string(e.what()) == "structured exception");
    trace += "base;";
  }
  assert(trace == "between;base;");
}

// A typed catch that does not match leaves the exception structured, for an
// outer __except.
__attribute__((noinline)) static void mismatch() {
  try {
    between();
  } catch (const std::bad_alloc &) {
    trace += "wrong;";
  }
}
static void test_typed_mismatch() {
  trace.clear();
  guard(mismatch);
  assert(trace == "between;except;");
}

// Caught while another exception is being handled, which a foreign
// exception could not be.
static void test_nested() {
  trace.clear();
  try {
    throw 1;
  } catch (int) {
    try {
      between();
    } catch (const seh_error &) {
      trace += "nested;";
    }
    trace += "still-in-catch;";
  }
  assert(trace == "between;nested;still-in-catch;");
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
  } catch (const seh_error &e) {
    assert(e.code == EXCEPTION_ACCESS_VIOLATION);
    trace += "rethrown;";
  }
  assert(trace == "between;rethrown;");
}

static void test_rethrow() {
  trace.clear();
  try {
    try {
      between();
    } catch (...) {
      throw;
    }
  } catch (const seh_error &) {
    trace += "outer;";
  }
  assert(trace == "between;outer;");
}

// The translator is asked once per C++ frame the search passes, as the
// MSVC runtime asks it.
__attribute__((noinline)) static void deep2() { Tracer t("deep2"); between(); }
__attribute__((noinline)) static void deep1() { Tracer t("deep1"); deep2(); }
static void test_asked_per_frame() {
  trace.clear();
  translations = 0;
  try {
    deep1();
  } catch (const seh_error &) {
  }
  assert(trace == "between;deep2;deep1;");
  assert(translations == 4);
}

// A translator that returns leaves the exception structured: catch (...)
// still takes it, a typed catch does not.
static void test_declines() {
  trace.clear();
  _se_translator_function previous = _set_se_translator(declining_translator);
  assert(previous == translator);
  try {
    between();
  } catch (const seh_error &) {
    trace += "wrong;";
  } catch (...) {
    trace += "catch-all;";
  }
  assert(trace == "between;catch-all;");
  _set_se_translator(translator);
}

static void test_reset() {
  trace.clear();
  _set_se_translator(nullptr);
  guard(mismatch);
  assert(trace == "between;except;");
  _set_se_translator(translator);
}

int main(int, char **) {
  _set_se_translator(translator);
  test_typed();
  test_base_class();
  test_typed_mismatch();
  test_nested();
  test_exception_ptr();
  test_rethrow();
  test_asked_per_frame();
  test_declines();
  test_reset();
  return 0;
}
