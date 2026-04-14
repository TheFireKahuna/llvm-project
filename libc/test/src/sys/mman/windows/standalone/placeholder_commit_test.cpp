// Benchmark: MBI-informed teardown vs blind teardown for MAP_FIXED.
//
// Current code: decommit → punch → release (always, regardless of state)
// Proposed: query MBI → skip decommit if already clean → punch
//
// Also benchmarks the lock-free MAP_FIXED path:
//   PRESERVE_PLACEHOLDER → REPLACE_PLACEHOLDER (no lock, no VEH guard)
//
// Build with -O2:
//   CLANG=F:/Git/llvm/llvm-project/build-wi/bin/clang.exe
//   $CLANG -target x86_64-unknown-windows-itanium -std=c++17 -fno-exceptions \
//     -fno-rtti -O2 -c placeholder_commit_test.cpp -o /tmp/ph_bench.o
//   $CLANG -target x86_64-unknown-windows-itanium -fuse-ld=lld \
//     /tmp/ph_bench.o -lucrt -lmsvcrt -lkernel32 -lntdll \
//     -llegacy_stdio_definitions -o /tmp/ph_bench.exe

extern "C" int printf(const char *, ...);
extern "C" int puts(const char *);
extern "C" void *memset(void *, int, unsigned long long);

using PVOID = void *;
using SIZE_T = unsigned long long;
using ULONG = unsigned long;
using NTSTATUS = long;
using HANDLE = void *;
using ULONG_PTR = unsigned long long;
using DWORD = unsigned long;
using LONGLONG = long long;
using BOOL = int;

#define NT_SUCCESS(st) ((st) >= 0)

#define MEM_COMMIT              0x00001000
#define MEM_RESERVE             0x00002000
#define MEM_DECOMMIT            0x00004000
#define MEM_RELEASE             0x00008000
#define MEM_FREE_STATE          0x00010000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001

#define PAGE_NOACCESS           0x01
#define PAGE_READWRITE          0x04

#define MemoryBasicInformation  0

struct LARGE_INTEGER { LONGLONG QuadPart; };
struct MEM_EXTENDED_PARAMETER {
  struct { ULONG_PTR Type : 8; ULONG_PTR Reserved : 56; };
  union { ULONG_PTR ULong64; PVOID Pointer; SIZE_T Size; HANDLE Handle; ULONG ULong; };
};

struct MEMORY_BASIC_INFORMATION {
  PVOID BaseAddress;
  PVOID AllocationBase;
  DWORD AllocationProtect;
  unsigned short PartitionId;
  SIZE_T RegionSize;
  DWORD State;
  DWORD Protect;
  DWORD Type;
};

extern "C" {
NTSTATUS __stdcall NtAllocateVirtualMemoryEx(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
NTSTATUS __stdcall NtFreeVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG);
NTSTATUS __stdcall NtQueryVirtualMemory(HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);
BOOLEAN __stdcall RtlQueryPerformanceCounter(LARGE_INTEGER *);
BOOLEAN __stdcall RtlQueryPerformanceFrequency(LARGE_INTEGER *);
}

inline HANDLE NtCurrentProcess() { return reinterpret_cast<HANDLE>(-1LL); }

static LONGLONG g_freq;
static inline LONGLONG now_ticks() {
  LARGE_INTEGER t; RtlQueryPerformanceCounter(&t); return t.QuadPart;
}
static inline long long ticks_to_ns(LONGLONG ticks) {
  return (ticks * 1000000000LL) / g_freq;
}
static void sort_i64(long long *a, int n) {
  for (int i = 1; i < n; i++) {
    long long k = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > k) { a[j+1] = a[j]; j--; }
    a[j+1] = k;
  }
}

constexpr int WARMUP = 50;
constexpr int ITERS = 500;
static long long timings[ITERS];
constexpr SIZE_T PAGE = 4096;

static void report(const char *name) {
  sort_i64(timings, ITERS);
  long long sum = 0;
  for (int i = 0; i < ITERS; i++) sum += timings[i];
  printf("  %-62s min=%6lld med=%6lld p99=%6lld ns\n",
         name, timings[0], timings[ITERS/2], timings[(int)(ITERS*0.99)]);
}

static void release(PVOID p) {
  PVOID b = p; SIZE_T s = 0;
  ::NtFreeVirtualMemory(NtCurrentProcess(), &b, &s, MEM_RELEASE);
}

// ── Allocation helpers ──────────────────────────────────────────────

static void *mmap_alloc(SIZE_T size) {
  HANDLE proc = NtCurrentProcess();
  PVOID b = nullptr; SIZE_T s = size;
  ::NtAllocateVirtualMemoryEx(proc, &b, &s,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  if (!b) return nullptr;
  PVOID r = b; SIZE_T rs = s;
  NTSTATUS st = ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
      PAGE_READWRITE, nullptr, 0);
  if (!NT_SUCCESS(st)) { release(b); return nullptr; }
  return b;
}

int main() {
  LARGE_INTEGER f; RtlQueryPerformanceFrequency(&f); g_freq = f.QuadPart;
  HANDLE proc = NtCurrentProcess();

  puts("============================================================");
  puts("  MBI-informed teardown vs blind teardown");
  puts("============================================================");

  // ==================================================================
  // 1. Single-page teardown: dirty vs clean vs decommitted
  //    Measures the raw cost of each teardown step.
  // ==================================================================
  puts("\n--- 1. Single page teardown costs by state ---\n");

  // 1a. Decommit a dirty page
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(PAGE);
    *static_cast<volatile int *>(m) = 0xDEAD; // dirty it
    LONGLONG t0 = now_ticks();
    PVOID d = m; SIZE_T ds = PAGE;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Decommit dirty page");

  // 1b. Decommit a clean (committed but untouched) page
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(PAGE);
    LONGLONG t0 = now_ticks();
    PVOID d = m; SIZE_T ds = PAGE;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Decommit clean page");

  // 1c. Punch dirty page (no decommit first)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(PAGE);
    *static_cast<volatile int *>(m) = 0xDEAD;
    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = PAGE;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Punch dirty page (no decommit)");

  // 1d. Punch clean page (no decommit first)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(PAGE);
    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = PAGE;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Punch clean page (no decommit)");

  // 1e. Decommit → punch (current path, dirty page)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(PAGE);
    *static_cast<volatile int *>(m) = 0xDEAD;
    LONGLONG t0 = now_ticks();
    PVOID d = m; SIZE_T ds = PAGE;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID p = m; SIZE_T ps = PAGE;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Decommit + punch dirty page (current path)");

  // 1f. Decommit → punch (current path, clean page)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(PAGE);
    LONGLONG t0 = now_ticks();
    PVOID d = m; SIZE_T ds = PAGE;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID p = m; SIZE_T ps = PAGE;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Decommit + punch clean page (current path)");

  // 1g. Punch already-decommitted page
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(PAGE);
    PVOID d = m; SIZE_T ds = PAGE;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT); // pre-decommit
    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = PAGE;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Punch already-decommitted page");

  // ==================================================================
  // 2. MBI query cost
  // ==================================================================
  puts("\n--- 2. MBI query cost ---\n");

  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(64*1024);
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T ret;
    LONGLONG t0 = now_ticks();
    ::NtQueryVirtualMemory(proc, m, MemoryBasicInformation,
        &mbi, sizeof(mbi), &ret);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("NtQueryVirtualMemory (single MBI)");

  // MBI walk: 16 regions (simulating 1MB with mixed state)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(1024*1024);
    char *base = static_cast<char *>(m);
    // Decommit every other 64KB to create mixed state
    for (int r = 0; r < 16; r += 2) {
      PVOID d = base + (SIZE_T)r * 64*1024;
      SIZE_T ds = 64*1024;
      ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    }

    LONGLONG t0 = now_ticks();
    char *cur = base;
    char *end = base + 1024*1024;
    int regions = 0;
    while (cur < end) {
      MEMORY_BASIC_INFORMATION mbi;
      SIZE_T ret;
      ::NtQueryVirtualMemory(proc, cur, MemoryBasicInformation,
          &mbi, sizeof(mbi), &ret);
      cur = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      regions++;
    }
    LONGLONG t1 = now_ticks();

    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("MBI walk 1MB mixed state (~16 regions)");

  // ==================================================================
  // 3. MAP_FIXED teardown: 1MB, various states
  //    Compare: blind (always decommit+punch) vs MBI-informed
  // ==================================================================
  puts("\n--- 3. MAP_FIXED teardown: 1MB region ---\n");

  constexpr SIZE_T REGION = 1024 * 1024;

  // 3a. Blind teardown: all dirty (worst case)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(REGION);
    memset(m, 0xAA, REGION);
    LONGLONG t0 = now_ticks();
    // Blind: decommit all + punch
    PVOID d = m; SIZE_T ds = REGION;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID p = m; SIZE_T ps = REGION;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    // Replace to restore
    PVOID r = m; SIZE_T rs = REGION;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Blind: decommit+punch 1MB all dirty");

  // 3b. Blind teardown: all clean (untouched)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(REGION);
    LONGLONG t0 = now_ticks();
    PVOID d = m; SIZE_T ds = REGION;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID p = m; SIZE_T ps = REGION;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    PVOID r = m; SIZE_T rs = REGION;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Blind: decommit+punch 1MB all clean");

  // 3c. Direct punch (skip decommit): all dirty
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(REGION);
    memset(m, 0xAA, REGION);
    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = REGION;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    PVOID r = m; SIZE_T rs = REGION;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Direct punch 1MB all dirty (no decommit)");

  // 3d. Direct punch: all clean
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(REGION);
    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = REGION;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();
    PVOID r = m; SIZE_T rs = REGION;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Direct punch 1MB all clean (no decommit)");

  // 3e. MBI-informed: 1MB half dirty, half decommitted
  //     Query state, skip decommit on already-decommitted regions
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(REGION);
    char *base = static_cast<char *>(m);
    // Dirty first half
    memset(base, 0xAA, REGION / 2);
    // Decommit second half (simulating freed pages)
    PVOID d2 = base + REGION/2; SIZE_T d2s = REGION/2;
    ::NtFreeVirtualMemory(proc, &d2, &d2s, MEM_DECOMMIT);

    LONGLONG t0 = now_ticks();
    // MBI walk + informed teardown
    char *cur = base;
    char *end_ptr = base + REGION;
    while (cur < end_ptr) {
      MEMORY_BASIC_INFORMATION mbi;
      SIZE_T ret;
      ::NtQueryVirtualMemory(proc, cur, MemoryBasicInformation,
          &mbi, sizeof(mbi), &ret);
      char *region_end = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      if (region_end > end_ptr) region_end = end_ptr;
      SIZE_T chunk = static_cast<SIZE_T>(region_end - cur);

      if (mbi.State == MEM_COMMIT) {
        // Decommit first (cheaper punch on clean pages)
        PVOID dc = cur; SIZE_T dcs = chunk;
        ::NtFreeVirtualMemory(proc, &dc, &dcs, MEM_DECOMMIT);
      }
      // MEM_RESERVE (decommitted): skip decommit, go straight to punch
      cur = region_end;
    }
    // Single punch for entire region
    PVOID p = m; SIZE_T ps = REGION;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();

    PVOID r = m; SIZE_T rs = REGION;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("MBI-informed: 1MB half dirty / half decommitted");

  // 3f. Blind on same mixed state (for comparison)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(REGION);
    char *base = static_cast<char *>(m);
    memset(base, 0xAA, REGION / 2);
    PVOID d2 = base + REGION/2; SIZE_T d2s = REGION/2;
    ::NtFreeVirtualMemory(proc, &d2, &d2s, MEM_DECOMMIT);

    LONGLONG t0 = now_ticks();
    PVOID d = m; SIZE_T ds = REGION;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID p = m; SIZE_T ps = REGION;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    LONGLONG t1 = now_ticks();

    PVOID r = m; SIZE_T rs = REGION;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Blind: decommit+punch 1MB half dirty / half decommitted");

  // ==================================================================
  // 4. Full lock-free MAP_FIXED: end-to-end
  //    PRESERVE_PLACEHOLDER → REPLACE_PLACEHOLDER (CAS model)
  // ==================================================================
  puts("\n--- 4. Lock-free MAP_FIXED: full end-to-end ---\n");

  // 4a. Replace within same AllocationBase (decommit+recommit)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(REGION);
    char *target = static_cast<char *>(m) + REGION/2;
    SIZE_T target_sz = PAGE;
    *static_cast<volatile int *>(static_cast<void *>(target)) = 42;

    LONGLONG t0 = now_ticks();
    PVOID d = target; SIZE_T ds = target_sz;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID c = target; SIZE_T cs = target_sz;
    ::NtAllocateVirtualMemoryEx(proc, &c, &cs,
        MEM_COMMIT, PAGE_READWRITE, nullptr, 0);
    LONGLONG t1 = now_ticks();

    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("MAP_FIXED same-alloc: decommit+recommit 4KB (no lock)");

  // 4b. Lock-free CAS model: PRESERVE → REPLACE (full 64KB)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(64*1024);
    memset(m, 0xBB, 64*1024);

    LONGLONG t0 = now_ticks();
    // Step 1: committed → placeholder (kernel atomic)
    PVOID d = m; SIZE_T ds = 64*1024;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID p = m; SIZE_T ps = 64*1024;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    // Step 2: placeholder → committed (kernel CAS)
    PVOID r = m; SIZE_T rs = 64*1024;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    LONGLONG t1 = now_ticks();

    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Lock-free MAP_FIXED: decommit+preserve+replace 64KB dirty");

  // 4c. Same but clean
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(64*1024);

    LONGLONG t0 = now_ticks();
    PVOID d = m; SIZE_T ds = 64*1024;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID p = m; SIZE_T ps = 64*1024;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    PVOID r = m; SIZE_T rs = 64*1024;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    LONGLONG t1 = now_ticks();

    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Lock-free MAP_FIXED: decommit+preserve+replace 64KB clean");

  // 4d. Direct preserve (skip decommit): clean
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(64*1024);

    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = 64*1024;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    PVOID r = m; SIZE_T rs = 64*1024;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    LONGLONG t1 = now_ticks();

    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Lock-free MAP_FIXED: preserve+replace 64KB clean (no decommit)");

  // 4e. Direct preserve: dirty
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(64*1024);
    memset(m, 0xCC, 64*1024);

    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = 64*1024;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    PVOID r = m; SIZE_T rs = 64*1024;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    LONGLONG t1 = now_ticks();

    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("Lock-free MAP_FIXED: preserve+replace 64KB dirty (no decommit)");

  // ==================================================================
  // 5. Scale: 16MB MAP_FIXED teardown+replace
  // ==================================================================
  puts("\n--- 5. 16MB MAP_FIXED teardown + replace ---\n");

  constexpr SIZE_T BIG = 16 * 1024 * 1024;

  // All dirty, blind
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(BIG);
    memset(m, 0xDD, BIG);
    LONGLONG t0 = now_ticks();
    PVOID d = m; SIZE_T ds = BIG;
    ::NtFreeVirtualMemory(proc, &d, &ds, MEM_DECOMMIT);
    PVOID p = m; SIZE_T ps = BIG;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    PVOID r = m; SIZE_T rs = BIG;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("16MB dirty: decommit + preserve + replace");

  // All dirty, direct preserve (no decommit)
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(BIG);
    memset(m, 0xDD, BIG);
    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = BIG;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    PVOID r = m; SIZE_T rs = BIG;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("16MB dirty: preserve + replace (no decommit)");

  // All clean
  for (int i = -WARMUP; i < ITERS; i++) {
    void *m = mmap_alloc(BIG);
    LONGLONG t0 = now_ticks();
    PVOID p = m; SIZE_T ps = BIG;
    ::NtFreeVirtualMemory(proc, &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    PVOID r = m; SIZE_T rs = BIG;
    ::NtAllocateVirtualMemoryEx(proc, &r, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE, nullptr, 0);
    LONGLONG t1 = now_ticks();
    release(m);
    if (i >= 0) timings[i] = ticks_to_ns(t1 - t0);
  }
  report("16MB clean: preserve + replace");

  printf("\nDone.\n");
  return 0;
}
