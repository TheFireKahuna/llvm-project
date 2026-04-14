// dirty_state_probe.c — Brute-force probe VmPageDirtyStateInformation flags
//
// NtSetInformationVirtualMemory(VmPageDirtyStateInformation) is undocumented.
// We know:
//   - It's a SET operation (not query)
//   - Used by Hyper-V for dirty page tracking
//   - MEMORY_PAGE_DIRTY_STATE_INFORMATION has a single ULONG Flags field
//   - Flag values are unknown
//
// Strategy: try every flag bit (0x1 through 0x80000000) on different memory
// types (private committed, section view, write-watch) and observe which
// flags the kernel accepts vs rejects, and what observable effect they have.

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
typedef unsigned short USHORT;

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
  PVOID VirtualAddress;
  SIZE_T NumberOfBytes;
} MEMORY_RANGE_ENTRY;

typedef struct {
  ULONG Flags;
} MEMORY_PAGE_DIRTY_STATE_INFORMATION;

// Working set ex info for observing dirty/modified state
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

#define MEM_COMMIT             0x00001000
#define MEM_RESERVE            0x00002000
#define MEM_RELEASE            0x00008000
#define MEM_WRITE_WATCH        0x00200000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000

#define PAGE_NOACCESS   0x01
#define PAGE_READWRITE  0x04
#define PAGE_WRITECOPY  0x08

#define SEC_COMMIT      0x08000000
#define SECTION_ALL_ACCESS 0x000F001F

#define MemoryBasicInformation          0
#define MemoryWorkingSetExInformation   4
#define VmPageDirtyStateInformation     3

#define WRITE_WATCH_FLAG_RESET 0x01

static HANDLE self(void) { return (HANDLE)(long long)(-1); }

extern NTSTATUS NTAPI NtAllocateVirtualMemoryEx(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG);
extern NTSTATUS NTAPI NtQueryVirtualMemory(
    HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);
extern NTSTATUS NTAPI NtSetInformationVirtualMemory(
    HANDLE, ULONG, SIZE_T, MEMORY_RANGE_ENTRY *, PVOID, ULONG);
extern NTSTATUS NTAPI NtCreateSectionEx(
    HANDLE *, ULONG, PVOID, LARGE_INTEGER *, ULONG, ULONG, HANDLE, PVOID, ULONG);
extern NTSTATUS NTAPI NtMapViewOfSectionEx(
    HANDLE, HANDLE, PVOID *, LARGE_INTEGER *, SIZE_T *, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtUnmapViewOfSectionEx(HANDLE, PVOID, ULONG);
extern NTSTATUS NTAPI NtClose(HANDLE);
extern NTSTATUS NTAPI NtGetWriteWatch(
    HANDLE, ULONG, PVOID, SIZE_T, PVOID *, ULONG_PTR *, ULONG *);

#define PAGE_SIZE 4096ULL
#define ALLOC_GRAN 65536ULL

static void query_wsex(void *addr, int num_pages) {
  MEMORY_WORKING_SET_EX_INFORMATION info[16];
  int n = num_pages < 16 ? num_pages : 16;
  for (int i = 0; i < n; i++) {
    info[i].VirtualAddress = (char *)addr + i * PAGE_SIZE;
    info[i].VirtualAttributes.Flags = 0;
  }
  NTSTATUS st = NtQueryVirtualMemory(self(), NULL, MemoryWorkingSetExInformation,
                                      info, n * sizeof(info[0]), NULL);
  if (!NT_SUCCESS(st)) {
    printf("    WSEX query failed: 0x%08lX\n", (unsigned long)st);
    return;
  }
  for (int i = 0; i < n; i++) {
    MEMORY_WORKING_SET_EX_BLOCK *b = &info[i].VirtualAttributes;
    if (b->Valid)
      printf("    page[%d]: Valid=1 Shared=%llu SharedOrig=%llu Prio=%llu\n",
             i, (unsigned long long)b->Shared,
             (unsigned long long)b->SharedOriginal,
             (unsigned long long)b->Priority);
    else
      printf("    page[%d]: Valid=0 (not in working set)\n", i);
  }
}

int main(void) {
  printf("=== VmPageDirtyStateInformation Flag Probe ===\n\n");

  // Allocate 4 pages of private committed memory
  PVOID priv = NULL;
  SIZE_T priv_size = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &priv, &priv_size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  if (!priv) { printf("alloc failed\n"); return 1; }

  // Dirty pages 0 and 1
  volatile char *p = (volatile char *)priv;
  p[0] = 'A';
  p[PAGE_SIZE] = 'B';
  // Pages 2 and 3 are clean (never touched after commit)

  printf("--- Private committed memory at %p ---\n", priv);
  printf("Pages 0,1 dirty; pages 2,3 clean\n\n");

  // Probe flags 0x0 through 0xF, then each power of 2 up to bit 31
  ULONG test_flags[] = {
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
    0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF,
    0x10, 0x20, 0x40, 0x80,
    0x100, 0x200, 0x400, 0x800,
    0x1000, 0x2000, 0x4000, 0x8000,
    0x10000, 0x20000, 0x40000, 0x80000,
    0x100000, 0x200000, 0x400000, 0x800000,
    0x1000000, 0x2000000, 0x4000000, 0x8000000,
    0x10000000, 0x20000000, 0x40000000, 0x80000000,
  };
  int num_flags = sizeof(test_flags) / sizeof(test_flags[0]);

  printf("Probing %d flag values on private memory...\n", num_flags);

  int found_success = 0;
  for (int i = 0; i < num_flags; i++) {
    MEMORY_RANGE_ENTRY range;
    range.VirtualAddress = priv;
    range.NumberOfBytes = priv_size;
    MEMORY_PAGE_DIRTY_STATE_INFORMATION info;
    info.Flags = test_flags[i];

    NTSTATUS st = NtSetInformationVirtualMemory(
        self(), VmPageDirtyStateInformation, 1, &range, &info, sizeof(info));

    if (NT_SUCCESS(st)) {
      printf("  flags=0x%08lX -> SUCCESS (out_flags=0x%08lX)\n",
             (unsigned long)test_flags[i], (unsigned long)info.Flags);
      found_success = 1;
    } else if (st != (NTSTATUS)0xC00000BB /*STATUS_NOT_SUPPORTED*/ &&
               st != (NTSTATUS)0xC000000D /*STATUS_INVALID_PARAMETER*/) {
      // Interesting non-standard error
      printf("  flags=0x%08lX -> 0x%08lX (interesting!)\n",
             (unsigned long)test_flags[i], (unsigned long)st);
    }
    // Silently skip NOT_SUPPORTED and INVALID_PARAMETER
  }

  if (!found_success) {
    printf("\n  No flag value succeeded on private memory.\n");
    printf("  All returned STATUS_NOT_SUPPORTED or STATUS_INVALID_PARAMETER.\n");
    printf("  This API may require:\n"
           "    - MEM_WRITE_WATCH memory\n"
           "    - Hyper-V partition context\n"
           "    - A specific process flag\n"
           "    - Newer Windows build\n\n");
  }

  // Try on MEM_WRITE_WATCH memory — dirty tracking is explicitly enabled
  printf("--- Trying on MEM_WRITE_WATCH memory ---\n");
  PVOID ww = NULL;
  SIZE_T ww_size = 4 * PAGE_SIZE;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ww, &ww_size,
                             MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                             PAGE_READWRITE, NULL, 0);
  if (NT_SUCCESS(st)) {
    ((volatile char *)ww)[0] = 'X';

    for (int i = 0; i < num_flags; i++) {
      MEMORY_RANGE_ENTRY range;
      range.VirtualAddress = ww;
      range.NumberOfBytes = ww_size;
      MEMORY_PAGE_DIRTY_STATE_INFORMATION info;
      info.Flags = test_flags[i];

      st = NtSetInformationVirtualMemory(
          self(), VmPageDirtyStateInformation, 1, &range, &info, sizeof(info));

      if (NT_SUCCESS(st)) {
        printf("  WW flags=0x%08lX -> SUCCESS (out_flags=0x%08lX)\n",
               (unsigned long)test_flags[i], (unsigned long)info.Flags);
      } else if (st != (NTSTATUS)0xC00000BB && st != (NTSTATUS)0xC000000D) {
        printf("  WW flags=0x%08lX -> 0x%08lX\n",
               (unsigned long)test_flags[i], (unsigned long)st);
      }
    }

    PVOID f = ww; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }

  // Try with different VmInformationLength — maybe the struct is bigger
  printf("\n--- Probing struct sizes (flags=0x1 on private) ---\n");
  for (ULONG sz = 0; sz <= 64; sz += 4) {
    char buf[64] = {0};
    ((MEMORY_PAGE_DIRTY_STATE_INFORMATION *)buf)->Flags = 0x1;

    MEMORY_RANGE_ENTRY range;
    range.VirtualAddress = priv;
    range.NumberOfBytes = priv_size;

    st = NtSetInformationVirtualMemory(
        self(), VmPageDirtyStateInformation, 1, &range, buf, sz);

    if (NT_SUCCESS(st))
      printf("  size=%lu -> SUCCESS\n", (unsigned long)sz);
    else if (st != (NTSTATUS)0xC00000BB && st != (NTSTATUS)0xC000000D)
      printf("  size=%lu -> 0x%08lX\n", (unsigned long)sz, (unsigned long)st);
    // First non-NOT_SUPPORTED different from INVALID_PARAMETER tells us something
  }

  // Try with a section view — maybe it only works on mapped memory
  printf("\n--- Trying on pagefile section view ---\n");
  LARGE_INTEGER max_size;
  max_size.QuadPart = 4 * PAGE_SIZE;
  HANDLE section = NULL;
  st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                          &max_size, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
  if (NT_SUCCESS(st)) {
    PVOID ph = NULL;
    SIZE_T ph_size = 4 * PAGE_SIZE;
    NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                               PAGE_NOACCESS, NULL, 0);
    PVOID view = ph;
    SIZE_T view_size = ph_size;
    LARGE_INTEGER offset = {0};
    st = NtMapViewOfSectionEx(section, self(), &view, &offset, &view_size,
                               MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    if (NT_SUCCESS(st)) {
      ((volatile char *)view)[0] = 'Z';

      for (int i = 0; i < 8; i++) { // Just test first 8 flags
        MEMORY_RANGE_ENTRY range;
        range.VirtualAddress = view;
        range.NumberOfBytes = view_size;
        MEMORY_PAGE_DIRTY_STATE_INFORMATION info;
        info.Flags = test_flags[i];

        st = NtSetInformationVirtualMemory(
            self(), VmPageDirtyStateInformation, 1, &range, &info, sizeof(info));

        if (NT_SUCCESS(st))
          printf("  section flags=0x%08lX -> SUCCESS\n", (unsigned long)test_flags[i]);
        else if (st != (NTSTATUS)0xC00000BB && st != (NTSTATUS)0xC000000D)
          printf("  section flags=0x%08lX -> 0x%08lX\n",
                 (unsigned long)test_flags[i], (unsigned long)st);
      }

      NtUnmapViewOfSectionEx(self(), view, 0);
    } else {
      PVOID f = ph; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    }
    NtClose(section);
  }

  // Try with WRITECOPY section (actual COW scenario)
  printf("\n--- Trying on WRITECOPY section view (COW) ---\n");
  st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                          &max_size, PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
  if (NT_SUCCESS(st)) {
    PVOID ph = NULL;
    SIZE_T ph_size = 4 * PAGE_SIZE;
    NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                               PAGE_NOACCESS, NULL, 0);
    PVOID view = ph;
    SIZE_T view_size = ph_size;
    LARGE_INTEGER offset = {0};
    st = NtMapViewOfSectionEx(section, self(), &view, &offset, &view_size,
                               MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);
    if (NT_SUCCESS(st)) {
      // Touch page 0 to trigger COW
      ((volatile char *)view)[0] = 'W';

      printf("  WSEX before dirty-state call:\n");
      query_wsex(view, 4);

      for (int i = 0; i < 8; i++) {
        MEMORY_RANGE_ENTRY range;
        range.VirtualAddress = view;
        range.NumberOfBytes = view_size;
        MEMORY_PAGE_DIRTY_STATE_INFORMATION info;
        info.Flags = test_flags[i];

        st = NtSetInformationVirtualMemory(
            self(), VmPageDirtyStateInformation, 1, &range, &info, sizeof(info));

        if (NT_SUCCESS(st)) {
          printf("  COW flags=0x%08lX -> SUCCESS (out=0x%08lX)\n",
                 (unsigned long)test_flags[i], (unsigned long)info.Flags);
          // Check if WSEX changed
          printf("  WSEX after:\n");
          query_wsex(view, 4);
        } else if (st != (NTSTATUS)0xC00000BB && st != (NTSTATUS)0xC000000D) {
          printf("  COW flags=0x%08lX -> 0x%08lX\n",
                 (unsigned long)test_flags[i], (unsigned long)st);
        }
      }

      NtUnmapViewOfSectionEx(self(), view, 0);
    } else {
      PVOID f = ph; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    }
    NtClose(section);
  }

  // Cleanup
  PVOID f = priv; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);

  printf("\n=== Probe complete ===\n");
  return 0;
}
