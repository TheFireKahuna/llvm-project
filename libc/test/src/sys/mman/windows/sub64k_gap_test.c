// sub64k_gap_test.c — Explore sub-64KB placeholder gaps and workarounds
//
// Core constraint: NtAllocateVirtualMemoryEx(MEM_RESERVE_PLACEHOLDER) at a
// specific address requires allocation-granularity (64KB) alignment and
// minimum size. But split_placeholder is page-granular (4KB).
//
// So page-sized placeholders EXIST (from splits) but can't be CREATED from
// MEM_FREE. Once you release a sub-64KB placeholder, the gap is permanent.
//
// Questions:
//   1. What's the actual minimum for create_placeholder? 4KB? 64KB? At aligned addr?
//   2. Can we create at a non-64KB-aligned address?
//   3. Can we create spanning a gap + adjacent FREE to reach ≥64KB?
//   4. Can we avoid the gap entirely with coalesce-before-release strategies?
//   5. Radical: can we grow a placeholder INTO adjacent FREE space?

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

static NTSTATUS try_create_ph(void *addr, SIZE_T size, void **out) {
  PVOID base = addr;
  SIZE_T sz = size;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &base, &sz,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (out) *out = NT_SUCCESS(st) ? base : NULL;
  return st;
}

static void *create_ph(void *addr, SIZE_T size) {
  void *out = NULL;
  try_create_ph(addr, size, &out);
  return out;
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

//=============================================================================
// TEST 1: Minimum placeholder creation size at aligned address
//=============================================================================
static void test_min_create_size(void) {
  printf("\n=== TEST 1: Minimum placeholder create size ===\n");

  // Get a 64KB-aligned address
  void *big = create_ph(NULL, ALLOC_GRAN);
  char *aligned = (char *)big;
  release_ph(big);

  SIZE_T sizes[] = {
    PAGE_SIZE,           // 4KB
    2 * PAGE_SIZE,       // 8KB
    4 * PAGE_SIZE,       // 16KB
    8 * PAGE_SIZE,       // 32KB
    ALLOC_GRAN,          // 64KB
  };

  for (int i = 0; i < 5; i++) {
    void *out = NULL;
    NTSTATUS st = try_create_ph(aligned, sizes[i], &out);
    printf("  create_ph(%p, %5lluKB): 0x%08lX %s\n",
           aligned, (unsigned long long)sizes[i] / 1024,
           (unsigned long)st, NT_SUCCESS(st) ? "OK" : "FAIL");
    if (out) {
      MEMORY_BASIC_INFORMATION mbi;
      query_mbi(out, &mbi);
      printf("    actual: base=%p size=%lluKB\n",
             mbi.BaseAddress, (unsigned long long)mbi.RegionSize / 1024);
      release_ph(out);
    }
  }
}

//=============================================================================
// TEST 2: Create at non-64KB-aligned address
//=============================================================================
static void test_unaligned_create(void) {
  printf("\n=== TEST 2: Placeholder create at non-64KB-aligned address ===\n");

  // Get a base address and offset by one page
  void *big = create_ph(NULL, 2 * ALLOC_GRAN);
  char *base = (char *)big;
  release_ph(big);

  // Try at base + 4KB (page-aligned but not 64KB-aligned)
  char *unaligned = base + PAGE_SIZE;
  void *out = NULL;
  NTSTATUS st = try_create_ph(unaligned, ALLOC_GRAN, &out);
  printf("  create_ph(base+4KB, 64KB): 0x%08lX\n", (unsigned long)st);
  if (out) {
    NOTE("Kernel accepted non-64KB-aligned placeholder at %p!", out);
    if (out == unaligned)
      NOTE("Created at EXACT requested address");
    else
      NOTE("Kernel rounded down to %p", out);
    release_ph(out);
  } else {
    NOTE("Non-64KB-aligned create fails — expected");
  }

  // Try at base + 4KB with page-sized request
  st = try_create_ph(unaligned, PAGE_SIZE, &out);
  printf("  create_ph(base+4KB, 4KB): 0x%08lX\n", (unsigned long)st);
  if (out) {
    NOTE("4KB placeholder at non-64KB addr %p!", out);
    release_ph(out);
  }

  // Try at base + 4KB with size that reaches next 64KB boundary
  SIZE_T to_boundary = ALLOC_GRAN - PAGE_SIZE; // 60KB
  st = try_create_ph(unaligned, to_boundary, &out);
  printf("  create_ph(base+4KB, 60KB to next boundary): 0x%08lX\n",
         (unsigned long)st);
  if (out) {
    NOTE("Created 60KB placeholder bridging to 64KB boundary at %p", out);
    release_ph(out);
  }
}

//=============================================================================
// TEST 3: The gap problem — detailed analysis
// Create [ph A 4KB][FREE 4KB][ph B 4KB] via split+release and try everything
//=============================================================================
static void test_gap_analysis(void) {
  printf("\n=== TEST 3: Sub-64KB gap analysis ===\n");

  void *big = create_ph(NULL, ALLOC_GRAN);
  char *base = (char *)big;

  // Split into pages: [p0][p1][p2][...p15]
  char *rem = base;
  for (int i = 0; i < 15; i++) {
    split_ph(rem, PAGE_SIZE);
    rem += PAGE_SIZE;
  }

  // Release page 1 → gap
  release_ph(base + PAGE_SIZE);

  printf("  Layout: [ph p0][FREE p1][ph p2]...[ph p15]\n");

  // Try creating a placeholder in the 4KB gap
  void *out = NULL;
  NTSTATUS st = try_create_ph(base + PAGE_SIZE, PAGE_SIZE, &out);
  printf("  create_ph in 4KB gap: 0x%08lX (%s)\n",
         (unsigned long)st, out ? "OK!" : "failed");
  if (out) {
    CHECK(1, "4KB placeholder created in gap!");
    // Can we coalesce p0 + gap + p2?
    st = coalesce(base, 3 * PAGE_SIZE);
    printf("    Coalesce p0+gap+p2: 0x%08lX\n", (unsigned long)st);
    CHECK(NT_SUCCESS(st), "Coalesced across filled gap");
    // Split back to original state for more tests
    split_ph(base, PAGE_SIZE);
    split_ph(base + PAGE_SIZE, PAGE_SIZE);
    release_ph(base + PAGE_SIZE);
  } else {
    NOTE("Cannot create placeholder in sub-64KB gap");
  }

  // Try creating a 64KB placeholder that STARTS at the gap (spans beyond)
  st = try_create_ph(base + PAGE_SIZE, ALLOC_GRAN, &out);
  printf("  create_ph(gap_addr, 64KB spanning beyond): 0x%08lX\n",
         (unsigned long)st);
  if (out) {
    NOTE("64KB placeholder starting at gap addr %p (actual %p)", base + PAGE_SIZE, out);
    release_ph(out);
  }

  // Strategy: can we coalesce p0 with p2..p15 WITHOUT filling the gap?
  // No — coalesce requires contiguous placeholders.
  st = coalesce(base, 3 * PAGE_SIZE);
  printf("  Coalesce p0 + FREE + p2: 0x%08lX (expect fail)\n", (unsigned long)st);

  // Strategy: release p0 too → [FREE p0][FREE p1][ph p2]...[ph p15]
  // Now p0+p1 = 8KB of FREE. Can we create_ph there?
  release_ph(base);
  st = try_create_ph(base, 2 * PAGE_SIZE, &out);
  printf("  create_ph in 8KB FREE: 0x%08lX\n", (unsigned long)st);
  if (out) {
    NOTE("8KB placeholder from 2 free pages!");
    // Coalesce with p2..p15
    st = coalesce(base, ALLOC_GRAN);
    printf("    Coalesce 8KB + p2..p15: 0x%08lX\n", (unsigned long)st);
    if (NT_SUCCESS(st)) CHECK(1, "Full defrag after 8KB gap fill!");
    release_ph(base);
  } else {
    NOTE("Cannot create sub-64KB placeholder even in larger free range");
    // Expand free range: release p2 also → 12KB free
    release_ph(base + 2 * PAGE_SIZE);
    st = try_create_ph(base, 3 * PAGE_SIZE, &out);
    printf("  create_ph in 12KB FREE: 0x%08lX\n", (unsigned long)st);
    if (out) release_ph(out);
  }

  // Cleanup remaining
  for (int i = 0; i < 16; i++)
    release_ph(base + i * PAGE_SIZE);
}

//=============================================================================
// TEST 4: The coalesce-before-release strategy
// Instead of: unmap → split → remap kept → RELEASE freed
// Do:         unmap → split → remap kept → COALESCE kept+freed → split → release
// This avoids ever creating a sub-64KB gap.
// Wait — after remap, kept fragments are section views, not placeholders.
// Can't coalesce section views. So this only works if we coalesce BEFORE remap.
//
// Real strategy: unmap → DON'T split at all → remap kept at sub-ranges
// using placeholder sub-splits, keep freed as placeholder, then release.
// Actually that's what we already do. The gap only appears when we RELEASE.
//
// Key insight: can we release a sub-range of a placeholder without creating
// an allocation-granularity gap? Test: release a page-sized piece and see
// if the adjacent placeholder absorbs it.
//=============================================================================
static void test_release_absorption(void) {
  printf("\n=== TEST 4: Does releasing a sub-split absorb into neighbor? ===\n");

  void *ph = create_ph(NULL, ALLOC_GRAN);
  char *base = (char *)ph;

  // Split: [p0 4KB][rest 60KB]
  split_ph(base, PAGE_SIZE);

  // Release p0
  release_ph(base);

  // Does 'rest' (at base+4KB) now show as 60KB or did it absorb?
  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base + PAGE_SIZE, &mbi);
  printf("  rest after p0 release: base=%p size=%lluKB\n",
         mbi.BaseAddress, (unsigned long long)mbi.RegionSize / 1024);
  CHECK(is_placeholder(base + PAGE_SIZE), "rest is still placeholder");

  // Check the FREE gap
  query_mbi(base, &mbi);
  printf("  gap: state=0x%lx size=%lluKB\n",
         (unsigned long)mbi.State, (unsigned long long)mbi.RegionSize / 1024);

  if (mbi.State == MEM_FREE && mbi.RegionSize == PAGE_SIZE)
    NOTE("4KB FREE gap created — no absorption");
  else if (mbi.State == MEM_FREE && mbi.RegionSize > PAGE_SIZE)
    NOTE("Larger FREE region: kernel may have merged free ranges");

  release_ph(base + PAGE_SIZE);
}

//=============================================================================
// TEST 5: The REVERSE strategy — create LARGE, split DOWN
// Instead of creating small placeholders and trying to coalesce,
// create a 64KB+ placeholder that spans the entire range including gaps,
// then split to carve out the pieces we need.
//
// Scenario: we want placeholders at [A 4KB][B 4KB][C 4KB] in a region
// where A+B+C = 12KB. Instead of 3 creates (which fail sub-64KB),
// create one 64KB placeholder starting at A, split at 4KB and 8KB.
// But this only works if the 64KB range starting at A is all FREE.
//=============================================================================
static void test_oversized_create_and_split(void) {
  printf("\n=== TEST 5: Oversized create → split down strategy ===\n");

  // Get a clean 64KB-aligned range
  void *big = create_ph(NULL, ALLOC_GRAN);
  char *base = (char *)big;
  release_ph(big);

  // Create a 64KB placeholder
  void *ph = create_ph(base, ALLOC_GRAN);
  CHECK(ph != NULL, "Created 64KB placeholder at aligned address");

  // Split into 16 × 4KB pages
  char *rem = base;
  for (int i = 0; i < 15; i++) {
    CHECK(split_ph(rem, PAGE_SIZE), "Split %d", i);
    rem += PAGE_SIZE;
  }

  // Use pages 0, 2, 4 as "kept" and release 1, 3 as "freed"
  // Then try to reclaim: coalesce 0+2 (fails — gap at 1)
  release_ph(base + PAGE_SIZE);
  release_ph(base + 3 * PAGE_SIZE);

  printf("  Layout: [ph0][FREE1][ph2][FREE3][ph4]...[ph15]\n");

  // Can we create a placeholder spanning base → base+5*PAGE_SIZE?
  // No — base has a placeholder, base+PAGE_SIZE is FREE, etc.
  // The range isn't all FREE.

  // The ONLY way to reclaim: release ALL placeholders in the 64KB range,
  // then create one big placeholder, then split to desired layout.
  printf("  Releasing all remaining...\n");
  for (int i = 0; i < 16; i += 2)
    release_ph(base + i * PAGE_SIZE);
  // Skip already-released odd indices

  // Verify entire range is FREE
  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  printf("  After full release: state=0x%lx size=%lluKB\n",
         (unsigned long)mbi.State, (unsigned long long)mbi.RegionSize / 1024);

  // Recreate
  ph = create_ph(base, ALLOC_GRAN);
  CHECK(ph != NULL, "Recreated 64KB placeholder after full release");

  if (ph) {
    // Split to desired layout
    rem = base;
    for (int i = 0; i < 15; i++) {
      split_ph(rem, PAGE_SIZE);
      rem += PAGE_SIZE;
    }
    NOTE("Recreated all 16 page-sized placeholders from scratch");

    // Cleanup
    for (int i = 0; i < 16; i++)
      release_ph(base + i * PAGE_SIZE);
  }
}

//=============================================================================
// TEST 6: What happens if we MEM_RESERVE (non-placeholder) in a gap?
// Regular MEM_RESERVE might work at page granularity. But can the result
// participate in anything useful?
//=============================================================================
static void test_regular_reserve_in_gap(void) {
  printf("\n=== TEST 6: Regular MEM_RESERVE (non-placeholder) in gap ===\n");

  void *big = create_ph(NULL, ALLOC_GRAN);
  char *base = (char *)big;

  // Split [p0][rest]
  split_ph(base, PAGE_SIZE);
  // Release p0
  release_ph(base);

  // Try regular MEM_RESERVE at the gap
  PVOID addr = base;
  SIZE_T size = PAGE_SIZE;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &addr, &size,
      MEM_RESERVE, PAGE_NOACCESS, NULL, 0);
  printf("  MEM_RESERVE in 4KB gap: 0x%08lX addr=%p\n",
         (unsigned long)st, addr);

  if (NT_SUCCESS(st)) {
    NOTE("Regular MEM_RESERVE works in page-sized gap!");
    MEMORY_BASIC_INFORMATION mbi;
    query_mbi(addr, &mbi);
    printf("    State=0x%lx Type=0x%lx Size=%lluKB AllocBase=%p\n",
           (unsigned long)mbi.State, (unsigned long)mbi.Type,
           (unsigned long long)mbi.RegionSize / 1024, mbi.AllocationBase);
    printf("    is_placeholder=%d\n", is_placeholder(addr));

    // Can we coalesce reserve + placeholder?
    st = coalesce(base, 2 * PAGE_SIZE);
    printf("    Coalesce reserve+placeholder: 0x%08lX\n", (unsigned long)st);

    // Can we MEM_COMMIT + MEM_REPLACE_PLACEHOLDER on regular reserve?
    // No — it's not a placeholder. But can we commit it normally?
    PVOID ca = base;
    SIZE_T cs = PAGE_SIZE;
    st = NtAllocateVirtualMemoryEx(self(), &ca, &cs,
        MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    printf("    MEM_COMMIT on reserve: 0x%08lX\n", (unsigned long)st);

    // Release
    PVOID fa = base;
    SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &fa, &fs, MEM_RELEASE);
  }

  release_ph(base + PAGE_SIZE);
}

//=============================================================================
// TEST 7: The nuclear option — can we just coalesce BEFORE the gap exists?
// Simulate partial_unmap_view but coalesce left+freed+right BEFORE any remap
// or release, then split to the layout we actually need.
//=============================================================================
static void test_coalesce_then_resplit(void) {
  printf("\n=== TEST 7: Coalesce-then-resplit strategy ===\n");

  HANDLE section = NULL;
  {
    LARGE_INTEGER ms;
    ms.QuadPart = 8 * PAGE_SIZE;
    NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                       &ms, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
  }

  // Setup: 8-page section view
  void *ph = create_ph(NULL, 8 * PAGE_SIZE);
  char *base = (char *)ph;
  PVOID view = base;
  SIZE_T vs = 8 * PAGE_SIZE;
  LARGE_INTEGER off = {0};
  NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  memset(base, 0xAA, 8 * (size_t)PAGE_SIZE);

  // Unmap → placeholder
  NtUnmapViewOfSectionEx(self(), base, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);

  // Split: [left 2 pages][freed 4 pages][right 2 pages]
  split_ph(base, 2 * PAGE_SIZE);
  split_ph(base + 2 * PAGE_SIZE, 4 * PAGE_SIZE);

  printf("  After split: [left 8KB][freed 16KB][right 8KB]\n");

  // STRATEGY: instead of releasing freed NOW, coalesce everything first
  // then re-split to only carve out what we need.

  // Coalesce left+freed+right back to one
  NTSTATUS st = coalesce(base, 8 * PAGE_SIZE);
  CHECK(NT_SUCCESS(st), "Coalesce left+freed+right back to one");

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  printf("  Coalesced: size=%lluKB\n", (unsigned long long)mbi.RegionSize / 1024);

  // Now split at JUST the boundaries we need for remap:
  // [left 2 pages][rest 6 pages]
  split_ph(base, 2 * PAGE_SIZE);
  // [left][freed 4 pages][right 2 pages]
  split_ph(base + 2 * PAGE_SIZE, 4 * PAGE_SIZE);

  // Remap left from section
  PVOID lv = base;
  SIZE_T ls = 2 * PAGE_SIZE;
  LARGE_INTEGER lo = {0};
  st = NtMapViewOfSectionEx(section, self(), &lv, &lo, &ls,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  CHECK(NT_SUCCESS(st), "Remap left");

  // Remap right from section
  PVOID rv = base + 6 * PAGE_SIZE;
  SIZE_T rs = 2 * PAGE_SIZE;
  LARGE_INTEGER ro;
  ro.QuadPart = 6 * PAGE_SIZE;
  st = NtMapViewOfSectionEx(section, self(), &rv, &ro, &rs,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  CHECK(NT_SUCCESS(st), "Remap right");

  // Release freed middle
  release_ph(base + 2 * PAGE_SIZE);

  printf("  Final: [section left][FREE middle][section right]\n");

  // Verify content survived
  CHECK((unsigned char)base[0] == 0xAA, "Left content preserved");
  CHECK((unsigned char)base[6 * PAGE_SIZE] == 0xAA, "Right content preserved");

  // The gap is now 16KB (4 pages) — same as before. The coalesce-then-resplit
  // didn't help because we still have to release the freed middle.
  // BUT: if later another partial munmap removes the left or right,
  // the resulting placeholders from those unmaps can potentially span
  // into the freed area if we re-create a single large placeholder.

  NtUnmapViewOfSectionEx(self(), base, 0);
  NtUnmapViewOfSectionEx(self(), base + 6 * PAGE_SIZE, 0);
  NtClose(section);

  // Now base..base+8*PAGE_SIZE should be: [ph][FREE][ph]
  // Try the defrag: release both placeholders
  release_ph(base);
  release_ph(base + 6 * PAGE_SIZE);

  // Now the entire 32KB should be FREE
  query_mbi(base, &mbi);
  printf("  After releasing all: state=0x%lx size=%lluKB\n",
         (unsigned long)mbi.State, (unsigned long long)mbi.RegionSize / 1024);

  // Can we reclaim the whole range?
  // Only if the free range is ≥ 64KB and 64KB-aligned.
  // Our 32KB range is sub-64KB. So...
  void *reclaim = create_ph(base, 8 * PAGE_SIZE);
  printf("  Reclaim 32KB as placeholder: %s\n", reclaim ? "OK" : "FAIL");
  if (reclaim) {
    NOTE("Sub-64KB range reclaimed! (kernel may have rounded up)");
    release_ph(reclaim);
  } else {
    NOTE("Cannot reclaim sub-64KB free range as placeholder");
  }
}

int main(void) {
  printf("=== Sub-64KB Placeholder Gap Tests ===\n");

  test_min_create_size();
  test_unaligned_create();
  test_gap_analysis();
  test_release_absorption();
  test_oversized_create_and_split();
  test_regular_reserve_in_gap();
  test_coalesce_then_resplit();

  printf("\n========================================\n");
  printf("RESULTS: %d passed, %d failed\n", pass_count, fail_count);
  printf("========================================\n");
  return fail_count > 0 ? 1 : 0;
}
