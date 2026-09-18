// TLS objects first constructed by a destructor must be destroyed before the
// older pending objects, including when the new registrations span blocks.
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <thread>
#include <utility>

static int order[103];
static int count;

struct Leaf {
  int id;
  ~Leaf() { order[count++] = id; }
};

template <int N> struct Object {
  ~Object() {
    order[count++] = N;
    if constexpr (N == 0) {
      thread_local Leaf leaf{-1};
    }
  }
};

template <int N> void construct() { thread_local Object<N> object; }

template <int... Ns> void constructAll(std::integer_sequence<int, Ns...>) {
  (construct<Ns>(), ...);
}

template <int N> struct Root {
  ~Root() {
    order[count++] = N;
    constructAll(std::make_integer_sequence<int, N>{});
  }
};

template <int N> void check() {
  count = 0;
  std::thread([] {
    // The oldest object must remain pending while Root registers new objects.
    thread_local Leaf oldest{-2};
    thread_local Root<N> root;
  }).join();
  assert(count == N + 3);
  assert(order[0] == N);
  for (int i = 0; i < N; ++i)
    assert(order[i + 1] == N - 1 - i);
  assert(order[N + 1] == -1);
  assert(order[N + 2] == -2);
}

int main() {
  check<1>();
  check<29>();
  check<30>();
  check<31>();
  check<100>();
}
