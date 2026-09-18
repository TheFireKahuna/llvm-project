// Termination boundaries must preserve the active C++ exception.
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe atexit
// RUN: %run %t.exe tls
// RUN: %run %t.exe noexcept
// RUN: %run %t.exe unwind
// RUN: %run %t.exe direct
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG -DSTATIC_INITIALIZER %s -o %t.init.exe
// RUN: %run %t.init.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <exception>
#include <stdlib.h>
#include <thread>

static bool expectException = true;
static void terminateHandler() {
  auto exception = std::current_exception();
  assert(bool(exception) == expectException);
  if (exception) {
    try {
      std::rethrow_exception(exception);
    } catch (int value) {
      assert(value == 42);
    } catch (...) {
      _Exit(3);
    }
  }
  _Exit(0);
}
static void throwing() { throw 42; }
struct ThrowingDestructor {
  ~ThrowingDestructor() noexcept(false) { throwing(); }
};
static void cannotThrow() noexcept { throwing(); }
#ifdef STATIC_INITIALIZER
static struct Install {
  Install() { std::set_terminate(terminateHandler); }
} install;
static struct Initialize {
  Initialize() { throwing(); }
} initialize;
int main() { return 1; }
#else
int main(int argc, char **argv) {
  assert(argc == 2);
  std::set_terminate(terminateHandler);
  switch (argv[1][0]) {
  case 'a':
    assert(atexit(throwing) == 0);
    try {
      exit(0);
    } catch (...) {
      _Exit(1);
    }
  case 't':
    std::thread([] {
      thread_local ThrowingDestructor object;
      (void)object;
    }).join();
    break;
  case 'n':
    try {
      cannotThrow();
    } catch (...) {
      _Exit(1);
    }
    break;
  case 'u':
    try {
      ThrowingDestructor object;
      throw 1;
    } catch (...) {
      _Exit(1);
    }
    break;
  case 'd':
    expectException = false;
    std::terminate();
  }
  return 2;
}
#endif
