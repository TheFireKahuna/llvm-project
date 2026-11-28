//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{(x86_64|aarch64)-.*-windows-itanium}}
// UNSUPPORTED: no-exceptions

// A cleanup is a funclet the unwinder calls, so every unwind that passes a
// C++ frame runs its cleanups, once: a C++ exception's, the unwind of a
// structured exception that an __except takes, and a longjmp. In a function
// compiled without -fasync-exceptions, catch (...) takes another C++
// runtime's exception and no structured exception, and no translator is
// called. A cleanup does not unwind: an exception that leaves one while an
// unwind is calling it ends the process there, each case in a child process.

#include <cassert>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <eh.h>
#include <exception>
#include <string>
#include <unwind.h>
#include <windows.h>

static int* volatile null;
static std::jmp_buf jump;

static std::string trace;
struct Tracer {
  const char* name;
  explicit Tracer(const char* n) : name(n) {}
  ~Tracer() {
    trace += name;
    trace += ';';
  }
};

static void fault() { *null = 1; }
static void throws() { throw 1; }
static void jumps() { std::longjmp(jump, 1); }

// An MSVC C++ exception of an object with a destructor, which this toolchain
// cannot throw: the record that vcruntime's _CxxThrowException raises for
// one, built by hand. A ThrowInfo holds its attributes and then the addresses,
// relative to the image, of the function that destroys the object, of a
// compatibility handler and of the types a handler may name.
extern "C" IMAGE_DOS_HEADER __ImageBase;
static void destroy(void* object) {
  assert(*static_cast<int*>(object) == 42);
  trace += "destroyed;";
}
static void msvc_throws() {
  char* image = reinterpret_cast<char*>(&__ImageBase);
  int info[4] = {0, static_cast<int>(reinterpret_cast<char*>(&destroy) - image), 0, 0};
  int object  = 42;
  ULONG_PTR parameters[] = {
      0x19930520, reinterpret_cast<ULONG_PTR>(&object), reinterpret_cast<ULONG_PTR>(info),
      reinterpret_cast<ULONG_PTR>(image)};
  RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 4, parameters);
}

static void (*volatile leave)();

__attribute__((noinline)) static void inner() {
  Tracer t("inner");
  leave();
}
__attribute__((noinline)) static void outer() {
  Tracer t("outer");
  try {
    inner();
  } catch (...) {
    trace += "catch;";
    throw;
  }
}

__attribute__((noinline)) static bool guard(void (*f)()) {
  __try {
    f();
  } __except (trace += "filter;", EXCEPTION_EXECUTE_HANDLER) {
    return true;
  }
  return false;
}

// Every level has an __except whose filter declines, but the one at depth 2:
// the unwind's target is one of several activations of one function.
static void recurse(int depth);
__attribute__((noinline)) static void recurse_guard(int depth) {
  if (depth == 5)
    fault();
  __try {
    recurse(depth + 1);
  } __except (depth == 2 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    trace += "except;";
  }
}
__attribute__((noinline)) static void recurse(int depth) {
  static const char* const names[] = {"0", "1", "2", "3", "4", "5"};
  Tracer t(names[depth]);
  recurse_guard(depth);
}

// What leaves a cleanup. A C++ exception calls std::terminate, and a
// structured exception goes to the unhandled-exception filter with its own
// record, whatever __except or catch (...) lies further out. A longjmp or a
// forced unwind is no exception: it goes on from the frame of the cleanup,
// whose objects are destroyed once.
static const char* mode;

static _Unwind_Reason_Code
stop(int, _Unwind_Action actions, _Unwind_Exception_Class, _Unwind_Exception*, struct _Unwind_Context*, void*) {
  if (actions & _UA_END_OF_STACK)
    _Exit(trace == "inner;leaves;outer;" ? 0 : 3);
  return _URC_NO_REASON;
}

struct Leaves {
  ~Leaves() noexcept(false) {
    trace += "leaves;";
    if (!std::strcmp(mode, "fault"))
      *null = 2;
    if (!std::strcmp(mode, "throw"))
      throw 2;
    if (!std::strcmp(mode, "longjmp"))
      std::longjmp(jump, 1);
    if (!std::strcmp(mode, "forced")) {
      static _Unwind_Exception exception;
      exception.exception_class = 0x434C4E47554E5700; // CLNGUNW\0
      _Unwind_ForcedUnwind(&exception, stop, nullptr);
    }
  }
};

static void (*volatile fault_ptr)() = fault;
__attribute__((noinline)) static void leaves_inner(bool structured) {
  Leaves l;
  Tracer t("inner");
  if (structured)
    fault_ptr();
  throw 1;
}
__attribute__((noinline)) static void leaves_outer(bool structured) {
  Tracer t("outer");
  leaves_inner(structured);
}

// The funclet of a cleanup that a landing pad's code calls on its way to a
// handler further out, and not the unwinder.
__attribute__((noinline)) static void pad_called() {
  try {
    Leaves l;
    try {
      throw 1;
    } catch (long) {
    }
  } catch (...) {
    _Exit(4);
  }
}

__attribute__((noinline)) static void in_except(bool structured) {
  __try {
    leaves_outer(structured);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static int child(const char* how) {
  std::set_terminate([] { _Exit(42); });
  mode = !std::strncmp(how, "fault", 5) ? "fault" : how;
  if (!std::strcmp(how, "fault-pad")) {
    pad_called();
    return 5;
  }
  if (!std::strcmp(how, "longjmp") || !std::strcmp(how, "forced")) {
    // The unwind collides with the one that called the cleanup, and the
    // frames above the cleanup's are cleaned up as usual.
    if (!setjmp(jump)) {
      try {
        leaves_outer(false);
      } catch (...) {
      }
    }
    return trace == "inner;leaves;outer;" ? 0 : 3;
  }
  // Neither the __except nor the catch (...) takes what leaves the cleanup.
  try {
    in_except(!std::strcmp(how, "fault-structured"));
  } catch (...) {
  }
  return 5;
}

static DWORD run(const char* self, const char* how) {
  std::string command  = std::string("\"") + self + "\" " + how;
  STARTUPINFOA startup = {};
  startup.cb           = sizeof(startup);
  PROCESS_INFORMATION process;
  BOOL created = CreateProcessA(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
  assert(created);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return code;
}

static void never(unsigned, EXCEPTION_POINTERS*) { assert(false); }

int main(int argc, char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (argc > 1)
    return child(argv[1]);
  _set_se_translator(never);

  // A fault is no exception to catch (...) here, and the frames the __except
  // unwinds are cleaned up.
  leave = fault;
  trace.clear();
  assert(guard(outer));
  assert(trace == "filter;inner;outer;");

  leave = throws;
  trace.clear();
  try {
    outer();
  } catch (int) {
    trace += "int;";
  }
  assert(trace == "inner;catch;outer;int;");

  leave = jumps;
  trace.clear();
  if (!setjmp(jump))
    outer();
  assert(trace == "inner;outer;");

  trace.clear();
  recurse(1);
  assert(trace == "5;4;3;except;2;1;");

  // Another C++ runtime's exception is a foreign exception, which
  // catch (...) takes and a typed handler does not. The handler that takes an
  // MSVC exception has its object destroyed before the frame that holds it
  // is unwound, and throw; raises a foreign exception with no object.
  leave = msvc_throws;
  trace.clear();
  try {
    try {
      outer();
    } catch (int) {
      assert(false);
    }
  } catch (...) {
    trace += "foreign;";
    assert(!std::current_exception());
  }
  assert(trace == "destroyed;inner;catch;outer;foreign;");

  // A destructor that faults during a C++ exception's unwind, during a
  // structured exception's, and in a funclet that a landing pad called.
  assert(run(argv[0], "fault") == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
  assert(run(argv[0], "fault-structured") == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
  assert(run(argv[0], "fault-pad") == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
  assert(run(argv[0], "throw") == 42);
  assert(run(argv[0], "longjmp") == 0);
  assert(run(argv[0], "forced") == 0);
  return 0;
}
