// A language TLS destructor may start and join a thread. Cleanup must precede
// Windows loader teardown, which would deadlock the new thread's startup.
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <thread>

static bool ran;

struct Object {
  ~Object() {
    std::thread([] { ran = true; }).join();
  }
};

int main() {
  std::thread([] { thread_local Object object; }).join();
  assert(ran);
}
