// reserve_replace_test.c — Thorough test of reserve-only placeholder replacement
//
// Validates the full lifecycle needed for a MAP_NORESERVE implementation:
//   placeholder → MEM_RESERVE|MEM_REPLACE_PLACEHOLDER → demand-commit →
//   decommit → preserve-to-placeholder → coalesce/release
//
// This is the path that could eliminate SEC_RESERVE sections entirely
// for MAP_NORESERVE, removing the split-remap protocol from that codepath.
//
// Build: clang-cl -O2 reserve_replace_test.c
//        /link ntdll.lib kernel32.lib /subsystem:console

#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef void *PVOID;
typedef void *HANDLE;
typedef unsigned long ULONG;
typedef unsigned long DWORD;
typedef unsigned long long SIZE_T;
typedef unsigned long long ULONG_PTR;
typedef unsigned long long DWORD64;
typedef long NTSTATUS;
typedef long LONG;
typedef unsigned long *PULONG;

#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define NT_ERROR(s)   ((NTSTATUS)(s) < 0)

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
  ULONG_PTR PartitionIdEx;
  ULONG_PTR NodePreference;
} MEMORY_REGION_INFORMATION;

typedef struct {
  struct {
    DWORD64 Type : 8;
    DWORD64 Reserved : 56;
  };
  union {
    DWORD64 ULong64;
    PVOID Pointer;
    SIZE_T Size;
    HANDLE Handle;
    DWORD ULong;
  };
} MEM_EXTENDED_PARAMETER;

#define MEM_COMMIT              0x00001000
#define MEM_RESERVE             0x00002000
#define MEM_DECOMMIT            0x00004000
#define MEM_RELEASE             0x00008000
#define MEM_FREE                0x00010000
#define MEM_PRIVATE             0x00020000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001

#define PAGE_NOACCESS           0x01
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_EXECUTE_READ       0x20
#define PAGE_GUARD              0x100

#define SEC_COMMIT              0x08000000
#define SEC_RESERVE             0x04000000
#define SECTION_ALL_ACCESS      0x000F001F

#define MemoryRegionInformationEx 7

extern NTSTATUS NTAPI NtAllocateVirtualMemoryEx(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
extern NTSTATUS NTAPI NtFreeVirtualMemory(
    HANDLE, PVOID *, SIZE_T *, ULONG);
extern NTSTATUS NTAPI NtProtectVirtualMemory(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG *);
extern NTSTATUS NTAPI NtQueryVirtualMemory(
    HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);
extern NTSTATUS NTAPI NtCreateSectionEx(
    HANDLE *, ULONG, PVOID, LARGE_INTEGER *, ULONG, ULONG, HANDLE,
    MEM_EXTENDED_PARAMETER *, ULONG);
extern NTSTATUS NTAPI NtMapViewOfSectionEx(
    HANDLE, HANDLE, PVOID *, LARGE_INTEGER *, SIZE_T *, ULONG, ULONG,
    MEM_EXTENDED_PARAMETER *, ULONG);
extern NTSTATUS NTAPI NtUnmapViewOfSectionEx(HANDLE, PVOID, ULONG);
extern NTSTATUS NTAPI NtClose(HANDLE);

extern unsigned char NTAPI RtlQueryPerformanceCounter(LARGE_INTEGER *);
extern unsigned char NTAPI RtlQueryPerformanceFrequency(LARGE_INTEGER *);

static HANDLE self(void) { return (HANDLE)(long long)(-1); }
#define PAGE_SIZE  4096ULL
#define ALLOC_GRAN 65536ULL

static int query_mbi(void *addr, MEMORY_BASIC_INFORMATION *mbi) {
  return NT_SUCCESS(NtQueryVirtualMemory(self(), addr, 0, mbi, sizeof(*mbi), 0));
}
static int query_mri(void *addr, MEMORY_REGION_INFORMATION *mri) {
  return NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryRegionInformationEx,
                                         mri, sizeof(*mri), 0));
}

static long long qpc_freq(void) {
  LARGE_INTEGER f; RtlQueryPerformanceFrequency(&f); return f.QuadPart;
}
static long long qpc_now(void) {
  LARGE_INTEGER t; RtlQueryPerformanceCounter(&t); return t.QuadPart;
}
static long long ticks_to_ns(long long t, long long f) {
  return (t * 1000000000LL) / f;
}

static int total_pass = 0, total_fail = 0, total_skip = 0;
#define PASS(fmt, ...) do { total_pass++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } while(0)
#define FAIL(fmt, ...) do { total_fail++; printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define SKIP(fmt, ...) do { total_skip++; printf("  SKIP: " fmt "\n", ##__VA_ARGS__); } while(0)
#define INFO(fmt, ...) printf("  info: " fmt "\n", ##__VA_ARGS__)

static void release_all(void *base) {
  PVOID b = base; SIZE_T s = 0;
  NtFreeVirtualMemory(self(), &b, &s, MEM_RELEASE);
}


//=============================================================================
// TEST 1: Basic lifecycle
//
// placeholder → reserve-replace → commit → write → read → decommit →
// preserve-to-placeholder → release
//=============================================================================
static void test_basic_lifecycle(void) {
  printf("\n=== TEST 1: Basic Lifecycle ===\n");

  PVOID ph = NULL;
  SIZE_T ph_size = 16 * ALLOC_GRAN; // 1MB
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("placeholder: 0x%08lX", (unsigned long)st); return; }
  PASS("Created 1MB placeholder at %p", ph);

  // Reserve-replace
  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("reserve-replace: 0x%08lX", (unsigned long)st); release_all(ph); return; }
  PASS("Reserve-replaced 1MB");

  // Commit scattered pages (simulating VEH demand-commit)
  int pages_to_commit[] = {0, 5, 10, 50, 100, 200, 255};
  int n_pages = sizeof(pages_to_commit) / sizeof(pages_to_commit[0]);
  for (int i = 0; i < n_pages; i++) {
    PVOID cm = (char *)rep + pages_to_commit[i] * PAGE_SIZE;
    SIZE_T cm_size = PAGE_SIZE;
    st = NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
        MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    if (NT_ERROR(st)) { FAIL("commit page %d: 0x%08lX", pages_to_commit[i], (unsigned long)st); }
  }
  PASS("Committed %d scattered pages", n_pages);

  // Write and read back
  int rw_ok = 1;
  for (int i = 0; i < n_pages; i++) {
    volatile char *p = (volatile char *)rep + pages_to_commit[i] * PAGE_SIZE;
    *p = (char)(i + 'A');
    if (*p != (char)(i + 'A')) rw_ok = 0;
  }
  if (rw_ok) PASS("Write/read on all committed pages");
  else FAIL("Write/read mismatch");

  // Decommit a range spanning committed and reserved pages
  PVOID decom = (char *)rep + 4 * PAGE_SIZE;
  SIZE_T decom_size = 8 * PAGE_SIZE; // pages 4-11 (includes committed page 5, 10)
  st = NtFreeVirtualMemory(self(), &decom, &decom_size, MEM_DECOMMIT);
  if (NT_SUCCESS(st)) PASS("Decommit mixed committed+reserved range");
  else FAIL("Decommit: 0x%08lX", (unsigned long)st);

  // Preserve-to-placeholder on a granule-aligned range
  PVOID punch = (char *)rep + 4 * ALLOC_GRAN;
  SIZE_T punch_size = 4 * ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &punch, &punch_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  if (NT_SUCCESS(st)) {
    PASS("Preserve-to-placeholder on 256KB sub-range");
    MEMORY_REGION_INFORMATION mri;
    if (query_mri((char *)rep + 4 * ALLOC_GRAN, &mri) && mri.PlaceholderReservation)
      PASS("Punched region is a valid placeholder");
    else
      FAIL("Punched region is not a placeholder");
  } else {
    FAIL("Preserve-to-placeholder: 0x%08lX", (unsigned long)st);
  }

  // Verify remaining region is intact
  MEMORY_BASIC_INFORMATION mbi;
  if (query_mbi(rep, &mbi))
    INFO("Remaining head: State=0x%lx AllocBase=%p RegionSize=%lluK",
         (unsigned long)mbi.State, mbi.AllocationBase,
         (unsigned long long)mbi.RegionSize / 1024);

  release_all(rep);
  // Release punched placeholder too (separate allocation now)
  PVOID f = (char *)ph + 4 * ALLOC_GRAN; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  // Release tail
  f = (char *)ph + 8 * ALLOC_GRAN; fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
}


//=============================================================================
// TEST 2: Multiple punches in same allocation
//
// Reserve-replace 1MB, then punch 3 holes → creates 4 live regions + 3
// placeholders. Simulates scattered partial munmap on a MAP_NORESERVE region.
//=============================================================================
static void test_multi_punch(void) {
  printf("\n=== TEST 2: Multiple Punches ===\n");

  PVOID ph = NULL;
  SIZE_T ph_size = 16 * ALLOC_GRAN;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("placeholder: 0x%08lX", (unsigned long)st); return; }

  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("reserve-replace: 0x%08lX", (unsigned long)st); release_all(ph); return; }

  // Punch 3 holes: [2*GRAN, 4*GRAN), [6*GRAN, 8*GRAN), [12*GRAN, 14*GRAN)
  struct { int start_gran; int size_gran; } holes[] = {
    {2, 2}, {6, 2}, {12, 2}
  };
  int punch_ok = 1;
  for (int i = 0; i < 3; i++) {
    PVOID p = (char *)rep + holes[i].start_gran * ALLOC_GRAN;
    SIZE_T ps = holes[i].size_gran * ALLOC_GRAN;
    st = NtFreeVirtualMemory(self(), &p, &ps,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    if (NT_ERROR(st)) {
      FAIL("Punch hole %d: 0x%08lX", i, (unsigned long)st);
      punch_ok = 0;
    }
  }
  if (punch_ok) PASS("Punched 3 holes (6 granules total)");

  // Verify: should have 4 reserved regions + 3 placeholders
  int n_reserved = 0, n_placeholder = 0, n_other = 0;
  for (int g = 0; g < 16; g++) {
    MEMORY_REGION_INFORMATION mri;
    char *addr = (char *)ph + g * ALLOC_GRAN;
    if (query_mri(addr, &mri)) {
      if (mri.PlaceholderReservation) n_placeholder++;
      else if (mri.Private) n_reserved++;
      else n_other++;
      // Skip to end of this region
      char *region_end = (char *)mri.AllocationBase + mri.RegionSize;
      int end_gran = (int)((region_end - (char *)ph) / ALLOC_GRAN);
      if (end_gran > g + 1) g = end_gran - 1; // -1 because loop increments
    } else {
      n_other++;
    }
  }
  INFO("Regions: %d reserved, %d placeholder, %d other", n_reserved, n_placeholder, n_other);
  if (n_placeholder == 3)
    PASS("3 placeholders from 3 punches");
  else
    FAIL("Expected 3 placeholders, got %d", n_placeholder);

  // Commit a page in each live region to verify they're usable
  int live_starts[] = {0, 4, 8, 14};
  int commit_ok = 1;
  for (int i = 0; i < 4; i++) {
    PVOID cm = (char *)ph + live_starts[i] * ALLOC_GRAN;
    SIZE_T cm_size = PAGE_SIZE;
    st = NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
        MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    if (NT_ERROR(st)) { commit_ok = 0; break; }
    volatile char *p = (volatile char *)cm;
    *p = (char)('0' + i);
    if (*p != (char)('0' + i)) { commit_ok = 0; break; }
  }
  if (commit_ok) PASS("All 4 live regions are committable and writable");
  else FAIL("Live region commit/write failed");

  // Cleanup: release each piece individually
  for (int g = 0; g < 16; ) {
    char *addr = (char *)ph + g * ALLOC_GRAN;
    MEMORY_REGION_INFORMATION mri;
    if (query_mri(addr, &mri)) {
      PVOID f = mri.AllocationBase; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
      int end_gran = (int)(((char *)mri.AllocationBase + mri.RegionSize - (char *)ph) / ALLOC_GRAN);
      g = end_gran > g + 1 ? end_gran : g + 1;
    } else {
      g++;
    }
  }
}


//=============================================================================
// TEST 3: Page-granular punch (sub-64KB)
//
// Can preserve-to-placeholder work at page granularity within a
// reserve-replaced region? Placeholders split at page boundaries,
// so this should work — but verify the exact alignment constraints.
//=============================================================================
static void test_page_granular_punch(void) {
  printf("\n=== TEST 3: Page-Granular Punch ===\n");

  PVOID ph = NULL;
  SIZE_T ph_size = 4 * ALLOC_GRAN;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("placeholder: 0x%08lX", (unsigned long)st); return; }

  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("reserve-replace: 0x%08lX", (unsigned long)st); release_all(ph); return; }

  // Try punching exactly 1 page in the middle
  PVOID punch1 = (char *)rep + ALLOC_GRAN + 4 * PAGE_SIZE;
  SIZE_T punch1_size = PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &punch1, &punch1_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  INFO("1-page punch at +68KB: 0x%08lX", (unsigned long)st);
  if (NT_SUCCESS(st)) {
    PASS("Single-page preserve-to-placeholder works!");
    MEMORY_REGION_INFORMATION mri;
    if (query_mri(punch1, &mri))
      INFO("Punched page: PlaceholderReservation=%u RegionSize=%llu",
           mri.PlaceholderReservation, (unsigned long long)mri.RegionSize);
  } else {
    FAIL("Single-page punch failed: 0x%08lX — may need granule alignment",
         (unsigned long)st);
  }

  // Try punching 3 pages (12KB) — not aligned to 64KB
  PVOID punch2 = (char *)rep + 2 * ALLOC_GRAN + 8 * PAGE_SIZE;
  SIZE_T punch2_size = 3 * PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &punch2, &punch2_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  INFO("3-page punch at +160KB: 0x%08lX", (unsigned long)st);
  if (NT_SUCCESS(st))
    PASS("Sub-granule (3-page) preserve-to-placeholder works");
  else
    FAIL("Sub-granule punch failed: 0x%08lX", (unsigned long)st);

  // Cleanup
  for (SIZE_T off = 0; off < ph_size; ) {
    char *addr = (char *)ph + off;
    MEMORY_REGION_INFORMATION mri;
    if (query_mri(addr, &mri)) {
      PVOID f = mri.AllocationBase; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
      SIZE_T extent = (char *)mri.AllocationBase + mri.RegionSize - (char *)ph;
      off = extent > off + PAGE_SIZE ? extent : off + PAGE_SIZE;
    } else {
      off += PAGE_SIZE;
    }
  }
}


//=============================================================================
// TEST 4: Protection changes on committed pages
//
// After reserve-replace + commit, can we change protection freely?
// This is needed for mprotect on MAP_NORESERVE regions.
//=============================================================================
static void test_protection_changes(void) {
  printf("\n=== TEST 4: Protection Changes ===\n");

  PVOID ph = NULL;
  SIZE_T ph_size = 4 * ALLOC_GRAN;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("placeholder: 0x%08lX", (unsigned long)st); return; }

  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("reserve-replace: 0x%08lX", (unsigned long)st); release_all(ph); return; }

  // Commit 4 pages as READWRITE
  PVOID cm = rep;
  SIZE_T cm_size = 4 * PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
      MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  if (NT_ERROR(st)) { FAIL("commit: 0x%08lX", (unsigned long)st); release_all(rep); return; }

  // Write data
  volatile char *p = (volatile char *)rep;
  p[0] = 'H'; p[PAGE_SIZE] = 'e'; p[2*PAGE_SIZE] = 'l'; p[3*PAGE_SIZE] = 'o';

  // Change to READONLY
  PVOID pp = rep;
  SIZE_T pp_size = 4 * PAGE_SIZE;
  ULONG old_prot;
  st = NtProtectVirtualMemory(self(), &pp, &pp_size, PAGE_READONLY, &old_prot);
  if (NT_SUCCESS(st)) {
    PASS("Changed to PAGE_READONLY (old=0x%lx)", (unsigned long)old_prot);
    // Verify read still works
    if (p[0] == 'H' && p[PAGE_SIZE] == 'e')
      PASS("Read after READONLY change works");
  } else {
    FAIL("NtProtectVirtualMemory READONLY: 0x%08lX", (unsigned long)st);
  }

  // Change to EXECUTE_READ
  pp = rep; pp_size = 4 * PAGE_SIZE;
  st = NtProtectVirtualMemory(self(), &pp, &pp_size, PAGE_EXECUTE_READ, &old_prot);
  if (NT_SUCCESS(st))
    PASS("Changed to PAGE_EXECUTE_READ");
  else
    FAIL("PAGE_EXECUTE_READ: 0x%08lX", (unsigned long)st);

  // Change back to READWRITE
  pp = rep; pp_size = 4 * PAGE_SIZE;
  st = NtProtectVirtualMemory(self(), &pp, &pp_size, PAGE_READWRITE, &old_prot);
  if (NT_SUCCESS(st)) {
    PASS("Changed back to PAGE_READWRITE");
    p[0] = 'Z';
    if (p[0] == 'Z') PASS("Write after READWRITE restore works");
  } else {
    FAIL("PAGE_READWRITE restore: 0x%08lX", (unsigned long)st);
  }

  // Commit with non-default protection
  PVOID cm2 = (char *)rep + ALLOC_GRAN;
  SIZE_T cm2_size = PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &cm2, &cm2_size,
      MEM_COMMIT, PAGE_READONLY, NULL, 0);
  if (NT_SUCCESS(st))
    PASS("Commit with PAGE_READONLY works");
  else
    FAIL("Commit PAGE_READONLY: 0x%08lX", (unsigned long)st);

  release_all(rep);
}


//=============================================================================
// TEST 5: Decommit of never-committed (reserved) pages
//
// In the current SEC_RESERVE path, pages start as MEM_RESERVE within
// the section view. Decommit on reserved pages should be a no-op or
// succeed — verify behavior for the reserve-replace model.
//=============================================================================
static void test_decommit_reserved(void) {
  printf("\n=== TEST 5: Decommit Reserved Pages ===\n");

  PVOID ph = NULL;
  SIZE_T ph_size = 4 * ALLOC_GRAN;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("placeholder: 0x%08lX", (unsigned long)st); return; }

  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("reserve-replace: 0x%08lX", (unsigned long)st); release_all(ph); return; }

  // Decommit on never-committed pages
  PVOID decom = (char *)rep + ALLOC_GRAN;
  SIZE_T decom_size = ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &decom, &decom_size, MEM_DECOMMIT);
  INFO("Decommit on reserved (never committed): 0x%08lX", (unsigned long)st);
  if (NT_SUCCESS(st))
    PASS("Decommit on reserved pages succeeds (no-op)");
  else
    INFO("Decommit on reserved pages returns 0x%08lX — may need commit-first",
         (unsigned long)st);

  // Commit, then decommit, then preserve-to-placeholder
  PVOID cm = (char *)rep + 2 * ALLOC_GRAN;
  SIZE_T cm_size = ALLOC_GRAN;
  NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
      MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  volatile char *p = (volatile char *)cm;
  *p = 'X'; // dirty the page

  decom = (char *)rep + 2 * ALLOC_GRAN;
  decom_size = ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &decom, &decom_size, MEM_DECOMMIT);
  if (NT_SUCCESS(st)) PASS("Decommit committed+dirty pages");

  // Preserve-to-placeholder on the decommitted range
  PVOID punch = (char *)rep + 2 * ALLOC_GRAN;
  SIZE_T punch_size = ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &punch, &punch_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  if (NT_SUCCESS(st))
    PASS("Preserve-to-placeholder after commit→decommit cycle");
  else
    FAIL("Preserve-to-placeholder after decommit: 0x%08lX", (unsigned long)st);

  // Can we preserve-to-placeholder directly on reserved (never-committed) pages?
  PVOID punch2 = (char *)rep + 3 * ALLOC_GRAN;
  SIZE_T punch2_size = ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &punch2, &punch2_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  INFO("Preserve-to-placeholder on reserved-only pages: 0x%08lX", (unsigned long)st);
  if (NT_SUCCESS(st))
    PASS("Direct preserve-to-placeholder on reserved pages works — no decommit needed!");
  else
    INFO("Must decommit before preserve (or pages were already decommitted by test above)");

  // Cleanup
  for (SIZE_T off = 0; off < ph_size; ) {
    char *addr = (char *)ph + off;
    MEMORY_REGION_INFORMATION mri;
    if (query_mri(addr, &mri)) {
      PVOID f = mri.AllocationBase; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
      SIZE_T extent = (char *)mri.AllocationBase + mri.RegionSize - (char *)ph;
      off = extent > off + ALLOC_GRAN ? extent : off + ALLOC_GRAN;
    } else {
      off += ALLOC_GRAN;
    }
  }
}


//=============================================================================
// TEST 6: Performance — reserve-replace vs SEC_RESERVE section
//
// Compare the cost of the two models:
//   Model A: placeholder → reserve-replace → commit pages on demand
//   Model B: placeholder → SEC_RESERVE section → map view → commit pages
//
// Measure: allocation setup, per-page commit, and partial unmap.
//=============================================================================
static void test_perf_vs_section(void) {
  printf("\n=== TEST 6: Performance vs SEC_RESERVE Section ===\n");

  long long freq = qpc_freq();
  int iters = 200;
  SIZE_T region_size = 64 * PAGE_SIZE; // 256KB
  int commit_pages = 16;

  // --- Model A: reserve-replace ---
  long long t0 = qpc_now();
  for (int i = 0; i < iters; i++) {
    // Setup
    PVOID ph = NULL;
    SIZE_T ps = region_size;
    NtAllocateVirtualMemoryEx(self(), &ph, &ps,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    PVOID rep = ph;
    SIZE_T rs = ps;
    NtAllocateVirtualMemoryEx(self(), &rep, &rs,
        MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);

    // Demand-commit scattered pages
    for (int p = 0; p < commit_pages; p++) {
      PVOID cm = (char *)rep + p * 4 * PAGE_SIZE;
      SIZE_T cms = PAGE_SIZE;
      NtAllocateVirtualMemoryEx(self(), &cm, &cms,
          MEM_COMMIT, PAGE_READWRITE, NULL, 0);
      *(volatile char *)cm = (char)p;
    }

    // Partial unmap (decommit + preserve middle third)
    SIZE_T third = region_size / 3;
    third = (third + ALLOC_GRAN - 1) & ~(ALLOC_GRAN - 1); // round up
    PVOID decom = (char *)rep + third;
    SIZE_T decom_size = third;
    NtFreeVirtualMemory(self(), &decom, &decom_size, MEM_DECOMMIT);
    PVOID punch = (char *)rep + third;
    SIZE_T punch_size = third;
    NtFreeVirtualMemory(self(), &punch, &punch_size,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);

    // Cleanup
    PVOID f1 = rep; SIZE_T f1s = 0;
    NtFreeVirtualMemory(self(), &f1, &f1s, MEM_RELEASE);
    PVOID f2 = (char *)ph + third; SIZE_T f2s = 0;
    NtFreeVirtualMemory(self(), &f2, &f2s, MEM_RELEASE);
    PVOID f3 = (char *)ph + 2 * third; SIZE_T f3s = 0;
    NtFreeVirtualMemory(self(), &f3, &f3s, MEM_RELEASE);
  }
  long long model_a_ns = ticks_to_ns(qpc_now() - t0, freq);

  // --- Model B: SEC_RESERVE section ---
  long long t1 = qpc_now();
  for (int i = 0; i < iters; i++) {
    // Setup: placeholder + section + map
    PVOID ph = NULL;
    SIZE_T ps = region_size;
    NtAllocateVirtualMemoryEx(self(), &ph, &ps,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);

    LARGE_INTEGER max_size;
    max_size.QuadPart = (long long)region_size;
    HANDLE section = NULL;
    NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL, &max_size,
                      PAGE_READWRITE, SEC_RESERVE, NULL, NULL, 0);

    PVOID view = ph;
    SIZE_T vs = ps;
    LARGE_INTEGER offset = {0};
    NtMapViewOfSectionEx(section, self(), &view, &offset, &vs,
        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

    // Demand-commit scattered pages
    for (int p = 0; p < commit_pages; p++) {
      PVOID cm = (char *)view + p * 4 * PAGE_SIZE;
      SIZE_T cms = PAGE_SIZE;
      NtAllocateVirtualMemoryEx(self(), &cm, &cms,
          MEM_COMMIT, PAGE_READWRITE, NULL, 0);
      *(volatile char *)cm = (char)p;
    }

    // Partial unmap — must unmap entire view, split, remap kept fragments
    NtUnmapViewOfSectionEx(self(), view, 0x02 /* PRESERVE_PLACEHOLDER */);
    // (In real code: split + remap. Here just measure the unmap cost.)
    NtClose(section);

    // Cleanup placeholder
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }
  long long model_b_ns = ticks_to_ns(qpc_now() - t1, freq);

  INFO("%d iterations, %llu KB region, %d committed pages:",
       iters, (unsigned long long)region_size / 1024, commit_pages);
  INFO("  Model A (reserve-replace): %lldns total, %lldns/iter",
       model_a_ns, model_a_ns / iters);
  INFO("  Model B (SEC_RESERVE):     %lldns total, %lldns/iter",
       model_b_ns, model_b_ns / iters);

  long long diff = model_b_ns - model_a_ns;
  if (model_a_ns < model_b_ns)
    PASS("Reserve-replace is %lldns/iter faster (%lld%% improvement)",
         diff / iters, (diff * 100) / model_b_ns);
  else
    INFO("SEC_RESERVE is faster by %lldns/iter — reserve-replace has no perf advantage",
         -diff / iters);
}


//=============================================================================
// TEST 7: Re-fill punched placeholder
//
// After preserve-to-placeholder creates a hole, can we reserve-replace
// back into it and commit new pages? Full create→punch→refill cycle.
//=============================================================================
static void test_refill_placeholder(void) {
  printf("\n=== TEST 7: Refill Punched Placeholder ===\n");

  PVOID ph = NULL;
  SIZE_T ph_size = 4 * ALLOC_GRAN;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("placeholder: 0x%08lX", (unsigned long)st); return; }

  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("reserve-replace: 0x%08lX", (unsigned long)st); release_all(ph); return; }

  // Punch middle 2 granules
  PVOID punch = (char *)rep + ALLOC_GRAN;
  SIZE_T punch_size = 2 * ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &punch, &punch_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  if (NT_ERROR(st)) { FAIL("punch: 0x%08lX", (unsigned long)st); release_all(rep); return; }
  PASS("Punched middle 128KB");

  // Re-fill: reserve-replace the placeholder
  PVOID refill = (char *)ph + ALLOC_GRAN;
  SIZE_T refill_size = 2 * ALLOC_GRAN;
  st = NtAllocateVirtualMemoryEx(self(), &refill, &refill_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_SUCCESS(st)) {
    PASS("Re-filled punched placeholder with reserve-replace");

    // Commit and write
    PVOID cm = refill;
    SIZE_T cm_size = PAGE_SIZE;
    st = NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
        MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    if (NT_SUCCESS(st)) {
      volatile char *p = (volatile char *)refill;
      *p = 'R';
      if (*p == 'R')
        PASS("Refilled region is committable and writable");
    }
  } else {
    FAIL("Re-fill reserve-replace: 0x%08lX", (unsigned long)st);
  }

  // Can we also re-fill with committed (the normal mmap path)?
  // Punch again, then use MEM_RESERVE|MEM_COMMIT|MEM_REPLACE_PLACEHOLDER
  punch = (char *)ph + ALLOC_GRAN;
  punch_size = 2 * ALLOC_GRAN;
  // First decommit anything committed
  NtFreeVirtualMemory(self(), &punch, &punch_size, MEM_DECOMMIT);
  punch = (char *)ph + ALLOC_GRAN;
  punch_size = 2 * ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &punch, &punch_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  if (NT_SUCCESS(st)) {
    PVOID refill2 = (char *)ph + ALLOC_GRAN;
    SIZE_T refill2_size = 2 * ALLOC_GRAN;
    st = NtAllocateVirtualMemoryEx(self(), &refill2, &refill2_size,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
        NULL, 0);
    if (NT_SUCCESS(st))
      PASS("Re-fill with committed replace also works (normal mmap path)");
    else
      FAIL("Committed re-fill: 0x%08lX", (unsigned long)st);
  }

  // Cleanup
  for (SIZE_T off = 0; off < ph_size; ) {
    char *addr = (char *)ph + off;
    MEMORY_REGION_INFORMATION mri;
    if (query_mri(addr, &mri)) {
      PVOID f = mri.AllocationBase; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
      SIZE_T extent = (char *)mri.AllocationBase + mri.RegionSize - (char *)ph;
      off = extent > off + ALLOC_GRAN ? extent : off + ALLOC_GRAN;
    } else {
      off += ALLOC_GRAN;
    }
  }
}


//=============================================================================
// main
//=============================================================================
int main(void) {
  printf("=== Reserve-Replace Comprehensive Test ===\n");
  printf("Testing the MAP_NORESERVE simplification path\n");

  test_basic_lifecycle();
  test_multi_punch();
  test_page_granular_punch();
  test_protection_changes();
  test_decommit_reserved();
  test_perf_vs_section();
  test_refill_placeholder();

  printf("\n=== SUMMARY ===\n");
  printf("  PASS: %d\n", total_pass);
  printf("  FAIL: %d\n", total_fail);
  printf("  SKIP: %d\n", total_skip);

  if (total_fail == 0)
    printf("\nAll tests passed — reserve-replace is viable for MAP_NORESERVE!\n");

  return total_fail > 0 ? 1 : 0;
}
