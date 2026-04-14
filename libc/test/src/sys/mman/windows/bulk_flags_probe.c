// bulk_flags_probe.c — Probe NtPssCaptureVaSpaceBulk QueryFlags
//
// Test whether undocumented flag values return MEMORY_REGION_INFORMATION
// (with PlaceholderReservation bit) instead of plain MBI entries.
//
// Build: clang-cl -O2 bulk_flags_probe.c /link ntdll.lib kernel32.lib
//        /subsystem:console /out:bulk_flags_probe.exe

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

#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

#define MEM_RESERVE             0x00002000
#define MEM_RELEASE             0x00008000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define PAGE_NOACCESS           0x01

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

// MRI — same size as MBI (48 bytes) but different layout
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
  ULONG QueryFlags;
  ULONG NumberOfEntries;
  PVOID NextValidAddress;
} NTPSS_MEMORY_BULK_INFORMATION;

extern NTSTATUS NTAPI NtPssCaptureVaSpaceBulk(
    HANDLE ProcessHandle, PVOID BaseAddress,
    NTPSS_MEMORY_BULK_INFORMATION *BulkInformation,
    SIZE_T BulkInformationLength, SIZE_T *ReturnLength);

extern NTSTATUS NTAPI NtAllocateVirtualMemoryEx(
    HANDLE ProcessHandle, PVOID *BaseAddress, SIZE_T *RegionSize,
    ULONG AllocationType, ULONG PageProtection,
    void *ExtendedParameters, ULONG ExtendedParameterCount);

extern NTSTATUS NTAPI NtFreeVirtualMemory(
    HANDLE ProcessHandle, PVOID *BaseAddress, SIZE_T *RegionSize,
    ULONG FreeType);

extern NTSTATUS NTAPI NtQueryVirtualMemory(
    HANDLE ProcessHandle, PVOID BaseAddress, ULONG MemoryInformationClass,
    PVOID MemoryInformation, SIZE_T MemoryInformationLength,
    SIZE_T *ReturnLength);

static HANDLE self(void) { return (HANDLE)(long long)(-1); }

#define PAGE_SIZE  4096ULL
#define ALLOC_GRAN 65536ULL

int main(void) {
  printf("=== NtPssCaptureVaSpaceBulk QueryFlags Probe ===\n\n");

  // Create a placeholder so we can test for PlaceholderReservation detection.
  PVOID ph = NULL;
  SIZE_T ph_size = ALLOC_GRAN;
  NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  printf("Placeholder at %p (64KB)\n\n", ph);

  // Get ground truth via MRI query.
  MEMORY_REGION_INFORMATION mri_truth;
  NtQueryVirtualMemory(self(), ph, 7 /* MemoryRegionInformationEx */,
                       &mri_truth, sizeof(mri_truth), NULL);
  printf("MRI ground truth: PlaceholderReservation=%u RegionType=0x%08lx\n",
         mri_truth.PlaceholderReservation, (unsigned long)mri_truth.RegionType);
  printf("  AllocationBase=%p RegionSize=%llu CommitSize=%llu\n\n",
         mri_truth.AllocationBase, (unsigned long long)mri_truth.RegionSize,
         (unsigned long long)mri_truth.CommitSize);

  // Probe flags 0x0 through 0x1F.
  __declspec(align(16)) char buf[sizeof(NTPSS_MEMORY_BULK_INFORMATION) +
                        10 * sizeof(MEMORY_BASIC_INFORMATION)];

  for (ULONG flag = 0; flag <= 0x1F; flag++) {
    NTPSS_MEMORY_BULK_INFORMATION *hdr =
        (NTPSS_MEMORY_BULK_INFORMATION *)buf;
    hdr->QueryFlags = flag;
    hdr->NumberOfEntries = 0;
    hdr->NextValidAddress = NULL;

    SIZE_T ret_len = 0;
    NTSTATUS st = NtPssCaptureVaSpaceBulk(self(), ph, hdr, sizeof(buf),
                                           &ret_len);

    printf("Flag 0x%02lx: status=0x%08lx entries=%lu ret_len=%llu",
           (unsigned long)flag, (unsigned long)st,
           (unsigned long)hdr->NumberOfEntries,
           (unsigned long long)ret_len);

    if (NT_SUCCESS(st) && hdr->NumberOfEntries > 0) {
      // Dump first entry as raw bytes.
      unsigned char *raw = (unsigned char *)(hdr + 1);
      SIZE_T entry_size = (ret_len - sizeof(*hdr)) / hdr->NumberOfEntries;
      printf(" entry_size=%llu", (unsigned long long)entry_size);

      // Interpret as MBI.
      MEMORY_BASIC_INFORMATION *mbi = (MEMORY_BASIC_INFORMATION *)(hdr + 1);
      printf("\n  MBI: Base=%p AllocBase=%p State=0x%lx Type=0x%lx",
             mbi->BaseAddress, mbi->AllocationBase,
             (unsigned long)mbi->State, (unsigned long)mbi->Type);

      // Also interpret same bytes as MRI to see if PlaceholderReservation shows.
      MEMORY_REGION_INFORMATION *mri = (MEMORY_REGION_INFORMATION *)(hdr + 1);
      printf("\n  MRI: AllocBase=%p RegionType=0x%08lx PlaceholderRes=%u",
             mri->AllocationBase, (unsigned long)mri->RegionType,
             mri->PlaceholderReservation);
      printf(" RegionSize=%llu CommitSize=%llu",
             (unsigned long long)mri->RegionSize,
             (unsigned long long)mri->CommitSize);

      // Check: does interpreting as MRI match ground truth?
      if (mri->PlaceholderReservation == mri_truth.PlaceholderReservation &&
          mri->AllocationBase == mri_truth.AllocationBase &&
          mri->RegionSize == mri_truth.RegionSize)
        printf("\n  *** MRI MATCH! Flag 0x%lx returns MRI entries! ***",
               (unsigned long)flag);

      // Raw hex of first 48 bytes
      printf("\n  raw:");
      for (int i = 0; i < 48 && i < (int)entry_size; i++)
        printf(" %02x", raw[i]);
    }
    printf("\n");
  }

  // Also try some power-of-2 flags beyond 0x1F.
  ULONG big_flags[] = {0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000};
  for (int f = 0; f < 8; f++) {
    NTPSS_MEMORY_BULK_INFORMATION *hdr =
        (NTPSS_MEMORY_BULK_INFORMATION *)buf;
    hdr->QueryFlags = big_flags[f];
    hdr->NumberOfEntries = 0;
    hdr->NextValidAddress = NULL;

    SIZE_T ret_len = 0;
    NTSTATUS st = NtPssCaptureVaSpaceBulk(self(), ph, hdr, sizeof(buf),
                                           &ret_len);
    printf("Flag 0x%04lx: status=0x%08lx entries=%lu ret_len=%llu\n",
           (unsigned long)big_flags[f], (unsigned long)st,
           (unsigned long)hdr->NumberOfEntries,
           (unsigned long long)ret_len);
  }

  // Cleanup
  PVOID f = ph; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);

  return 0;
}
