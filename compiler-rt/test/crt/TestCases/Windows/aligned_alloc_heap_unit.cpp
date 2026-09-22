// Exercise the production public entry with substituted PEB observations.
// The real PEB and UCRT state are never modified, even for the cold-entry cases.
// RUN: %clang_crt_main -std=c++17 -O2 -mguard=cf -fms-extensions -fno-exceptions -fno-rtti -fno-builtin -UNDEBUG -Wall -Wextra -Werror %s -o %t.exe -Wl,/guard:cf
// RUN: %run %t.exe
// REQUIRES: windows, crt

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../../../lib/wincrt/heap.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <windows.h>

namespace wincrt {
static void *Candidate;
static void *testProcessHeap() { return Candidate; }
} // namespace wincrt

// Source inclusion replaces only the PEB observation; validation, UCRT owner
// acquisition, atomic publication and allocation are the production code.
#define processHeap testProcessHeap
#include "../../../../lib/wincrt/aligned_alloc.cpp"
#undef processHeap

int main() {
  void *owner = GetProcessHeap();
  HANDLE foreign = HeapCreate(0, 0, 0);
  assert(owner && foreign && owner != foreign);
  alignas(16) unsigned char forged[64] = {};
  const uint32_t signature = 0xddeeddee;
  memcpy(forged + 0x10, &signature, sizeof(signature));
  void *invalid[] = {nullptr, reinterpret_cast<void *>(1), foreign, forged};
  const size_t alignments[] = {16, 32, 4096, 1 << 20};
  for (void *candidate : invalid) {
    // A forged PEB value must not become the owner even on first use.
    __atomic_store_n(&AllocationHeap, nullptr, __ATOMIC_RELAXED);
    wincrt::Candidate = candidate;
    for (size_t alignment : alignments) {
      void *output = owner;
      errno = EDOM;
      assert(posix_memalign(&output, alignment, 17) == ENOMEM);
      assert(output == owner && errno == EDOM);
      assert(__atomic_load_n(&AllocationHeap, __ATOMIC_RELAXED) == owner);
      errno = EDOM;
      assert(aligned_alloc(alignment, 17) == nullptr);
      assert(errno == EINVAL);
      assert(__atomic_load_n(&AllocationHeap, __ATOMIC_RELAXED) == owner);
    }
    // Recovery of the observation uses the original UCRT owner, not the last
    // candidate. Fundamental alignment works independently of private support.
    wincrt::Candidate = owner;
    void *p = aligned_alloc(16, 17);
    assert(p && reinterpret_cast<uintptr_t>(p) % 16 == 0);
    assert(HeapValidate(owner, 0, p));
    free(p);
    wincrt::Candidate = candidate;
    errno = EDOM;
    assert(aligned_alloc(16, 17) == nullptr && errno == EINVAL);
  }
  // Alignment errors are distinct even while the observed heap is invalid.
  wincrt::Candidate = nullptr;
  void *output = owner;
  errno = EDOM;
  assert(posix_memalign(&output, 4, 17) == EINVAL);
  assert(output == owner && errno == EDOM);
  assert(HeapDestroy(foreign));
}
