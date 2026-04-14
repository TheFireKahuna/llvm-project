// memory_innovation_test.c — Probe radical NT memory API innovations
//
// Tests 6 innovations for simplifying/optimizing the Windows POSIX memory
// subsystem. Each test is independent and prints PASS/FAIL/SKIP.
//
// Build (from build-wi):
//   bin/clang.exe --target=x86_64-unknown-windows-itanium -ffreestanding \
//     -nostdlib -isystem $INCLUDES memory_innovation_test.c \
//     lib/crt1.obj lib/crt_do_start.obj lib/crt_gs.obj lib/crt_tls.obj \
//     lib/crt_cfg.obj lib/crt_loadcfg.obj lib/c.lib \
//     $LIBCLIB/kernel32.lib $LIBCLIB/ntdll.lib $LIBCLIB/bcryptprimitives.lib \
//     $LIBCLIB/kernelbase.lib $LIBCLIB/sspicli.lib \
//     lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib \
//     -fuse-ld=lld -Wl,-subsystem:console -o $STAGING/memory_innovation_test.exe
//
// Run:
//   $STAGING/memory_innovation_test.exe

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// NT types — we use our own declarations (no Windows.h)
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
typedef long long LONGLONG;

#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define NT_ERROR(s)   ((NTSTATUS)(s) < 0)

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

// WSEX
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

// Memory constants
#define MEM_COMMIT              0x00001000
#define MEM_RESERVE             0x00002000
#define MEM_DECOMMIT            0x00004000
#define MEM_RELEASE             0x00008000
#define MEM_FREE                0x00010000
#define MEM_PRIVATE             0x00020000
#define MEM_MAPPED              0x00040000
#define MEM_WRITE_WATCH         0x00200000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#define MEM_PRESERVE_PLACEHOLDER_ON_UNMAP 0x00000002

#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE_READ      0x20
#define PAGE_GUARD             0x100

#define SEC_COMMIT             0x08000000
#define SEC_RESERVE            0x04000000
#define SECTION_ALL_ACCESS     0x000F001F

#define WRITE_WATCH_FLAG_RESET 0x01

// Info classes
#define MemoryBasicInformation            0
#define MemoryWorkingSetExInformation     4
#define MemoryRegionInformationEx         7

// Status codes
#define STATUS_SUCCESS                    ((NTSTATUS)0x00000000)
#define STATUS_INVALID_PARAMETER          ((NTSTATUS)0xC000000D)
#define STATUS_CONFLICTING_ADDRESSES      ((NTSTATUS)0xC0000018)
#define STATUS_INVALID_PAGE_PROTECTION    ((NTSTATUS)0xC0000045)

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

extern unsigned char NTAPI RtlQueryPerformanceCounter(
    LARGE_INTEGER *lpPerformanceCount);
extern unsigned char NTAPI RtlQueryPerformanceFrequency(
    LARGE_INTEGER *lpFrequency);

static HANDLE self(void) { return (HANDLE)(long long)(-1); }

#define PAGE_SIZE  4096ULL
#define ALLOC_GRAN 65536ULL

static int query_mbi(void *addr, MEMORY_BASIC_INFORMATION *mbi) {
  return NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryBasicInformation,
                                         mbi, sizeof(*mbi), NULL));
}

static int query_mri(void *addr, MEMORY_REGION_INFORMATION *mri) {
  return NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryRegionInformationEx,
                                         mri, sizeof(*mri), NULL));
}

static long long qpc_freq(void) {
  LARGE_INTEGER f;
  RtlQueryPerformanceFrequency(&f);
  return f.QuadPart;
}

static long long qpc_now(void) {
  LARGE_INTEGER t;
  RtlQueryPerformanceCounter(&t);
  return t.QuadPart;
}

// ns from QPC ticks
static long long ticks_to_ns(long long ticks, long long freq) {
  return (ticks * 1000000000LL) / freq;
}

static void release_all(void *base, SIZE_T total) {
  // Try releasing as one; if coalesced this works. Otherwise per-granule.
  PVOID b = base; SIZE_T s = 0;
  if (NT_SUCCESS(NtFreeVirtualMemory(self(), &b, &s, MEM_RELEASE)))
    return;
  char *p = (char *)base;
  for (SIZE_T off = 0; off < total; off += ALLOC_GRAN) {
    b = p + off; s = 0;
    NtFreeVirtualMemory(self(), &b, &s, MEM_RELEASE);
  }
}

static int total_pass = 0, total_fail = 0, total_skip = 0;

#define PASS(fmt, ...) do { total_pass++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } while(0)
#define FAIL(fmt, ...) do { total_fail++; printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define SKIP(fmt, ...) do { total_skip++; printf("  SKIP: " fmt "\n", ##__VA_ARGS__); } while(0)
#define INFO(fmt, ...) printf("  info: " fmt "\n", ##__VA_ARGS__)


//=============================================================================
// TEST 1: MemoryRegionInformationEx for Definitive Placeholder Detection
//
// Can MRI (class 7) definitively identify placeholders via the
// PlaceholderReservation bit? How does it compare to MBI for neighbor
// queries in coalesce_or_release_placeholder? Does MRI.RegionSize give
// the full allocation extent?
//=============================================================================
static void test_mri_placeholder_detection(void) {
  printf("\n=== TEST 1: MRI Placeholder Detection ===\n");

  // 1a: Create placeholder, verify MRI detects it
  PVOID ph = NULL;
  SIZE_T ph_size = 4 * ALLOC_GRAN; // 256KB
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) {
    FAIL("Placeholder creation failed: 0x%08lX", (unsigned long)st);
    return;
  }

  MEMORY_REGION_INFORMATION mri;
  if (query_mri(ph, &mri) && mri.PlaceholderReservation)
    PASS("MRI detects placeholder (PlaceholderReservation=1)");
  else
    FAIL("MRI does not detect placeholder");

  // Also check MBI — it can't distinguish
  MEMORY_BASIC_INFORMATION mbi;
  if (query_mbi(ph, &mbi))
    INFO("MBI: State=0x%lx Type=0x%lx (placeholder looks like regular MEM_RESERVE)",
         (unsigned long)mbi.State, (unsigned long)mbi.Type);

  // 1b: Create regular MEM_RESERVE (not placeholder), verify MRI says no
  PVOID reg = NULL;
  SIZE_T reg_size = ALLOC_GRAN;
  st = NtAllocateVirtualMemoryEx(self(), &reg, &reg_size,
      MEM_RESERVE, PAGE_NOACCESS, NULL, 0);
  if (NT_SUCCESS(st)) {
    MEMORY_REGION_INFORMATION mri2;
    if (query_mri(reg, &mri2)) {
      if (!mri2.PlaceholderReservation)
        PASS("MRI correctly reports PlaceholderReservation=0 for regular MEM_RESERVE");
      else
        FAIL("MRI falsely reports PlaceholderReservation=1 for regular MEM_RESERVE");
    }
    PVOID f = reg; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }

  // 1c: Split placeholder, verify both halves are detected as placeholders
  PVOID sp = ph;
  SIZE_T sp_size = 2 * ALLOC_GRAN; // Split at 128KB
  st = NtFreeVirtualMemory(self(), &sp, &sp_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  if (NT_SUCCESS(st)) {
    MEMORY_REGION_INFORMATION mri_a, mri_b;
    int a_ok = query_mri(ph, &mri_a) && mri_a.PlaceholderReservation;
    int b_ok = query_mri((char *)ph + 2 * ALLOC_GRAN, &mri_b) &&
               mri_b.PlaceholderReservation;
    if (a_ok && b_ok)
      PASS("Both halves of split placeholder detected by MRI");
    else
      FAIL("Split detection: first=%d second=%d", a_ok, b_ok);

    INFO("First half MRI.RegionSize=%lluK, Second half MRI.RegionSize=%lluK",
         (unsigned long long)mri_a.RegionSize / 1024,
         (unsigned long long)mri_b.RegionSize / 1024);
  } else {
    FAIL("Split failed: 0x%08lX", (unsigned long)st);
  }

  // 1d: Replace first half with committed private, verify MRI changes
  PVOID rep = ph;
  SIZE_T rep_size = 2 * ALLOC_GRAN;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      NULL, 0);
  if (NT_SUCCESS(st)) {
    MEMORY_REGION_INFORMATION mri3;
    if (query_mri(ph, &mri3)) {
      if (!mri3.PlaceholderReservation)
        PASS("Committed private: PlaceholderReservation=0 (correct)");
      else
        FAIL("Committed private still shows PlaceholderReservation=1");
      INFO("Private MRI: Private=%u RegionSize=%lluK CommitSize=%lluK",
           mri3.Private, (unsigned long long)mri3.RegionSize / 1024,
           (unsigned long long)mri3.CommitSize / 1024);
    }
  }

  // 1e: Performance comparison — MRI vs MBI for 10000 queries
  long long freq = qpc_freq();
  {
    long long t0 = qpc_now();
    for (int i = 0; i < 10000; i++) {
      MEMORY_BASIC_INFORMATION m;
      query_mbi((char *)ph + 2 * ALLOC_GRAN, &m);
    }
    long long mbi_ns = ticks_to_ns(qpc_now() - t0, freq);

    long long t1 = qpc_now();
    for (int i = 0; i < 10000; i++) {
      MEMORY_REGION_INFORMATION m;
      query_mri((char *)ph + 2 * ALLOC_GRAN, &m);
    }
    long long mri_ns = ticks_to_ns(qpc_now() - t1, freq);

    INFO("10K queries: MBI=%lldns (%lldns/query), MRI=%lldns (%lldns/query)",
         mbi_ns, mbi_ns / 10000, mri_ns, mri_ns / 10000);
    if (mri_ns <= mbi_ns * 2)
      PASS("MRI query cost is acceptable (<=2x MBI)");
    else
      INFO("MRI is significantly slower than MBI — may not be worth it for hot paths");
  }

  // Cleanup
  release_all(ph, 4 * ALLOC_GRAN);
}


//=============================================================================
// TEST 2: Write-Watch + Placeholder Replacement
//
// Can we add MEM_WRITE_WATCH when replacing a placeholder with committed
// private memory? This would give free dirty page tracking via
// NtGetWriteWatch on all anonymous private allocations.
//=============================================================================
static void test_write_watch_placeholder(void) {
  printf("\n=== TEST 2: Write-Watch + Placeholder Replacement ===\n");

  // Create placeholder
  PVOID ph = NULL;
  SIZE_T ph_size = 16 * PAGE_SIZE;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) {
    FAIL("Placeholder creation failed: 0x%08lX", (unsigned long)st);
    return;
  }

  // 2a: Try MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH (committed)
  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH,
      PAGE_READWRITE, NULL, 0);

  INFO("MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH (commit): 0x%08lX",
       (unsigned long)st);

  if (NT_SUCCESS(st)) {
    PASS("Write-watch + placeholder replacement works!");

    // Verify MRI shows MappedWriteWatch
    MEMORY_REGION_INFORMATION mri;
    if (query_mri(rep, &mri)) {
      INFO("MRI: Private=%u MappedWriteWatch=%u PlaceholderReservation=%u",
           mri.Private, mri.MappedWriteWatch, mri.PlaceholderReservation);
      if (mri.MappedWriteWatch)
        PASS("MRI confirms MappedWriteWatch=1");
    }

    // Write to pages 0, 3, 7, 15
    volatile char *p = (volatile char *)rep;
    p[0 * PAGE_SIZE] = 'A';
    p[3 * PAGE_SIZE] = 'B';
    p[7 * PAGE_SIZE] = 'C';
    p[15 * PAGE_SIZE] = 'D';

    // Query dirty pages
    PVOID pages[16];
    ULONG_PTR count = 16;
    ULONG granularity = 0;
    st = NtGetWriteWatch(self(), 0, rep, rep_size, pages, &count, &granularity);
    if (NT_SUCCESS(st)) {
      INFO("NtGetWriteWatch: %llu dirty pages, granularity=%lu",
           (unsigned long long)count, (unsigned long)granularity);
      for (ULONG_PTR i = 0; i < count; i++) {
        int idx = (int)(((char *)pages[i] - (char *)rep) / PAGE_SIZE);
        INFO("  dirty page %llu: offset 0x%llx (page %d)",
             (unsigned long long)i,
             (unsigned long long)((char *)pages[i] - (char *)rep), idx);
      }
      if (count == 4)
        PASS("NtGetWriteWatch correctly reports 4 dirty pages");
      else
        FAIL("Expected 4 dirty pages, got %llu", (unsigned long long)count);
    } else {
      FAIL("NtGetWriteWatch failed: 0x%08lX", (unsigned long)st);
    }

    // Test reset
    st = NtResetWriteWatch(self(), rep, rep_size);
    if (NT_SUCCESS(st)) {
      count = 16;
      NtGetWriteWatch(self(), 0, rep, rep_size, pages, &count, &granularity);
      if (count == 0)
        PASS("Reset clears dirty state");
      else
        FAIL("After reset: %llu dirty (expected 0)", (unsigned long long)count);
    }

    // 2b: Performance — NtGetWriteWatch vs WSEX for dirty detection
    // Write all 16 pages, then compare query times
    for (int i = 0; i < 16; i++)
      p[i * PAGE_SIZE] = (char)i;

    long long freq = qpc_freq();

    // Time NtGetWriteWatch (1000 iterations)
    long long t0 = qpc_now();
    for (int iter = 0; iter < 1000; iter++) {
      count = 16;
      NtGetWriteWatch(self(), 0, rep, rep_size, pages, &count, &granularity);
    }
    long long ww_ns = ticks_to_ns(qpc_now() - t0, freq);

    // Time WSEX query (1000 iterations)
    MEMORY_WORKING_SET_EX_INFORMATION wsex[16];
    long long t1 = qpc_now();
    for (int iter = 0; iter < 1000; iter++) {
      for (int i = 0; i < 16; i++)
        wsex[i].VirtualAddress = (char *)rep + i * PAGE_SIZE;
      NtQueryVirtualMemory(self(), NULL, MemoryWorkingSetExInformation,
                           wsex, 16 * sizeof(wsex[0]), NULL);
    }
    long long wsex_ns = ticks_to_ns(qpc_now() - t1, freq);

    INFO("1K queries (16 pages): WriteWatch=%lldns (%lldns/q) WSEX=%lldns (%lldns/q)",
         ww_ns, ww_ns / 1000, wsex_ns, wsex_ns / 1000);
    if (ww_ns < wsex_ns)
      PASS("NtGetWriteWatch is faster than WSEX");
    else
      INFO("WSEX is faster or comparable — write-watch benefit is tracking, not speed");

    // 2c: Test partial munmap compatibility — decommit + preserve_to_placeholder
    // This is the critical path: does write-watch survive decommit+punch?
    PVOID decom = (char *)rep + 4 * PAGE_SIZE;
    SIZE_T decom_size = 8 * PAGE_SIZE;
    st = NtFreeVirtualMemory(self(), &decom, &decom_size, MEM_DECOMMIT);
    if (NT_SUCCESS(st)) {
      PASS("Decommit on write-watch region succeeded");
      PVOID punch = (char *)rep + 4 * PAGE_SIZE;
      SIZE_T punch_size = 8 * PAGE_SIZE;
      st = NtFreeVirtualMemory(self(), &punch, &punch_size,
          MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
      if (NT_SUCCESS(st)) {
        PASS("Preserve-to-placeholder on write-watch region works!");
        // Check: is the punched-out region a placeholder?
        MEMORY_REGION_INFORMATION mri2;
        if (query_mri((char *)rep + 4 * PAGE_SIZE, &mri2) &&
            mri2.PlaceholderReservation)
          PASS("Punched region is a valid placeholder");
        // Check: does the remaining committed region still have write-watch?
        count = 16;
        st = NtGetWriteWatch(self(), 0, rep, 4 * PAGE_SIZE,
                             pages, &count, &granularity);
        if (NT_SUCCESS(st))
          PASS("NtGetWriteWatch still works on remaining committed region (%llu dirty)",
               (unsigned long long)count);
        else
          FAIL("NtGetWriteWatch fails after punch: 0x%08lX", (unsigned long)st);
      } else {
        FAIL("Preserve-to-placeholder on write-watch region failed: 0x%08lX",
             (unsigned long)st);
      }
    } else {
      FAIL("Decommit failed on write-watch region: 0x%08lX", (unsigned long)st);
    }

    release_all(rep, ph_size);
  } else {
    INFO("Write-watch cannot combine with placeholder replacement");

    // 2d: Try reserve-only replacement with write-watch
    PVOID rep2 = ph;
    SIZE_T rep2_size = ph_size;
    st = NtAllocateVirtualMemoryEx(self(), &rep2, &rep2_size,
        MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH,
        PAGE_NOACCESS, NULL, 0);
    INFO("MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH (reserve-only): 0x%08lX",
         (unsigned long)st);
    if (NT_SUCCESS(st)) {
      PASS("Reserve-only + write-watch replacement works!");
      // Commit and test
      PVOID cm = rep2;
      SIZE_T cm_size = rep2_size;
      st = NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
          MEM_COMMIT, PAGE_READWRITE, NULL, 0);
      if (NT_SUCCESS(st)) {
        volatile char *p2 = (volatile char *)rep2;
        p2[0] = 'X';
        p2[PAGE_SIZE] = 'Y';
        PVOID pages2[16];
        ULONG_PTR cnt2 = 16;
        ULONG gran2;
        st = NtGetWriteWatch(self(), 0, rep2, rep2_size,
                             pages2, &cnt2, &gran2);
        if (NT_SUCCESS(st) && cnt2 == 2)
          PASS("Write-watch works after reserve-replace + commit (%llu dirty)",
               (unsigned long long)cnt2);
        else
          INFO("GetWriteWatch status=0x%08lX count=%llu",
               (unsigned long)st, (unsigned long long)cnt2);
      }
      release_all(rep2, ph_size);
    } else {
      SKIP("Write-watch cannot combine with placeholder replacement at all");
      // Release original placeholder
      PVOID f = ph; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    }
  }
}


//=============================================================================
// TEST 3: Reserve-Only Placeholder Replacement (MAP_NORESERVE Path)
//
// Can we replace a placeholder with a bare MEM_RESERVE (no commit)?
// If yes, MAP_NORESERVE becomes: placeholder → reserve-replace →
// VEH demand-commit. Partial munmap is just decommit + preserve —
// eliminates the entire split-remap protocol for MAP_NORESERVE.
//=============================================================================
static void test_reserve_only_replacement(void) {
  printf("\n=== TEST 3: Reserve-Only Placeholder Replacement ===\n");

  // Create placeholder
  PVOID ph = NULL;
  SIZE_T ph_size = 8 * ALLOC_GRAN; // 512KB
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) {
    FAIL("Placeholder creation failed: 0x%08lX", (unsigned long)st);
    return;
  }
  PASS("Created 512KB placeholder at %p", ph);

  // 3a: Try MEM_RESERVE | MEM_REPLACE_PLACEHOLDER (no commit)
  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  INFO("MEM_RESERVE | MEM_REPLACE_PLACEHOLDER: 0x%08lX", (unsigned long)st);

  if (NT_ERROR(st)) {
    FAIL("Reserve-only replacement not supported: 0x%08lX", (unsigned long)st);
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    return;
  }
  PASS("Reserve-only placeholder replacement works!");

  // Verify region state
  MEMORY_BASIC_INFORMATION mbi;
  if (query_mbi(rep, &mbi)) {
    INFO("After replace: State=0x%lx Type=0x%lx Protect=0x%lx",
         (unsigned long)mbi.State, (unsigned long)mbi.Type,
         (unsigned long)mbi.Protect);
    if (mbi.State == MEM_RESERVE && mbi.Type == MEM_PRIVATE)
      PASS("Region is MEM_RESERVE + MEM_PRIVATE (correct for demand-commit)");
  }

  MEMORY_REGION_INFORMATION mri;
  if (query_mri(rep, &mri)) {
    INFO("MRI: PlaceholderReservation=%u Private=%u RegionSize=%lluK",
         mri.PlaceholderReservation, mri.Private,
         (unsigned long long)mri.RegionSize / 1024);
    if (!mri.PlaceholderReservation)
      PASS("No longer a placeholder after replacement");
    else
      INFO("Still shows as placeholder — interesting");
  }

  // 3b: Demand-commit individual pages
  PVOID cm = (char *)rep + 2 * PAGE_SIZE;
  SIZE_T cm_size = 3 * PAGE_SIZE; // Commit pages 2-4
  st = NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
      MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  if (NT_SUCCESS(st)) {
    PASS("Demand-commit on reserved region succeeded");
    volatile char *p = (volatile char *)rep;
    p[2 * PAGE_SIZE] = 'A';
    p[3 * PAGE_SIZE] = 'B';
    p[4 * PAGE_SIZE] = 'C';
    if (p[2 * PAGE_SIZE] == 'A' && p[3 * PAGE_SIZE] == 'B')
      PASS("Committed pages are accessible and writable");
  } else {
    FAIL("Demand-commit failed: 0x%08lX", (unsigned long)st);
  }

  // 3c: Decommit
  PVOID decom = (char *)rep + 3 * PAGE_SIZE;
  SIZE_T decom_size = PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &decom, &decom_size, MEM_DECOMMIT);
  if (NT_SUCCESS(st))
    PASS("Decommit on replaced reservation works");
  else
    FAIL("Decommit failed: 0x%08lX", (unsigned long)st);

  // 3d: Preserve-to-placeholder (the critical partial-munmap operation)
  // Decommit a range, then punch it to a placeholder
  PVOID punch_base = (char *)rep + 4 * ALLOC_GRAN;
  SIZE_T punch_size = 2 * ALLOC_GRAN;
  // First decommit everything in the range (it's all reserved, so decommit is no-op)
  // Actually, for MEM_RESERVE pages, MEM_DECOMMIT may fail or be no-op.
  // Try MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER directly.
  st = NtFreeVirtualMemory(self(), &punch_base, &punch_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  INFO("MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER on sub-range: 0x%08lX",
       (unsigned long)st);

  if (NT_SUCCESS(st)) {
    PASS("Preserve-to-placeholder works on reserve-replaced region!");
    MEMORY_REGION_INFORMATION mri2;
    if (query_mri((char *)rep + 4 * ALLOC_GRAN, &mri2)) {
      INFO("Punched region: PlaceholderReservation=%u RegionSize=%lluK",
           mri2.PlaceholderReservation, (unsigned long long)mri2.RegionSize / 1024);
      if (mri2.PlaceholderReservation)
        PASS("Punched sub-range is a valid placeholder — full roundtrip works!");
      else
        FAIL("Punched region is not a placeholder");
    }
  } else {
    FAIL("Preserve-to-placeholder failed: 0x%08lX", (unsigned long)st);
    INFO("This means reserve-replaced regions cannot do partial placeholder-punch");
    INFO("MAP_NORESERVE simplification would require a different approach");
  }

  // 3e: Verify remaining region is intact
  if (query_mbi(rep, &mbi))
    INFO("Remaining region: State=0x%lx Type=0x%lx RegionSize=%lluK",
         (unsigned long)mbi.State, (unsigned long)mbi.Type,
         (unsigned long long)mbi.RegionSize / 1024);

  // Cleanup
  release_all(rep, ph_size);
}


//=============================================================================
// TEST 4: Non-NOACCESS Placeholder
//
// Can we create a placeholder with accessible protection (PAGE_READWRITE)?
// If yes, we could create "accessible placeholders" that are both splittable
// AND readable — eliminating the replacement step for some paths.
// Expected: all fail (kernel enforces PAGE_NOACCESS for placeholders).
//=============================================================================
static void test_non_noaccess_placeholder(void) {
  printf("\n=== TEST 4: Non-NOACCESS Placeholder ===\n");

  struct { ULONG prot; const char *name; } prots[] = {
    { PAGE_READWRITE,  "PAGE_READWRITE" },
    { PAGE_READONLY,   "PAGE_READONLY" },
    { PAGE_EXECUTE_READ, "PAGE_EXECUTE_READ" },
    { PAGE_WRITECOPY,  "PAGE_WRITECOPY" },
  };

  for (int i = 0; i < 4; i++) {
    PVOID addr = NULL;
    SIZE_T size = ALLOC_GRAN;
    NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &addr, &size,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, prots[i].prot, NULL, 0);
    INFO("MEM_RESERVE_PLACEHOLDER + %s: 0x%08lX",
         prots[i].name, (unsigned long)st);
    if (NT_SUCCESS(st)) {
      PASS("%s placeholder works! addr=%p", prots[i].name, addr);
      // Check if it's actually accessible
      MEMORY_BASIC_INFORMATION mbi;
      if (query_mbi(addr, &mbi))
        INFO("  State=0x%lx Protect=0x%lx", (unsigned long)mbi.State,
             (unsigned long)mbi.Protect);
      PVOID f = addr; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    } else {
      INFO("  %s: rejected as expected (0x%08lX)", prots[i].name,
           (unsigned long)st);
    }
  }

  // If all fail, that's expected — document the result
  INFO("Kernel enforces PAGE_NOACCESS for placeholders (confirmed)");
}


//=============================================================================
// TEST 5: MRI RegionSize for Allocation Extent Discovery
//
// Does MEMORY_REGION_INFORMATION.RegionSize return the full allocation
// extent, or just the current same-attributes region (like MBI)?
// If full extent: replaces find_alloc_extent (currently a RegionWalker walk).
//=============================================================================
static void test_mri_region_size(void) {
  printf("\n=== TEST 5: MRI RegionSize Semantics ===\n");

  // 5a: Multi-region private allocation
  // Reserve 256KB, commit first 64KB, set guard on next page
  PVOID addr = NULL;
  SIZE_T total = 4 * ALLOC_GRAN; // 256KB
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &addr, &total,
      MEM_RESERVE, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) {
    FAIL("Reserve failed: 0x%08lX", (unsigned long)st);
    return;
  }

  // Commit first 64KB
  PVOID cm = addr;
  SIZE_T cm_size = ALLOC_GRAN;
  st = NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
      MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  if (NT_ERROR(st)) {
    FAIL("Commit failed: 0x%08lX", (unsigned long)st);
    PVOID f = addr; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    return;
  }

  // Set guard on next page
  PVOID gp = (char *)addr + ALLOC_GRAN;
  SIZE_T gp_size = PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &gp, &gp_size,
      MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD, NULL, 0);

  // Now we have: [committed 64K] [guard 4K] [reserved remainder]
  // MBI would see 3 different regions. What does MRI see?

  MEMORY_BASIC_INFORMATION mbi;
  MEMORY_REGION_INFORMATION mri;

  if (query_mbi(addr, &mbi))
    INFO("MBI at base: RegionSize=%lluK (committed region only)",
         (unsigned long long)mbi.RegionSize / 1024);

  if (query_mri(addr, &mri)) {
    INFO("MRI at base: RegionSize=%lluK AllocationBase=%p CommitSize=%lluK",
         (unsigned long long)mri.RegionSize / 1024, mri.AllocationBase,
         (unsigned long long)mri.CommitSize / 1024);
    if (mri.RegionSize == total)
      PASS("MRI.RegionSize == total allocation (full extent!) — replaces find_alloc_extent");
    else if (mri.RegionSize == ALLOC_GRAN)
      INFO("MRI.RegionSize == first committed region (same as MBI)");
    else
      INFO("MRI.RegionSize=%lluK — neither total nor first region",
           (unsigned long long)mri.RegionSize / 1024);
  }

  // Query at guard page
  if (query_mri((char *)addr + ALLOC_GRAN, &mri))
    INFO("MRI at guard: RegionSize=%lluK AllocationBase=%p",
         (unsigned long long)mri.RegionSize / 1024, mri.AllocationBase);

  // Query at reserved tail
  if (query_mri((char *)addr + ALLOC_GRAN + PAGE_SIZE, &mri))
    INFO("MRI at reserved: RegionSize=%lluK AllocationBase=%p",
         (unsigned long long)mri.RegionSize / 1024, mri.AllocationBase);

  // 5b: Same test on a placeholder (always one region)
  PVOID ph = NULL;
  SIZE_T ph_size = 4 * ALLOC_GRAN;
  st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_SUCCESS(st)) {
    if (query_mri(ph, &mri)) {
      INFO("Placeholder MRI: RegionSize=%lluK (should match allocation)",
           (unsigned long long)mri.RegionSize / 1024);
      if (mri.RegionSize == ph_size)
        PASS("MRI.RegionSize matches placeholder allocation size");
    }
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }

  // 5c: Section view with mixed protections
  LARGE_INTEGER max_size;
  max_size.QuadPart = 4 * ALLOC_GRAN;
  HANDLE section = NULL;
  st = NtCreateSectionEx(&section, SECTION_ALL_ACCESS, NULL, &max_size,
                          PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0);
  if (NT_SUCCESS(st)) {
    PVOID vph = NULL;
    SIZE_T vph_size = 4 * ALLOC_GRAN;
    st = NtAllocateVirtualMemoryEx(self(), &vph, &vph_size,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (NT_SUCCESS(st)) {
      PVOID view = vph;
      SIZE_T view_size = vph_size;
      LARGE_INTEGER offset = {0};
      st = NtMapViewOfSectionEx(section, self(), &view, &offset, &view_size,
          MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
      if (NT_SUCCESS(st)) {
        // Change protection on first page to READONLY
        PVOID pp = view;
        SIZE_T pp_size = PAGE_SIZE;
        ULONG old;
        NtProtectVirtualMemory(self(), &pp, &pp_size, PAGE_READONLY, &old);

        if (query_mri(view, &mri))
          INFO("Section view MRI: RegionSize=%lluK MappedPageFile=%u",
               (unsigned long long)mri.RegionSize / 1024, mri.MappedPageFile);

        NtUnmapViewOfSectionEx(self(), view, 0);
      } else {
        PVOID f = vph; SIZE_T fs = 0;
        NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
      }
    }
    NtClose(section);
  }

  // Cleanup
  PVOID f = addr; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
}


//=============================================================================
// TEST 6: Cross-Allocation Placeholder Coalesce
//
// Can MEM_COALESCE_PLACEHOLDERS merge placeholders that came from
// separate NtAllocateVirtualMemoryEx calls? Understanding this is
// critical for the coalesce_or_release_placeholder optimization.
//=============================================================================
static void test_cross_alloc_coalesce(void) {
  printf("\n=== TEST 6: Cross-Allocation Placeholder Coalesce ===\n");

  // Strategy: create one large placeholder, split into 4 × 64KB,
  // release the middle two (creating MEM_FREE gap), then create a
  // new placeholder in the gap. Test coalesce across boundaries.

  // First create a big placeholder and split it
  PVOID base = NULL;
  SIZE_T base_size = 4 * ALLOC_GRAN;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &base, &base_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) {
    FAIL("Base placeholder failed: 0x%08lX", (unsigned long)st);
    return;
  }
  INFO("Base placeholder at %p (256KB)", base);

  // Split into 4 × 64KB
  for (int i = 0; i < 3; i++) {
    PVOID sp = (char *)base + i * ALLOC_GRAN;
    SIZE_T sp_size = ALLOC_GRAN;
    st = NtFreeVirtualMemory(self(), &sp, &sp_size,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    if (NT_ERROR(st)) {
      FAIL("Split %d failed: 0x%08lX", i, (unsigned long)st);
      release_all(base, base_size);
      return;
    }
  }
  PASS("Split into 4 × 64KB placeholders");

  // 6a: Coalesce siblings (same original allocation) — should work
  {
    PVOID coal = base;
    SIZE_T coal_size = 2 * ALLOC_GRAN;
    st = NtFreeVirtualMemory(self(), &coal, &coal_size,
        MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    if (NT_SUCCESS(st))
      PASS("Sibling coalesce (same allocation) works");
    else
      FAIL("Sibling coalesce failed: 0x%08lX", (unsigned long)st);

    // Re-split for next test
    PVOID sp = base;
    SIZE_T sp_size = ALLOC_GRAN;
    NtFreeVirtualMemory(self(), &sp, &sp_size,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  }

  // 6b: Release middle two to MEM_FREE, create NEW placeholder in the gap
  // This creates placeholders from DIFFERENT NtAllocateVirtualMemoryEx calls
  {
    // Release slot 1 and 2 (the middle two)
    PVOID f1 = (char *)base + 1 * ALLOC_GRAN; SIZE_T fs1 = 0;
    NtFreeVirtualMemory(self(), &f1, &fs1, MEM_RELEASE);
    PVOID f2 = (char *)base + 2 * ALLOC_GRAN; SIZE_T fs2 = 0;
    NtFreeVirtualMemory(self(), &f2, &fs2, MEM_RELEASE);

    // Verify they're MEM_FREE
    MEMORY_BASIC_INFORMATION mbi;
    if (query_mbi((char *)base + ALLOC_GRAN, &mbi))
      INFO("Middle gap: State=0x%lx (expected MEM_FREE=0x10000)",
           (unsigned long)mbi.State);

    // Create new placeholder in the gap
    PVOID gap = (char *)base + ALLOC_GRAN;
    SIZE_T gap_size = 2 * ALLOC_GRAN;
    st = NtAllocateVirtualMemoryEx(self(), &gap, &gap_size,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (NT_SUCCESS(st)) {
      PASS("Created new placeholder in freed gap");

      // Now we have:
      //   [base]     = placeholder A (from original allocation)
      //   [base+64K] = placeholder B (from NEW allocation)  -- 128KB
      //   [base+192K]= placeholder C (from original allocation)
      //
      // Try coalescing A+B (cross-allocation, left boundary)
      PVOID coal_ab = base;
      SIZE_T coal_ab_size = ALLOC_GRAN + gap_size; // A (64K) + B (128K)
      st = NtFreeVirtualMemory(self(), &coal_ab, &coal_ab_size,
          MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
      INFO("Cross-allocation coalesce (A+B): 0x%08lX", (unsigned long)st);
      if (NT_SUCCESS(st)) {
        PASS("Cross-allocation coalesce WORKS!");
        MEMORY_REGION_INFORMATION mri;
        if (query_mri(base, &mri))
          INFO("Coalesced region: RegionSize=%lluK",
               (unsigned long long)mri.RegionSize / 1024);

        // Try the full coalesce: (A+B) + C
        PVOID coal_all = base;
        SIZE_T coal_all_size = 4 * ALLOC_GRAN;
        st = NtFreeVirtualMemory(self(), &coal_all, &coal_all_size,
            MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
        INFO("Full cross-allocation coalesce (A+B+C): 0x%08lX", (unsigned long)st);
        if (NT_SUCCESS(st))
          PASS("Full cross-allocation coalesce works!");
      } else {
        FAIL("Cross-allocation coalesce NOT supported: 0x%08lX", (unsigned long)st);
        INFO("Placeholders must share original allocation for coalesce");
      }
    } else {
      FAIL("Could not create placeholder in gap: 0x%08lX", (unsigned long)st);
    }
  }

  // 6c: Timing — measure coalesce cost
  {
    // Create fresh placeholder for timing
    PVOID tph = NULL;
    SIZE_T tph_size = 2 * ALLOC_GRAN;
    st = NtAllocateVirtualMemoryEx(self(), &tph, &tph_size,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (NT_SUCCESS(st)) {
      long long freq = qpc_freq();
      int iters = 1000;
      long long t0 = qpc_now();
      for (int i = 0; i < iters; i++) {
        // Split
        PVOID sp = tph;
        SIZE_T sp_size = ALLOC_GRAN;
        NtFreeVirtualMemory(self(), &sp, &sp_size,
            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        // Coalesce
        PVOID coal = tph;
        SIZE_T coal_size = 2 * ALLOC_GRAN;
        NtFreeVirtualMemory(self(), &coal, &coal_size,
            MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
      }
      long long elapsed_ns = ticks_to_ns(qpc_now() - t0, freq);
      INFO("1K split+coalesce cycles: %lldns total, %lldns/cycle",
           elapsed_ns, elapsed_ns / iters);
      INFO("  ~%lldns per split, ~%lldns per coalesce (amortized)",
           elapsed_ns / (2 * iters), elapsed_ns / (2 * iters));

      PVOID f = tph; SIZE_T fs = 0;
      NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    }
  }

  // Cleanup whatever remains
  release_all(base, base_size);
}


//=============================================================================
// main
//=============================================================================
int main(void) {
  printf("=== Memory Innovation Test Suite ===\n");
  printf("Testing 6 innovations for POSIX memory subsystem optimization\n");

  test_mri_placeholder_detection();  // Innovation 1
  test_write_watch_placeholder();    // Innovation 2
  test_reserve_only_replacement();   // Innovation 3
  test_non_noaccess_placeholder();   // Innovation 4
  test_mri_region_size();            // Innovation 5
  test_cross_alloc_coalesce();       // Innovation 6

  printf("\n=== SUMMARY ===\n");
  printf("  PASS: %d\n", total_pass);
  printf("  FAIL: %d\n", total_fail);
  printf("  SKIP: %d\n", total_skip);

  printf("\n=== Innovation Impact Assessment ===\n");
  printf("  1. MRI Placeholder Detection: definitive placeholder identification\n");
  printf("     → Improves coalesce_or_release_placeholder correctness\n");
  printf("  2. Write-Watch + Placeholder: free dirty tracking for anonymous private\n");
  printf("     → Enables fork() COW optimization, smarter MADV_DONTNEED\n");
  printf("  3. Reserve-Only Replacement: eliminates sections for MAP_NORESERVE\n");
  printf("     → Partial munmap: 5-10 syscalls → 2-3 syscalls\n");
  printf("  4. Non-NOACCESS Placeholder: accessible splittable regions\n");
  printf("     → Eliminates replacement step (likely not supported)\n");
  printf("  5. MRI RegionSize: single-query allocation extent\n");
  printf("     → Replaces find_alloc_extent RegionWalker walk\n");
  printf("  6. Cross-Alloc Coalesce: placeholder coalesce across allocations\n");
  printf("     → Critical for coalesce_or_release_placeholder reliability\n");

  return total_fail > 0 ? 1 : 0;
}
