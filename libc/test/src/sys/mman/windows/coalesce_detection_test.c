// coalesce_detection_test.c — Can we DETECT placeholder coalesce eligibility?
//
// Split fragments have different AllocationBase but CAN coalesce.
// Independent adjacent placeholders have different AllocationBase and CANNOT.
// What observable property distinguishes them?
//
// Probes: MBI fields, MRI fields, WSEX fields, section info.
// Goal: find a query that reveals "these are siblings from the same split."

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

// Shared commit info
typedef struct {
  SIZE_T CommitSize;
} MEMORY_SHARED_COMMIT_INFORMATION;

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

#define MEM_COMMIT              0x00001000
#define MEM_RESERVE             0x00002000
#define MEM_FREE                0x00010000
#define MEM_PRIVATE             0x00020000
#define MEM_RELEASE             0x00008000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001

#define PAGE_NOACCESS   0x01

#define MemoryBasicInformation        0
#define MemoryWorkingSetExInformation 4
#define MemorySharedCommitInformation 5
#define MemoryRegionInformationEx     7

#define PAGE_SIZE  4096ULL
#define ALLOC_GRAN 65536ULL

static HANDLE self(void) { return (HANDLE)(long long)(-1); }

extern NTSTATUS NTAPI NtAllocateVirtualMemoryEx(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtFreeVirtualMemory(
    HANDLE, PVOID *, SIZE_T *, ULONG);
extern NTSTATUS NTAPI NtQueryVirtualMemory(
    HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);

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

static void dump_mbi(const char *label, void *addr) {
  MEMORY_BASIC_INFORMATION mbi;
  if (NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryBasicInformation,
                                       &mbi, sizeof(mbi), NULL))) {
    printf("  %s MBI: Base=%p AllocBase=%p AllocProt=0x%lx State=0x%lx "
           "Prot=0x%lx Type=0x%lx Size=%llu PartId=%u\n",
           label, mbi.BaseAddress, mbi.AllocationBase,
           (unsigned long)mbi.AllocationProtect,
           (unsigned long)mbi.State, (unsigned long)mbi.Protect,
           (unsigned long)mbi.Type,
           (unsigned long long)mbi.RegionSize, mbi.PartitionId);
  }
}

static void dump_mri(const char *label, void *addr) {
  MEMORY_REGION_INFORMATION mri = {};
  if (NT_SUCCESS(NtQueryVirtualMemory(self(), addr, MemoryRegionInformationEx,
                                       &mri, sizeof(mri), NULL))) {
    printf("  %s MRI: AllocBase=%p AllocProt=0x%lx RegionType=0x%lx "
           "Size=%llu Commit=%llu PartId=%llu Node=%llu PH=%u\n",
           label, mri.AllocationBase,
           (unsigned long)mri.AllocationProtect,
           (unsigned long)mri.RegionType,
           (unsigned long long)mri.RegionSize,
           (unsigned long long)mri.CommitSize,
           (unsigned long long)mri.PartitionId,
           (unsigned long long)mri.NodePreference,
           mri.PlaceholderReservation);
  } else {
    printf("  %s MRI: query failed\n", label);
  }
}

static void dump_wsex(const char *label, void *addr) {
  MEMORY_WORKING_SET_EX_INFORMATION info;
  info.VirtualAddress = addr;
  info.VirtualAttributes.Flags = 0;
  NTSTATUS st = NtQueryVirtualMemory(self(), NULL, MemoryWorkingSetExInformation,
                                      &info, sizeof(info), NULL);
  if (NT_SUCCESS(st)) {
    printf("  %s WSEX: Flags=0x%016llx Valid=%llu Shared=%llu SharedOrig=%llu "
           "Location=%llu ModList=%llu\n",
           label, (unsigned long long)info.VirtualAttributes.Flags,
           (unsigned long long)(info.VirtualAttributes.Flags & 1),
           (unsigned long long)info.VirtualAttributes.Shared,
           (unsigned long long)info.VirtualAttributes.SharedOriginal,
           (unsigned long long)info.VirtualAttributes.Invalid.Location,
           (unsigned long long)info.VirtualAttributes.Invalid.ModifiedList);
  } else {
    printf("  %s WSEX: query returned 0x%08lX\n", label, (unsigned long)st);
  }
}

static void dump_shared_commit(const char *label, void *addr) {
  MEMORY_SHARED_COMMIT_INFORMATION sci = {};
  NTSTATUS st = NtQueryVirtualMemory(self(), addr, MemorySharedCommitInformation,
                                      &sci, sizeof(sci), NULL);
  if (NT_SUCCESS(st))
    printf("  %s SharedCommit: %llu\n", label, (unsigned long long)sci.CommitSize);
  else
    printf("  %s SharedCommit: 0x%08lX\n", label, (unsigned long)st);
}

//=============================================================================
// Compare: split siblings vs independent adjacent placeholders
//=============================================================================
int main(void) {
  printf("=== Coalesce Detection: Split Siblings vs Independent ===\n");

  //--- Create split siblings ---
  printf("\n--- SPLIT SIBLINGS (coalesceable) ---\n");
  void *parent = create_ph(NULL, 4 * ALLOC_GRAN);
  char *sbase = (char *)parent;
  printf("  Parent placeholder: %p (256KB)\n", parent);

  // Split into 4
  split_ph(sbase, ALLOC_GRAN);
  split_ph(sbase + ALLOC_GRAN, ALLOC_GRAN);
  split_ph(sbase + 2 * ALLOC_GRAN, ALLOC_GRAN);

  printf("\n  Sibling A (offset 0):\n");
  dump_mbi("A", sbase);
  dump_mri("A", sbase);
  dump_wsex("A", sbase);
  dump_shared_commit("A", sbase);

  printf("\n  Sibling B (offset 64K):\n");
  dump_mbi("B", sbase + ALLOC_GRAN);
  dump_mri("B", sbase + ALLOC_GRAN);
  dump_wsex("B", sbase + ALLOC_GRAN);
  dump_shared_commit("B", sbase + ALLOC_GRAN);

  printf("\n  Sibling C (offset 128K):\n");
  dump_mbi("C", sbase + 2 * ALLOC_GRAN);
  dump_mri("C", sbase + 2 * ALLOC_GRAN);

  printf("\n  Sibling D (offset 192K):\n");
  dump_mbi("D", sbase + 3 * ALLOC_GRAN);
  dump_mri("D", sbase + 3 * ALLOC_GRAN);

  // Verify coalesce works
  NTSTATUS st = coalesce(sbase, 4 * ALLOC_GRAN);
  printf("\n  Coalesce A+B+C+D: %s (0x%08lX)\n",
         NT_SUCCESS(st) ? "OK" : "FAIL", (unsigned long)st);
  release_ph(sbase);

  //--- Create independent adjacent placeholders ---
  printf("\n--- INDEPENDENT ADJACENT (NOT coalesceable) ---\n");

  // Allocate 4 × 64KB independently. They may not be adjacent unless
  // we force it. Let's allocate a big chunk, release it, then allocate
  // 4 pieces at the exact addresses.
  void *block = create_ph(NULL, 4 * ALLOC_GRAN);
  char *ibase = (char *)block;
  printf("  Reserved block at %p, releasing to get exact addresses\n", block);
  release_ph(block);

  // Now allocate 4 independent placeholders at the same addresses
  void *i0 = create_ph(ibase, ALLOC_GRAN);
  void *i1 = create_ph(ibase + ALLOC_GRAN, ALLOC_GRAN);
  void *i2 = create_ph(ibase + 2 * ALLOC_GRAN, ALLOC_GRAN);
  void *i3 = create_ph(ibase + 3 * ALLOC_GRAN, ALLOC_GRAN);

  if (!i0 || !i1 || !i2 || !i3) {
    printf("  Could not allocate all 4 at exact addresses — skipping\n");
    if (i0) release_ph(i0);
    if (i1) release_ph(i1);
    if (i2) release_ph(i2);
    if (i3) release_ph(i3);
    return 0;
  }

  printf("  4 independent placeholders at %p..%p\n", ibase, ibase + 4 * ALLOC_GRAN);

  printf("\n  Independent I0 (offset 0):\n");
  dump_mbi("I0", ibase);
  dump_mri("I0", ibase);
  dump_wsex("I0", ibase);
  dump_shared_commit("I0", ibase);

  printf("\n  Independent I1 (offset 64K):\n");
  dump_mbi("I1", ibase + ALLOC_GRAN);
  dump_mri("I1", ibase + ALLOC_GRAN);
  dump_wsex("I1", ibase + ALLOC_GRAN);
  dump_shared_commit("I1", ibase + ALLOC_GRAN);

  printf("\n  Independent I2 (offset 128K):\n");
  dump_mbi("I2", ibase + 2 * ALLOC_GRAN);
  dump_mri("I2", ibase + 2 * ALLOC_GRAN);

  printf("\n  Independent I3 (offset 192K):\n");
  dump_mbi("I3", ibase + 3 * ALLOC_GRAN);
  dump_mri("I3", ibase + 3 * ALLOC_GRAN);

  st = coalesce(ibase, 4 * ALLOC_GRAN);
  printf("\n  Coalesce I0+I1+I2+I3: %s (0x%08lX)\n",
         NT_SUCCESS(st) ? "OK (unexpected!)" : "FAIL (expected)", (unsigned long)st);

  //--- Side-by-side comparison ---
  printf("\n=== DIFF: Split Siblings vs Independent ===\n");
  printf("  (Look for ANY field that differs between the two groups)\n");

  // Re-create siblings for comparison
  parent = create_ph(NULL, 2 * ALLOC_GRAN);
  if (parent) {
    char *sb = (char *)parent;
    split_ph(sb, ALLOC_GRAN);

    printf("\n  SIBLING pair:\n");
    dump_mbi("sib0", sb);
    dump_mbi("sib1", sb + ALLOC_GRAN);
    dump_mri("sib0", sb);
    dump_mri("sib1", sb + ALLOC_GRAN);

    release_ph(sb);
    release_ph(sb + ALLOC_GRAN);
  }

  printf("\n  INDEPENDENT pair:\n");
  dump_mbi("ind0", ibase);
  dump_mbi("ind1", ibase + ALLOC_GRAN);
  dump_mri("ind0", ibase);
  dump_mri("ind1", ibase + ALLOC_GRAN);

  // Cleanup
  release_ph(ibase);
  release_ph(ibase + ALLOC_GRAN);
  release_ph(ibase + 2 * ALLOC_GRAN);
  release_ph(ibase + 3 * ALLOC_GRAN);

  printf("\n=== Done ===\n");
  return 0;
}
