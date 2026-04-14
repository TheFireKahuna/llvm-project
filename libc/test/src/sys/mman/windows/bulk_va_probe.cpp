//===-- Comprehensive NtPssCaptureVaSpaceBulk probe -----------------------===//
//
// Pure NT test — no Win32 wrappers. Validates:
//   T1: Basic call, header layout, MBI offset at sizeof(header)
//   T2: Cross-check all entries against NtQueryVirtualMemory
//   T3: Range-bounded BaseAddress behavior (mid-region, exact boundary)
//   T4: STATUS_BUFFER_OVERFLOW partial fill + NextValidAddress validity
//   T5: Placeholder regions (MEM_RESERVE_PLACEHOLDER) visibility
//   T6: Self-referential query (buffer appears in own results)
//   T7: Full pagination walk — covers entire VA space
//   T8: Performance comparison vs NtQueryVirtualMemory loop
//
// Build:
//   clang++ --target=x86_64-pc-windows-msvc -fms-extensions \
//     -fms-compatibility -fexceptions -c bulk_va_probe.cpp
//   lld-link bulk_va_probe.obj ntdll.lib -defaultlib:libcmt -out:probe.exe
//
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <cstdint>
#include <cstring>

//===----------------------------------------------------------------------===//
// NT type declarations — no SDK headers
//===----------------------------------------------------------------------===//

typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef void *HANDLE;
typedef void *PVOID;
typedef unsigned __int64 SIZE_T;
typedef SIZE_T *PSIZE_T;
typedef unsigned char BYTE;
typedef unsigned __int64 ULONG_PTR;

#define NTAPI __stdcall
#define NtCurrentProcess() ((HANDLE)(long long)-1)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

// NTSTATUS codes
#define STATUS_SUCCESS             ((NTSTATUS)0x00000000)
#define STATUS_BUFFER_OVERFLOW     ((NTSTATUS)0x80000005)
#define STATUS_BUFFER_TOO_SMALL    ((NTSTATUS)0xC0000023)
#define STATUS_INVALID_PARAMETER   ((NTSTATUS)0xC000000D)

// Memory constants
#define MEM_COMMIT                 0x00001000
#define MEM_RESERVE                0x00002000
#define MEM_DECOMMIT               0x00004000
#define MEM_RELEASE                0x00008000
#define MEM_FREE                   0x00010000
#define MEM_PRIVATE                0x00020000
#define MEM_MAPPED                 0x00040000
#define MEM_RESERVE_PLACEHOLDER    0x00040000
#define MEM_REPLACE_PLACEHOLDER    0x00004000
#define MEM_PRESERVE_PLACEHOLDER   0x00000002
#define PAGE_NOACCESS              0x01
#define PAGE_READWRITE             0x04

#define MEMORY_BULK_INFORMATION_FLAG_BASIC 0x1

struct MEMORY_BASIC_INFORMATION {
  PVOID BaseAddress;
  PVOID AllocationBase;
  ULONG AllocationProtect;
  USHORT PartitionId;
  USHORT Reserved;
  SIZE_T RegionSize;
  ULONG State;
  ULONG Protect;
  ULONG Type;
  ULONG Pad;
};

static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 0x30,
              "MBI must be 48 bytes on x64");

struct NTPSS_MEMORY_BULK_INFORMATION {
  ULONG QueryFlags;
  ULONG NumberOfEntries;
  PVOID NextValidAddress;
};

static_assert(sizeof(NTPSS_MEMORY_BULK_INFORMATION) == 0x10,
              "Bulk header must be 16 bytes");

// Memory info classes
enum { MemoryBasicInformation = 0 };

// MEM_EXTENDED_PARAMETER
struct MEM_EXTENDED_PARAMETER {
  struct { ULONG_PTR Type : 8; ULONG_PTR Reserved : 56; };
  union { ULONG_PTR ULong64; PVOID Pointer; SIZE_T Size; HANDLE Handle; ULONG ULong; };
};

//===----------------------------------------------------------------------===//
// NT imports
//===----------------------------------------------------------------------===//

extern "C" {

__declspec(dllimport) NTSTATUS NTAPI
NtPssCaptureVaSpaceBulk(HANDLE ProcessHandle, PVOID BaseAddress,
                         NTPSS_MEMORY_BULK_INFORMATION *BulkInformation,
                         SIZE_T BulkInformationLength, PSIZE_T ReturnLength);

__declspec(dllimport) NTSTATUS NTAPI
NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                     int MemoryInformationClass, PVOID MemoryInformation,
                     SIZE_T MemoryInformationLength, PSIZE_T ReturnLength);

__declspec(dllimport) NTSTATUS NTAPI
NtAllocateVirtualMemoryEx(HANDLE ProcessHandle, PVOID *BaseAddress,
                           PSIZE_T RegionSize, ULONG AllocationType,
                           ULONG PageProtection,
                           MEM_EXTENDED_PARAMETER *ExtendedParameters,
                           ULONG ExtendedParameterCount);

__declspec(dllimport) NTSTATUS NTAPI
NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                     PSIZE_T RegionSize, ULONG FreeType);

__declspec(dllimport) NTSTATUS NTAPI
NtQueryPerformanceCounter(long long *PerformanceCounter,
                          long long *PerformanceFrequency);

} // extern "C"

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

static MEMORY_BASIC_INFORMATION *bulk_entries(NTPSS_MEMORY_BULK_INFORMATION *h) {
  return reinterpret_cast<MEMORY_BASIC_INFORMATION *>(h + 1);
}

static void hexdump(const void *data, SIZE_T len) {
  auto *p = static_cast<const BYTE *>(data);
  for (SIZE_T i = 0; i < len; i += 16) {
    printf("  %04llx: ", (unsigned long long)i);
    for (SIZE_T j = 0; j < 16 && i + j < len; ++j)
      printf("%02x ", p[i + j]);
    printf("\n");
  }
}

static void *alloc_buf(SIZE_T size) {
  // Placeholder → commit (matches our libc pattern).
  PVOID base = nullptr;
  SIZE_T actual = size;
  NTSTATUS st = NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &base, &actual,
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, nullptr, 0);
  return NT_SUCCESS(st) ? base : nullptr;
}

static void free_buf(void *p) {
  PVOID base = p;
  SIZE_T sz = 0;
  NtFreeVirtualMemory(NtCurrentProcess(), &base, &sz, MEM_RELEASE);
}

static bool query_mbi(PVOID addr, MEMORY_BASIC_INFORMATION &mbi) {
  SIZE_T ret = 0;
  return NT_SUCCESS(NtQueryVirtualMemory(NtCurrentProcess(), addr,
                                          MemoryBasicInformation,
                                          &mbi, sizeof(mbi), &ret));
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
  if (cond) { ++g_pass; } else { \
    ++g_fail; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
  } \
} while(0)

//===----------------------------------------------------------------------===//
// T1: Basic call, header layout confirmation
//===----------------------------------------------------------------------===//

static void test_basic_layout() {
  printf("\n=== T1: Basic layout ===\n");

  constexpr SIZE_T BUF_SIZE = 0x10000;
  auto *buf = static_cast<BYTE *>(alloc_buf(BUF_SIZE));
  CHECK(buf != nullptr, "alloc_buf");
  if (!buf) return;

  memset(buf, 0xCC, BUF_SIZE);
  auto *hdr = reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
  hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;

  SIZE_T ret_len = 0;
  NTSTATUS st = NtPssCaptureVaSpaceBulk(NtCurrentProcess(), nullptr,
                                         hdr, BUF_SIZE, &ret_len);

  CHECK(NT_SUCCESS(st), "status=0x%08lx", st);
  CHECK(hdr->NumberOfEntries > 0, "no entries");

  // Verify MBI offset: ret_len == sizeof(header) + N * sizeof(MBI)
  SIZE_T expected = sizeof(NTPSS_MEMORY_BULK_INFORMATION) +
                    (SIZE_T)hdr->NumberOfEntries * sizeof(MEMORY_BASIC_INFORMATION);
  CHECK(expected == ret_len,
        "offset mismatch: expected=0x%llx actual=0x%llx",
        (unsigned long long)expected, (unsigned long long)ret_len);

  printf("  Entries: %lu  RetLen: 0x%llx  NextValid: %p\n",
         hdr->NumberOfEntries, (unsigned long long)ret_len,
         hdr->NextValidAddress);

  // Verify no 0xCC poison leaks into header fields.
  CHECK(hdr->QueryFlags == MEMORY_BULK_INFORMATION_FLAG_BASIC,
        "QueryFlags clobbered: 0x%lx", hdr->QueryFlags);

  free_buf(buf);
}

//===----------------------------------------------------------------------===//
// T2: Cross-check every entry against NtQueryVirtualMemory
//===----------------------------------------------------------------------===//

static void test_cross_check() {
  printf("\n=== T2: Cross-check all entries ===\n");

  constexpr SIZE_T BUF_SIZE = 0x80000; // 512KB — plenty
  auto *buf = static_cast<BYTE *>(alloc_buf(BUF_SIZE));
  if (!buf) { printf("  SKIP: alloc failed\n"); return; }

  auto *hdr = reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
  hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
  SIZE_T ret_len = 0;
  NTSTATUS st = NtPssCaptureVaSpaceBulk(NtCurrentProcess(), nullptr,
                                         hdr, BUF_SIZE, &ret_len);
  if (!NT_SUCCESS(st)) { printf("  SKIP: bulk failed\n"); free_buf(buf); return; }

  auto *entries = bulk_entries(hdr);
  int ok = 0, bad = 0;
  for (ULONG i = 0; i < hdr->NumberOfEntries; ++i) {
    MEMORY_BASIC_INFORMATION d = {};
    if (!query_mbi(entries[i].BaseAddress, d)) { ++bad; continue; }

    if (entries[i].BaseAddress != d.BaseAddress ||
        entries[i].AllocationBase != d.AllocationBase ||
        entries[i].RegionSize != d.RegionSize ||
        entries[i].State != d.State ||
        entries[i].Type != d.Type) {
      if (bad < 3) {
        printf("  [%lu] MISMATCH Base=%p vs %p Size=0x%llx vs 0x%llx\n",
               i, entries[i].BaseAddress, d.BaseAddress,
               (unsigned long long)entries[i].RegionSize,
               (unsigned long long)d.RegionSize);
      }
      ++bad;
    } else {
      ++ok;
    }
  }

  CHECK(bad == 0, "%d mismatches out of %lu entries", bad, hdr->NumberOfEntries);
  printf("  %d/%lu entries matched\n", ok, hdr->NumberOfEntries);

  free_buf(buf);
}

//===----------------------------------------------------------------------===//
// T3: Range-bounded BaseAddress behavior
//===----------------------------------------------------------------------===//

static void test_base_address() {
  printf("\n=== T3: BaseAddress behavior ===\n");

  constexpr SIZE_T BUF_SIZE = 0x10000;
  auto *buf = static_cast<BYTE *>(alloc_buf(BUF_SIZE));
  if (!buf) { printf("  SKIP: alloc\n"); return; }

  // First, do a full query to find a multi-page committed region.
  auto *hdr = reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
  hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
  SIZE_T ret_len = 0;
  NtPssCaptureVaSpaceBulk(NtCurrentProcess(), nullptr, hdr, BUF_SIZE, &ret_len);

  auto *entries = bulk_entries(hdr);
  PVOID mid_addr = nullptr;
  PVOID region_base = nullptr;
  for (ULONG i = 0; i < hdr->NumberOfEntries; ++i) {
    if (entries[i].State == MEM_COMMIT && entries[i].RegionSize >= 0x10000) {
      region_base = entries[i].BaseAddress;
      mid_addr = static_cast<char *>(region_base) + 0x1000;
      break;
    }
  }

  if (!mid_addr) { printf("  SKIP: no suitable region\n"); free_buf(buf); return; }

  printf("  Testing with region Base=%p, mid=%p\n", region_base, mid_addr);

  // T3a: BaseAddress = exact region start — should return that region first.
  hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
  NtPssCaptureVaSpaceBulk(NtCurrentProcess(), region_base, hdr, BUF_SIZE, &ret_len);
  entries = bulk_entries(hdr);
  CHECK(hdr->NumberOfEntries > 0 && entries[0].BaseAddress == region_base,
        "exact base: first entry Base=%p expected %p",
        hdr->NumberOfEntries > 0 ? entries[0].BaseAddress : nullptr, region_base);

  // T3b: BaseAddress = mid-region — does it snap to region start or skip?
  hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
  NtPssCaptureVaSpaceBulk(NtCurrentProcess(), mid_addr, hdr, BUF_SIZE, &ret_len);
  entries = bulk_entries(hdr);
  if (hdr->NumberOfEntries > 0) {
    PVOID first = entries[0].BaseAddress;
    if (first == region_base)
      printf("  Mid-region snaps to containing region start (%p)\n", first);
    else if (first == mid_addr)
      printf("  Mid-region splits at query point (%p)\n", first);
    else if (first > mid_addr)
      printf("  Mid-region skips to next region (%p, past %p)\n", first, mid_addr);
    else
      printf("  Mid-region unexpected: first=%p mid=%p\n", first, mid_addr);
  }

  // T3c: BaseAddress past all regions — should return 0 entries.
  PVOID high = reinterpret_cast<PVOID>(0x00007FFFFFFF0000ULL);
  hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
  NTSTATUS st = NtPssCaptureVaSpaceBulk(NtCurrentProcess(), high, hdr, BUF_SIZE, &ret_len);
  printf("  BaseAddress=max: status=0x%08lx entries=%lu\n", st, hdr->NumberOfEntries);

  free_buf(buf);
}

//===----------------------------------------------------------------------===//
// T4: STATUS_BUFFER_OVERFLOW partial fill
//===----------------------------------------------------------------------===//

static void test_overflow() {
  printf("\n=== T4: Buffer overflow / partial fill ===\n");

  // Tiny buffer: header + space for exactly 2 MBI entries.
  SIZE_T tiny_size = sizeof(NTPSS_MEMORY_BULK_INFORMATION) +
                     sizeof(MEMORY_BASIC_INFORMATION) * 2;
  auto *buf = static_cast<BYTE *>(alloc_buf(0x10000)); // alloc granularity
  if (!buf) { printf("  SKIP: alloc\n"); return; }

  auto *hdr = reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
  hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
  SIZE_T ret_len = 0;
  NTSTATUS st = NtPssCaptureVaSpaceBulk(NtCurrentProcess(), nullptr,
                                         hdr, tiny_size, &ret_len);

  printf("  Status: 0x%08lx  Entries: %lu  NextValid: %p  RetLen: 0x%llx\n",
         st, hdr->NumberOfEntries, hdr->NextValidAddress,
         (unsigned long long)ret_len);

  bool is_overflow = (st == STATUS_BUFFER_OVERFLOW);
  CHECK(is_overflow || NT_SUCCESS(st),
        "unexpected status 0x%08lx", st);

  if (is_overflow) {
    CHECK(hdr->NumberOfEntries <= 2,
          "overflow returned more than buffer capacity: %lu", hdr->NumberOfEntries);
    CHECK(hdr->NextValidAddress != nullptr,
          "overflow but NextValidAddress is null");

    // Validate the returned entries are real.
    auto *entries = bulk_entries(hdr);
    for (ULONG i = 0; i < hdr->NumberOfEntries; ++i) {
      MEMORY_BASIC_INFORMATION d = {};
      bool ok = query_mbi(entries[i].BaseAddress, d);
      CHECK(ok && entries[i].BaseAddress == d.BaseAddress,
            "overflow entry[%lu] invalid: Base=%p", i, entries[i].BaseAddress);
    }
    printf("  Partial fill entries are valid\n");

    // Resume from NextValidAddress — should succeed.
    PVOID resume = hdr->NextValidAddress;
    hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
    SIZE_T ret2 = 0;
    NTSTATUS st2 = NtPssCaptureVaSpaceBulk(NtCurrentProcess(), resume,
                                             hdr, 0x10000, &ret2);
    CHECK(NT_SUCCESS(st2) || st2 == STATUS_BUFFER_OVERFLOW,
          "resume failed: 0x%08lx", st2);
    printf("  Resume from %p: status=0x%08lx entries=%lu\n",
           resume, st2, hdr->NumberOfEntries);
  }

  free_buf(buf);
}

//===----------------------------------------------------------------------===//
// T5: Placeholder visibility
//===----------------------------------------------------------------------===//

static void test_placeholders() {
  printf("\n=== T5: Placeholder visibility ===\n");

  // Create a placeholder.
  PVOID ph_base = nullptr;
  SIZE_T ph_size = 0x10000; // 64KB
  NTSTATUS st = NtAllocateVirtualMemoryEx(
      NtCurrentProcess(), &ph_base, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  if (!NT_SUCCESS(st)) {
    printf("  SKIP: placeholder alloc failed 0x%08lx\n", st);
    return;
  }
  printf("  Placeholder at %p size=0x%llx\n", ph_base,
         (unsigned long long)ph_size);

  // Query it directly first.
  MEMORY_BASIC_INFORMATION ph_mbi = {};
  query_mbi(ph_base, ph_mbi);
  printf("  Direct MBI: State=0x%lx Type=0x%lx Protect=0x%lx\n",
         ph_mbi.State, ph_mbi.Type, ph_mbi.Protect);

  // Bulk query — find the placeholder.
  constexpr SIZE_T BUF_SIZE = 0x10000;
  auto *buf = static_cast<BYTE *>(alloc_buf(BUF_SIZE));
  if (!buf) { printf("  SKIP: alloc\n"); goto cleanup; }

  {
    auto *hdr = reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
    hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
    SIZE_T ret_len = 0;
    // Start from just before the placeholder.
    NtPssCaptureVaSpaceBulk(NtCurrentProcess(), ph_base, hdr, BUF_SIZE, &ret_len);

    auto *entries = bulk_entries(hdr);
    bool found = false;
    for (ULONG i = 0; i < hdr->NumberOfEntries && i < 10; ++i) {
      if (entries[i].BaseAddress == ph_base) {
        printf("  Bulk entry: State=0x%lx Type=0x%lx Protect=0x%lx Size=0x%llx\n",
               entries[i].State, entries[i].Type, entries[i].Protect,
               (unsigned long long)entries[i].RegionSize);
        CHECK(entries[i].State == ph_mbi.State &&
              entries[i].Type == ph_mbi.Type,
              "placeholder entry mismatch vs direct query");
        found = true;
        break;
      }
    }
    CHECK(found, "placeholder not found in bulk results");

    free_buf(buf);
  }

cleanup:
  // Release placeholder.
  PVOID rel_base = ph_base;
  SIZE_T rel_size = 0;
  NtFreeVirtualMemory(NtCurrentProcess(), &rel_base, &rel_size, MEM_RELEASE);
}

//===----------------------------------------------------------------------===//
// T6: Self-referential query (buffer in results)
//===----------------------------------------------------------------------===//

static void test_self_reference() {
  printf("\n=== T6: Self-referential query ===\n");

  constexpr SIZE_T BUF_SIZE = 0x10000;
  auto *buf = static_cast<BYTE *>(alloc_buf(BUF_SIZE));
  if (!buf) { printf("  SKIP: alloc\n"); return; }

  auto *hdr = reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
  hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;

  // Query starting from the buffer's own address.
  SIZE_T ret_len = 0;
  NtPssCaptureVaSpaceBulk(NtCurrentProcess(), buf, hdr, BUF_SIZE, &ret_len);

  auto *entries = bulk_entries(hdr);
  bool found = false;
  for (ULONG i = 0; i < hdr->NumberOfEntries && i < 20; ++i) {
    char *base = static_cast<char *>(entries[i].BaseAddress);
    char *end = base + entries[i].RegionSize;
    if (base <= static_cast<char *>(static_cast<void *>(buf)) &&
        end > static_cast<char *>(static_cast<void *>(buf))) {
      printf("  Buffer found in entry[%lu]: Base=%p Size=0x%llx State=0x%lx\n",
             i, entries[i].BaseAddress,
             (unsigned long long)entries[i].RegionSize, entries[i].State);
      found = true;
      break;
    }
  }
  CHECK(found, "buffer not found in own results");

  free_buf(buf);
}

//===----------------------------------------------------------------------===//
// T7: Full pagination walk — count total entries
//===----------------------------------------------------------------------===//

static void test_full_pagination() {
  printf("\n=== T7: Full pagination walk ===\n");

  constexpr SIZE_T BUF_SIZE = 0x10000;
  auto *buf = static_cast<BYTE *>(alloc_buf(BUF_SIZE));
  if (!buf) { printf("  SKIP: alloc\n"); return; }

  auto *hdr = reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
  PVOID cursor = nullptr;
  ULONG total = 0;
  int pages = 0;

  for (;;) {
    hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
    SIZE_T ret_len = 0;
    NTSTATUS st = NtPssCaptureVaSpaceBulk(NtCurrentProcess(), cursor,
                                           hdr, BUF_SIZE, &ret_len);
    if (!NT_SUCCESS(st) && st != STATUS_BUFFER_OVERFLOW)
      break;

    total += hdr->NumberOfEntries;
    ++pages;

    if (hdr->NumberOfEntries == 0 || hdr->NextValidAddress == nullptr)
      break;

    // Stop if NextValidAddress didn't advance (prevent infinite loop).
    if (hdr->NextValidAddress <= cursor)
      break;

    cursor = hdr->NextValidAddress;

    // Safety: if we've done many pages, something is wrong.
    if (pages > 1000) break;
  }

  // Compare against direct NtQueryVirtualMemory walk.
  ULONG direct_count = 0;
  {
    char *addr = nullptr;
    for (;;) {
      MEMORY_BASIC_INFORMATION mbi = {};
      if (!query_mbi(addr, mbi)) break;
      ++direct_count;
      char *next = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      if (next <= addr) break;
      addr = next;
    }
  }

  printf("  Bulk: %lu entries in %d pages\n", total, pages);
  printf("  Direct walk: %lu entries\n", direct_count);
  // Allow small difference — concurrent allocations can shift counts.
  int diff = (int)total - (int)direct_count;
  CHECK(diff >= -5 && diff <= 5,
        "entry count divergence: bulk=%lu direct=%lu diff=%d",
        total, direct_count, diff);

  free_buf(buf);
}

//===----------------------------------------------------------------------===//
// T8: Performance comparison
//===----------------------------------------------------------------------===//

static void test_performance() {
  printf("\n=== T8: Performance comparison ===\n");

  constexpr SIZE_T BUF_SIZE = 0x80000;
  auto *buf = static_cast<BYTE *>(alloc_buf(BUF_SIZE));
  if (!buf) { printf("  SKIP: alloc\n"); return; }

  long long freq = 0;
  long long t0, t1, t2;
  NtQueryPerformanceCounter(&freq, nullptr); // get dummy to prime
  NtQueryPerformanceCounter(&t0, &freq);

  // Bulk: single call.
  constexpr int ITERS = 100;
  ULONG bulk_count = 0;
  NtQueryPerformanceCounter(&t0, nullptr);
  for (int iter = 0; iter < ITERS; ++iter) {
    auto *hdr = reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf);
    hdr->QueryFlags = MEMORY_BULK_INFORMATION_FLAG_BASIC;
    SIZE_T ret_len = 0;
    NtPssCaptureVaSpaceBulk(NtCurrentProcess(), nullptr, hdr, BUF_SIZE, &ret_len);
    bulk_count = hdr->NumberOfEntries;
  }
  NtQueryPerformanceCounter(&t1, nullptr);

  // Direct: NtQueryVirtualMemory loop.
  ULONG direct_count = 0;
  for (int iter = 0; iter < ITERS; ++iter) {
    direct_count = 0;
    char *addr = nullptr;
    for (;;) {
      MEMORY_BASIC_INFORMATION mbi = {};
      if (!query_mbi(addr, mbi)) break;
      ++direct_count;
      char *next = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
      if (next <= addr) break;
      addr = next;
    }
  }
  NtQueryPerformanceCounter(&t2, nullptr);

  double bulk_us = (double)(t1 - t0) / (double)freq * 1e6 / ITERS;
  double direct_us = (double)(t2 - t1) / (double)freq * 1e6 / ITERS;

  printf("  Bulk:   %.1f us/call (%lu entries)\n", bulk_us, bulk_count);
  printf("  Direct: %.1f us/call (%lu entries)\n", direct_us, direct_count);
  printf("  Speedup: %.1fx\n", direct_us / bulk_us);

  free_buf(buf);
}

//===----------------------------------------------------------------------===//
// Main
//===----------------------------------------------------------------------===//

int main() {
  test_basic_layout();
  test_cross_check();
  test_base_address();
  test_overflow();
  test_placeholders();
  test_self_reference();
  test_full_pagination();
  test_performance();

  printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
