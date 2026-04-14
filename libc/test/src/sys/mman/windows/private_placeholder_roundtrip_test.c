// private_placeholder_roundtrip_test.c — Explore MEM_PRIVATE ↔ placeholder
// transitions at various granularities.
//
// Questions:
//   1. Can preserve_to_placeholder work on sub-ranges? (expect: no)
//   2. Can it work after decommit of a sub-range? (expect: no)
//   3. What about decommit whole + preserve whole?
//   4. Can we punch a placeholder HOLE in a committed allocation?
//      (MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER on a committed sub-range)
//   5. What if we decommit a sub-range first, then punch?
//   6. Can we split a MEM_PRIVATE allocation via MEM_PRESERVE_PLACEHOLDER
//      the same way we split placeholders?
//   7. What's the actual difference between:
//      a) decommit+recommit (current inplace)
//      b) preserve_to_placeholder+replace_placeholder_commit (replace path)
//      c) any hybrid?
//   8. After MEM_REPLACE_PLACEHOLDER creates MEM_PRIVATE, can the resulting
//      allocation be split via MEM_PRESERVE_PLACEHOLDER?

#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef void *PVOID;
typedef void *HANDLE;
typedef unsigned long ULONG;
typedef unsigned long DWORD;
typedef unsigned long long SIZE_T;
typedef unsigned long long ULONG_PTR;
typedef long NTSTATUS;
typedef long LONG;

#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

typedef union {
  struct { ULONG LowPart; LONG HighPart; };
  long long QuadPart;
} LARGE_INTEGER;

typedef struct {
  PVOID BaseAddress;
  PVOID AllocationBase;
  DWORD AllocationProtect;
  unsigned short PartitionId;
  unsigned short Alignment;
  SIZE_T RegionSize;
  DWORD State;
  DWORD Protect;
  DWORD Type;
} MEMORY_BASIC_INFORMATION;

typedef struct {
  PVOID AllocationBase;
  ULONG AllocationProtect;
  union {
    ULONG RegionType;
    struct {
      ULONG Private : 1;
      ULONG MappedDataFile : 1;
      ULONG MappedImage : 1;
      ULONG MappedPageFile : 1;
      ULONG MappedPhysical : 1;
      ULONG DirectMapped : 1;
      ULONG SoftwareEnclave : 1;
      ULONG PageSize64K : 1;
      ULONG PlaceholderReservation : 1;
      ULONG Reserved2 : 23;
    };
  };
  SIZE_T RegionSize;
  SIZE_T CommitSize;
  ULONG_PTR PartitionId;
  ULONG_PTR NodePreference;
} MEMORY_REGION_INFORMATION;

#define MEM_COMMIT              0x00001000
#define MEM_RESERVE             0x00002000
#define MEM_DECOMMIT            0x00004000
#define MEM_FREE                0x00010000
#define MEM_PRIVATE             0x00020000
#define MEM_RELEASE             0x00008000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001

#define PAGE_NOACCESS   0x01
#define PAGE_READWRITE  0x04

#define MemoryBasicInformation    0
#define MemoryRegionInformationEx 7

#define PAGE_SIZE  4096ULL
#define ALLOC_GRAN 65536ULL

static HANDLE self(void) { return (HANDLE)(long long)(-1); }

extern NTSTATUS NTAPI NtAllocateVirtualMemoryEx(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtFreeVirtualMemory(
    HANDLE, PVOID *, SIZE_T *, ULONG);
extern NTSTATUS NTAPI NtQueryVirtualMemory(
    HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, fmt, ...) do { \
  if (cond) { pass_count++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } \
  else { fail_count++; printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

#define NOTE(fmt, ...) printf("  NOTE: " fmt "\n", ##__VA_ARGS__)

static int query_mbi(void *addr, MEMORY_BASIC_INFORMATION *mbi) {
  return NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryBasicInformation,
                                         mbi, sizeof(*mbi), NULL));
}

static int is_placeholder(void *addr) {
  MEMORY_REGION_INFORMATION mri;
  if (!NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryRegionInformationEx,
                                        &mri, sizeof(mri), NULL)))
    return 0;
  return mri.PlaceholderReservation;
}

static void dump(const char *label, void *addr) {
  MEMORY_BASIC_INFORMATION mbi;
  if (query_mbi(addr, &mbi))
    printf("    %s: Base=%p AllocBase=%p State=0x%lx Type=0x%lx Prot=0x%lx "
           "Size=%llu ph=%d\n",
           label, mbi.BaseAddress, mbi.AllocationBase,
           (unsigned long)mbi.State, (unsigned long)mbi.Type,
           (unsigned long)mbi.Protect,
           (unsigned long long)mbi.RegionSize, is_placeholder(addr));
}

static const char *st_name(NTSTATUS st) {
  if (st == 0) return "SUCCESS";
  if (st == (NTSTATUS)0xC0000018) return "CONFLICTING_ADDRESSES";
  if (st == (NTSTATUS)0xC000000D) return "INVALID_PARAMETER";
  if (st == (NTSTATUS)0xC00000F1) return "MEMBER_NOT_IN_GROUP";
  if (st == (NTSTATUS)0xC0000045) return "INVALID_PAGE_PROTECTION";
  if (st == (NTSTATUS)0xC0000001) return "UNSUCCESSFUL";
  return "OTHER";
}

//=============================================================================
// TEST 1: preserve_to_placeholder on full MEM_PRIVATE allocation
//=============================================================================
static void test_full_preserve(void) {
  printf("\n=== TEST 1: preserve_to_placeholder on full allocation ===\n");

  PVOID ph = NULL;
  SIZE_T sz = ALLOC_GRAN;
  NtAllocateVirtualMemoryEx(self(), &ph, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID addr = ph;
  SIZE_T as = sz;
  NtAllocateVirtualMemoryEx(self(), &addr, &as,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      NULL, 0);

  memset(addr, 0xAA, (size_t)ALLOC_GRAN);
  dump("committed", addr);

  // Preserve full allocation → placeholder
  PVOID base = addr;
  SIZE_T size = ALLOC_GRAN;
  NTSTATUS st = NtFreeVirtualMemory(self(), &base, &size,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  preserve_to_placeholder(full): 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));
  if (NT_SUCCESS(st)) {
    CHECK(1, "Full preserve works");
    dump("preserved", addr);
    CHECK(is_placeholder(addr), "Result is placeholder");

    // Can we re-commit?
    PVOID rc = addr;
    SIZE_T rs = ALLOC_GRAN;
    st = NtAllocateVirtualMemoryEx(self(), &rc, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
        NULL, 0);
    CHECK(NT_SUCCESS(st), "Re-commit after preserve");
    if (NT_SUCCESS(st)) {
      CHECK(((unsigned char *)addr)[0] == 0, "Content zeroed after roundtrip");
    }
  }

  PVOID f = addr; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
}

//=============================================================================
// TEST 2: preserve_to_placeholder on sub-range (mid-allocation)
//=============================================================================
static void test_subrange_preserve(void) {
  printf("\n=== TEST 2: preserve_to_placeholder on sub-range ===\n");

  PVOID ph = NULL;
  SIZE_T sz = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID addr = ph;
  SIZE_T as = sz;
  NtAllocateVirtualMemoryEx(self(), &addr, &as,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      NULL, 0);

  char *base = (char *)addr;

  // Try preserve on page 1 only (not at AllocationBase)
  PVOID p1 = base + PAGE_SIZE;
  SIZE_T s1 = PAGE_SIZE;
  NTSTATUS st = NtFreeVirtualMemory(self(), &p1, &s1,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  preserve(page1, 4KB): 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));

  if (NT_SUCCESS(st)) {
    NOTE("Sub-range preserve WORKS!");
    dump("page0", base);
    dump("page1", base + PAGE_SIZE);
    dump("page2", base + 2 * PAGE_SIZE);
    CHECK(is_placeholder(base + PAGE_SIZE), "Page 1 is placeholder");
    // This would be huge — means we CAN punch placeholder holes
  } else {
    NOTE("Sub-range preserve fails — expected");
  }

  // Try preserve at AllocationBase with sub-range size (split semantics)
  PVOID p0 = base;
  SIZE_T s0 = PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &p0, &s0,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  preserve(AllocBase, 4KB split): 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));

  if (NT_SUCCESS(st)) {
    NOTE("Split-preserve at AllocationBase WORKS on committed MEM_PRIVATE!");
    dump("page0 (split off)", base);
    dump("page1+ (remainder)", base + PAGE_SIZE);
    CHECK(is_placeholder(base), "Page 0 is placeholder after split-preserve");

    // What's the remainder?
    MEMORY_BASIC_INFORMATION mbi;
    query_mbi(base + PAGE_SIZE, &mbi);
    printf("    remainder State=0x%lx Type=0x%lx ph=%d\n",
           (unsigned long)mbi.State, (unsigned long)mbi.Type,
           is_placeholder(base + PAGE_SIZE));
  }

  // Cleanup
  for (SIZE_T i = 0; i < 4; i++) {
    PVOID f = base + i * PAGE_SIZE;
    SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }
}

//=============================================================================
// TEST 3: Decommit sub-range then preserve
//=============================================================================
static void test_decommit_then_preserve(void) {
  printf("\n=== TEST 3: Decommit sub-range then preserve ===\n");

  PVOID ph = NULL;
  SIZE_T sz = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID addr = ph;
  SIZE_T as = sz;
  NtAllocateVirtualMemoryEx(self(), &addr, &as,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      NULL, 0);

  char *base = (char *)addr;

  // Decommit page 1
  PVOID dc = base + PAGE_SIZE;
  SIZE_T ds = PAGE_SIZE;
  NtFreeVirtualMemory(self(), &dc, &ds, MEM_DECOMMIT);
  dump("page1 decommitted", base + PAGE_SIZE);

  // Now try preserve on the decommitted sub-range
  PVOID p = base + PAGE_SIZE;
  SIZE_T ps = PAGE_SIZE;
  NTSTATUS st = NtFreeVirtualMemory(self(), &p, &ps,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  preserve(decommitted page1): 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));

  if (NT_SUCCESS(st)) {
    NOTE("Decommit+preserve WORKS on sub-range!");
    dump("page0", base);
    dump("page1", base + PAGE_SIZE);
    dump("page2", base + 2 * PAGE_SIZE);
  }

  // Try: decommit ALL then preserve ALL
  for (int i = 0; i < 4; i++) {
    PVOID d = base + i * PAGE_SIZE;
    SIZE_T s = PAGE_SIZE;
    NtFreeVirtualMemory(self(), &d, &s, MEM_DECOMMIT);
  }
  PVOID pa = base;
  SIZE_T psa = 4 * PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &pa, &psa,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  preserve(decommit all, full): 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));
  if (NT_SUCCESS(st)) {
    CHECK(is_placeholder(base), "Full decommit+preserve → placeholder");
  }

  PVOID f = base; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
}

//=============================================================================
// TEST 4: Split a committed MEM_PRIVATE allocation via MEM_PRESERVE_PLACEHOLDER
// Like splitting a placeholder, but on committed memory.
// preserve(base, split_offset) should split [base, end) into:
//   [base, base+offset) = placeholder (freed)
//   [base+offset, end)  = ??? (committed? placeholder?)
//=============================================================================
static void test_split_committed(void) {
  printf("\n=== TEST 4: Split committed MEM_PRIVATE via preserve ===\n");

  PVOID ph = NULL;
  SIZE_T sz = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID addr = ph;
  SIZE_T as = sz;
  NtAllocateVirtualMemoryEx(self(), &addr, &as,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      NULL, 0);

  char *base = (char *)addr;
  memset(base, 0xBB, 4 * (size_t)PAGE_SIZE);

  // Split at page 2: [p0-p1][p2-p3]
  PVOID sp = base;
  SIZE_T ss = 2 * PAGE_SIZE;
  NTSTATUS st = NtFreeVirtualMemory(self(), &sp, &ss,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  preserve(base, 2 pages): 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));

  if (NT_SUCCESS(st)) {
    NOTE("Split-preserve on committed memory WORKS!");
    dump("first half (p0-p1)", base);
    dump("second half (p2-p3)", base + 2 * PAGE_SIZE);

    // Is first half a placeholder?
    CHECK(is_placeholder(base), "First half → placeholder");

    // Is second half still committed with content?
    MEMORY_BASIC_INFORMATION mbi;
    query_mbi(base + 2 * PAGE_SIZE, &mbi);
    int still_committed = (mbi.State == MEM_COMMIT);
    int content_ok = ((unsigned char)base[2 * PAGE_SIZE] == 0xBB);
    printf("    second half: committed=%d content_preserved=%d\n",
           still_committed, content_ok);

    if (still_committed && content_ok) {
      CHECK(1, "Split-preserve: first→placeholder, second→committed with content!");
      NOTE("This means we can punch placeholder holes in committed allocations");
      NOTE("and the remaining committed portions keep their content.");
    }

    // Can we coalesce the two halves?
    // First half is placeholder, second half is committed — expect fail
    st = NtFreeVirtualMemory(self(), &sp, &(SIZE_T){4 * PAGE_SIZE},
                              MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    printf("  coalesce(placeholder + committed): 0x%08lX (%s)\n",
           (unsigned long)st, st_name(st));

    // Can we re-fill the placeholder with committed memory?
    if (is_placeholder(base)) {
      PVOID rc = base;
      SIZE_T rs = 2 * PAGE_SIZE;
      st = NtAllocateVirtualMemoryEx(self(), &rc, &rs,
          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
          NULL, 0);
      CHECK(NT_SUCCESS(st), "Re-commit first half via REPLACE_PLACEHOLDER");
    }
  }

  // Cleanup
  for (SIZE_T i = 0; i < 4; i++) {
    PVOID f = base + i * PAGE_SIZE;
    SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }
}

//=============================================================================
// TEST 5: Chain — committed → split-preserve → per-piece operations
// Can we split a 4-page committed allocation into individual pages,
// each becoming a placeholder, then selectively re-commit some?
//=============================================================================
static void test_per_page_split_preserve(void) {
  printf("\n=== TEST 5: Per-page split-preserve chain ===\n");

  PVOID ph = NULL;
  SIZE_T sz = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID addr = ph;
  SIZE_T as = sz;
  NtAllocateVirtualMemoryEx(self(), &addr, &as,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      NULL, 0);

  char *base = (char *)addr;
  for (int i = 0; i < 4; i++)
    memset(base + i * PAGE_SIZE, 0xC0 + i, (size_t)PAGE_SIZE);

  printf("  4 pages committed with patterns C0,C1,C2,C3\n");

  // Split off page 0 → placeholder
  PVOID sp = base;
  SIZE_T ss = PAGE_SIZE;
  NTSTATUS st = NtFreeVirtualMemory(self(), &sp, &ss,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  split-preserve page 0: 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));

  if (!NT_SUCCESS(st)) {
    NOTE("Split-preserve doesn't work — can't chain");
    PVOID f = base; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    return;
  }

  // Page 0 is placeholder, pages 1-3 still committed
  // Split page 1 from pages 1-3
  sp = base + PAGE_SIZE;
  ss = PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &sp, &ss,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  split-preserve page 1: 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));

  // Split page 2 from pages 2-3
  sp = base + 2 * PAGE_SIZE;
  ss = PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &sp, &ss,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  split-preserve page 2: 0x%08lX (%s)\n",
         (unsigned long)st, st_name(st));

  // Page 3 is the remainder — committed or placeholder?
  dump("page 0", base);
  dump("page 1", base + PAGE_SIZE);
  dump("page 2", base + 2 * PAGE_SIZE);
  dump("page 3", base + 3 * PAGE_SIZE);

  // Check content on whatever is still committed
  for (int i = 0; i < 4; i++) {
    MEMORY_BASIC_INFORMATION mbi;
    query_mbi(base + i * PAGE_SIZE, &mbi);
    if (mbi.State == MEM_COMMIT) {
      unsigned char val = ((unsigned char *)base)[i * PAGE_SIZE];
      printf("    page %d content: 0x%02x (expected 0x%02x) %s\n",
             i, val, 0xC0 + i, val == (0xC0 + i) ? "OK" : "WRONG");
    }
  }

  // Re-commit page 0 and 2 as placeholder → committed
  for (int i = 0; i < 3; i += 2) {
    if (is_placeholder(base + i * PAGE_SIZE)) {
      PVOID rc = base + i * PAGE_SIZE;
      SIZE_T rs = PAGE_SIZE;
      st = NtAllocateVirtualMemoryEx(self(), &rc, &rs,
          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
          NULL, 0);
      printf("  re-commit page %d: 0x%08lX (%s)\n", i,
             (unsigned long)st, st_name(st));
    }
  }

  // Cleanup
  for (int i = 0; i < 4; i++) {
    PVOID f = base + i * PAGE_SIZE;
    SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }
}

//=============================================================================
// TEST 6: The big question — can split-preserve replace decommit+recommit
// for the inplace MAP_FIXED path?
// Compare: decommit(sub) + recommit(sub) vs preserve(base,offset) + replace
//=============================================================================
static void test_inplace_comparison(void) {
  printf("\n=== TEST 6: Split-preserve as inplace MAP_FIXED alternative ===\n");

  PVOID ph = NULL;
  SIZE_T sz = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID addr = ph;
  SIZE_T as = sz;
  NtAllocateVirtualMemoryEx(self(), &addr, &as,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      NULL, 0);

  char *base = (char *)addr;
  memset(base, 0xDD, 4 * (size_t)PAGE_SIZE);
  printf("  4 pages committed with 0xDD\n");

  // Scenario: MAP_FIXED on page 1-2 (middle sub-range)
  // Current approach: decommit(page1-2) + recommit(page1-2) → zeroed
  // Alternative: preserve(base, PAGE_SIZE) splits off page 0,
  //              then preserve(page1, 2*PAGE_SIZE) splits off pages 1-2,
  //              then replace(page1-2) with new content.
  // But pages 0 and 3 must keep their content.

  // Step 1: split off page 0 (preserve first PAGE_SIZE)
  PVOID sp = base;
  SIZE_T ss = PAGE_SIZE;
  NTSTATUS st = NtFreeVirtualMemory(self(), &sp, &ss,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  split-preserve page 0: 0x%08lX\n", (unsigned long)st);
  if (!NT_SUCCESS(st)) { NOTE("Can't proceed"); goto cleanup; }

  // Step 2: split off pages 1-2 (preserve 2*PAGE_SIZE from page1 base)
  sp = base + PAGE_SIZE;
  ss = 2 * PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &sp, &ss,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  split-preserve pages 1-2: 0x%08lX\n", (unsigned long)st);
  if (!NT_SUCCESS(st)) { NOTE("Can't proceed"); goto cleanup; }

  // Now: [ph page0][ph pages1-2][committed? page3]
  printf("  After split-preserve:\n");
  dump("page0", base);
  dump("page1", base + PAGE_SIZE);
  dump("page3", base + 3 * PAGE_SIZE);

  // Page 3 should still be committed with 0xDD
  {
    MEMORY_BASIC_INFORMATION mbi;
    query_mbi(base + 3 * PAGE_SIZE, &mbi);
    if (mbi.State == MEM_COMMIT) {
      CHECK(((unsigned char *)base)[3 * PAGE_SIZE] == 0xDD,
            "Page 3 content preserved through split-preserve");
    } else {
      CHECK(0, "Page 3 lost committed state!");
    }
  }

  // Re-commit page 0 (restore it as committed with zero content)
  {
    PVOID rc = base;
    SIZE_T rs = PAGE_SIZE;
    st = NtAllocateVirtualMemoryEx(self(), &rc, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
        NULL, 0);
    printf("  re-commit page 0: 0x%08lX\n", (unsigned long)st);
    if (NT_SUCCESS(st))
      CHECK(((unsigned char *)base)[0] == 0, "Page 0 zeroed after roundtrip");
  }

  // Replace pages 1-2 with new committed (simulating MAP_FIXED content)
  {
    PVOID rc = base + PAGE_SIZE;
    SIZE_T rs = 2 * PAGE_SIZE;
    st = NtAllocateVirtualMemoryEx(self(), &rc, &rs,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
        NULL, 0);
    printf("  replace pages 1-2: 0x%08lX\n", (unsigned long)st);
    if (NT_SUCCESS(st)) {
      memset(base + PAGE_SIZE, 0xEE, 2 * (size_t)PAGE_SIZE);
      CHECK(((unsigned char *)base)[PAGE_SIZE] == 0xEE,
            "Pages 1-2 have new content");
    }
  }

  printf("\n  Final state:\n");
  dump("page0", base);
  dump("page1", base + PAGE_SIZE);
  dump("page3", base + 3 * PAGE_SIZE);

  // Are all 4 pages independently addressable as separate allocations?
  {
    MEMORY_BASIC_INFORMATION m0, m1, m3;
    query_mbi(base, &m0);
    query_mbi(base + PAGE_SIZE, &m1);
    query_mbi(base + 3 * PAGE_SIZE, &m3);
    printf("    page0 AllocBase=%p page1 AllocBase=%p page3 AllocBase=%p\n",
           m0.AllocationBase, m1.AllocationBase, m3.AllocationBase);
    NOTE("Each piece is now a separate allocation — coalesce possible");
  }

  // Can we coalesce all 3 pieces back?
  {
    // First: all must be placeholders. Convert committed ones.
    // preserve page 0
    sp = base; ss = PAGE_SIZE;
    NtFreeVirtualMemory(self(), &sp, &ss, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    // preserve pages 1-2
    sp = base + PAGE_SIZE; ss = 2 * PAGE_SIZE;
    NtFreeVirtualMemory(self(), &sp, &ss, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    // preserve page 3
    sp = base + 3 * PAGE_SIZE; ss = PAGE_SIZE;
    NtFreeVirtualMemory(self(), &sp, &ss, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);

    st = NtFreeVirtualMemory(self(), &(PVOID){base}, &(SIZE_T){4 * PAGE_SIZE},
                              MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    printf("  coalesce all 4: 0x%08lX (%s)\n", (unsigned long)st, st_name(st));
    CHECK(NT_SUCCESS(st), "Full coalesce after split-preserve chain");
  }

cleanup:;
  for (int i = 0; i < 4; i++) {
    PVOID f = base + i * PAGE_SIZE;
    SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }
}

int main(void) {
  printf("=== MEM_PRIVATE ↔ Placeholder Roundtrip Tests ===\n");

  test_full_preserve();
  test_subrange_preserve();
  test_decommit_then_preserve();
  test_split_committed();
  test_per_page_split_preserve();
  test_inplace_comparison();

  printf("\n========================================\n");
  printf("RESULTS: %d passed, %d failed\n", pass_count, fail_count);
  printf("========================================\n");
  return fail_count > 0 ? 1 : 0;
}
