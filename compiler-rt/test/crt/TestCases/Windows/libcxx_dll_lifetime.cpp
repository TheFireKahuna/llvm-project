// Exercise shared libc++ state, exceptions, and allocation across a DLL boundary.
// Destroy all returned objects before unloading the DLL that created them.
// RUN: %clangxx_crt_dll -std=c++17 -O2 -DBUILD_DLL %s -o %t.dll
// RUN: %clangxx_crt_main -std=c++17 -O2 %s -o %t.exe
// RUN: %run %t.exe %t.dll 2>&1 | FileCheck %s
// REQUIRES: windows, crt

#include <assert.h>
#include <exception>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#ifdef BUILD_DLL
extern "C" __declspec(dllexport) void raiseException() {
  struct Object {
    ~Object() { fprintf(stderr, "DLL stack unwound\n"); }
  } object;
  throw std::runtime_error("DLL exception");
}

extern "C" __declspec(dllexport) void
captureException(std::exception_ptr *result) {
  *result = std::make_exception_ptr(std::logic_error("saved exception"));
}

extern "C" __declspec(dllexport) bool
checkTerminate(std::terminate_handler handler) {
  return std::get_terminate() == handler;
}

extern "C" __declspec(dllexport) void makeString(std::string **result) {
  *result = new std::string(300, 'x');
}
#else
#  include <windows.h>

static void terminateHandler() { _Exit(1); }

int main(int argc, char **argv) {
  assert(argc == 2);
  HMODULE module = LoadLibraryA(argv[1]);
  assert(module);
  auto raise =
      reinterpret_cast<void (*)()>(GetProcAddress(module, "raiseException"));
  auto capture = reinterpret_cast<void (*)(std::exception_ptr *)>(
      GetProcAddress(module, "captureException"));
  auto check = reinterpret_cast<bool (*)(std::terminate_handler)>(
      GetProcAddress(module, "checkTerminate"));
  auto make = reinterpret_cast<void (*)(std::string **)>(
      GetProcAddress(module, "makeString"));
  assert(raise && capture && check && make);

  auto previous = std::set_terminate(terminateHandler);
  assert(check(terminateHandler));
  fprintf(stderr, "shared terminate handler\n");
  try {
    raise();
    return 1;
  } catch (const std::runtime_error &error) {
    fprintf(stderr, "caught %s\n", error.what());
  }
  {
    std::exception_ptr exception;
    capture(&exception);
    try {
      std::rethrow_exception(exception);
    } catch (const std::logic_error &error) {
      fprintf(stderr, "caught %s\n", error.what());
    }
  }
  std::string *string = nullptr;
  make(&string);
  assert(string && *string == std::string(300, 'x'));
  delete string;
  fprintf(stderr, "cross-module allocation released\n");
  std::set_terminate(previous);
  assert(FreeLibrary(module));
  fprintf(stderr, "DLL unloaded\n");
}
#endif

// CHECK: shared terminate handler
// CHECK-NEXT: DLL stack unwound
// CHECK-NEXT: caught DLL exception
// CHECK-NEXT: caught saved exception
// CHECK-NEXT: cross-module allocation released
// CHECK-NEXT: DLL unloaded
