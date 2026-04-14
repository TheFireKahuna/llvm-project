// wsex_cow_probe.c — Can MemoryWorkingSetExInformation detect COW pages?
//
// Hypothesis: SharedOriginal=1 means page is clean (still shared with section).
// SharedOriginal=0 means page was COW'd (private copy exists).
// This would give us per-page COW detection for the split-remap algorithm,
// replacing the region-granular WRITECOPY→READWRITE heuristic.
//
// Also probes: MemoryPhysicalContiguityInformation, MemoryBadInformation,
// MemorySharedCommitInformation, MemoryBasicInformationCapped — anything
// that might give us useful per-page state.

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

#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

typedef union {
  struct { ULONG LowPart; LONG HighPart; };
  long long QuadPart;
} LARGE_INTEGER;

// WSEX structures
typedef union {
  ULONG_PTR Flags;
  struct {
    ULONG_PTR Valid : 1;
    ULONG_PTR ShareCount : 3;
    ULONG_PTR Win32Protection : 11;
    ULONG_PTR Shared : 1;
    ULONG_PTR Node : 6;
    ULONG_PTR Locked : 1;
    ULONG_PTR LargePage : 1;
    ULONG_PTR Priority : 3;
    ULONG_PTR Reserved : 3;
    ULONG_PTR SharedOriginal : 1;
    ULONG_PTR Bad : 1;
    ULONG_PTR Win32GraphicsProtection : 4;
    ULONG_PTR ReservedUlong : 28;
  };
  struct {
    ULONG_PTR Valid_ : 1;
    ULONG_PTR Reserved0 : 14;
    ULONG_PTR Shared_ : 1;
    ULONG_PTR Reserved1 : 5;
    ULONG_PTR PageTable : 1;
    ULONG_PTR Location : 2;
    ULONG_PTR Priority_ : 3;
    ULONG_PTR ModifiedList : 1;
    ULONG_PTR Reserved2 : 2;
    ULONG_PTR SharedOriginal_ : 1;
    ULONG_PTR Bad_ : 1;
    ULONG_PTR ReservedUlong_ : 32;
  } Invalid;
} MEMORY_WORKING_SET_EX_BLOCK;

typedef struct {
  PVOID VirtualAddress;
  MEMORY_WORKING_SET_EX_BLOCK VirtualAttributes;
} MEMORY_WORKING_SET_EX_INFORMATION;

// Shared commit info
typedef struct {
  SIZE_T CommitSize;
} MEMORY_SHARED_COMMIT_INFORMATION;

#define MEM_COMMIT             0x00001000
#define MEM_RESERVE            0x00002000
#define MEM_RELEASE            0x00008000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000

#define PAGE_NOACCESS   0x01
#define PAGE_READONLY   0x02
#define PAGE_READWRITE  0x04
#define PAGE_WRITECOPY  0x08

#define SEC_COMMIT      0x08000000
#define SECTION_ALL_ACCESS 0x000F001F

#define MemoryWorkingSetExInformation   4
#define MemorySharedCommitInformation   5
#define MemoryPhysicalContiguityInformation 11
#define MemoryBadInformation            12
#define MemoryBasicInformationCapped    10

#define PAGE_SIZE 4096ULL
#define ALLOC_GRAN 65536ULL

static HANDLE self(void) { return (HANDLE)(long long)(-1); }

extern NTSTATUS NTAPI NtAllocateVirtualMemoryEx(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG);
extern NTSTATUS NTAPI NtQueryVirtualMemory(
    HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);
extern NTSTATUS NTAPI NtCreateSectionEx(
    HANDLE *, ULONG, PVOID, LARGE_INTEGER *, ULONG, ULONG, HANDLE, PVOID, ULONG);
extern NTSTATUS NTAPI NtMapViewOfSectionEx(
    HANDLE, HANDLE, PVOID *, LARGE_INTEGER *, SIZE_T *, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtUnmapViewOfSectionEx(HANDLE, PVOID, ULONG);
extern NTSTATUS NTAPI NtClose(HANDLE);
extern NTSTATUS NTAPI NtProtectVirtualMemory(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG *);

static void dump_wsex(const char *label, void *base, int num_pages) {
  MEMORY_WORKING_SET_EX_INFORMATION info[32];
  int n = num_pages < 32 ? num_pages : 32;
  for (int i = 0; i < n; i++) {
    info[i].VirtualAddress = (char *)base + i * PAGE_SIZE;
    info[i].VirtualAttributes.Flags = 0;
  }
  NTSTATUS st = NtQueryVirtualMemory(self(), NULL, MemoryWorkingSetExInformation,
                                      info, n * sizeof(info[0]), NULL);
  if (!NT_SUCCESS(st)) {
    printf("  [%s] WSEX query failed: 0x%08lX\n", label, (unsigned long)st);
    return;
  }
  printf("  [%s]\n", label);
  for (int i = 0; i < n; i++) {
    MEMORY_WORKING_SET_EX_BLOCK *b = &info[i].VirtualAttributes;
    if (b->Valid) {
      printf("    page[%2d]: Valid=1 ShareCnt=%llu Shared=%llu "
             "SharedOrig=%llu Prio=%llu Locked=%llu Win32Prot=0x%03llx\n",
             i,
             (unsigned long long)b->ShareCount,
             (unsigned long long)b->Shared,
             (unsigned long long)b->SharedOriginal,
             (unsigned long long)b->Priority,
             (unsigned long long)b->Locked,
             (unsigned long long)b->Win32Protection);
    } else {
      printf("    page[%2d]: Valid=0 Shared=%llu ModifiedList=%llu "
             "Location=%llu SharedOrig=%llu\n",
             i,
             (unsigned long long)b->Invalid.Shared_,
             (unsigned long long)b->Invalid.ModifiedList,
             (unsigned long long)b->Invalid.Location,
             (unsigned long long)b->Invalid.SharedOriginal_);
    }
  }
}

int main(void) {
  printf("=== WSEX COW Detection Probe ===\n\n");

  //=========================================================================
  // TEST A: WRITECOPY section — the actual MAP_PRIVATE file scenario
  //=========================================================================
  printf("--- TEST A: WRITECOPY pagefile section (MAP_PRIVATE analog) ---\n");

  LARGE_INTEGER max_size;
  max_size.QuadPart = 8 * PAGE_SIZE;
  HANDLE section = NULL;
  NTSTATUS st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                                   &max_size, PAGE_READWRITE, SEC_COMMIT,
                                   NULL, NULL, 0);
  if (!NT_SUCCESS(st)) { printf("section failed\n"); return 1; }

  // Map as WRITECOPY (simulates MAP_PRIVATE)
  PVOID ph = NULL;
  SIZE_T ph_size = 8 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                             PAGE_NOACCESS, NULL, 0);
  PVOID view = ph;
  SIZE_T view_size = ph_size;
  LARGE_INTEGER offset = {0};
  st = NtMapViewOfSectionEx(section, self(), &view, &offset, &view_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);
  if (!NT_SUCCESS(st)) {
    printf("map WRITECOPY failed: 0x%08lX\n", (unsigned long)st);
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    NtClose(section);
    return 1;
  }
  printf("  Mapped 8-page WRITECOPY view at %p\n\n", view);

  // Step 1: Read all pages (fault them in, but don't write)
  volatile char *p = (volatile char *)view;
  for (int i = 0; i < 8; i++) {
    volatile char dummy = p[i * PAGE_SIZE]; // read-only touch
    (void)dummy;
  }
  dump_wsex("After read-only touch (all pages clean)", view, 8);

  // Step 2: Write to pages 1, 4, 6 (COW them)
  printf("\n  Writing to pages 1, 4, 6 (triggering COW)...\n\n");
  p[1 * PAGE_SIZE] = 'A';
  p[4 * PAGE_SIZE] = 'B';
  p[6 * PAGE_SIZE] = 'C';
  dump_wsex("After writing pages 1,4,6 (should show COW)", view, 8);

  // Step 3: Analyze the results
  printf("\n  Expected pattern:\n"
         "    Clean pages (0,2,3,5,7): SharedOriginal=1, Shared=1\n"
         "    COW'd pages (1,4,6):     SharedOriginal=0, Shared=0\n\n");

  //=========================================================================
  // TEST B: READWRITE section — MAP_SHARED analog (no COW)
  //=========================================================================
  printf("--- TEST B: READWRITE section (MAP_SHARED analog, no COW) ---\n");

  HANDLE section2 = NULL;
  max_size.QuadPart = 4 * PAGE_SIZE;
  st = NtCreateSectionEx(&section2, SECTION_ALL_ACCESS, NULL,
                          &max_size, PAGE_READWRITE, SEC_COMMIT,
                          NULL, NULL, 0);
  if (!NT_SUCCESS(st)) goto skip_b;

  PVOID ph2 = NULL;
  SIZE_T ph2_size = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph2, &ph2_size,
                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                             PAGE_NOACCESS, NULL, 0);
  PVOID view2 = ph2;
  SIZE_T view2_size = ph2_size;
  offset.QuadPart = 0;
  st = NtMapViewOfSectionEx(section2, self(), &view2, &offset, &view2_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  if (!NT_SUCCESS(st)) {
    PVOID f = ph2; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    goto skip_b;
  }

  // Touch all pages as read+write
  volatile char *q = (volatile char *)view2;
  for (int i = 0; i < 4; i++) q[i * PAGE_SIZE] = (char)('X' + i);
  dump_wsex("READWRITE shared view (all written, no COW)", view2, 4);

  NtUnmapViewOfSectionEx(self(), view2, 0);
skip_b:
  if (section2) NtClose(section2);

  //=========================================================================
  // TEST C: Private committed memory (no section, no COW concept)
  //=========================================================================
  printf("\n--- TEST C: Private committed memory (MEM_PRIVATE) ---\n");
  PVOID priv = NULL;
  SIZE_T priv_size = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &priv, &priv_size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  if (priv) {
    volatile char *r = (volatile char *)priv;
    r[0] = 'P';
    // page 1 never touched
    r[2 * PAGE_SIZE] = 'Q';
    dump_wsex("Private: page 0,2 written, page 1,3 untouched", priv, 4);
    PVOID f = priv; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }

  //=========================================================================
  // TEST D: MemorySharedCommitInformation — can it distinguish COW charge?
  //=========================================================================
  printf("\n--- TEST D: MemorySharedCommitInformation on WRITECOPY view ---\n");
  MEMORY_SHARED_COMMIT_INFORMATION sci = {0};
  st = NtQueryVirtualMemory(self(), view, MemorySharedCommitInformation,
                             &sci, sizeof(sci), NULL);
  printf("  Status: 0x%08lX, CommitSize: %llu\n",
         (unsigned long)st, (unsigned long long)sci.CommitSize);
  if (NT_SUCCESS(st)) {
    printf("  (3 COW'd pages = %llu bytes private commit expected)\n",
           (unsigned long long)(3 * PAGE_SIZE));
    if (sci.CommitSize > 0)
      printf("  Shared commit remaining: %llu bytes (clean pages still shared)\n",
             (unsigned long long)sci.CommitSize);
  }

  //=========================================================================
  // TEST E: Probe other query classes for useful state
  //=========================================================================
  printf("\n--- TEST E: Other query classes ---\n");

  // MemoryPhysicalContiguityInformation (class 11)
  char contiguity_buf[128] = {0};
  st = NtQueryVirtualMemory(self(), view, MemoryPhysicalContiguityInformation,
                             contiguity_buf, sizeof(contiguity_buf), NULL);
  printf("  MemoryPhysicalContiguityInformation: 0x%08lX\n", (unsigned long)st);

  // MemoryBadInformation (class 12)
  char bad_buf[64] = {0};
  st = NtQueryVirtualMemory(self(), view, MemoryBadInformation,
                             bad_buf, sizeof(bad_buf), NULL);
  printf("  MemoryBadInformation: 0x%08lX\n", (unsigned long)st);

  // MemoryBasicInformationCapped (class 10)
  char capped_buf[64] = {0};
  st = NtQueryVirtualMemory(self(), view, MemoryBasicInformationCapped,
                             capped_buf, sizeof(capped_buf), NULL);
  printf("  MemoryBasicInformationCapped: 0x%08lX\n", (unsigned long)st);

  //=========================================================================
  // TEST F: Can WSEX detect COW on pages trimmed from working set?
  //=========================================================================
  printf("\n--- TEST F: WSEX after mprotect(NOACCESS) + mprotect(WRITECOPY) ---\n");
  printf("  (Simulating pages evicted from working set)\n");

  // Force page 1 (COW'd) and page 2 (clean) out of working set via
  // PAGE_NOACCESS then back. This simulates working-set trimming.
  PVOID prot_addr = (char *)view + 1 * PAGE_SIZE;
  SIZE_T prot_size = 2 * PAGE_SIZE; // pages 1 and 2
  ULONG old_prot;
  st = NtProtectVirtualMemory(self(), &prot_addr, &prot_size,
                               PAGE_NOACCESS, &old_prot);
  printf("  mprotect(NOACCESS) pages 1-2: 0x%08lX old=0x%lx\n",
         (unsigned long)st, (unsigned long)old_prot);

  // Back to WRITECOPY
  prot_addr = (char *)view + 1 * PAGE_SIZE;
  prot_size = 2 * PAGE_SIZE;
  st = NtProtectVirtualMemory(self(), &prot_addr, &prot_size,
                               PAGE_WRITECOPY, &old_prot);
  printf("  mprotect(WRITECOPY) pages 1-2: 0x%08lX\n", (unsigned long)st);

  dump_wsex("After NOACCESS→WRITECOPY roundtrip", view, 8);
  printf("  (Page 1 was COW'd — does WSEX still know?)\n");

  // Cleanup
  NtUnmapViewOfSectionEx(self(), view, 0);
  NtClose(section);

  printf("\n=== Probe complete ===\n");
  return 0;
}
