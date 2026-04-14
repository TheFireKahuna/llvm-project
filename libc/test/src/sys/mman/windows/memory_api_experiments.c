// memory_api_experiments.c — Probe radical NT memory APIs for POSIX subsystem
//
// Tests 7 hypotheses for simplifying/optimizing the Windows mmap subsystem.
// Each test is independent and prints PASS/FAIL/SKIP with details.
//
// Build (from build-wi):
//   bin/clang.exe --target=x86_64-unknown-windows-itanium -ffreestanding \
//     -nostdlib -isystem $INCLUDES memory_api_experiments.c \
//     lib/crt1.obj lib/crt_do_start.obj lib/crt_gs.obj lib/crt_tls.obj \
//     lib/crt_cfg.obj lib/crt_loadcfg.obj lib/c.lib \
//     $LIBCLIB/kernel32.lib $LIBCLIB/ntdll.lib $LIBCLIB/bcryptprimitives.lib \
//     $LIBCLIB/kernelbase.lib $LIBCLIB/sspicli.lib \
//     lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib \
//     -fuse-ld=lld -Wl,-subsystem:console -o $STAGING/memory_api_experiments.exe
//
// Run:
//   $STAGING/memory_api_experiments.exe

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// NT types and APIs — we use our own declarations (no Windows.h)
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
typedef unsigned long long *PULONG_PTR;
typedef unsigned short USHORT;
typedef unsigned char BOOLEAN;

#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

typedef union {
  struct { ULONG LowPart; LONG HighPart; };
  long long QuadPart;
} LARGE_INTEGER;

// MBI
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

// Memory constants
#define MEM_COMMIT             0x00001000
#define MEM_RESERVE            0x00002000
#define MEM_DECOMMIT           0x00004000
#define MEM_RELEASE            0x00008000
#define MEM_FREE               0x00010000
#define MEM_PRIVATE            0x00020000
#define MEM_MAPPED             0x00040000
#define MEM_RESET              0x00080000
#define MEM_RESET_UNDO         0x01000000
#define MEM_WRITE_WATCH        0x00200000
#define MEM_LARGE_PAGES        0x20000000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#define MEM_PRESERVE_PLACEHOLDER_ON_UNMAP 0x00000002
#define MEM_UNMAP_WITH_TRANSIENT_BOOST 0x00000001

#define PAGE_NOACCESS   0x01
#define PAGE_READONLY   0x02
#define PAGE_READWRITE  0x04
#define PAGE_WRITECOPY  0x08
#define PAGE_EXECUTE_READ 0x20

#define SEC_COMMIT      0x08000000
#define SEC_RESERVE     0x04000000
#define SEC_NO_CHANGE   0x00400000

#define SECTION_ALL_ACCESS 0x000F001F
#define SECTION_QUERY      0x0001
#define SECTION_MAP_WRITE  0x0002
#define SECTION_MAP_READ   0x0004

#define WRITE_WATCH_FLAG_RESET 0x01

// MEMORY_REGION_INFORMATION (class 7)
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

// VmPageDirtyStateInformation
typedef struct {
  ULONG Flags;
} MEMORY_PAGE_DIRTY_STATE_INFORMATION;

typedef struct {
  PVOID VirtualAddress;
  SIZE_T NumberOfBytes;
} MEMORY_RANGE_ENTRY;

// Extended parameters
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

typedef struct {
  PVOID LowestStartingAddress;
  PVOID HighestEndingAddress;
  SIZE_T Alignment;
} MEM_ADDRESS_REQUIREMENTS;

// Partition info
#define SystemMemoryPartitionInformation 0

// Info classes
#define MemoryBasicInformation    0
#define MemoryRegionInformationEx 7
#define VmPageDirtyStateInformation 3

// Extended parameter types
#define MemExtendedParameterAddressRequirements 1
#define MemExtendedParameterPartitionHandle     3
#define MemExtendedParameterAttributeFlags      5

// Status codes
#define STATUS_SUCCESS             ((NTSTATUS)0x00000000)
#define STATUS_INVALID_PARAMETER   ((NTSTATUS)0xC000000D)
#define STATUS_CONFLICTING_ADDRESSES ((NTSTATUS)0xC0000018)
#define STATUS_INVALID_PAGE_PROTECTION ((NTSTATUS)0xC0000045)
#define STATUS_ACCESS_DENIED       ((NTSTATUS)0xC0000022)
#define STATUS_PRIVILEGE_NOT_HELD  ((NTSTATUS)0xC0000061)
#define STATUS_NOT_SUPPORTED       ((NTSTATUS)0xC00000BB)
#define STATUS_SECTION_PROTECTION  ((NTSTATUS)0xC000004E)

// NT API declarations
extern NTSTATUS NTAPI NtAllocateVirtualMemoryEx(
    HANDLE ProcessHandle, PVOID *BaseAddress, SIZE_T *RegionSize,
    ULONG AllocationType, ULONG PageProtection,
    MEM_EXTENDED_PARAMETER *ExtendedParameters, ULONG ExtendedParameterCount);

extern NTSTATUS NTAPI NtFreeVirtualMemory(
    HANDLE ProcessHandle, PVOID *BaseAddress, SIZE_T *RegionSize,
    ULONG FreeType);

extern NTSTATUS NTAPI NtQueryVirtualMemory(
    HANDLE ProcessHandle, PVOID BaseAddress, ULONG MemoryInformationClass,
    PVOID MemoryInformation, SIZE_T MemoryInformationLength,
    SIZE_T *ReturnLength);

extern NTSTATUS NTAPI NtProtectVirtualMemory(
    HANDLE ProcessHandle, PVOID *BaseAddress, SIZE_T *RegionSize,
    ULONG NewProtection, ULONG *OldProtection);

extern NTSTATUS NTAPI NtCreateSectionEx(
    HANDLE *SectionHandle, ULONG DesiredAccess, PVOID ObjectAttributes,
    LARGE_INTEGER *MaximumSize, ULONG SectionPageProtection,
    ULONG AllocationAttributes, HANDLE FileHandle,
    MEM_EXTENDED_PARAMETER *ExtendedParameters, ULONG ExtendedParameterCount);

extern NTSTATUS NTAPI NtMapViewOfSectionEx(
    HANDLE SectionHandle, HANDLE ProcessHandle, PVOID *BaseAddress,
    LARGE_INTEGER *SectionOffset, SIZE_T *ViewSize, ULONG AllocationType,
    ULONG PageProtection, MEM_EXTENDED_PARAMETER *ExtendedParameters,
    ULONG ExtendedParameterCount);

extern NTSTATUS NTAPI NtUnmapViewOfSectionEx(
    HANDLE ProcessHandle, PVOID BaseAddress, ULONG Flags);

extern NTSTATUS NTAPI NtClose(HANDLE Handle);

extern NTSTATUS NTAPI NtGetWriteWatch(
    HANDLE ProcessHandle, ULONG Flags, PVOID BaseAddress, SIZE_T RegionSize,
    PVOID *UserAddressArray, PULONG_PTR EntriesInUserAddressArray,
    PULONG Granularity);

extern NTSTATUS NTAPI NtResetWriteWatch(
    HANDLE ProcessHandle, PVOID BaseAddress, SIZE_T RegionSize);

extern NTSTATUS NTAPI NtSetInformationVirtualMemory(
    HANDLE ProcessHandle, ULONG VmInformationClass, SIZE_T NumberOfEntries,
    MEMORY_RANGE_ENTRY *VirtualAddresses, PVOID VmInformation,
    ULONG VmInformationLength);

extern NTSTATUS NTAPI NtCreatePartition(
    HANDLE ParentPartitionHandle, HANDLE *PartitionHandle,
    ULONG DesiredAccess, PVOID ObjectAttributes);

extern NTSTATUS NTAPI NtManagePartition(
    HANDLE TargetHandle, HANDLE SourceHandle,
    ULONG PartitionInformationClass,
    PVOID PartitionInformation, ULONG PartitionInformationLength);

extern NTSTATUS NTAPI NtExtendSection(
    HANDLE SectionHandle, LARGE_INTEGER *NewMaximumSize);

static HANDLE self(void) { return (HANDLE)(long long)(-1); }

#define PAGE_SIZE 4096ULL
#define ALLOC_GRAN 65536ULL

// Helpers
static void hexdump(const char *label, const void *p, int n) {
  const unsigned char *b = (const unsigned char *)p;
  printf("  %s:", label);
  for (int i = 0; i < n && i < 32; i++)
    printf(" %02x", b[i]);
  printf("\n");
}

static int query_mbi(void *addr, MEMORY_BASIC_INFORMATION *mbi) {
  return NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryBasicInformation,
                                         mbi, sizeof(*mbi), NULL));
}

static int query_mri(void *addr, MEMORY_REGION_INFORMATION *mri) {
  return NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryRegionInformationEx,
                                         mri, sizeof(*mri), NULL));
}

static int total_pass = 0, total_fail = 0, total_skip = 0;

#define PASS(fmt, ...) do { total_pass++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } while(0)
#define FAIL(fmt, ...) do { total_fail++; printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define SKIP(fmt, ...) do { total_skip++; printf("  SKIP: " fmt "\n", ##__VA_ARGS__); } while(0)

//=============================================================================
// TEST 1: Memory Partition Isolation
//
// Hypothesis: A dedicated memory partition can scope mmap VA away from system
// regions, potentially eliminating the ForeignRegionTable.
//
// What we test:
//   - Can we create a partition without privileges?
//   - Can we allocate within a partition?
//   - Does a partition restrict VA scope or just commit accounting?
//   - Can the partition's PartitionId be seen in MBI/MRI queries?
//=============================================================================
static void test_partition_isolation(void) {
  printf("\n=== TEST 1: Memory Partition Isolation ===\n");

  HANDLE part = NULL;
  NTSTATUS st = NtCreatePartition(NULL, &part, 0x000F001F, NULL);
  printf("  NtCreatePartition: 0x%08lX\n", (unsigned long)st);

  if (!NT_SUCCESS(st)) {
    if (st == STATUS_PRIVILEGE_NOT_HELD || st == STATUS_ACCESS_DENIED) {
      SKIP("Requires SeLockMemoryPrivilege or admin — partition APIs are privileged");
    } else {
      FAIL("Unexpected error 0x%08lX creating partition", (unsigned long)st);
    }
    return;
  }

  PASS("Created memory partition handle=%p", part);

  // Try allocating in the partition
  PVOID addr = NULL;
  SIZE_T size = ALLOC_GRAN;
  MEM_EXTENDED_PARAMETER param = {0};
  param.Type = MemExtendedParameterPartitionHandle;
  param.Handle = part;

  st = NtAllocateVirtualMemoryEx(self(), &addr, &size,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE,
                                  &param, 1);
  printf("  NtAllocateVirtualMemoryEx(partition): 0x%08lX addr=%p\n",
         (unsigned long)st, addr);

  if (NT_SUCCESS(st)) {
    PASS("Allocated 64KB in partition at %p", addr);

    // Check PartitionId in MBI
    MEMORY_BASIC_INFORMATION mbi;
    if (query_mbi(addr, &mbi)) {
      printf("  MBI.PartitionId = %u (system=0, ours=?)\n", mbi.PartitionId);
      if (mbi.PartitionId != 0)
        PASS("PartitionId is non-zero (%u) — partition-scoped allocations visible!",
             mbi.PartitionId);
      else
        FAIL("PartitionId is 0 — partition may only affect commit accounting");
    }

    // Check MRI
    MEMORY_REGION_INFORMATION mri;
    if (query_mri(addr, &mri)) {
      printf("  MRI.PartitionId = %llu\n", (unsigned long long)mri.PartitionId);
    }

    // Can we allocate at the SAME address from a DIFFERENT partition (system)?
    // If partitions truly isolate VA, this should conflict.
    // (We already have it allocated, so it will conflict regardless — this is
    // more about observing the PartitionId field behavior)

    // Cleanup
    PVOID free_addr = addr;
    SIZE_T free_size = 0;
    NtFreeVirtualMemory(self(), &free_addr, &free_size, MEM_RELEASE);
  } else {
    FAIL("Could not allocate in partition — 0x%08lX", (unsigned long)st);
  }

  NtClose(part);
}

//=============================================================================
// TEST 2: Write-Watch for Precise Dirty Tracking
//
// Hypothesis: MEM_WRITE_WATCH + NtGetWriteWatch gives us exact per-page dirty
// information on private anonymous memory. This could:
//   - Make MADV_DONTNEED only zero actually-dirty pages
//   - Provide precise dirty tracking for fork() COW optimization
//   - Enable smarter partial-unmap decisions
//=============================================================================
static void test_write_watch(void) {
  printf("\n=== TEST 2: Write-Watch for Precise Dirty Tracking ===\n");

  // Allocate 16 pages with MEM_WRITE_WATCH
  PVOID addr = NULL;
  SIZE_T size = 16 * PAGE_SIZE;
  NTSTATUS st = NtAllocateVirtualMemoryEx(
      self(), &addr, &size,
      MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE,
      NULL, 0);

  if (!NT_SUCCESS(st)) {
    FAIL("MEM_WRITE_WATCH allocation failed: 0x%08lX", (unsigned long)st);
    return;
  }
  PASS("Allocated 16 pages with MEM_WRITE_WATCH at %p", addr);

  // Write to pages 0, 3, 7, 15
  volatile char *p = (volatile char *)addr;
  p[0 * PAGE_SIZE] = 'A';
  p[3 * PAGE_SIZE] = 'B';
  p[7 * PAGE_SIZE] = 'C';
  p[15 * PAGE_SIZE] = 'D';

  // Query written pages
  PVOID pages[16];
  ULONG_PTR count = 16;
  ULONG granularity = 0;
  st = NtGetWriteWatch(self(), 0, addr, size, pages, &count, &granularity);

  if (!NT_SUCCESS(st)) {
    FAIL("NtGetWriteWatch failed: 0x%08lX", (unsigned long)st);
  } else {
    printf("  NtGetWriteWatch: %llu dirty pages, granularity=%lu\n",
           (unsigned long long)count, (unsigned long)granularity);
    int expected_dirty[16] = {0};
    expected_dirty[0] = expected_dirty[3] = expected_dirty[7] = expected_dirty[15] = 1;

    int match = 1;
    for (ULONG_PTR i = 0; i < count; i++) {
      int page_idx = (int)(((char *)pages[i] - (char *)addr) / PAGE_SIZE);
      printf("    dirty page[%llu] = offset 0x%llx (page %d)\n",
             (unsigned long long)i,
             (unsigned long long)((char *)pages[i] - (char *)addr), page_idx);
      if (page_idx < 0 || page_idx >= 16 || !expected_dirty[page_idx])
        match = 0;
    }

    if (count == 4 && match)
      PASS("Write-watch correctly identified exactly 4 dirty pages");
    else
      FAIL("Expected 4 dirty pages (0,3,7,15), got %llu", (unsigned long long)count);
  }

  // Test reset and re-query
  st = NtResetWriteWatch(self(), addr, size);
  if (NT_SUCCESS(st)) {
    count = 16;
    NtGetWriteWatch(self(), 0, addr, size, pages, &count, &granularity);
    if (count == 0)
      PASS("After NtResetWriteWatch, dirty count is 0");
    else
      FAIL("After reset, still %llu dirty pages", (unsigned long long)count);
  }

  // Test: can we combine write-watch with MEM_RESERVE_PLACEHOLDER?
  // This would be the holy grail for placeholder-based anon mappings.
  PVOID addr2 = NULL;
  SIZE_T size2 = ALLOC_GRAN;
  st = NtAllocateVirtualMemoryEx(
      self(), &addr2, &size2,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER | MEM_WRITE_WATCH, PAGE_NOACCESS,
      NULL, 0);
  printf("  MEM_RESERVE_PLACEHOLDER | MEM_WRITE_WATCH: 0x%08lX\n", (unsigned long)st);
  if (NT_SUCCESS(st)) {
    PASS("Write-watch + placeholder combo works! addr=%p", addr2);
    // Try replacing placeholder with committed write-watch
    PVOID rep_addr = addr2;
    SIZE_T rep_size = size2;
    st = NtAllocateVirtualMemoryEx(
        self(), &rep_addr, &rep_size,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH,
        PAGE_READWRITE, NULL, 0);
    printf("  REPLACE_PLACEHOLDER + WRITE_WATCH: 0x%08lX\n", (unsigned long)st);
    if (NT_SUCCESS(st))
      PASS("Placeholder → committed+write-watch transition works!");
    else
      FAIL("Cannot replace placeholder with write-watch commit: 0x%08lX", (unsigned long)st);

    // Cleanup
    PVOID f = addr2; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  } else {
    printf("  (Write-watch cannot combine with placeholder reservation)\n");
    SKIP("MEM_WRITE_WATCH | MEM_RESERVE_PLACEHOLDER not supported");
  }

  // Check MRI for write-watch flag
  MEMORY_REGION_INFORMATION mri;
  if (query_mri(addr, &mri)) {
    printf("  MRI.MappedWriteWatch = %u\n", mri.MappedWriteWatch);
    if (mri.MappedWriteWatch)
      PASS("MRI correctly reports MappedWriteWatch=1");
  }

  // Cleanup
  PVOID fa = addr; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &fa, &fs, MEM_RELEASE);
}

//=============================================================================
// TEST 3: VmPageDirtyStateInformation for Section Views
//
// Hypothesis: NtSetInformationVirtualMemory(VmPageDirtyStateInformation) can
// query/reset dirty state on section views, giving precise COW detection
// without the WRITECOPY→READWRITE heuristic.
//=============================================================================
static void test_dirty_state_info(void) {
  printf("\n=== TEST 3: VmPageDirtyStateInformation ===\n");

  // Create a pagefile-backed section (mimics MAP_ANONYMOUS|MAP_SHARED)
  LARGE_INTEGER max_size;
  max_size.QuadPart = 16 * PAGE_SIZE;
  HANDLE section = NULL;
  NTSTATUS st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                                   &max_size, PAGE_READWRITE, SEC_COMMIT,
                                   NULL, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("NtCreateSectionEx failed: 0x%08lX", (unsigned long)st);
    return;
  }

  // Create placeholder, map section into it
  PVOID ph = NULL;
  SIZE_T ph_size = 16 * PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                  PAGE_NOACCESS, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Placeholder creation failed: 0x%08lX", (unsigned long)st);
    NtClose(section);
    return;
  }

  PVOID view = ph;
  SIZE_T view_size = ph_size;
  LARGE_INTEGER offset = {0};
  st = NtMapViewOfSectionEx(section, self(), &view, &offset, &view_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("NtMapViewOfSectionEx failed: 0x%08lX", (unsigned long)st);
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    NtClose(section);
    return;
  }
  PASS("Mapped 16-page section view at %p", view);

  // Write to pages 2, 5, 11
  volatile char *p = (volatile char *)view;
  p[2 * PAGE_SIZE] = 'X';
  p[5 * PAGE_SIZE] = 'Y';
  p[11 * PAGE_SIZE] = 'Z';

  // Try querying dirty state
  MEMORY_RANGE_ENTRY range;
  range.VirtualAddress = view;
  range.NumberOfBytes = view_size;
  MEMORY_PAGE_DIRTY_STATE_INFORMATION dirty_info = {0};

  st = NtSetInformationVirtualMemory(self(), VmPageDirtyStateInformation, 1,
                                      &range, &dirty_info, sizeof(dirty_info));
  printf("  VmPageDirtyStateInformation query: 0x%08lX flags=0x%lx\n",
         (unsigned long)st, (unsigned long)dirty_info.Flags);

  if (NT_SUCCESS(st)) {
    PASS("VmPageDirtyStateInformation works on section views!");
    printf("  Flags: 0x%lx — need to decode per-page dirty bits\n",
           (unsigned long)dirty_info.Flags);
  } else if (st == STATUS_INVALID_PARAMETER) {
    SKIP("VmPageDirtyStateInformation not supported for section views (expected)");
  } else if (st == STATUS_NOT_SUPPORTED) {
    SKIP("VmPageDirtyStateInformation not supported on this Windows build");
  } else {
    FAIL("Unexpected status: 0x%08lX", (unsigned long)st);
  }

  // Also test on private memory for comparison
  PVOID priv = NULL;
  SIZE_T priv_size = 4 * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &priv, &priv_size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  if (priv) {
    ((volatile char *)priv)[0] = 1;
    range.VirtualAddress = priv;
    range.NumberOfBytes = priv_size;
    dirty_info.Flags = 0;
    st = NtSetInformationVirtualMemory(self(), VmPageDirtyStateInformation, 1,
                                        &range, &dirty_info, sizeof(dirty_info));
    printf("  VmPageDirtyState on private: 0x%08lX flags=0x%lx\n",
           (unsigned long)st, (unsigned long)dirty_info.Flags);
    PVOID f = priv; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }

  // Cleanup section view
  NtUnmapViewOfSectionEx(self(), view, 0);
  NtClose(section);
}

//=============================================================================
// TEST 4: MEM_RESET as Single-Syscall MADV_DONTNEED
//
// Hypothesis: MEM_RESET could replace the 2-syscall decommit+recommit for
// MADV_DONTNEED. We test:
//   - Does MEM_RESET guarantee zero-fill on next access?
//   - Can MEM_RESET_UNDO recover content?
//   - What's the actual behavior — zeroed or stale?
//=============================================================================
static void test_mem_reset_semantics(void) {
  printf("\n=== TEST 4: MEM_RESET Semantics (Single-Syscall DONTNEED?) ===\n");

  PVOID addr = NULL;
  SIZE_T size = 4 * PAGE_SIZE;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &addr, &size,
                                           MEM_RESERVE | MEM_COMMIT,
                                           PAGE_READWRITE, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Allocation failed: 0x%08lX", (unsigned long)st);
    return;
  }

  // Fill with known pattern
  memset(addr, 0xAA, (size_t)size);
  printf("  Filled %llu bytes with 0xAA\n", (unsigned long long)size);
  hexdump("before reset", addr, 16);

  // MEM_RESET page 0 and 1
  PVOID reset_addr = addr;
  SIZE_T reset_size = 2 * PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &reset_addr, &reset_size,
                                  MEM_RESET, PAGE_READWRITE, NULL, 0);
  printf("  MEM_RESET on first 2 pages: 0x%08lX\n", (unsigned long)st);

  if (!NT_SUCCESS(st)) {
    FAIL("MEM_RESET failed: 0x%08lX", (unsigned long)st);
    goto cleanup4;
  }

  // Read immediately — content usually survives if pages aren't under pressure
  hexdump("after reset (immediate read)", addr, 16);
  int survived = (((unsigned char *)addr)[0] == 0xAA);
  printf("  Content %s immediately after MEM_RESET\n",
         survived ? "SURVIVED" : "was ZEROED");

  if (survived) {
    printf("  (This is expected — MEM_RESET only MARKS pages as discardable.\n"
           "   Content survives until memory pressure reclaims them.)\n");
    PASS("MEM_RESET does NOT guarantee zero-fill — cannot replace MADV_DONTNEED alone");
  } else {
    PASS("MEM_RESET zeroed content — could be a MADV_DONTNEED replacement!");
  }

  // Test MEM_RESET_UNDO — can we recover?
  PVOID undo_addr = addr;
  SIZE_T undo_size = 2 * PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &undo_addr, &undo_size,
                                  MEM_RESET_UNDO, PAGE_READWRITE, NULL, 0);
  printf("  MEM_RESET_UNDO: 0x%08lX\n", (unsigned long)st);
  if (NT_SUCCESS(st)) {
    hexdump("after undo", addr, 16);
    PASS("MEM_RESET_UNDO succeeded — content recovery possible");
  }

  // Compare cost: MEM_RESET (1 syscall) vs decommit+recommit (2 syscalls)
  // The semantic difference is crucial:
  //   MEM_RESET: content MAY survive (MADV_FREE semantics)
  //   decommit+recommit: content GUARANTEED zero (MADV_DONTNEED semantics)
  printf("\n  Summary:\n"
         "    MEM_RESET     = MADV_FREE     (1 syscall, lazy discard)\n"
         "    decommit+commit = MADV_DONTNEED (2 syscalls, guaranteed zero)\n"
         "    MEM_RESET is ideal for MADV_FREE, NOT for MADV_DONTNEED\n");

  // Test: pages 2-3 should be untouched (still 0xAA)
  if (((unsigned char *)addr)[2 * PAGE_SIZE] == 0xAA)
    PASS("Non-reset pages preserved correctly");
  else
    FAIL("Non-reset pages corrupted!");

cleanup4:;
  PVOID f = addr; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
}

//=============================================================================
// TEST 5: MEM_UNMAP_WITH_TRANSIENT_BOOST During Split-Remap
//
// Hypothesis: Using MEM_UNMAP_WITH_TRANSIENT_BOOST when unmapping a view for
// split-remap keeps pages at higher priority on the standby list, reducing
// re-fault cost when the kept fragments are remapped.
//
// We can't directly measure page faults here, but we verify the flag works
// and observe any behavior differences.
//=============================================================================
static void test_transient_boost(void) {
  printf("\n=== TEST 5: MEM_UNMAP_WITH_TRANSIENT_BOOST ===\n");

  // Create section + view to unmap
  LARGE_INTEGER max_size;
  max_size.QuadPart = 256 * PAGE_SIZE; // 1MB
  HANDLE section = NULL;
  NTSTATUS st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                                   &max_size, PAGE_READWRITE, SEC_COMMIT,
                                   NULL, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Section creation failed: 0x%08lX", (unsigned long)st);
    return;
  }

  // Create placeholder
  PVOID ph = NULL;
  SIZE_T ph_size = 256 * PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                  PAGE_NOACCESS, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Placeholder failed: 0x%08lX", (unsigned long)st);
    NtClose(section);
    return;
  }

  // Map section into placeholder
  PVOID view = ph;
  SIZE_T view_size = ph_size;
  LARGE_INTEGER offset = {0};
  st = NtMapViewOfSectionEx(section, self(), &view, &offset, &view_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Map failed: 0x%08lX", (unsigned long)st);
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    NtClose(section);
    return;
  }

  // Touch all pages to put them in working set
  volatile char *p = (volatile char *)view;
  for (int i = 0; i < 256; i++)
    p[i * PAGE_SIZE] = (char)i;

  // Test 1: Unmap with TRANSIENT_BOOST + PRESERVE_PLACEHOLDER
  st = NtUnmapViewOfSectionEx(self(), view,
                                MEM_UNMAP_WITH_TRANSIENT_BOOST |
                                MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
  printf("  Unmap with TRANSIENT_BOOST | PRESERVE_PLACEHOLDER: 0x%08lX\n",
         (unsigned long)st);

  if (NT_SUCCESS(st)) {
    PASS("TRANSIENT_BOOST + PRESERVE_PLACEHOLDER combo accepted by kernel!");

    // Verify it's a placeholder now
    MEMORY_BASIC_INFORMATION mbi;
    if (query_mbi(view, &mbi)) {
      printf("  After unmap: State=0x%lx Type=0x%lx\n",
             (unsigned long)mbi.State, (unsigned long)mbi.Type);
      if (mbi.State == MEM_RESERVE && mbi.Type == MEM_FREE) {
        // It's a placeholder — MBI shows MEM_FREE type for placeholders
        // (MemoryRegionInformationEx would show PlaceholderReservation=1)
        MEMORY_REGION_INFORMATION mri;
        if (query_mri(view, &mri) && mri.PlaceholderReservation)
          PASS("Region is a placeholder after boost-unmap — ready for remap");
      }
    }

    // Remap the section back
    PVOID remap = view;
    SIZE_T remap_size = view_size;
    offset.QuadPart = 0;
    st = NtMapViewOfSectionEx(section, self(), &remap, &offset, &remap_size,
                               MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    if (NT_SUCCESS(st)) {
      PASS("Remap after transient-boost unmap succeeded");
      // Check content survived (pages were boosted, should still be in RAM)
      if (((volatile char *)remap)[0] == 0 && ((volatile char *)remap)[PAGE_SIZE] == 1)
        PASS("Content preserved through boost-unmap-remap cycle!");
      NtUnmapViewOfSectionEx(self(), remap, 0);
    } else {
      FAIL("Remap failed: 0x%08lX", (unsigned long)st);
      // Release the placeholder
      PVOID f = view; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    }
  } else {
    FAIL("TRANSIENT_BOOST | PRESERVE_PLACEHOLDER rejected: 0x%08lX", (unsigned long)st);
    // Try just PRESERVE_PLACEHOLDER as fallback
    st = NtUnmapViewOfSectionEx(self(), view, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
    printf("  Fallback PRESERVE_PLACEHOLDER only: 0x%08lX\n", (unsigned long)st);
    if (NT_SUCCESS(st)) {
      PVOID f = view; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    }
  }

  NtClose(section);
}

//=============================================================================
// TEST 6: Placeholder Coalescing After Partial Unmap
//
// Hypothesis: After partial munmap creates adjacent free placeholders, we can
// coalesce them to simplify future operations. Tests:
//   - Split into 3 placeholders
//   - Release the middle one
//   - Verify the outer two cannot be coalesced (they're separate allocations)
//   - OR: release middle, verify state, test coalesce of adjacent
//=============================================================================
static void test_placeholder_coalesce(void) {
  printf("\n=== TEST 6: Placeholder Coalescing ===\n");

  // Create a large placeholder
  PVOID ph = NULL;
  SIZE_T ph_size = 4 * ALLOC_GRAN; // 256KB
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                                           MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                           PAGE_NOACCESS, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Large placeholder failed: 0x%08lX", (unsigned long)st);
    return;
  }
  PASS("Created 256KB placeholder at %p", ph);

  // Split at 64KB and 192KB → three 64KB placeholders
  PVOID split1 = ph;
  SIZE_T split1_size = ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &split1, &split1_size,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  Split at 64KB: 0x%08lX\n", (unsigned long)st);
  if (!NT_SUCCESS(st)) { FAIL("First split failed"); goto cleanup6; }

  PVOID split2 = (char *)ph + 2 * ALLOC_GRAN;
  SIZE_T split2_size = ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &split2, &split2_size,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  printf("  Split at 192KB: 0x%08lX (creating 3rd placeholder)\n", (unsigned long)st);

  // Now we have:
  //   [ph, ph+64K)        = placeholder A
  //   [ph+64K, ph+128K)   = placeholder B (middle)
  //   [ph+128K, ph+192K)  = placeholder C
  //   [ph+192K, ph+256K)  = placeholder D

  // Actually the split creates: [0-64K] [64K-192K] → [64K-128K] [128K-192K] [192K-256K]
  // Let me re-think. First split at 64K: [0-64K] [64K-256K]
  // Second split at 192K on the [64K-256K] piece:
  //   split2 = ph+192K, size=64K → splits [64K-256K] at offset (192K-64K)=128K into [64K-192K] [192K-256K]
  // Wait, the split API: NtFreeVirtualMemory(addr, size, MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)
  // addr = base of placeholder, size = split point within that placeholder.
  // Actually, addr must be the allocation base and size is the split offset.

  // Let me verify what we have
  MEMORY_BASIC_INFORMATION mbi;
  char *base = (char *)ph;
  printf("  Region map after splits:\n");
  for (int i = 0; i < 4; i++) {
    if (query_mbi(base + i * ALLOC_GRAN, &mbi)) {
      MEMORY_REGION_INFORMATION mri;
      int is_ph = 0;
      if (query_mri(base + i * ALLOC_GRAN, &mri))
        is_ph = mri.PlaceholderReservation;
      printf("    +%dK: State=0x%lx Type=0x%lx RegionSize=%lluK placeholder=%d\n",
             (int)(i * 64), (unsigned long)mbi.State, (unsigned long)mbi.Type,
             (unsigned long long)mbi.RegionSize / 1024, is_ph);
    }
  }

  // Test coalescing: can we coalesce placeholder A + B?
  // MEM_COALESCE_PLACEHOLDERS requires: addr = start, size = total span
  PVOID coal_addr = ph;
  SIZE_T coal_size = 2 * ALLOC_GRAN; // First two 64K placeholders
  st = NtFreeVirtualMemory(self(), &coal_addr, &coal_size,
                            MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
  printf("  Coalesce first two placeholders: 0x%08lX\n", (unsigned long)st);

  if (NT_SUCCESS(st)) {
    PASS("Coalesced two adjacent placeholders into one 128KB placeholder!");
    if (query_mbi(ph, &mbi))
      printf("    Coalesced region size: %lluK\n",
             (unsigned long long)mbi.RegionSize / 1024);
  } else {
    FAIL("Coalesce failed: 0x%08lX — placeholders may not be from same allocation",
         (unsigned long)st);
  }

  // Now try the full coalesce — all remaining pieces
  coal_addr = ph;
  coal_size = 4 * ALLOC_GRAN;
  st = NtFreeVirtualMemory(self(), &coal_addr, &coal_size,
                            MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
  printf("  Coalesce ALL placeholders back to one: 0x%08lX\n", (unsigned long)st);
  if (NT_SUCCESS(st))
    PASS("Full coalesce succeeded — all 4 fragments back to one 256KB placeholder");

cleanup6:;
  // Release whatever is left
  // Since coalesce may have succeeded, release from the base
  PVOID f = ph; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  // Release remaining fragments if coalesce failed
  for (int i = 1; i < 4; i++) {
    f = (char *)ph + i * ALLOC_GRAN;
    fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }
}

//=============================================================================
// TEST 7: SEC_NO_CHANGE for Immutable Mappings
//
// Hypothesis: SEC_NO_CHANGE prevents mprotect on a section view. This could
// be used for immutable zero-page mappings, shared read-only sections, etc.
// The kernel can potentially optimize knowing protection won't change.
//=============================================================================
static void test_sec_no_change(void) {
  printf("\n=== TEST 7: SEC_NO_CHANGE Immutable Sections ===\n");

  // Create a pagefile section with SEC_NO_CHANGE | SEC_COMMIT
  LARGE_INTEGER max_size;
  max_size.QuadPart = 4 * PAGE_SIZE;
  HANDLE section = NULL;
  NTSTATUS st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                                   &max_size, PAGE_READWRITE,
                                   SEC_COMMIT | SEC_NO_CHANGE,
                                   NULL, NULL, 0);
  printf("  NtCreateSectionEx(SEC_COMMIT | SEC_NO_CHANGE): 0x%08lX\n",
         (unsigned long)st);

  if (!NT_SUCCESS(st)) {
    FAIL("SEC_NO_CHANGE section creation failed: 0x%08lX", (unsigned long)st);
    return;
  }
  PASS("Created SEC_NO_CHANGE section");

  // Map it via placeholder
  PVOID ph = NULL;
  SIZE_T ph_size = 4 * PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                  PAGE_NOACCESS, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Placeholder failed"); NtClose(section); return;
  }

  PVOID view = ph;
  SIZE_T view_size = ph_size;
  LARGE_INTEGER offset = {0};
  st = NtMapViewOfSectionEx(section, self(), &view, &offset, &view_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  printf("  Map view: 0x%08lX at %p\n", (unsigned long)st, view);

  if (!NT_SUCCESS(st)) {
    FAIL("Map failed: 0x%08lX", (unsigned long)st);
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    NtClose(section);
    return;
  }

  // Write some data (should work — protection is READWRITE)
  memset(view, 0xBB, PAGE_SIZE);
  PASS("Write to SEC_NO_CHANGE view succeeded");

  // Try to change protection — this SHOULD fail
  PVOID prot_addr = view;
  SIZE_T prot_size = PAGE_SIZE;
  ULONG old_prot;
  st = NtProtectVirtualMemory(self(), &prot_addr, &prot_size,
                               PAGE_READONLY, &old_prot);
  printf("  NtProtectVirtualMemory(READONLY): 0x%08lX\n", (unsigned long)st);

  if (!NT_SUCCESS(st)) {
    PASS("mprotect correctly REJECTED on SEC_NO_CHANGE section (status=0x%08lX)",
         (unsigned long)st);
    if (st == STATUS_INVALID_PAGE_PROTECTION)
      printf("    STATUS_INVALID_PAGE_PROTECTION — as expected\n");
    else if (st == STATUS_SECTION_PROTECTION)
      printf("    STATUS_SECTION_PROTECTION — section-level restriction\n");
  } else {
    FAIL("mprotect SUCCEEDED on SEC_NO_CHANGE section — not immutable!");
    printf("    old_prot=0x%lx — kernel allowed the change\n", (unsigned long)old_prot);
  }

  // Try PAGE_EXECUTE_READ — definitely should fail
  prot_addr = view;
  prot_size = PAGE_SIZE;
  st = NtProtectVirtualMemory(self(), &prot_addr, &prot_size,
                               PAGE_EXECUTE_READ, &old_prot);
  printf("  NtProtectVirtualMemory(EXECUTE_READ): 0x%08lX\n", (unsigned long)st);
  if (!NT_SUCCESS(st))
    PASS("Execute protection change also rejected — fully immutable");

  // Cleanup
  NtUnmapViewOfSectionEx(self(), view, 0);
  NtClose(section);
}

//=============================================================================
// BONUS TEST: NtExtendSection — Can We Grow Sections In Place?
//
// Important for mremap grow-in-place: if we can extend the section without
// recreating it, the remap is simpler.
//=============================================================================
static void test_extend_section(void) {
  printf("\n=== BONUS: NtExtendSection ===\n");

  LARGE_INTEGER max_size;
  max_size.QuadPart = 4 * PAGE_SIZE;
  HANDLE section = NULL;
  NTSTATUS st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                                   &max_size, PAGE_READWRITE, SEC_COMMIT,
                                   NULL, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Section creation failed: 0x%08lX", (unsigned long)st);
    return;
  }

  // Extend to 8 pages
  LARGE_INTEGER new_size;
  new_size.QuadPart = 8 * PAGE_SIZE;
  st = NtExtendSection(section, &new_size);
  printf("  NtExtendSection(4 pages → 8 pages): 0x%08lX\n", (unsigned long)st);

  if (NT_SUCCESS(st))
    PASS("Section extended from 4 to 8 pages — mremap grow can use this");
  else
    FAIL("NtExtendSection failed: 0x%08lX", (unsigned long)st);

  // Extend to 1MB
  new_size.QuadPart = 256 * PAGE_SIZE;
  st = NtExtendSection(section, &new_size);
  printf("  NtExtendSection(8 pages → 256 pages): 0x%08lX\n", (unsigned long)st);
  if (NT_SUCCESS(st))
    PASS("Section extended to 1MB — large extends work");

  // Try SEC_RESERVE section extend
  LARGE_INTEGER res_size;
  res_size.QuadPart = 4 * PAGE_SIZE;
  HANDLE res_section = NULL;
  st = NtCreateSectionEx(&res_section, SECTION_ALL_ACCESS | 0x0010 /*SECTION_EXTEND_SIZE*/,
                          NULL, &res_size, PAGE_READWRITE, SEC_RESERVE,
                          NULL, NULL, 0);
  if (NT_SUCCESS(st)) {
    new_size.QuadPart = 16 * PAGE_SIZE;
    st = NtExtendSection(res_section, &new_size);
    printf("  NtExtendSection on SEC_RESERVE: 0x%08lX\n", (unsigned long)st);
    if (NT_SUCCESS(st))
      PASS("SEC_RESERVE section extend works — demand-commit sections are growable");
    else
      FAIL("SEC_RESERVE extend failed: 0x%08lX", (unsigned long)st);
    NtClose(res_section);
  }

  NtClose(section);
}

//=============================================================================
// BONUS TEST: Write-Watch on Section Views (negative test)
//
// Confirm MEM_WRITE_WATCH cannot be used on section views — this validates
// that our private-memory write-watch approach is the only path.
//=============================================================================
static void test_write_watch_sections(void) {
  printf("\n=== BONUS: Write-Watch on Section Views (negative test) ===\n");

  // Create a placeholder with MEM_WRITE_WATCH (already tested above)
  // Now create section and try to map with MEM_REPLACE_PLACEHOLDER
  PVOID ph = NULL;
  SIZE_T ph_size = ALLOC_GRAN;
  // First just a normal placeholder
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
                                           MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                           PAGE_NOACCESS, NULL, 0);
  if (!NT_SUCCESS(st)) { FAIL("Placeholder failed"); return; }

  LARGE_INTEGER max_size;
  max_size.QuadPart = ALLOC_GRAN;
  HANDLE section = NULL;
  st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL,
                          &max_size, PAGE_READWRITE, SEC_COMMIT,
                          NULL, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Section failed");
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    return;
  }

  PVOID view = ph;
  SIZE_T view_size = ph_size;
  LARGE_INTEGER offset = {0};
  st = NtMapViewOfSectionEx(section, self(), &view, &offset, &view_size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  if (!NT_SUCCESS(st)) {
    FAIL("Map failed");
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    NtClose(section);
    return;
  }

  // Try NtGetWriteWatch on the section view — should fail
  ((volatile char *)view)[0] = 'X';
  PVOID pages[16];
  ULONG_PTR count = 16;
  ULONG granularity = 0;
  st = NtGetWriteWatch(self(), 0, view, (SIZE_T)view_size,
                        pages, &count, &granularity);
  printf("  NtGetWriteWatch on section view: 0x%08lX\n", (unsigned long)st);
  if (!NT_SUCCESS(st))
    PASS("Confirmed: write-watch does NOT work on section views (0x%08lX)",
         (unsigned long)st);
  else
    FAIL("Unexpected: write-watch works on section views! count=%llu",
         (unsigned long long)count);

  NtUnmapViewOfSectionEx(self(), view, 0);
  NtClose(section);
}

int main(void) {
  printf("=== NT Memory API Experiments for POSIX Subsystem ===\n");
  printf("Testing radical optimization hypotheses against live kernel\n");

  test_partition_isolation();
  test_write_watch();
  test_dirty_state_info();
  test_mem_reset_semantics();
  test_transient_boost();
  test_placeholder_coalesce();
  test_sec_no_change();
  test_extend_section();
  test_write_watch_sections();

  printf("\n========================================\n");
  printf("RESULTS: %d passed, %d failed, %d skipped\n",
         total_pass, total_fail, total_skip);
  printf("========================================\n");

  return total_fail > 0 ? 1 : 0;
}
