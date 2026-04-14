// placeholder_coalesce_test.c — Validate placeholder coalescing scenarios
// relevant to partial munmap.
//
// After partial munmap of a view, the freed middle becomes MEM_FREE.
// Adjacent placeholder fragments from prior partial unmaps may exist.
// Can we coalesce them to reduce fragmentation?
//
// Tests:
//   1. Basic coalesce: split → release middle → coalesce outer
//   2. Coalesce after partial munmap simulation (freed + adjacent placeholders)
//   3. Coalesce non-adjacent placeholders (should fail — gap in between)
//   4. Coalesce placeholder + committed (should fail — different types)
//   5. Coalesce after multiple partial unmaps creating many fragments
//   6. Coalesce placeholder + section view (should fail)
//   7. Performance: many fragments coalesced in one call vs individual releases

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
      ULONG MappedAwe : 1;
      ULONG MappedWriteWatch : 1;
      ULONG PageSizeLarge : 1;
      ULONG PageSizeHuge : 1;
      ULONG Reserved : 19;
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

static void *create_ph(void *addr, SIZE_T size) {
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &base, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  return NT_SUCCESS(st) ? base : NULL;
}

//=============================================================================
// TEST 1: Basic split → coalesce roundtrip
//=============================================================================
static void test_basic_coalesce(void) {
  printf("\n=== TEST 1: Basic split → coalesce roundtrip ===\n");
  void *ph = create_ph(NULL, 4 * ALLOC_GRAN);
  CHECK(ph != NULL, "Created 256KB placeholder at %p", ph);

  CHECK(split_ph(ph, ALLOC_GRAN), "Split at 64KB");
  CHECK(split_ph((char *)ph + ALLOC_GRAN, ALLOC_GRAN), "Split at 128KB");
  CHECK(split_ph((char *)ph + 2 * ALLOC_GRAN, ALLOC_GRAN), "Split at 192KB");

  // Now 4 × 64KB placeholders
  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(ph, &mbi);
  CHECK(mbi.RegionSize == ALLOC_GRAN, "First piece is 64KB");

  // Coalesce all 4 back into one
  NTSTATUS st = coalesce(ph, 4 * ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Coalesce 4 fragments: 0x%08lX", (unsigned long)st);

  query_mbi(ph, &mbi);
  CHECK(mbi.RegionSize == 4 * ALLOC_GRAN, "Coalesced size is 256KB (got %lluKB)",
        (unsigned long long)mbi.RegionSize / 1024);
  CHECK(is_placeholder(ph), "Still a placeholder after coalesce");

  release_ph(ph);
}

//=============================================================================
// TEST 2: Coalesce after partial munmap simulation
// Create [A][B][C][D], release B → MEM_FREE, can we coalesce A+C+D?
//=============================================================================
static void test_coalesce_with_gap(void) {
  printf("\n=== TEST 2: Coalesce with MEM_FREE gap (non-adjacent) ===\n");
  void *ph = create_ph(NULL, 4 * ALLOC_GRAN);
  CHECK(ph != NULL, "Created 256KB placeholder");

  split_ph(ph, ALLOC_GRAN);                          // [A 64K][BCD 192K]
  split_ph((char *)ph + ALLOC_GRAN, ALLOC_GRAN);     // [A][B 64K][CD 128K]
  split_ph((char *)ph + 2 * ALLOC_GRAN, ALLOC_GRAN); // [A][B][C 64K][D 64K]

  // Release B → MEM_FREE
  release_ph((char *)ph + ALLOC_GRAN);
  MEMORY_BASIC_INFORMATION mbi;
  query_mbi((char *)ph + ALLOC_GRAN, &mbi);
  CHECK(mbi.State == MEM_FREE, "B is MEM_FREE after release");

  // Try coalescing A alone (trivial — already 1 piece)
  NTSTATUS st = coalesce(ph, ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Coalesce single placeholder A: 0x%08lX", (unsigned long)st);

  // Try coalescing C+D (adjacent, both placeholders)
  st = coalesce((char *)ph + 2 * ALLOC_GRAN, 2 * ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Coalesce C+D: 0x%08lX", (unsigned long)st);
  query_mbi((char *)ph + 2 * ALLOC_GRAN, &mbi);
  CHECK(mbi.RegionSize == 2 * ALLOC_GRAN, "C+D coalesced to 128KB");

  // Try coalescing A + gap + C+D (should fail — gap in between)
  st = coalesce(ph, 4 * ALLOC_GRAN);
  printf("  Coalesce A+gap+CD: 0x%08lX\n", (unsigned long)st);
  CHECK(!NT_SUCCESS(st), "Cannot coalesce across MEM_FREE gap");

  // Cleanup
  release_ph(ph);
  release_ph((char *)ph + 2 * ALLOC_GRAN);
}

//=============================================================================
// TEST 3: Coalesce placeholder + committed private (should fail)
//=============================================================================
static void test_coalesce_mixed_types(void) {
  printf("\n=== TEST 3: Coalesce placeholder + committed (should fail) ===\n");
  void *ph = create_ph(NULL, 2 * ALLOC_GRAN);
  CHECK(ph != NULL, "Created 128KB placeholder");

  split_ph(ph, ALLOC_GRAN); // [A ph][B ph]

  // Commit B as private memory
  PVOID b = (char *)ph + ALLOC_GRAN;
  SIZE_T bs = ALLOC_GRAN;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &b, &bs,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  CHECK(NT_SUCCESS(st), "Committed B as private");

  // Try coalesce A (placeholder) + B (committed)
  st = coalesce(ph, 2 * ALLOC_GRAN);
  printf("  Coalesce ph+committed: 0x%08lX\n", (unsigned long)st);
  CHECK(!NT_SUCCESS(st), "Cannot coalesce placeholder + committed memory");

  // Cleanup
  release_ph(ph);
  PVOID fb = (char *)ph + ALLOC_GRAN;
  SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &fb, &fs, MEM_RELEASE);
}

//=============================================================================
// TEST 4: Coalesce placeholder + section view (should fail)
//=============================================================================
static void test_coalesce_with_section(void) {
  printf("\n=== TEST 4: Coalesce placeholder + section view (should fail) ===\n");
  void *ph = create_ph(NULL, 2 * ALLOC_GRAN);
  CHECK(ph != NULL, "Created 128KB placeholder");

  split_ph(ph, ALLOC_GRAN);

  // Map a section into the second half
  LARGE_INTEGER max_size;
  max_size.QuadPart = ALLOC_GRAN;
  HANDLE section = NULL;
  NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                     &max_size, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);

  PVOID view = (char *)ph + ALLOC_GRAN;
  SIZE_T vs = ALLOC_GRAN;
  LARGE_INTEGER off = {0};
  NTSTATUS st = NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                                      MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                      NULL, 0);
  CHECK(NT_SUCCESS(st), "Mapped section into second half");

  st = coalesce(ph, 2 * ALLOC_GRAN);
  printf("  Coalesce ph+section: 0x%08lX\n", (unsigned long)st);
  CHECK(!NT_SUCCESS(st), "Cannot coalesce placeholder + section view");

  // Cleanup
  NtUnmapViewOfSectionEx(self(), (char *)ph + ALLOC_GRAN, 0);
  release_ph(ph);
  NtClose(section);
}

//=============================================================================
// TEST 5: Many fragments from repeated partial unmaps
// Simulates the fragmentation pattern: split a view into 16 page-sized
// pieces, release every other one, then coalesce remaining adjacent pairs.
//=============================================================================
static void test_many_fragments(void) {
  printf("\n=== TEST 5: 16 page-sized fragments, coalesce surviving pairs ===\n");
  SIZE_T total = 16 * PAGE_SIZE;
  void *ph = create_ph(NULL, total);
  CHECK(ph != NULL, "Created 16-page placeholder");

  // Split into 16 page-sized placeholders
  char *base = (char *)ph;
  char *remaining = base;
  for (int i = 0; i < 15; i++) {
    CHECK(split_ph(remaining, PAGE_SIZE), "Split %d", i);
    remaining += PAGE_SIZE;
  }

  // Verify we have 16 independent placeholders
  int ph_count = 0;
  for (int i = 0; i < 16; i++) {
    if (is_placeholder(base + i * PAGE_SIZE))
      ph_count++;
  }
  CHECK(ph_count == 16, "All 16 are placeholders (got %d)", ph_count);

  // Release even-indexed placeholders (0, 2, 4, ..., 14)
  for (int i = 0; i < 16; i += 2)
    release_ph(base + i * PAGE_SIZE);

  // Remaining: placeholders at odd indices (1, 3, 5, ..., 15)
  // These are NOT adjacent — each has a MEM_FREE gap on both sides.
  // Coalescing any pair should fail.
  NTSTATUS st = coalesce(base + PAGE_SIZE, 2 * PAGE_SIZE);
  printf("  Coalesce non-adjacent odd pages: 0x%08lX\n", (unsigned long)st);
  CHECK(!NT_SUCCESS(st), "Non-adjacent placeholder coalesce fails");

  // Now re-create placeholders in the even slots to fill gaps
  for (int i = 0; i < 16; i += 2)
    create_ph(base + i * PAGE_SIZE, PAGE_SIZE);

  // Now all 16 are placeholders again — coalesce the whole thing
  st = coalesce(base, total);
  CHECK(NT_SUCCESS(st), "Full coalesce after re-filling gaps: 0x%08lX",
        (unsigned long)st);

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == total, "Coalesced back to %llu bytes",
        (unsigned long long)total);

  release_ph(base);
}

//=============================================================================
// TEST 6: Coalesce partial range (not from base)
// Can we coalesce a subset of adjacent placeholders?
//=============================================================================
static void test_partial_coalesce(void) {
  printf("\n=== TEST 6: Partial coalesce (middle fragments only) ===\n");
  void *ph = create_ph(NULL, 4 * ALLOC_GRAN);
  CHECK(ph != NULL, "Created 256KB placeholder");

  char *base = (char *)ph;
  split_ph(base, ALLOC_GRAN);                      // [A][BCD]
  split_ph(base + ALLOC_GRAN, ALLOC_GRAN);          // [A][B][CD]
  split_ph(base + 2 * ALLOC_GRAN, ALLOC_GRAN);      // [A][B][C][D]

  // Coalesce only B+C (middle two)
  NTSTATUS st = coalesce(base + ALLOC_GRAN, 2 * ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Coalesce B+C only: 0x%08lX", (unsigned long)st);

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base + ALLOC_GRAN, &mbi);
  CHECK(mbi.RegionSize == 2 * ALLOC_GRAN, "B+C coalesced to 128KB");

  // A and D should still be separate 64KB placeholders
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == ALLOC_GRAN, "A still 64KB");
  query_mbi(base + 3 * ALLOC_GRAN, &mbi);
  CHECK(mbi.RegionSize == ALLOC_GRAN, "D still 64KB");

  // Now coalesce all 3 remaining pieces (A + BC + D)
  st = coalesce(base, 4 * ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Full coalesce A+BC+D");
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == 4 * ALLOC_GRAN, "Back to 256KB");

  release_ph(base);
}

//=============================================================================
// TEST 7: Coalesce after unmap-to-placeholder (simulating munmap freed range)
// The actual pattern: section view → unmap with PRESERVE → placeholder.
// Can we coalesce this new placeholder with an adjacent existing placeholder?
//=============================================================================
static void test_coalesce_after_unmap(void) {
  printf("\n=== TEST 7: Coalesce placeholder from unmap with adjacent ===\n");

  // Create 2-block placeholder, map a section into the first half
  void *ph = create_ph(NULL, 2 * ALLOC_GRAN);
  CHECK(ph != NULL, "Created 128KB placeholder");
  char *base = (char *)ph;

  split_ph(base, ALLOC_GRAN); // [A ph][B ph]

  LARGE_INTEGER max_size;
  max_size.QuadPart = ALLOC_GRAN;
  HANDLE section = NULL;
  NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                     &max_size, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
  PVOID view = base;
  SIZE_T vs = ALLOC_GRAN;
  LARGE_INTEGER off = {0};
  NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  // Now: [A section view][B placeholder]
  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.Type == MEM_MAPPED, "A is section view");
  CHECK(is_placeholder(base + ALLOC_GRAN), "B is placeholder");

  // Unmap A back to placeholder
  NTSTATUS st = NtUnmapViewOfSectionEx(self(), base,
                                        MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
  CHECK(NT_SUCCESS(st), "Unmap A to placeholder");
  CHECK(is_placeholder(base), "A is now placeholder");
  CHECK(is_placeholder(base + ALLOC_GRAN), "B still placeholder");

  // Coalesce A + B
  st = coalesce(base, 2 * ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Coalesce unmap-placeholder + existing placeholder");
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == 2 * ALLOC_GRAN, "Coalesced to 128KB");

  release_ph(base);
  NtClose(section);
}

int main(void) {
  printf("=== Placeholder Coalescing Tests ===\n");

  test_basic_coalesce();
  test_coalesce_with_gap();
  test_coalesce_mixed_types();
  test_coalesce_with_section();
  test_many_fragments();
  test_partial_coalesce();
  test_coalesce_after_unmap();

  printf("\n========================================\n");
  printf("RESULTS: %d passed, %d failed\n", pass_count, fail_count);
  printf("========================================\n");
  return fail_count > 0 ? 1 : 0;
}
