// libc++'s own exception type must work across the PE shared-library boundary.
// RUN: %clangxx_crt_dll -std=c++20 -O0 -UNDEBUG -DBUILD_DLL %s -o %t.dll -Wl,/implib:%t.lib
// RUN: %clangxx_crt_main -std=c++20 -O0 -UNDEBUG %s %t.lib -o %t.exe
// RUN: %run %t.exe
// RUN: %clangxx_crt_dll -std=c++23 -O2 -flto=thin -mguard=cf -UNDEBUG -DBUILD_DLL %s -o %t.opt.dll -Wl,/implib:%t.opt.lib -Wl,/guard:cf
// RUN: %clangxx_crt_main -std=c++23 -O2 -flto=thin -mguard=cf -UNDEBUG %s %t.opt.lib -o %t.opt.exe -Wl,/guard:cf
// RUN: %run %t.opt.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <exception>
#include <format>
#include <string>
#include <thread>
#include <typeinfo>

#ifdef BUILD_DLL
#  define API __declspec(dllexport)
#else
#  define API __declspec(dllimport)
#endif

extern "C" {
API void invalid_format();
API void catch_format(void (*)());
API std::exception *make_error();
}

static void invalid() { (void)std::vformat("{", std::make_format_args()); }

#ifdef BUILD_DLL
void invalid_format() { invalid(); }
void catch_format(void (*function)()) {
  try {
    function();
  } catch (const std::format_error &error) {
    assert(error.what()[0] && typeid(error) == typeid(std::format_error));
    return;
  }
  assert(false);
}
std::exception *make_error() { return new std::format_error("from DLL"); }
#else
int main() {
  std::exception_ptr saved;
  try {
    invalid_format();
  } catch (const std::format_error &error) {
    assert(error.what()[0] && typeid(error) == typeid(std::format_error));
    saved = std::current_exception();
  }
  assert(saved);
  std::thread([saved] {
    try {
      std::rethrow_exception(saved);
    } catch (const std::format_error &error) {
      assert(error.what()[0]);
      return;
    }
    assert(false);
  }).join();
  catch_format(invalid);

  std::exception *base = make_error();
  auto *error = dynamic_cast<std::format_error *>(base);
  assert(error && std::string(error->what()) == "from DLL");
  assert(typeid(*base) == typeid(std::format_error));
  delete base;
}
#endif
