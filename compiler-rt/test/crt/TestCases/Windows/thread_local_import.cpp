// A thread_local marked as crossing the shared-library boundary can be used
// from another image, as on ELF. The defining DLL exports a record of the
// variable's TLS index, offset and guard, never the variable itself, and the
// importer's wrapper finds the variable through it, running the DLL's
// initialization on each thread's first use and registering its destructor.
//
// RUN: %clangxx_crt -std=c++20 -DBUILD_DLL -shared %s -o %t.dll -Wl,-implib:%t.import.lib
// RUN: %clangxx_crt_main -std=c++20 %s %t.import.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-exports %t.dll | FileCheck %s --check-prefix=EXPORTS
//
// RUN: %clangxx_crt_main -std=c++20 -flto %s %t.import.lib -o %t.lto.exe
// RUN: %run %t.lto.exe | FileCheck %s
//
// The record survives link-time optimization of the DLL.
// RUN: %clangxx_crt -std=c++20 -flto -DBUILD_DLL -shared %s -o %t.lto.dll -Wl,-implib:%t.lto.import.lib
// RUN: %clangxx_crt_main -std=c++20 %s %t.lto.import.lib -o %t.lto-dll.exe
// RUN: %run %t.lto-dll.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <thread>

#define API __attribute__((visibility("default")))

struct API Tracked {
  Tracked();
  ~Tracked();
  int Value;
};

API extern thread_local int Plain;
API extern constinit thread_local int Constant;
API extern thread_local int Dynamic;
API extern thread_local Tracked Object;
API extern thread_local int &Reference;
template <class T> struct API Holder {
  static thread_local T Value;
};
template <class T> thread_local T Holder<T>::Value = T(5);
extern template struct Holder<int>;

API int *dll_plain_address();
API int dll_destructions();

// EXPORTS-NOT: Name: Plain{{$}}
// EXPORTS-DAG: Name: Plain$tls
// EXPORTS-DAG: Name: Constant$tls
// EXPORTS-DAG: Name: Dynamic$tls
// EXPORTS-DAG: Name: Object$tls
// EXPORTS-DAG: Name: _ZN6HolderIiE5ValueE$tls
// EXPORTS-DAG: Name: _ZTH5Plain
// EXPORTS-DAG: Name: _ZTH7Dynamic
// EXPORTS-NOT: Name: Plain{{$}}

#ifdef BUILD_DLL

static int Initializations;
static int Destructions;
static int next() { return ++Initializations * 100; }

Tracked::Tracked() : Value(++Initializations) {}
Tracked::~Tracked() { ++Destructions; }

thread_local int Plain = 1;
constinit thread_local int Constant = 2;
thread_local int Dynamic = next();
thread_local Tracked Object;
thread_local int &Reference = Plain;
template struct Holder<int>;

int *dll_plain_address() { return &Plain; }
int dll_destructions() { return Destructions; }

#else

int main() {
  // Constant initialization needs nothing to run.
  // CHECK: main: 1 2 5
  printf("main: %d %d %d\n", Plain, Constant, Holder<int>::Value);

  // The first dynamic access runs the DLL's initialization once for the
  // thread: Dynamic, then Object.
  // CHECK: dynamic: 100 2 100
  printf("dynamic: %d %d %d\n", Dynamic, Object.Value, Dynamic);

  // The address is the DLL's, and a write is seen there.
  Plain = 10;
  // CHECK: identity: 1 1 10
  printf("identity: %d %d %d\n", &Plain == dll_plain_address(),
         &Reference == &Plain, *dll_plain_address());

  // A new thread has its own copies, initialized on its first use, and the
  // DLL's destructor runs when it exits.
  std::thread([] {
    // CHECK: thread: 1 300 4 0
    printf("thread: %d %d %d %d\n", Plain, Dynamic, Object.Value,
           &Plain == dll_plain_address() ? 0 : 1);
    fflush(stdout);
  }).join();
  // CHECK: destructions: 1
  printf("destructions: %d\n", dll_destructions());

  // CHECK: main again: 10 100 2
  printf("main again: %d %d %d\n", Plain, Dynamic, Object.Value);
  return 0;
}

#endif
