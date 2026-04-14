// coalesce_practical_test.c — Real-world coalescing patterns
//
// Now that we know ANY adjacent placeholders coalesce regardless of lineage,
// explore practical scenarios from the mmap/munmap lifecycle.

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
#define MEM_FREE                0x00010000
#define MEM_PRIVATE             0x00020000
#define MEM_MAPPED              0x00040000
#define MEM_RELEASE             0x00008000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#define MEM_PRESERVE_PLACEHOLDER_ON_UNMAP 0x00000002

#define PAGE_NOACCESS   0x01
#define PAGE_READWRITE  0x04

#define SEC_COMMIT      0x08000000
#define SECTION_ALL_ACCESS 0x000F001F

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
extern NTSTATUS NTAPI NtCreateSectionEx(
    HANDLE *, ULONG, PVOID, LARGE_INTEGER *, ULONG, ULONG, HANDLE, PVOID, ULONG);
extern NTSTATUS NTAPI NtMapViewOfSectionEx(
    HANDLE, HANDLE, PVOID *, LARGE_INTEGER *, SIZE_T *, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtUnmapViewOfSectionEx(HANDLE, PVOID, ULONG);
extern NTSTATUS NTAPI NtClose(HANDLE);

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, fmt, ...) do { \
  if (cond) { pass_count++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } \
  else { fail_count++; printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

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

static void *create_ph(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &base, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  return NT_SUCCESS(st) ? base : NULL;
}

static int split_ph(void *addr, SIZE_T offset) {
  PVOID base = addr;
  SIZE_T size = offset;
  return NT_SUCCESS(NtFreeVirtualMemory(self(), &base, &size,
                                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
}

static int release_ph(void *addr) {
  PVOID base = addr;
  SIZE_T size = 0;
  return NT_SUCCESS(NtFreeVirtualMemory(self(), &base, &size, MEM_RELEASE));
}

static NTSTATUS coalesce(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NtFreeVirtualMemory(self(), &base, &sz,
                              MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
}

static HANDLE create_section(SIZE_T size) {
  LARGE_INTEGER max_size;
  max_size.QuadPart = (long long)size;
  HANDLE section = NULL;
  NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                     &max_size, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
  return section;
}

static int map_section(HANDLE section, void *ph, SIZE_T size) {
  PVOID view = ph;
  SIZE_T vs = size;
  LARGE_INTEGER off = {0};
  return NT_SUCCESS(NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                     MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0));
}

static int unmap_to_ph(void *addr) {
  return NT_SUCCESS(NtUnmapViewOfSectionEx(self(), addr,
                     MEM_PRESERVE_PLACEHOLDER_ON_UNMAP));
}

static int commit_ph(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NT_SUCCESS(NtAllocateVirtualMemoryEx(self(), &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      NULL, 0));
}

// Count placeholder fragments in a range
static int count_placeholders(char *start, SIZE_T total) {
  int count = 0;
  char *cur = start;
  char *end = start + total;
  while (cur < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!query_mbi(cur, &mbi)) break;
    if (is_placeholder(cur)) count++;
    char *next = (char *)mbi.BaseAddress + mbi.RegionSize;
    if (next <= cur) break;
    cur = next;
  }
  return count;
}

// Count distinct AllocationBase values (proxy for "allocation count")
static int count_allocations(char *start, SIZE_T total) {
  int count = 0;
  void *last_ab = NULL;
  char *cur = start;
  char *end = start + total;
  while (cur < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!query_mbi(cur, &mbi)) break;
    if (mbi.State != MEM_FREE && mbi.AllocationBase != last_ab) {
      count++;
      last_ab = mbi.AllocationBase;
    }
    char *next = (char *)mbi.BaseAddress + mbi.RegionSize;
    if (next <= cur) break;
    cur = next;
  }
  return count;
}

//=============================================================================
// TEST 1: Grow a placeholder by coalescing with an adjacent new one
// Simulates: mmap returns a placeholder, user wants more VA → allocate
// adjacent and coalesce for a larger contiguous range.
//=============================================================================
static void test_grow_placeholder(void) {
  printf("\n=== TEST 1: Grow placeholder via adjacent coalesce ===\n");

  void *ph1 = create_ph(NULL, ALLOC_GRAN);
  char *base = (char *)ph1;
  printf("  Initial 64KB placeholder at %p\n", base);

  // Create adjacent placeholder right after
  void *ph2 = create_ph(base + ALLOC_GRAN, ALLOC_GRAN);
  if (!ph2) {
    // Address might be taken — try from the other side
    printf("  Adjacent allocation failed — address taken\n");
    release_ph(ph1);
    return;
  }
  CHECK(ph2 == base + ALLOC_GRAN, "Adjacent placeholder at %p", ph2);

  NTSTATUS st = coalesce(base, 2 * ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Coalesce to grow from 64KB to 128KB");

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == 2 * ALLOC_GRAN, "Grown to 128KB");
  CHECK(is_placeholder(base), "Still a placeholder");

  // Can we map a section into the grown placeholder?
  HANDLE section = create_section(2 * ALLOC_GRAN);
  CHECK(map_section(section, base, 2 * ALLOC_GRAN),
        "Section maps into grown placeholder");

  // Verify content
  memset(base, 0xEE, 2 * (size_t)ALLOC_GRAN);
  CHECK((unsigned char)base[ALLOC_GRAN + 100] == 0xEE,
        "Content accessible across original boundary");

  NtUnmapViewOfSectionEx(self(), base, 0);
  NtClose(section);
}

//=============================================================================
// TEST 2: Defragment after repeated partial munmap
// Simulate: 8-page view → partial munmap pages 2-3 → partial munmap pages 5-6
// Result: [view 0-1][FREE 2-3][view 4][FREE 5-6][view 7]
// Then full munmap → [ph 0-1][FREE 2-3][ph 4][FREE 5-6][ph 7]
// Fill gaps → [ph 0-1][ph 2-3][ph 4][ph 5-6][ph 7]
// Coalesce → [ph 0-7] — one big placeholder, VA fully reclaimed
//=============================================================================
static void test_defrag_after_partial_munmaps(void) {
  printf("\n=== TEST 2: VA defragmentation after partial munmaps ===\n");

  SIZE_T total = 8 * PAGE_SIZE;
  void *ph = create_ph(NULL, total);
  char *base = (char *)ph;

  HANDLE section = create_section(total);
  map_section(section, base, total);
  printf("  8-page section view at %p\n", base);

  // Simulate partial munmap of pages 2-3: unmap whole → split → remap kept
  unmap_to_ph(base);
  split_ph(base, 2 * PAGE_SIZE);                    // [0-1][2-7]
  split_ph(base + 2 * PAGE_SIZE, 2 * PAGE_SIZE);    // [0-1][2-3][4-7]

  // Remap kept [0-1]
  PVOID v01 = base;
  SIZE_T s01 = 2 * PAGE_SIZE;
  LARGE_INTEGER off01 = {0};
  NtMapViewOfSectionEx(section, self(), &v01, &off01, &s01,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  // Remap kept [4-7]
  PVOID v47 = base + 4 * PAGE_SIZE;
  SIZE_T s47 = 4 * PAGE_SIZE;
  LARGE_INTEGER off47;
  off47.QuadPart = 4 * PAGE_SIZE;
  NtMapViewOfSectionEx(section, self(), &v47, &off47, &s47,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  // Release freed [2-3]
  release_ph(base + 2 * PAGE_SIZE);
  printf("  After munmap(2-3): [view 0-1][FREE 2-3][view 4-7]\n");

  // Simulate partial munmap of pages 5-6 from view [4-7]
  unmap_to_ph(base + 4 * PAGE_SIZE);
  split_ph(base + 4 * PAGE_SIZE, PAGE_SIZE);         // [4][5-7]
  split_ph(base + 5 * PAGE_SIZE, 2 * PAGE_SIZE);     // [4][5-6][7]

  PVOID v4 = base + 4 * PAGE_SIZE;
  SIZE_T s4 = PAGE_SIZE;
  LARGE_INTEGER off4;
  off4.QuadPart = 4 * PAGE_SIZE;
  NtMapViewOfSectionEx(section, self(), &v4, &off4, &s4,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  PVOID v7 = base + 7 * PAGE_SIZE;
  SIZE_T s7 = PAGE_SIZE;
  LARGE_INTEGER off7;
  off7.QuadPart = 7 * PAGE_SIZE;
  NtMapViewOfSectionEx(section, self(), &v7, &off7, &s7,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  release_ph(base + 5 * PAGE_SIZE);
  printf("  After munmap(5-6): [view 0-1][FREE 2-3][view 4][FREE 5-6][view 7]\n");

  int allocs = count_allocations(base, total);
  printf("  Allocation count: %d (fragmented)\n", allocs);

  // Full munmap: unmap all remaining views
  unmap_to_ph(base);                    // [0-1] → placeholder
  unmap_to_ph(base + 4 * PAGE_SIZE);    // [4] → placeholder
  unmap_to_ph(base + 7 * PAGE_SIZE);    // [7] → placeholder

  int phs = count_placeholders(base, total);
  printf("  After full unmap: %d placeholders + 2 FREE gaps\n", phs);

  // Fill the FREE gaps with new placeholders
  void *gap1 = create_ph(base + 2 * PAGE_SIZE, 2 * PAGE_SIZE);
  void *gap2 = create_ph(base + 5 * PAGE_SIZE, 2 * PAGE_SIZE);
  CHECK(gap1 != NULL && gap2 != NULL, "Filled both FREE gaps with placeholders");

  phs = count_placeholders(base, total);
  printf("  After filling gaps: %d placeholders (5 expected)\n", phs);

  // Coalesce ALL placeholders into one
  NTSTATUS st = coalesce(base, total);
  CHECK(NT_SUCCESS(st), "Full VA defragmentation coalesce");

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == total, "Defragmented to single %lluKB placeholder",
        (unsigned long long)total / 1024);

  allocs = count_allocations(base, total);
  CHECK(allocs == 1, "Single allocation (was %d fragmented)", allocs);

  // Can we use the defragmented placeholder?
  CHECK(map_section(section, base, total), "Map section into defragmented placeholder");

  NtUnmapViewOfSectionEx(self(), base, 0);
  NtClose(section);
}

//=============================================================================
// TEST 3: Coalesce mixed-history placeholders
// Some went through section roundtrip, some were freshly created, some
// were split from different parents. All adjacent → coalesce.
//=============================================================================
static void test_mixed_history_coalesce(void) {
  printf("\n=== TEST 3: Mixed-history placeholder coalesce ===\n");

  // Reserve a 5-block range
  SIZE_T total = 5 * ALLOC_GRAN;
  void *block = create_ph(NULL, total);
  char *base = (char *)block;
  release_ph(block); // Free it so we can re-use the addresses

  // Block 0: fresh placeholder
  void *b0 = create_ph(base, ALLOC_GRAN);

  // Block 1: placeholder that went through section roundtrip
  void *b1 = create_ph(base + ALLOC_GRAN, ALLOC_GRAN);
  HANDLE sec1 = create_section(ALLOC_GRAN);
  map_section(sec1, b1, ALLOC_GRAN);
  unmap_to_ph(b1);
  NtClose(sec1);

  // Block 2: placeholder from a different parent (split)
  void *parent2 = create_ph(base + 2 * ALLOC_GRAN, 2 * ALLOC_GRAN);
  split_ph(parent2, ALLOC_GRAN);
  // Now [block2 = 64K placeholder][block3 = 64K placeholder]

  // Block 4: fresh placeholder
  void *b4 = create_ph(base + 4 * ALLOC_GRAN, ALLOC_GRAN);

  if (!b0 || !b1 || !parent2 || !b4) {
    printf("  Could not allocate all blocks at exact addresses — skipping\n");
    // Best effort cleanup
    if (b0) release_ph(b0);
    if (b1) release_ph(b1);
    if (parent2) { release_ph(parent2); release_ph(base + 3 * ALLOC_GRAN); }
    if (b4) release_ph(b4);
    return;
  }

  printf("  5 adjacent placeholders with mixed history:\n");
  printf("    [0] fresh   [1] section-roundtripped   [2-3] split siblings   [4] fresh\n");

  int phs = count_placeholders(base, total);
  CHECK(phs == 5, "5 placeholders present (got %d)", phs);

  // Coalesce ALL
  NTSTATUS st = coalesce(base, total);
  CHECK(NT_SUCCESS(st), "Mixed-history coalesce: 0x%08lX", (unsigned long)st);

  if (NT_SUCCESS(st)) {
    MEMORY_BASIC_INFORMATION mbi;
    query_mbi(base, &mbi);
    CHECK(mbi.RegionSize == total, "Coalesced to %lluKB",
          (unsigned long long)mbi.RegionSize / 1024);
    release_ph(base);
  } else {
    // Cleanup individual pieces
    for (int i = 0; i < 5; i++)
      release_ph(base + i * ALLOC_GRAN);
  }
}

//=============================================================================
// TEST 4: Coalesce after remap_with_cow_splits cleanup
// After a full munmap of a COW-split view: [section][private][section]
// Unmap sections → [ph][private][ph]. Can't coalesce (private in middle).
// Release private → [ph][FREE][ph]. Fill gap → [ph][ph][ph]. Coalesce!
//=============================================================================
static void test_cow_split_cleanup_coalesce(void) {
  printf("\n=== TEST 4: Coalesce after COW-split cleanup ===\n");

  SIZE_T total = 3 * PAGE_SIZE;
  void *ph = create_ph(NULL, total);
  char *base = (char *)ph;

  // Simulate the result of remap_with_cow_splits:
  // [section page 0][private page 1][section page 2]
  split_ph(base, PAGE_SIZE);
  split_ph(base + PAGE_SIZE, PAGE_SIZE);
  // [ph0][ph1][ph2]

  // Map sections into 0 and 2
  HANDLE sec = create_section(total);
  PVOID v0 = base;
  SIZE_T s0 = PAGE_SIZE;
  LARGE_INTEGER o0 = {0};
  NtMapViewOfSectionEx(sec, self(), &v0, &o0, &s0,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  PVOID v2 = base + 2 * PAGE_SIZE;
  SIZE_T s2 = PAGE_SIZE;
  LARGE_INTEGER o2;
  o2.QuadPart = 2 * PAGE_SIZE;
  NtMapViewOfSectionEx(sec, self(), &v2, &o2, &s2,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  // Commit page 1 as private (dirty cluster)
  commit_ph(base + PAGE_SIZE, PAGE_SIZE);

  printf("  Layout: [section][private][section]\n");

  // Now simulate full munmap of this range
  // Unmap section views → placeholders
  unmap_to_ph(base);
  unmap_to_ph(base + 2 * PAGE_SIZE);

  // Release private → MEM_FREE
  PVOID fp = base + PAGE_SIZE;
  SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &fp, &fs, MEM_RELEASE);

  printf("  After full unmap: [ph 0][FREE 1][ph 2]\n");

  // Can't coalesce yet (FREE gap)
  NTSTATUS st = coalesce(base, total);
  CHECK(!NT_SUCCESS(st), "Coalesce with FREE gap fails (expected)");

  // Fill the gap
  void *fill = create_ph(base + PAGE_SIZE, PAGE_SIZE);
  CHECK(fill != NULL, "Filled FREE gap with placeholder");

  printf("  After fill: [ph 0][ph 1][ph 2]\n");

  // Now coalesce!
  st = coalesce(base, total);
  CHECK(NT_SUCCESS(st), "Coalesce after gap fill");

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == total, "Clean %lluKB placeholder",
        (unsigned long long)total / 1024);

  release_ph(base);
  NtClose(sec);
}

//=============================================================================
// TEST 5: Repeated split-coalesce cycles (stress test for kernel stability)
//=============================================================================
static void test_split_coalesce_stress(void) {
  printf("\n=== TEST 5: Split-coalesce stress (100 cycles) ===\n");

  void *ph = create_ph(NULL, 16 * PAGE_SIZE);
  char *base = (char *)ph;
  CHECK(ph != NULL, "Created 16-page placeholder");

  int ok = 1;
  for (int cycle = 0; cycle < 100 && ok; cycle++) {
    // Split into 16 page-sized pieces
    char *rem = base;
    for (int i = 0; i < 15; i++) {
      if (!split_ph(rem, PAGE_SIZE)) { ok = 0; break; }
      rem += PAGE_SIZE;
    }
    if (!ok) break;

    // Coalesce back
    NTSTATUS st = coalesce(base, 16 * PAGE_SIZE);
    if (!NT_SUCCESS(st)) { ok = 0; break; }
  }

  CHECK(ok, "100 split-coalesce cycles completed");

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == 16 * PAGE_SIZE, "Final placeholder is 64KB");

  release_ph(base);
}

//=============================================================================
// TEST 6: Coalesce as MAP_FIXED preparation
// When MAP_FIXED targets a range with multiple small placeholders from
// prior operations, coalescing first gives us a single placeholder to
// map into — avoiding N separate maps.
//=============================================================================
static void test_coalesce_for_map_fixed(void) {
  printf("\n=== TEST 6: Coalesce as MAP_FIXED preparation ===\n");

  // Create 4 independent small placeholders at known addresses
  SIZE_T total = 4 * ALLOC_GRAN;
  void *block = create_ph(NULL, total);
  char *base = (char *)block;
  release_ph(block);

  for (int i = 0; i < 4; i++) {
    void *p = create_ph(base + i * ALLOC_GRAN, ALLOC_GRAN);
    CHECK(p != NULL, "Placeholder %d at %p", i, p);
  }

  int allocs_before = count_allocations(base, total);
  printf("  Before coalesce: %d allocations\n", allocs_before);

  // Coalesce into one
  NTSTATUS st = coalesce(base, total);
  CHECK(NT_SUCCESS(st), "Coalesce for MAP_FIXED");

  int allocs_after = count_allocations(base, total);
  printf("  After coalesce: %d allocation(s)\n", allocs_after);
  CHECK(allocs_after == 1, "Single allocation");

  // Map a single large section (simulating MAP_FIXED)
  HANDLE section = create_section(total);
  CHECK(map_section(section, base, total),
        "Single-shot section map into coalesced placeholder");

  // Verify it's one contiguous view
  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == total, "Single %lluKB view",
        (unsigned long long)total / 1024);

  NtUnmapViewOfSectionEx(self(), base, 0);
  NtClose(section);
}

//=============================================================================
// TEST 7: Can we coalesce to bridge two DIFFERENT section views?
// [section A view][placeholder gap][section A view] → fill gap → coalesce?
// No — section views can't coalesce. But what about after unmap?
// [ph from A][ph (filled gap)][ph from A] → coalesce → single ph
// → map NEW section B → one big view replacing three fragments
//=============================================================================
static void test_bridge_and_remap(void) {
  printf("\n=== TEST 7: Bridge gap between old views, remap as one ===\n");

  SIZE_T total = 3 * ALLOC_GRAN;
  void *ph = create_ph(NULL, total);
  char *base = (char *)ph;

  split_ph(base, ALLOC_GRAN);
  split_ph(base + ALLOC_GRAN, ALLOC_GRAN);
  // [ph0][ph1][ph2]

  // Map section A into ph0 and ph2
  HANDLE secA = create_section(total);
  PVOID v0 = base;
  SIZE_T s0 = ALLOC_GRAN;
  LARGE_INTEGER o0 = {0};
  NtMapViewOfSectionEx(secA, self(), &v0, &o0, &s0,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  PVOID v2 = base + 2 * ALLOC_GRAN;
  SIZE_T s2 = ALLOC_GRAN;
  LARGE_INTEGER o2;
  o2.QuadPart = 2 * ALLOC_GRAN;
  NtMapViewOfSectionEx(secA, self(), &v2, &o2, &s2,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  // Middle (ph1) is still a placeholder

  printf("  Layout: [secA view 0][placeholder 1][secA view 2]\n");

  // Unmap both section views
  unmap_to_ph(base);
  unmap_to_ph(base + 2 * ALLOC_GRAN);

  printf("  After unmap: [ph 0][ph 1][ph 2]\n");

  // Coalesce all three
  NTSTATUS st = coalesce(base, total);
  CHECK(NT_SUCCESS(st), "Coalesce three placeholders (2 from unmap + 1 original)");

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == total, "Single %lluKB placeholder",
        (unsigned long long)total / 1024);

  // Map a completely different section B as one contiguous view
  HANDLE secB = create_section(total);
  CHECK(map_section(secB, base, total),
        "Map new section B into bridged+coalesced placeholder");

  memset(base, 0xFF, (size_t)total);
  CHECK((unsigned char)base[ALLOC_GRAN] == 0xFF, "Content spans bridge point");

  NtUnmapViewOfSectionEx(self(), base, 0);
  NtClose(secA);
  NtClose(secB);
}

int main(void) {
  printf("=== Practical Placeholder Coalescing Tests ===\n");

  test_grow_placeholder();
  test_defrag_after_partial_munmaps();
  test_mixed_history_coalesce();
  test_cow_split_cleanup_coalesce();
  test_split_coalesce_stress();
  test_coalesce_for_map_fixed();
  test_bridge_and_remap();

  printf("\n========================================\n");
  printf("RESULTS: %d passed, %d failed\n", pass_count, fail_count);
  printf("========================================\n");
  return fail_count > 0 ? 1 : 0;
}
