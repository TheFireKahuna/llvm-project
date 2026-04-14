// coalesce_lineage_test.c — Probe placeholder AllocationBase lineage
// through lifecycle transitions and coalescing eligibility.
//
// Core question: when a placeholder fragment goes through
//   placeholder → section view → unmap-to-placeholder
// does it retain its AllocationBase from the original parent allocation?
// If yes, siblings from the same parent can always be coalesced.
//
// Also tests:
//   - Coalesce after section view roundtrip (the munmap recovery pattern)
//   - Coalesce after committed private roundtrip (the COW split pattern)
//   - AllocationBase tracking through split chains
//   - What happens when we coalesce after remap_with_cow_splits-style ops
//   - Coalesce eligibility after MEM_DECOMMIT → MEM_PRESERVE_PLACEHOLDER

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
#define MEM_DECOMMIT            0x00004000
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
#define PAGE_WRITECOPY  0x08

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

static void *alloc_base_of(void *addr) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!query_mbi(addr, &mbi)) return NULL;
  return mbi.AllocationBase;
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

static HANDLE create_section(SIZE_T size) {
  LARGE_INTEGER max_size;
  max_size.QuadPart = (long long)size;
  HANDLE section = NULL;
  NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                     &max_size, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
  return section;
}

static void dump_region(const char *label, void *addr) {
  MEMORY_BASIC_INFORMATION mbi;
  if (query_mbi(addr, &mbi)) {
    printf("    %s: addr=%p AllocBase=%p State=0x%lx Type=0x%lx Size=%lluK ph=%d\n",
           label, addr, mbi.AllocationBase,
           (unsigned long)mbi.State, (unsigned long)mbi.Type,
           (unsigned long long)mbi.RegionSize / 1024,
           is_placeholder(addr));
  }
}

//=============================================================================
// TEST 1: AllocationBase through placeholder → section → unmap-to-placeholder
//=============================================================================
static void test_alloc_base_lineage(void) {
  printf("\n=== TEST 1: AllocationBase lineage through section roundtrip ===\n");

  void *ph = create_ph(NULL, 3 * ALLOC_GRAN);
  char *base = (char *)ph;
  printf("  Original placeholder at %p\n", ph);

  split_ph(base, ALLOC_GRAN);                       // [A][BC]
  split_ph(base + ALLOC_GRAN, ALLOC_GRAN);           // [A][B][C]

  void *ab_a = alloc_base_of(base);
  void *ab_b = alloc_base_of(base + ALLOC_GRAN);
  void *ab_c = alloc_base_of(base + 2 * ALLOC_GRAN);
  printf("  After split: A.AllocBase=%p B.AllocBase=%p C.AllocBase=%p\n",
         ab_a, ab_b, ab_c);
  CHECK(ab_a == base && ab_b == base + ALLOC_GRAN && ab_c == base + 2 * ALLOC_GRAN,
        "Each split fragment has its own AllocationBase");

  // Map section into B
  HANDLE section = create_section(ALLOC_GRAN);
  PVOID view = base + ALLOC_GRAN;
  SIZE_T vs = ALLOC_GRAN;
  LARGE_INTEGER off = {0};
  NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  void *ab_b_mapped = alloc_base_of(base + ALLOC_GRAN);
  printf("  B as section view: AllocBase=%p\n", ab_b_mapped);

  // Unmap B back to placeholder
  NtUnmapViewOfSectionEx(self(), base + ALLOC_GRAN,
                          MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);

  void *ab_b_restored = alloc_base_of(base + ALLOC_GRAN);
  printf("  B after unmap-to-placeholder: AllocBase=%p\n", ab_b_restored);
  CHECK(is_placeholder(base + ALLOC_GRAN), "B is placeholder again");

  // Key question: does B's AllocationBase match what it was pre-map?
  CHECK(ab_b_restored == ab_b,
        "AllocationBase preserved through section roundtrip (%p == %p)",
        ab_b_restored, ab_b);

  // Can we coalesce A + B?
  NTSTATUS st = coalesce(base, 2 * ALLOC_GRAN);
  printf("  Coalesce A+B after B's section roundtrip: 0x%08lX\n",
         (unsigned long)st);
  if (NT_SUCCESS(st)) {
    CHECK(1, "Coalesce A+B works after section roundtrip!");
    MEMORY_BASIC_INFORMATION mbi;
    query_mbi(base, &mbi);
    printf("    Coalesced size: %lluK\n",
           (unsigned long long)mbi.RegionSize / 1024);

    // Can we coalesce AB + C?
    st = coalesce(base, 3 * ALLOC_GRAN);
    CHECK(NT_SUCCESS(st), "Full coalesce AB+C: 0x%08lX", (unsigned long)st);
  } else {
    CHECK(0, "Coalesce A+B FAILED — lineage broken by section roundtrip");
    // Try just B+C
    st = coalesce(base + ALLOC_GRAN, 2 * ALLOC_GRAN);
    printf("  Coalesce B+C: 0x%08lX\n", (unsigned long)st);
  }

  // Cleanup
  release_ph(base);
  // Release remaining fragments if coalesce failed
  release_ph(base + ALLOC_GRAN);
  release_ph(base + 2 * ALLOC_GRAN);
  NtClose(section);
}

//=============================================================================
// TEST 2: AllocationBase through placeholder → private commit → decommit
//         → preserve_placeholder roundtrip (the COW split recovery pattern)
//=============================================================================
static void test_alloc_base_private_roundtrip(void) {
  printf("\n=== TEST 2: AllocationBase through private commit roundtrip ===\n");

  void *ph = create_ph(NULL, 3 * ALLOC_GRAN);
  char *base = (char *)ph;

  split_ph(base, ALLOC_GRAN);
  split_ph(base + ALLOC_GRAN, ALLOC_GRAN);

  void *ab_b_pre = alloc_base_of(base + ALLOC_GRAN);
  printf("  B placeholder AllocBase: %p\n", ab_b_pre);

  // Replace B with private committed
  PVOID b = base + ALLOC_GRAN;
  SIZE_T bs = ALLOC_GRAN;
  NtAllocateVirtualMemoryEx(self(), &b, &bs,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  void *ab_b_committed = alloc_base_of(base + ALLOC_GRAN);
  printf("  B committed AllocBase: %p\n", ab_b_committed);

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base + ALLOC_GRAN, &mbi);
  printf("  B committed: State=0x%lx Type=0x%lx\n",
         (unsigned long)mbi.State, (unsigned long)mbi.Type);

  // Decommit B
  PVOID db = base + ALLOC_GRAN;
  SIZE_T ds = ALLOC_GRAN;
  NtFreeVirtualMemory(self(), &db, &ds, MEM_DECOMMIT);

  query_mbi(base + ALLOC_GRAN, &mbi);
  printf("  B decommitted: State=0x%lx Type=0x%lx\n",
         (unsigned long)mbi.State, (unsigned long)mbi.Type);

  // Convert back to placeholder via MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER
  // This is: NtFreeVirtualMemory(base, size=0, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)
  // Wait — MEM_PRESERVE_PLACEHOLDER on MEM_RELEASE converts committed→placeholder?
  // Actually, for MEM_PRIVATE regions, we need:
  //   NtFreeVirtualMemory(base, &size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)
  // where size>0 does a split, size=0 would release. Let me try the
  // preserve_to_placeholder approach:
  // Actually, the correct API for committed→placeholder is different.
  // Let me check if MEM_RELEASE on decommitted private with size=0 and
  // MEM_PRESERVE_PLACEHOLDER works.

  PVOID pb = base + ALLOC_GRAN;
  SIZE_T ps = 0;
  NTSTATUS st = NtFreeVirtualMemory(self(), &pb, &ps,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER on decommitted B: 0x%08lX\n",
         (unsigned long)st);

  if (NT_SUCCESS(st)) {
    CHECK(is_placeholder(base + ALLOC_GRAN),
          "B is placeholder after private roundtrip");
    void *ab_b_restored = alloc_base_of(base + ALLOC_GRAN);
    printf("  B restored AllocBase: %p\n", ab_b_restored);

    // Try coalesce A + B
    st = coalesce(base, 2 * ALLOC_GRAN);
    printf("  Coalesce A+B after private roundtrip: 0x%08lX\n",
           (unsigned long)st);
    CHECK(NT_SUCCESS(st), "Coalesce after private roundtrip");
  } else {
    printf("  Cannot convert decommitted private back to placeholder\n");
    // Try alternative: just release and see
    pb = base + ALLOC_GRAN;
    ps = 0;
    st = NtFreeVirtualMemory(self(), &pb, &ps, MEM_RELEASE);
    printf("  Plain MEM_RELEASE: 0x%08lX → B is now MEM_FREE\n",
           (unsigned long)st);
    CHECK(0, "Private→placeholder roundtrip not supported");
  }

  // Cleanup
  release_ph(base);
  release_ph(base + ALLOC_GRAN);
  release_ph(base + 2 * ALLOC_GRAN);
}

//=============================================================================
// TEST 3: The actual munmap pattern — full lifecycle
// [section view] → unmap middle → [left ph][FREE][right ph]
// → can left and right be coalesced if we re-create the middle placeholder?
//=============================================================================
static void test_munmap_pattern(void) {
  printf("\n=== TEST 3: Full munmap lifecycle with coalesce ===\n");

  // Create a 4-page placeholder, map section, then partial unmap middle 2 pages
  SIZE_T total = 4 * PAGE_SIZE;
  void *ph = create_ph(NULL, total);
  char *base = (char *)ph;

  HANDLE section = create_section(total);
  PVOID view = base;
  SIZE_T vs = total;
  LARGE_INTEGER off = {0};
  NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  printf("  Mapped 4-page section view at %p\n", base);

  // Write content
  for (int i = 0; i < 4; i++)
    ((char *)base)[i * PAGE_SIZE] = (char)(0x10 + i);

  // Simulate partial munmap: unmap entire view → placeholder
  NtUnmapViewOfSectionEx(self(), base, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);

  // Split: [page0-1][page2-3]
  split_ph(base, 2 * PAGE_SIZE);

  // Remap page 0-1 from section (the "kept left")
  PVOID left = base;
  SIZE_T left_size = 2 * PAGE_SIZE;
  LARGE_INTEGER left_off = {0};
  NtMapViewOfSectionEx(section, self(), &left, &left_off, &left_size,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  // Release page 2-3 (the "freed middle+right")
  release_ph(base + 2 * PAGE_SIZE);

  printf("  After partial munmap: [left view 0-1][FREE 2-3]\n");
  dump_region("left", base);
  dump_region("freed", base + 2 * PAGE_SIZE);

  // Now do ANOTHER partial munmap on the left: unmap page 0 only, keep page 1
  NtUnmapViewOfSectionEx(self(), base, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);

  // Split: [page0][page1]
  split_ph(base, PAGE_SIZE);

  // Release page 0 (freed)
  release_ph(base);

  // Keep page 1 as placeholder (simulating: about to remap it)
  printf("  After second partial munmap: [FREE 0][ph 1][FREE 2-3]\n");
  dump_region("page0", base);
  dump_region("page1", base + PAGE_SIZE);
  dump_region("page2", base + 2 * PAGE_SIZE);

  CHECK(is_placeholder(base + PAGE_SIZE), "Page 1 is still a placeholder");

  // Page 1 is an orphaned placeholder. It came from the same original
  // allocation as the view we just unmapped. Can we do anything useful with it?
  // In the real code, we'd remap it from the section. Let's verify that works.
  PVOID p1 = base + PAGE_SIZE;
  SIZE_T p1s = PAGE_SIZE;
  LARGE_INTEGER p1off;
  p1off.QuadPart = PAGE_SIZE;
  NTSTATUS st = NtMapViewOfSectionEx(section, self(), &p1, &p1off, &p1s,
                                      MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                      NULL, 0);
  CHECK(NT_SUCCESS(st), "Remap page 1 from section into orphaned placeholder");
  if (NT_SUCCESS(st))
    CHECK(((char *)base)[PAGE_SIZE] == 0x11,
          "Page 1 content correct after remap (0x%02x)",
          (unsigned char)((char *)base)[PAGE_SIZE]);

  // Cleanup
  NtUnmapViewOfSectionEx(self(), base + PAGE_SIZE, 0);
  NtClose(section);
}

//=============================================================================
// TEST 4: Can we coalesce BEFORE releasing the freed portion?
// Pattern: unmap → split [left][freed][right] → coalesce [left+freed+right]
// → then decide what to do with the single large placeholder
//=============================================================================
static void test_coalesce_before_release(void) {
  printf("\n=== TEST 4: Coalesce all fragments before release ===\n");

  SIZE_T total = 4 * ALLOC_GRAN;
  void *ph = create_ph(NULL, total);
  char *base = (char *)ph;

  HANDLE section = create_section(total);
  PVOID view = base;
  SIZE_T vs = total;
  LARGE_INTEGER off = {0};
  NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  // Unmap → placeholder
  NtUnmapViewOfSectionEx(self(), base, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);

  // Split into 4 pieces
  split_ph(base, ALLOC_GRAN);
  split_ph(base + ALLOC_GRAN, ALLOC_GRAN);
  split_ph(base + 2 * ALLOC_GRAN, ALLOC_GRAN);

  printf("  4 placeholder fragments:\n");
  for (int i = 0; i < 4; i++)
    dump_region("  frag", base + i * ALLOC_GRAN);

  // Coalesce ALL 4 back into one (before releasing any)
  NTSTATUS st = coalesce(base, total);
  CHECK(NT_SUCCESS(st), "Coalesce all 4 fragments: 0x%08lX", (unsigned long)st);

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == total, "Single placeholder of %lluKB",
        (unsigned long long)mbi.RegionSize / 1024);

  // Now we can re-split differently or release as one
  release_ph(base);
  NtClose(section);
}

//=============================================================================
// TEST 5: Coalesce subset, then use the coalesced piece
// Pattern: [A][B][C][D] → coalesce [B+C] → map section into [B+C]
//=============================================================================
static void test_coalesce_and_reuse(void) {
  printf("\n=== TEST 5: Coalesce subset then map into it ===\n");

  SIZE_T total = 4 * ALLOC_GRAN;
  void *ph = create_ph(NULL, total);
  char *base = (char *)ph;

  split_ph(base, ALLOC_GRAN);
  split_ph(base + ALLOC_GRAN, ALLOC_GRAN);
  split_ph(base + 2 * ALLOC_GRAN, ALLOC_GRAN);

  // Coalesce B+C
  NTSTATUS st = coalesce(base + ALLOC_GRAN, 2 * ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Coalesce B+C");

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base + ALLOC_GRAN, &mbi);
  CHECK(mbi.RegionSize == 2 * ALLOC_GRAN, "B+C is 128KB placeholder");

  // Map a section into the coalesced B+C
  HANDLE section = create_section(2 * ALLOC_GRAN);
  PVOID view = base + ALLOC_GRAN;
  SIZE_T vs = 2 * ALLOC_GRAN;
  LARGE_INTEGER off = {0};
  st = NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  CHECK(NT_SUCCESS(st), "Map section into coalesced B+C");

  // Write and verify
  if (NT_SUCCESS(st)) {
    memset(base + ALLOC_GRAN, 0xBB, 2 * (size_t)ALLOC_GRAN);
    CHECK((unsigned char)base[ALLOC_GRAN] == 0xBB, "Content correct");
    NtUnmapViewOfSectionEx(self(), base + ALLOC_GRAN, 0);
  }

  release_ph(base);
  release_ph(base + ALLOC_GRAN); // may fail if section view cleanup handles it
  release_ph(base + 3 * ALLOC_GRAN);
  NtClose(section);
}

//=============================================================================
// TEST 6: Page-granular split/coalesce (not just 64KB granularity)
//=============================================================================
static void test_page_granular_coalesce(void) {
  printf("\n=== TEST 6: Page-granular split and coalesce ===\n");

  // Create a 64KB placeholder, split into 16 × 4KB pages
  void *ph = create_ph(NULL, ALLOC_GRAN);
  char *base = (char *)ph;
  CHECK(ph != NULL, "Created 64KB placeholder");

  char *remaining = base;
  for (int i = 0; i < 15; i++) {
    CHECK(split_ph(remaining, PAGE_SIZE), "Page split %d", i);
    remaining += PAGE_SIZE;
  }

  // Verify all 16 are placeholders
  int count = 0;
  for (int i = 0; i < 16; i++) {
    if (is_placeholder(base + i * PAGE_SIZE)) count++;
  }
  CHECK(count == 16, "16 page-sized placeholders");

  // Coalesce pages 4-7 (4 pages = 16KB)
  NTSTATUS st = coalesce(base + 4 * PAGE_SIZE, 4 * PAGE_SIZE);
  CHECK(NT_SUCCESS(st), "Coalesce pages 4-7");

  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base + 4 * PAGE_SIZE, &mbi);
  CHECK(mbi.RegionSize == 4 * PAGE_SIZE, "Pages 4-7 coalesced to 16KB");

  // Pages 0-3 and 8-15 should still be individual
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == PAGE_SIZE, "Page 0 still 4KB");
  query_mbi(base + 8 * PAGE_SIZE, &mbi);
  CHECK(mbi.RegionSize == PAGE_SIZE, "Page 8 still 4KB");

  // Coalesce everything back
  st = coalesce(base, ALLOC_GRAN);
  CHECK(NT_SUCCESS(st), "Full coalesce back to 64KB");
  query_mbi(base, &mbi);
  CHECK(mbi.RegionSize == ALLOC_GRAN, "Back to 64KB");

  release_ph(base);
}

//=============================================================================
// TEST 7: Section view roundtrip preserves coalesce eligibility
// [A][B][C] → map section into B → unmap B → coalesce A+B+C
//=============================================================================
static void test_section_roundtrip_coalesce(void) {
  printf("\n=== TEST 7: Section roundtrip + full coalesce ===\n");

  SIZE_T total = 3 * ALLOC_GRAN;
  void *ph = create_ph(NULL, total);
  char *base = (char *)ph;

  split_ph(base, ALLOC_GRAN);
  split_ph(base + ALLOC_GRAN, ALLOC_GRAN);
  // [A][B][C]

  // Map section into B
  HANDLE section = create_section(ALLOC_GRAN);
  PVOID view = base + ALLOC_GRAN;
  SIZE_T vs = ALLOC_GRAN;
  LARGE_INTEGER off = {0};
  NtMapViewOfSectionEx(section, self(), &view, &off, &vs,
                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  // Write content
  memset(base + ALLOC_GRAN, 0xCC, (size_t)ALLOC_GRAN);

  // Unmap B back to placeholder
  NtUnmapViewOfSectionEx(self(), base + ALLOC_GRAN,
                          MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);

  CHECK(is_placeholder(base), "A is placeholder");
  CHECK(is_placeholder(base + ALLOC_GRAN), "B is placeholder (restored)");
  CHECK(is_placeholder(base + 2 * ALLOC_GRAN), "C is placeholder");

  // Coalesce all three
  NTSTATUS st = coalesce(base, total);
  printf("  Coalesce A+B(roundtripped)+C: 0x%08lX\n", (unsigned long)st);
  CHECK(NT_SUCCESS(st), "Full coalesce after section roundtrip on middle piece");

  MEMORY_BASIC_INFORMATION mbi;
  if (NT_SUCCESS(st)) {
    query_mbi(base, &mbi);
    CHECK(mbi.RegionSize == total, "Coalesced to %lluKB",
          (unsigned long long)mbi.RegionSize / 1024);
  }

  release_ph(base);
  release_ph(base + ALLOC_GRAN);
  release_ph(base + 2 * ALLOC_GRAN);
  NtClose(section);
}

int main(void) {
  printf("=== Placeholder Coalesce Lineage Tests ===\n");

  test_alloc_base_lineage();
  test_alloc_base_private_roundtrip();
  test_munmap_pattern();
  test_coalesce_before_release();
  test_coalesce_and_reuse();
  test_page_granular_coalesce();
  test_section_roundtrip_coalesce();

  printf("\n========================================\n");
  printf("RESULTS: %d passed, %d failed\n", pass_count, fail_count);
  printf("========================================\n");
  return fail_count > 0 ? 1 : 0;
}
