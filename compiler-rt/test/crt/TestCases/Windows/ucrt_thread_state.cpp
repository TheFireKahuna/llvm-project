// libc++ threads must retain UCRT per-thread errno, locale and floating-point state.
// RUN: %clangxx_crt_main -std=c++20 -O2 -ffp-model=strict -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <barrier>
#include <cerrno>
#include <cfenv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <thread>

int main() {
  assert(_configthreadlocale(_ENABLE_PER_THREAD_LOCALE) != -1);
  assert(std::setlocale(LC_NUMERIC, "C"));
  assert(std::fesetround(FE_DOWNWARD) == 0);
  std::barrier gate(2);
  std::thread worker([&] {
    assert(_configthreadlocale(_ENABLE_PER_THREAD_LOCALE) != -1);
    assert(std::setlocale(LC_NUMERIC, "fr-FR"));
    assert(std::fesetround(FE_UPWARD) == 0);
    errno = ERANGE;
    gate.arrive_and_wait();
    assert(errno == ERANGE && std::fegetround() == FE_UPWARD);
    volatile double value = 1.25;
    assert(std::nearbyint(value) == 2.0);
    // Test locale parsing independently of directed decimal-conversion rounding.
    assert(std::fesetround(FE_TONEAREST) == 0);
    char *end;
    assert(std::strtod("1,5", &end) == 1.5 && *end == '\0');
    std::istringstream input("1,5");
    input.imbue(std::locale("fr-FR"));
    double parsed = 0;
    input >> parsed;
    assert(input && parsed == 1.5);
    gate.arrive_and_wait();
  });
  errno = EDOM;
  gate.arrive_and_wait();
  assert(errno == EDOM && std::fegetround() == FE_DOWNWARD);
  volatile double value = 1.25;
  assert(std::nearbyint(value) == 1.0);
  assert(std::fesetround(FE_TONEAREST) == 0);
  char *end;
  assert(std::strtod("1.5", &end) == 1.5 && *end == '\0');
  assert(std::fesetround(FE_DOWNWARD) == 0);
  gate.arrive_and_wait();
  worker.join();
  assert(std::fegetround() == FE_DOWNWARD);
  assert(std::fesetround(FE_TONEAREST) == 0);
}
