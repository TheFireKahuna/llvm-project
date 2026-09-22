// RUN: %clangxx_crt_main -std=c++17 -O0 -UNDEBUG -I%S/../../../../../libcxx/src %s -o %t.exe
// RUN: %run %t.exe
// RUN: %clangxx_crt_main -std=c++20 -O2 -UNDEBUG -I%S/../../../../../libcxx/src %s -o %t.opt.exe
// RUN: %run %t.opt.exe
// REQUIRES: windows, crt

#include "include/aligned_alloc.h"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <sstream>
#include <string>
#include <system_error>

static void check(void *pointer, std::size_t alignment, std::size_t size) {
  assert(pointer);
  assert(reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0);
  // UCRT must recognize the returned pointer itself, not a hidden base pointer
  // as required by _aligned_malloc/_aligned_free.
  assert(_msize(pointer) >= size);
  std::memset(pointer, 0x5a, size);
}

int main() {
  const std::size_t alignments[] = {sizeof(void *), 16, 32, 4096, 1 << 20};
  for (std::size_t alignment : alignments) {
    void *pointer = std::__libcpp_aligned_alloc(alignment, 37);
    check(pointer, alignment, 37);
    std::free(pointer);

    pointer = ::operator new(37, std::align_val_t(alignment));
    check(pointer, alignment, 37);
    ::operator delete(pointer, std::align_val_t(alignment));
  }

  const int errors[] = {EADDRINUSE, EWOULDBLOCK};
  for (int error : errors) {
    char expected[128];
    assert(strerror_s(expected, sizeof(expected), error) == 0);
    assert(std::generic_category().message(error) == expected);
  }
  assert(std::to_wstring(1.25) == L"1.250000");

  // Exercise the SDK's explicit-locale conversion and formatting paths.
  std::istringstream input("1.25");
  input.imbue(std::locale::classic());
  long double value = 0;
  input >> value;
  assert(!input.fail() && value == 1.25L);
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << value;
  assert(output.str() == "1.25");
}
