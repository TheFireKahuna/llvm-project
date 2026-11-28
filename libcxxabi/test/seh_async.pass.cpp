//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{(x86_64|aarch64)-.*-windows-itanium}}
// UNSUPPORTED: no-exceptions, no-threads
// ADDITIONAL_COMPILE_FLAGS: -fasync-exceptions

// A function compiled for asynchronous exceptions takes a structured exception
// in catch (...), whichever instruction raises it, a hardware fault or a
// software raise, and the objects alive at that instruction are destroyed
// once. _set_se_translator installs a thread's translator, which is called
// while the exception's handler is searched for, in a frame that has a
// handler where it was raised; what it throws is matched against that frame's
// handlers by type.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <eh.h>
#include <exception>
#include <string>
#include <thread>
#include <windows.h>

static int* volatile null;
static volatile int sink;

enum Kind { Access, Raise };
#define FAULT(kind) ((kind) == Access ? (void)(*null = 1) : RaiseException(0xE0001234, 0, 0, nullptr))
static const DWORD codes[] = {EXCEPTION_ACCESS_VIOLATION, 0xE0001234};

static std::string trace;
struct Tracer {
  const char* name;
  explicit Tracer(const char* n) : name(n) {}
  ~Tracer() {
    trace += name;
    trace += ';';
  }
};

static DWORD seen_code;
static ULONG_PTR seen_parameter;
static int filter(EXCEPTION_POINTERS* pointers, int result) {
  seen_code      = pointers->ExceptionRecord->ExceptionCode;
  seen_parameter = pointers->ExceptionRecord->NumberParameters ? pointers->ExceptionRecord->ExceptionInformation[0] : 0;
  trace += "filter;";
  return result;
}

template <class F>
__attribute__((noinline)) static bool guard(F f, int result = EXCEPTION_EXECUTE_HANDLER) {
  __try {
    f();
  } __except (filter(GetExceptionInformation(), result)) {
    trace += "except;";
    return true;
  }
  return false;
}

// The fault is an instruction of the frame that holds the object and the
// handler.
__attribute__((noinline)) static void in_frame(Kind kind) {
  try {
    Tracer t("object");
    FAULT(kind);
    trace += "resumed;";
  } catch (...) {
    trace += "catch;";
  }
}

static volatile unsigned long long* volatile sentinel;
__attribute__((noinline)) static void leaf(Kind kind) {
  volatile unsigned long long here = 0x5E5E5E5E5E5E5E5Eull;
  sentinel                         = &here;
  Tracer t("leaf");
  FAULT(kind);
}
__attribute__((noinline)) static void above(Kind kind) {
  Tracer t("above");
  leaf(kind);
}

// An object is destroyed for a fault inside its lifetime and for no other, to
// the instruction. The lifetime starts behind the constructor's call and ends
// ahead of the destructor's, and FAULT_HERE is one instruction with no operand
// to set up: in turn the first of the lifetime, the last of it and the first
// behind it. A fault in the constructor reaches the frame at the return
// address of a call that ends the range ahead of the lifetime, and one in
// the last call of the lifetime at that of a call that ends its range.
#if defined(__x86_64__)
#  define FAULT_HERE() __asm__ volatile("ud2")
#else
#  define FAULT_HERE() __asm__ volatile("udf #0")
#endif
struct Made {
  __attribute__((noinline)) explicit Made(bool fault) {
    if (fault)
      *null = 1;
  }
  __attribute__((noinline)) ~Made() { trace += "made;"; }
};
__attribute__((noinline)) static void faults() { *null = 1; }
enum Where { InConstructor, First, Last, InLastCall, Behind };
template <Where where>
__attribute__((noinline)) static void lifetime() {
  {
    Made made(where == InConstructor);
    if (where == First)
      FAULT_HERE();
    sink = 1;
    if (where == Last)
      FAULT_HERE();
    if (where == InLastCall)
      faults();
  }
  if (where == Behind)
    FAULT_HERE();
}

__attribute__((noinline)) static void typed_only(Kind kind) {
  Tracer t("typed");
  try {
    leaf(kind);
  } catch (int) {
    trace += "catch int;";
  }
}

__attribute__((noinline)) static void rethrows(bool pointer) {
  try {
    Tracer t("inner");
    ULONG_PTR parameter = 0x5EED;
    RaiseException(0xE0004321, 0, 1, &parameter);
  } catch (...) {
    trace += "catch;";
    assert(std::uncaught_exceptions() == 0);
    if (pointer) {
      std::exception_ptr p = std::current_exception();
      assert(p);
      std::rethrow_exception(p);
    }
    throw;
  }
}

__attribute__((noinline)) static void nothrow_body(Kind kind) noexcept {
  Tracer t("noexcept");
  FAULT(kind);
}
__attribute__((noinline)) static void nothrow_callee(Kind kind) noexcept {
  Tracer t("noexcept");
  leaf(kind);
}

// The copy of a catch parameter is made in a terminate scope, which a
// structured exception passes by as it passes a noexcept function. The C++
// exception that was being caught ends where the frame is unwound.
struct Copied {
  static int alive;
  Copied() { ++alive; }
  Copied(const Copied&) { *null = 1; }
  ~Copied() { --alive; }
};
int Copied::alive;
__attribute__((noinline)) static void copies() {
  Tracer t("copies");
  try {
    throw Copied();
  } catch (Copied) {
    trace += "catch;";
  }
}

// A destructor that raises a structured exception and handles it itself.
struct Guarded {
  ~Guarded() {
    guard([] { *null = 2; });
    trace += "guarded;";
  }
};
__attribute__((noinline)) static void nested() {
  Tracer outer("outer");
  Guarded g;
  Tracer inner("inner");
  *null = 1;
}

struct Base {
  virtual ~Base() {}
};
struct Translated : Base {
  static int alive;
  unsigned code;
  explicit Translated(unsigned c) : code(c) { ++alive; }
  Translated(const Translated& other) : code(other.code) { ++alive; }
  ~Translated() override { --alive; }
};
int Translated::alive;

static int calls;
static void throws(unsigned code, EXCEPTION_POINTERS* pointers) {
  ++calls;
  // Nothing is unwound: the faulting frame is alive below the translator.
  assert(pointers->ExceptionRecord->ExceptionCode == code);
  assert(*sentinel == 0x5E5E5E5E5E5E5E5Eull);
  throw Translated(code);
}
static void returns(unsigned, EXCEPTION_POINTERS*) { ++calls; }

// A frame with cleanups and no handler, and one whose handlers do not take
// what the translator throws.
__attribute__((noinline)) static void cleanup_only() {
  Tracer t("cleanup");
  leaf(Access);
}
__attribute__((noinline)) static void declines() {
  Tracer t("declines");
  try {
    cleanup_only();
  } catch (int) {
    trace += "catch int;";
  }
}

int main(int, char**) {
  std::set_terminate([] {
    std::puts("terminate called");
    _Exit(1);
  });

  for (Kind kind : {Access, Raise}) {
    trace.clear();
    in_frame(kind);
    assert(trace == "object;catch;");

    trace.clear();
    try {
      above(kind);
    } catch (...) {
      trace += "catch;";
    }
    assert(trace == "leaf;above;catch;");
    assert(std::uncaught_exceptions() == 0);

    // A typed handler does not take a structured exception no translator
    // made a C++ exception of; the frame is unwound through all the same.
    trace.clear();
    assert(guard([=] { typed_only(kind); }));
    assert(trace == "filter;leaf;typed;except;" && seen_code == codes[kind]);

    // Whichever of an __except and a catch (...) is nearer takes it.
    trace.clear();
    assert(guard([=] { in_frame(kind); }) == false);
    assert(trace == "object;catch;");
    trace.clear();
    try {
      assert(guard([=] { leaf(kind); }));
    } catch (...) {
      assert(false);
    }
    assert(trace == "filter;leaf;except;");

    // A structured exception crosses a noexcept function with its objects
    // destroyed, and std::terminate is not called.
    trace.clear();
    assert(guard([=] { nothrow_body(kind); }));
    assert(trace == "filter;noexcept;except;");
    trace.clear();
    try {
      nothrow_callee(kind);
    } catch (...) {
      trace += "catch;";
    }
    assert(trace == "leaf;noexcept;catch;");
  }

  trace.clear();
  assert(guard(copies));
  assert(trace == "filter;copies;except;");
  assert(Copied::alive == 0 && std::uncaught_exceptions() == 0);

  trace.clear();
  assert(guard(lifetime<InConstructor>));
  assert(trace == "filter;except;");
  for (void (*inside)() : {lifetime<First>, lifetime<Last>, lifetime<InLastCall>}) {
    trace.clear();
    assert(guard(inside));
    assert(trace == "filter;made;except;");
  }
  trace.clear();
  assert(guard(lifetime<Behind>));
  assert(trace == "made;filter;except;");

  // throw; and std::rethrow_exception raise the structured exception again:
  // a filter further out sees its code and parameters, and a catch (...) a
  // new object.
  for (bool pointer : {false, true}) {
    trace.clear();
    assert(guard([=] { rethrows(pointer); }));
    assert(trace == "inner;catch;filter;except;");
    assert(seen_code == 0xE0004321 && seen_parameter == 0x5EED);
    trace.clear();
    try {
      rethrows(pointer);
    } catch (...) {
      trace += "outer;";
    }
    assert(trace == "inner;catch;outer;");
    assert(std::uncaught_exceptions() == 0);
  }

  // A breakpoint is the debugger's.
  trace.clear();
  assert(guard([] {
    try {
      Tracer t("object");
      RaiseException(EXCEPTION_BREAKPOINT, 0, 0, nullptr);
    } catch (...) {
      trace += "catch;";
    }
  }));
  assert(trace == "filter;object;except;");

  trace.clear();
  assert(guard(nested));
  assert(trace == "filter;inner;filter;except;guarded;outer;except;");

  // A C++ exception is what it was.
  try {
    Tracer t("object");
    throw 42;
  } catch (int value) {
    assert(value == 42);
  }

  // The translator, by type and by base class, with one call for the frame
  // that has the handler and none for the frame that has cleanups only.
  assert(_set_se_translator(throws) == nullptr);
  trace.clear();
  calls = 0;
  try {
    cleanup_only();
  } catch (const Translated& e) {
    assert(e.code == EXCEPTION_ACCESS_VIOLATION);
    assert(std::uncaught_exceptions() == 0);
    trace += "catch;";
  }
  assert(trace == "leaf;cleanup;catch;" && calls == 1 && Translated::alive == 0);
  try {
    leaf(Access);
  } catch (const Base&) {
  } catch (...) {
    assert(false);
  }
  assert(Translated::alive == 0);

  // A frame that declines the object destroys it, and the next frame with a
  // handler has the translator called again.
  trace.clear();
  calls = 0;
  try {
    declines();
  } catch (const Translated&) {
    trace += "catch;";
  }
  assert(trace == "leaf;cleanup;declines;catch;" && calls == 2 && Translated::alive == 0);

  // A translation that no frame catches goes on as the structured exception
  // it was.
  trace.clear();
  assert(guard(declines));
  assert(trace == "filter;leaf;cleanup;declines;except;" && seen_code == EXCEPTION_ACCESS_VIOLATION);
  assert(Translated::alive == 0);

  // The translated exception is a C++ exception: it can be rethrown and kept.
  std::exception_ptr kept;
  try {
    try {
      leaf(Access);
    } catch (const Translated&) {
      kept = std::current_exception();
      throw;
    }
  } catch (const Translated& e) {
    assert(e.code == EXCEPTION_ACCESS_VIOLATION);
  }
  try {
    std::rethrow_exception(kept);
  } catch (const Translated&) {
  }
  kept = nullptr;
  assert(Translated::alive == 0);

  // A translator that returns leaves the exception structured, for
  // catch (...) alone.
  assert(_set_se_translator(returns) == throws);
  calls = 0;
  try {
    leaf(Access);
  } catch (const Translated&) {
    assert(false);
  } catch (...) {
  }
  assert(calls == 1);

  // The translator is the thread's.
  std::thread([] {
    assert(_set_se_translator(nullptr) == nullptr);
    calls = 0;
    try {
      leaf(Access);
    } catch (...) {
    }
    assert(calls == 0);
  }).join();
  assert(_set_se_translator(nullptr) == returns);

  // A translator that raises a structured exception itself calls
  // std::terminate: no handler takes that exception.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  std::set_terminate([] { _Exit(0); });
  _set_se_translator([](unsigned, EXCEPTION_POINTERS*) { *null = 2; });
  guard([] { in_frame(Access); });
  return 1;
}
