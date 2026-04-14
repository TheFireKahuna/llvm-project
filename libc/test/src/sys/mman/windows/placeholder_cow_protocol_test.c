// placeholder_cow_protocol_test.c — Validate the placeholder-preserving
// dirty-page protocol for split-remap.
//
// Tests the exact sequence that would replace the current save/restore-through-COW
// approach. Instead of remapping ALL pages from the section (including dirty ones
// that show wrong content), we:
//   1. Remap only CLEAN ranges from the section
//   2. Leave dirty page placeholders as NOACCESS (hardware-enforced safety)
//   3. replace_placeholder_commit dirty clusters → private committed
//   4. memcpy saved content into private pages (no COW fault, 2 copies not 3)
//
// Validates:
//   - Fine-grained placeholder splitting (per dirty cluster)
//   - Section remap of non-contiguous clean ranges
//   - replace_placeholder_commit for dirty clusters
//   - Content correctness after the full cycle
//   - Placeholder NOACCESS state between steps (defense-in-depth)

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
} MEMORY_WORKING_SET_EX_BLOCK;

typedef struct {
  PVOID VirtualAddress;
  MEMORY_WORKING_SET_EX_BLOCK VirtualAttributes;
} MEMORY_WORKING_SET_EX_INFORMATION;

#define MEM_COMMIT              0x00001000
#define MEM_RESERVE             0x00002000
#define MEM_FREE                0x00010000
#define MEM_RELEASE             0x00008000
#define MEM_PRIVATE             0x00020000
#define MEM_MAPPED              0x00040000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_PRESERVE_PLACEHOLDER_ON_UNMAP 0x00000002
#define MEM_UNMAP_WITH_TRANSIENT_BOOST    0x00000001
#define MEM_COALESCE_PLACEHOLDERS         0x00000001

#define PAGE_NOACCESS   0x01
#define PAGE_READONLY   0x02
#define PAGE_READWRITE  0x04
#define PAGE_WRITECOPY  0x08

#define SEC_COMMIT      0x08000000
#define SECTION_ALL_ACCESS 0x000F001F

#define MemoryBasicInformation        0
#define MemoryWorkingSetExInformation 4

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

// WSEX per-page COW detection (proven in wsex_cow_probe)
static int is_page_dirty(void *addr) {
  MEMORY_WORKING_SET_EX_INFORMATION info;
  info.VirtualAddress = addr;
  info.VirtualAttributes.Flags = 0;
  NTSTATUS st = NtQueryVirtualMemory(self(), NULL, MemoryWorkingSetExInformation,
                                      &info, sizeof(info), NULL);
  if (!NT_SUCCESS(st)) return -1; // unknown
  if (info.VirtualAttributes.Valid)
    return info.VirtualAttributes.SharedOriginal == 0 ? 1 : 0;
  // Invalid page: check ModifiedList
  return info.VirtualAttributes.Flags == 0 ? 0 : -1; // can't tell for sure
}

static int split_placeholder(void *addr, SIZE_T split_offset) {
  PVOID base = addr;
  SIZE_T size = split_offset;
  return NT_SUCCESS(NtFreeVirtualMemory(self(), &base, &size,
                                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
}

static int release_placeholder(void *addr) {
  PVOID base = addr;
  SIZE_T size = 0;
  return NT_SUCCESS(NtFreeVirtualMemory(self(), &base, &size, MEM_RELEASE));
}

static NTSTATUS replace_placeholder_commit(void *addr, SIZE_T size, DWORD prot) {
  PVOID base = addr;
  SIZE_T sz = size;
  return NtAllocateVirtualMemoryEx(self(), &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, prot, NULL, 0);
}

//=============================================================================
// THE TEST
//
// Layout: 16-page WRITECOPY section view
//
//   Page: 0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15
//         C  C  D  C  C  C  D  D  D  C  C  C  C  D  C  C
//
//   C = clean (SharedOriginal=1, content from section)
//   D = dirty (SharedOriginal=0, COW'd with user content)
//
// Dirty clusters: [2], [6-8], [13]
// Clean ranges:   [0-1], [3-5], [9-12], [14-15]
//
// Simulate partial munmap of pages 4-11 (the middle). Keep left [0-3] and
// right [12-15]. This exercises:
//   - Clean-only left fragment [0-3]: pages 0,1,3 clean, page 2 dirty
//   - Clean-only right fragment [12-15]: page 13 dirty, pages 12,14,15 clean
//
// Protocol:
//   1. Save dirty pages (2, 13) to temp buffer
//   2. Unmap entire view → placeholder
//   3. Split into [0-3] [4-11] [12-15]
//   4. For [0-3]: sub-split around page 2 → [0-1] [2] [3]
//      - Remap [0-1] and [3] from section (WRITECOPY)
//      - replace_placeholder_commit [2] → private READWRITE
//      - memcpy saved content for page 2
//   5. For [12-15]: sub-split around page 13 → [12-12] [13] [14-15]
//      - Remap [12-12] and [14-15] from section (WRITECOPY)
//      - replace_placeholder_commit [13] → private READWRITE
//      - memcpy saved content for page 13
//   6. Release [4-11]
//   7. Verify all content
//=============================================================================

int main(void) {
  printf("=== Placeholder-Preserving COW Protocol Test ===\n\n");

  const int NUM_PAGES = 16;
  const SIZE_T view_bytes = NUM_PAGES * PAGE_SIZE;

  // Create pagefile section (simulates file-backed section for COW testing)
  LARGE_INTEGER max_size;
  max_size.QuadPart = (long long)view_bytes;
  HANDLE section = NULL;
  NTSTATUS st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                                   &max_size, PAGE_READWRITE, SEC_COMMIT,
                                   NULL, NULL, 0);
  CHECK(NT_SUCCESS(st), "Created 16-page section");
  if (!NT_SUCCESS(st)) return 1;

  // Write known content into the section via a READWRITE mapping first.
  // Each page gets a unique byte pattern: page N filled with (N + 0x10).
  {
    PVOID tmp_ph = NULL;
    SIZE_T tmp_size = view_bytes;
    NtAllocateVirtualMemoryEx(self(), &tmp_ph, &tmp_size,
                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                               PAGE_NOACCESS, NULL, 0);
    PVOID tmp_view = tmp_ph;
    SIZE_T tmp_vs = tmp_size;
    LARGE_INTEGER zero_off = {0};
    NtMapViewOfSectionEx(section, self(), &tmp_view, &zero_off, &tmp_vs,
                          MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    for (int i = 0; i < NUM_PAGES; i++)
      memset((char *)tmp_view + i * PAGE_SIZE, i + 0x10, (size_t)PAGE_SIZE);
    NtUnmapViewOfSectionEx(self(), tmp_view, 0);
    printf("  Section initialized with per-page patterns\n");
  }

  // Map as WRITECOPY (simulates MAP_PRIVATE)
  PVOID ph = NULL;
  SIZE_T ph_size = view_bytes;
  st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                  PAGE_NOACCESS, NULL, 0);
  CHECK(NT_SUCCESS(st), "Created placeholder for WRITECOPY view");

  PVOID view = ph;
  SIZE_T vs = ph_size;
  LARGE_INTEGER offset = {0};
  st = NtMapViewOfSectionEx(section, self(), &view, &offset, &vs,
                             MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);
  CHECK(NT_SUCCESS(st), "Mapped WRITECOPY view at %p", view);
  if (!NT_SUCCESS(st)) return 1;

  char *base = (char *)view;

  // Fault in all pages (read-only touch)
  for (int i = 0; i < NUM_PAGES; i++) {
    volatile char dummy = base[i * PAGE_SIZE];
    (void)dummy;
  }

  // Dirty pages 2, 6, 7, 8, 13 with distinct patterns
  int dirty_pages[] = {2, 6, 7, 8, 13};
  int num_dirty = sizeof(dirty_pages) / sizeof(dirty_pages[0]);
  for (int i = 0; i < num_dirty; i++) {
    int pg = dirty_pages[i];
    memset(base + pg * PAGE_SIZE, 0xD0 + pg, (size_t)PAGE_SIZE);
  }
  printf("  Dirtied pages 2, 6, 7, 8, 13 with unique patterns\n");

  // Verify WSEX detects dirty pages correctly
  printf("\n--- WSEX COW scan ---\n");
  int detected_dirty[NUM_PAGES];
  for (int i = 0; i < NUM_PAGES; i++) {
    detected_dirty[i] = is_page_dirty(base + i * PAGE_SIZE);
    printf("  page[%2d]: %s (byte=0x%02x)\n", i,
           detected_dirty[i] == 1 ? "DIRTY" :
           detected_dirty[i] == 0 ? "clean" : "unknown",
           (unsigned char)base[i * PAGE_SIZE]);
  }

  // Verify detection matches expectations
  int expected_dirty[] = {0,0,1,0,0,0,1,1,1,0,0,0,0,1,0,0};
  int wsex_correct = 1;
  for (int i = 0; i < NUM_PAGES; i++) {
    if (detected_dirty[i] != expected_dirty[i]) {
      wsex_correct = 0;
      printf("  MISMATCH page %d: expected %d got %d\n",
             i, expected_dirty[i], detected_dirty[i]);
    }
  }
  CHECK(wsex_correct, "WSEX correctly identified all 5 dirty pages");

  //=========================================================================
  // SAVE dirty page content (pages 2, 13 — the ones in kept fragments)
  // Pages 6-8 are in the freed middle, no need to save.
  //=========================================================================
  printf("\n--- Save dirty pages in kept fragments ---\n");

  // Left kept: [0-3], dirty page 2
  char save_page2[PAGE_SIZE];
  memcpy(save_page2, base + 2 * PAGE_SIZE, PAGE_SIZE);

  // Right kept: [12-15], dirty page 13
  char save_page13[PAGE_SIZE];
  memcpy(save_page13, base + 13 * PAGE_SIZE, PAGE_SIZE);

  printf("  Saved pages 2 and 13 (%d bytes total)\n", (int)(2 * PAGE_SIZE));

  //=========================================================================
  // UNMAP entire view → single placeholder
  //=========================================================================
  printf("\n--- Unmap with TRANSIENT_BOOST ---\n");
  st = NtUnmapViewOfSectionEx(self(), view,
                                MEM_UNMAP_WITH_TRANSIENT_BOOST |
                                MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
  CHECK(NT_SUCCESS(st), "Unmap with transient boost succeeded");

  // Verify it's a placeholder
  MEMORY_BASIC_INFORMATION mbi;
  query_mbi(base, &mbi);
  CHECK(mbi.State == MEM_RESERVE, "Region is reserved (placeholder)");

  //=========================================================================
  // SPLIT into [0-3] [4-11] [12-15]
  //=========================================================================
  printf("\n--- Primary splits ---\n");
  // Split at page 4 (offset 4*PAGE_SIZE from base)
  CHECK(split_placeholder(base, 4 * PAGE_SIZE),
        "Split at page 4 boundary");
  // Split at page 12 (offset 8*PAGE_SIZE from base+4*PAGE_SIZE)
  CHECK(split_placeholder(base + 4 * PAGE_SIZE, 8 * PAGE_SIZE),
        "Split at page 12 boundary");

  //=========================================================================
  // LEFT FRAGMENT [0-3]: sub-split around dirty page 2
  // [0-1] clean, [2] dirty, [3] clean
  //=========================================================================
  printf("\n--- Left fragment sub-splits ---\n");
  // Split [0-3] at page 2
  CHECK(split_placeholder(base, 2 * PAGE_SIZE),
        "Sub-split left at page 2");
  // Split [2-3] at page 3
  CHECK(split_placeholder(base + 2 * PAGE_SIZE, PAGE_SIZE),
        "Sub-split left at page 3");

  // Now we have: [0-1] [2] [3] [4-11] [12-15]

  // Remap clean ranges from section
  printf("\n--- Remap clean ranges (left) ---\n");

  // [0-1] from section offset 0
  PVOID r01 = base;
  SIZE_T r01_size = 2 * PAGE_SIZE;
  LARGE_INTEGER off01 = {0};
  st = NtMapViewOfSectionEx(section, self(), &r01, &off01, &r01_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);
  CHECK(NT_SUCCESS(st), "Remap pages [0-1] from section");

  // [3] from section offset 3*PAGE_SIZE
  PVOID r3 = base + 3 * PAGE_SIZE;
  SIZE_T r3_size = PAGE_SIZE;
  LARGE_INTEGER off3;
  off3.QuadPart = 3 * PAGE_SIZE;
  st = NtMapViewOfSectionEx(section, self(), &r3, &off3, &r3_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);
  CHECK(NT_SUCCESS(st), "Remap page [3] from section");

  // Page 2 is still a placeholder — NOACCESS, hardware-enforced safety
  query_mbi(base + 2 * PAGE_SIZE, &mbi);
  CHECK(mbi.State == MEM_RESERVE, "Page 2 still placeholder (NOACCESS) — safe!");

  // Commit page 2 as private + write saved content
  printf("\n--- Commit dirty page 2 ---\n");
  st = replace_placeholder_commit(base + 2 * PAGE_SIZE, PAGE_SIZE, PAGE_READWRITE);
  CHECK(NT_SUCCESS(st), "replace_placeholder_commit page 2");

  memcpy(base + 2 * PAGE_SIZE, save_page2, PAGE_SIZE);
  CHECK((unsigned char)base[2 * PAGE_SIZE] == 0xD2,
        "Page 2 content restored correctly (0xD2)");

  //=========================================================================
  // RIGHT FRAGMENT [12-15]: sub-split around dirty page 13
  // [12] clean, [13] dirty, [14-15] clean
  //=========================================================================
  printf("\n--- Right fragment sub-splits ---\n");
  // Split [12-15] at page 13
  CHECK(split_placeholder(base + 12 * PAGE_SIZE, PAGE_SIZE),
        "Sub-split right at page 13");
  // Split [13-15] at page 14
  CHECK(split_placeholder(base + 13 * PAGE_SIZE, PAGE_SIZE),
        "Sub-split right at page 14");

  // Now: [0-1] [2] [3] [4-11] [12] [13] [14-15]

  // Remap clean ranges from section
  printf("\n--- Remap clean ranges (right) ---\n");

  // [12] from section offset 12*PAGE_SIZE
  PVOID r12 = base + 12 * PAGE_SIZE;
  SIZE_T r12_size = PAGE_SIZE;
  LARGE_INTEGER off12;
  off12.QuadPart = 12 * PAGE_SIZE;
  st = NtMapViewOfSectionEx(section, self(), &r12, &off12, &r12_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);
  CHECK(NT_SUCCESS(st), "Remap page [12] from section");

  // [14-15] from section offset 14*PAGE_SIZE
  PVOID r1415 = base + 14 * PAGE_SIZE;
  SIZE_T r1415_size = 2 * PAGE_SIZE;
  LARGE_INTEGER off14;
  off14.QuadPart = 14 * PAGE_SIZE;
  st = NtMapViewOfSectionEx(section, self(), &r1415, &off14, &r1415_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);
  CHECK(NT_SUCCESS(st), "Remap pages [14-15] from section");

  // Page 13 still placeholder
  query_mbi(base + 13 * PAGE_SIZE, &mbi);
  CHECK(mbi.State == MEM_RESERVE, "Page 13 still placeholder (NOACCESS) — safe!");

  // Commit page 13 as private + write saved content
  printf("\n--- Commit dirty page 13 ---\n");
  st = replace_placeholder_commit(base + 13 * PAGE_SIZE, PAGE_SIZE, PAGE_READWRITE);
  CHECK(NT_SUCCESS(st), "replace_placeholder_commit page 13");

  memcpy(base + 13 * PAGE_SIZE, save_page13, PAGE_SIZE);
  CHECK((unsigned char)base[13 * PAGE_SIZE] == 0xDD,
        "Page 13 content restored correctly (0xDD)");

  //=========================================================================
  // Release freed middle [4-11]
  //=========================================================================
  printf("\n--- Release freed middle ---\n");
  CHECK(release_placeholder(base + 4 * PAGE_SIZE),
        "Released freed middle [4-11]");

  //=========================================================================
  // VERIFY ALL CONTENT
  //=========================================================================
  printf("\n--- Final content verification ---\n");

  // Left fragment [0-3]
  int content_ok = 1;
  // Clean pages from section: byte = page + 0x10
  for (int pg = 0; pg <= 3; pg++) {
    if (pg == 2) continue; // dirty, checked separately
    unsigned char expected = (unsigned char)(pg + 0x10);
    unsigned char actual = (unsigned char)base[pg * PAGE_SIZE];
    if (actual != expected) {
      printf("  MISMATCH page %d: expected 0x%02x got 0x%02x\n",
             pg, expected, actual);
      content_ok = 0;
    }
  }
  // Dirty page 2
  if ((unsigned char)base[2 * PAGE_SIZE] != 0xD2) {
    printf("  MISMATCH page 2: expected 0xD2 got 0x%02x\n",
           (unsigned char)base[2 * PAGE_SIZE]);
    content_ok = 0;
  }
  CHECK(content_ok, "Left fragment [0-3] content correct");

  // Right fragment [12-15]
  content_ok = 1;
  for (int pg = 12; pg <= 15; pg++) {
    if (pg == 13) continue;
    unsigned char expected = (unsigned char)(pg + 0x10);
    unsigned char actual = (unsigned char)base[pg * PAGE_SIZE];
    if (actual != expected) {
      printf("  MISMATCH page %d: expected 0x%02x got 0x%02x\n",
             pg, expected, actual);
      content_ok = 0;
    }
  }
  if ((unsigned char)base[13 * PAGE_SIZE] != 0xDD) {
    printf("  MISMATCH page 13: expected 0xDD got 0x%02x\n",
           (unsigned char)base[13 * PAGE_SIZE]);
    content_ok = 0;
  }
  CHECK(content_ok, "Right fragment [12-15] content correct");

  // Verify full page content (not just first byte)
  content_ok = 1;
  for (int pg = 0; pg <= 3; pg++) {
    unsigned char expected = (pg == 2) ? 0xD2 : (unsigned char)(pg + 0x10);
    char *page = base + pg * PAGE_SIZE;
    for (int j = 0; j < (int)PAGE_SIZE; j++) {
      if ((unsigned char)page[j] != expected) {
        printf("  BYTE MISMATCH page %d offset %d: expected 0x%02x got 0x%02x\n",
               pg, j, expected, (unsigned char)page[j]);
        content_ok = 0;
        break;
      }
    }
  }
  for (int pg = 12; pg <= 15; pg++) {
    unsigned char expected = (pg == 13) ? 0xDD : (unsigned char)(pg + 0x10);
    char *page = base + pg * PAGE_SIZE;
    for (int j = 0; j < (int)PAGE_SIZE; j++) {
      if ((unsigned char)page[j] != expected) {
        printf("  BYTE MISMATCH page %d offset %d: expected 0x%02x got 0x%02x\n",
               pg, j, expected, (unsigned char)page[j]);
        content_ok = 0;
        break;
      }
    }
  }
  CHECK(content_ok, "Full page content verification (all bytes)");

  // Verify MBI types
  query_mbi(base + 2 * PAGE_SIZE, &mbi);
  CHECK(mbi.Type == MEM_PRIVATE, "Page 2 is MEM_PRIVATE (committed, no section)");

  query_mbi(base, &mbi);
  CHECK(mbi.Type == MEM_MAPPED, "Page 0 is MEM_MAPPED (section view)");

  query_mbi(base + 13 * PAGE_SIZE, &mbi);
  CHECK(mbi.Type == MEM_PRIVATE, "Page 13 is MEM_PRIVATE");

  // Verify freed middle is actually free
  query_mbi(base + 4 * PAGE_SIZE, &mbi);
  CHECK(mbi.State == MEM_FREE, "Middle [4-11] is MEM_FREE");

  //=========================================================================
  // Cleanup
  //=========================================================================
  printf("\n--- Cleanup ---\n");
  // Unmap section views: [0-1], [3], [12], [14-15]
  NtUnmapViewOfSectionEx(self(), base, 0);               // [0-1]
  NtUnmapViewOfSectionEx(self(), base + 3 * PAGE_SIZE, 0); // [3]
  NtUnmapViewOfSectionEx(self(), base + 12 * PAGE_SIZE, 0); // [12]
  NtUnmapViewOfSectionEx(self(), base + 14 * PAGE_SIZE, 0); // [14-15]

  // Free private committed: [2], [13]
  PVOID f2 = base + 2 * PAGE_SIZE; SIZE_T fs2 = 0;
  NtFreeVirtualMemory(self(), &f2, &fs2, MEM_RELEASE);
  PVOID f13 = base + 13 * PAGE_SIZE; SIZE_T fs13 = 0;
  NtFreeVirtualMemory(self(), &f13, &fs13, MEM_RELEASE);

  NtClose(section);

  printf("\n========================================\n");
  printf("RESULTS: %d passed, %d failed\n", pass_count, fail_count);
  printf("========================================\n");
  return fail_count > 0 ? 1 : 0;
}
