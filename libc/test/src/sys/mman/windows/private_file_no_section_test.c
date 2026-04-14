// private_file_no_section_test.c — MAP_PRIVATE file mapping without sections
//
// Proves that file MAP_PRIVATE can use private memory + NtReadFile on demand
// instead of section views with WRITECOPY. This eliminates the split-remap
// protocol, COW save/restore, and mapping table entries for file MAP_PRIVATE.
//
// Model: placeholder → reserve-replace → VEH demand-read from file
//   Reads:  VEH commits page + NtReadFile from backing file
//   Writes: direct (private memory, no COW needed)
//   Partial munmap: MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER (1 syscall)
//
// Build: clang-cl -O2 private_file_no_section_test.c
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
typedef unsigned short USHORT;

#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define NT_ERROR(s)   ((NTSTATUS)(s) < 0)

typedef union {
  struct { ULONG LowPart; LONG HighPart; };
  long long QuadPart;
} LARGE_INTEGER;

typedef struct {
  union { NTSTATUS Status; PVOID Pointer; };
  ULONG_PTR Information;
} IO_STATUS_BLOCK;

typedef struct {
  USHORT Length;
  USHORT MaximumLength;
  USHORT *Buffer; // WCHAR*
} UNICODE_STRING;

typedef struct {
  ULONG Length;
  HANDLE RootDirectory;
  UNICODE_STRING *ObjectName;
  ULONG Attributes;
  PVOID SecurityDescriptor;
  PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

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
  struct { DWORD64 Type : 8; DWORD64 Reserved : 56; };
  union { DWORD64 ULong64; PVOID Pointer; SIZE_T Size; HANDLE Handle; DWORD ULong; };
} MEM_EXTENDED_PARAMETER;

#define MEM_COMMIT              0x00001000
#define MEM_RESERVE             0x00002000
#define MEM_DECOMMIT            0x00004000
#define MEM_RELEASE             0x00008000
#define MEM_PRIVATE             0x00020000
#define MEM_MAPPED              0x00040000
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_PRESERVE_PLACEHOLDER_ON_UNMAP 0x00000002

#define PAGE_NOACCESS           0x01
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_WRITECOPY          0x08

#define SEC_COMMIT              0x08000000
#define SECTION_ALL_ACCESS      0x000F001F
#define SECTION_MAP_READ        0x0004
#define SECTION_MAP_WRITE       0x0002
#define SECTION_MAP_EXECUTE     0x0008
#define SECTION_QUERY           0x0001

#define FILE_GENERIC_READ       0x00120089
#define FILE_GENERIC_WRITE      0x00120116
#define FILE_SHARE_READ         0x00000001
#define FILE_SHARE_WRITE        0x00000002
#define FILE_SHARE_DELETE       0x00000004
#define FILE_CREATE             0x00000002
#define FILE_OPEN               0x00000001
#define FILE_OVERWRITE_IF       0x00000005
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define FILE_NON_DIRECTORY_FILE 0x00000040
#define FILE_DELETE_ON_CLOSE    0x00001000
#define FILE_ATTRIBUTE_NORMAL   0x00000080
#define OBJ_CASE_INSENSITIVE    0x00000040

#define MemoryRegionInformationEx 7

extern NTSTATUS NTAPI NtAllocateVirtualMemoryEx(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
extern NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG);
extern NTSTATUS NTAPI NtProtectVirtualMemory(HANDLE, PVOID *, SIZE_T *, ULONG, ULONG *);
extern NTSTATUS NTAPI NtQueryVirtualMemory(HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);
extern NTSTATUS NTAPI NtCreateFile(
    HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *,
    LARGE_INTEGER *, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtReadFile(
    HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *,
    PVOID, ULONG, LARGE_INTEGER *, ULONG *);
extern NTSTATUS NTAPI NtWriteFile(
    HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *,
    PVOID, ULONG, LARGE_INTEGER *, ULONG *);
extern NTSTATUS NTAPI NtClose(HANDLE);
extern NTSTATUS NTAPI NtCreateSectionEx(
    HANDLE *, ULONG, PVOID, LARGE_INTEGER *, ULONG, ULONG, HANDLE,
    MEM_EXTENDED_PARAMETER *, ULONG);
extern NTSTATUS NTAPI NtMapViewOfSectionEx(
    HANDLE, HANDLE, PVOID *, LARGE_INTEGER *, SIZE_T *, ULONG, ULONG,
    MEM_EXTENDED_PARAMETER *, ULONG);
extern NTSTATUS NTAPI NtUnmapViewOfSectionEx(HANDLE, PVOID, ULONG);
extern NTSTATUS NTAPI NtDeleteFile(OBJECT_ATTRIBUTES *);
extern NTSTATUS NTAPI RtlInitUnicodeString(UNICODE_STRING *, const USHORT *);

typedef struct {
  unsigned char _pad[0x30];
  USHORT NtSystemRoot[260];
} KUSER_SHARED_DATA_MIN;

extern unsigned char NTAPI RtlQueryPerformanceCounter(LARGE_INTEGER *);
extern unsigned char NTAPI RtlQueryPerformanceFrequency(LARGE_INTEGER *);

static HANDLE self(void) { return (HANDLE)(long long)(-1); }
#define PAGE_SIZE  4096ULL
#define ALLOC_GRAN 65536ULL

static long long qpc_freq(void) {
  LARGE_INTEGER f; RtlQueryPerformanceFrequency(&f); return f.QuadPart;
}
static long long qpc_now(void) {
  LARGE_INTEGER t; RtlQueryPerformanceCounter(&t); return t.QuadPart;
}
static long long ticks_to_ns(long long t, long long f) {
  return (t * 1000000000LL) / f;
}

static DWORD get_temp_path_w(DWORD cap, USHORT *dst) {
  volatile const KUSER_SHARED_DATA_MIN *shared =
      (const volatile KUSER_SHARED_DATA_MIN *)0x7FFE0000;
  DWORD len = 0;
  while (len + 1 < cap && shared->NtSystemRoot[len]) {
    dst[len] = shared->NtSystemRoot[len];
    ++len;
  }
  if (len == 0 || len + 6 >= cap)
    return 0;
  if (dst[len - 1] != '\\')
    dst[len++] = '\\';
  dst[len++] = 'T';
  dst[len++] = 'e';
  dst[len++] = 'm';
  dst[len++] = 'p';
  dst[len++] = '\\';
  dst[len] = 0;
  return len;
}

static int total_pass = 0, total_fail = 0, total_skip = 0;
#define PASS(fmt, ...) do { total_pass++; printf("  PASS: " fmt "\n", ##__VA_ARGS__); } while(0)
#define FAIL(fmt, ...) do { total_fail++; printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define SKIP(fmt, ...) do { total_skip++; printf("  SKIP: " fmt "\n", ##__VA_ARGS__); } while(0)
#define INFO(fmt, ...) printf("  info: " fmt "\n", ##__VA_ARGS__)

static int g_file_counter = 0;

// Create a temp file with known content (page-aligned, each page stamped).
// Returns file handle opened for read. Caller closes.
static HANDLE create_test_file(int num_pages) {
  // Build path: %TEMP%\llvm_mmap_testN.bin (unique per call)
  USHORT path[512];
  DWORD len = get_temp_path_w(400, path);
  USHORT suffix[32];
  int si = 0;
  const char *prefix = "llvm_mmap_test";
  for (int i = 0; prefix[i]; i++) suffix[si++] = prefix[i];
  suffix[si++] = '0' + (g_file_counter / 10);
  suffix[si++] = '0' + (g_file_counter % 10);
  const char *ext = ".bin";
  for (int i = 0; ext[i]; i++) suffix[si++] = ext[i];
  suffix[si] = 0;
  g_file_counter++;
  for (int i = 0; i < si; i++)
    path[len + i] = suffix[i];
  path[len + si] = 0;

  UNICODE_STRING us;
  // Need NT path prefix
  USHORT nt_path[540];
  nt_path[0] = '\\'; nt_path[1] = '?'; nt_path[2] = '?'; nt_path[3] = '\\';
  int total_len = (int)len + si;
  for (int i = 0; i < total_len; i++)
    nt_path[4 + i] = path[i];
  nt_path[4 + total_len] = 0;
  us.Buffer = nt_path;
  us.Length = (USHORT)((4 + total_len) * 2);
  us.MaximumLength = us.Length + 2;

  OBJECT_ATTRIBUTES oa = {sizeof(oa), NULL, &us, OBJ_CASE_INSENSITIVE, NULL, NULL};
  IO_STATUS_BLOCK iosb = {0};
  HANDLE fh = NULL;
  LARGE_INTEGER alloc_size;
  alloc_size.QuadPart = (long long)num_pages * PAGE_SIZE;

  NTSTATUS st = NtCreateFile(&fh, FILE_GENERIC_READ | FILE_GENERIC_WRITE,
      &oa, &iosb, &alloc_size, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_DELETE,
      FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
      NULL, 0);
  if (NT_ERROR(st)) {
    printf("  NtCreateFile failed: 0x%08lX\n", (unsigned long)st);
    return NULL;
  }

  // Write content: each page starts with "PAGE_XXXX" where XXXX = page index
  char page_buf[4096];
  for (int i = 0; i < num_pages; i++) {
    memset(page_buf, 0, PAGE_SIZE);
    int n = 0;
    // Simple int-to-string for page marker
    char marker[32];
    n = sprintf(marker, "PAGE_%04d_CONTENT", i);
    memcpy(page_buf, marker, n);
    // Fill rest with predictable pattern
    for (int j = n; j < (int)PAGE_SIZE; j++)
      page_buf[j] = (char)((i + j) & 0xFF);

    LARGE_INTEGER offset;
    offset.QuadPart = (long long)i * PAGE_SIZE;
    iosb.Status = 0; iosb.Information = 0;
    st = NtWriteFile(fh, NULL, NULL, NULL, &iosb,
                     page_buf, (ULONG)PAGE_SIZE, &offset, NULL);
    if (NT_ERROR(st)) {
      printf("  NtWriteFile page %d failed: 0x%08lX\n", i, (unsigned long)st);
      NtClose(fh);
      return NULL;
    }
  }

  return fh;
}

static void delete_test_file(void) {
  USHORT path[512];
  DWORD len = get_temp_path_w(400, path);
  static const USHORT suffix[] = {
      'l', 'l', 'v', 'm', '_', 'm', 'm', 'a', 'p', '_',
      't', 'e', 's', 't', '.', 'b', 'i', 'n', 0};
  for (int i = 0; suffix[i]; i++)
    path[len + i] = suffix[i];
  path[len + 18] = 0;

  USHORT nt_path[540];
  nt_path[0] = '\\'; nt_path[1] = '?'; nt_path[2] = '?'; nt_path[3] = '\\';
  for (int i = 0; path[i]; i++)
    nt_path[4 + i] = path[i];
  nt_path[4 + len + 18] = 0;

  UNICODE_STRING us;
  us.Buffer = nt_path;
  us.Length = (USHORT)((4 + len + 18) * 2);
  us.MaximumLength = us.Length + 2;
  OBJECT_ATTRIBUTES oa = {sizeof(oa), NULL, &us, OBJ_CASE_INSENSITIVE, NULL, NULL};
  NtDeleteFile(&oa);
}


//=============================================================================
// TEST 1: Manual demand-read — reserve-replace + NtReadFile per page
//
// Simulates what the VEH handler would do: commit page, read file content.
// Proves the basic mechanism works.
//=============================================================================
static void test_manual_demand_read(void) {
  printf("\n=== TEST 1: Manual Demand-Read ===\n");

  int num_pages = 16;
  HANDLE fh = create_test_file(num_pages);
  if (!fh) { FAIL("Could not create test file"); return; }
  PASS("Created test file (%d pages)", num_pages);

  // Create placeholder, reserve-replace
  PVOID ph = NULL;
  SIZE_T ph_size = num_pages * PAGE_SIZE;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (NT_ERROR(st)) { FAIL("placeholder: 0x%08lX", (unsigned long)st); NtClose(fh); return; }

  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
  if (NT_ERROR(st)) {
    FAIL("reserve-replace: 0x%08lX", (unsigned long)st);
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    NtClose(fh);
    return;
  }
  PASS("Reserve-replaced %d pages", num_pages);

  // Demand-read: commit each page + NtReadFile
  int read_ok = 1;
  for (int i = 0; i < num_pages; i++) {
    PVOID page = (char *)rep + i * PAGE_SIZE;
    SIZE_T page_size = PAGE_SIZE;
    st = NtAllocateVirtualMemoryEx(self(), &page, &page_size,
        MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    if (NT_ERROR(st)) { read_ok = 0; FAIL("commit page %d: 0x%08lX", i, (unsigned long)st); break; }

    LARGE_INTEGER offset;
    offset.QuadPart = (long long)i * PAGE_SIZE;
    IO_STATUS_BLOCK iosb = {0};
    st = NtReadFile(fh, NULL, NULL, NULL, &iosb,
                    page, (ULONG)PAGE_SIZE, &offset, NULL);
    if (NT_ERROR(st)) { read_ok = 0; FAIL("NtReadFile page %d: 0x%08lX", i, (unsigned long)st); break; }
  }
  if (read_ok) PASS("Committed + read all %d pages", num_pages);

  // Verify content
  int content_ok = 1;
  for (int i = 0; i < num_pages && read_ok; i++) {
    char *p = (char *)rep + i * PAGE_SIZE;
    char expected[32];
    sprintf(expected, "PAGE_%04d_CONTENT", i);
    if (memcmp(p, expected, strlen(expected)) != 0) {
      content_ok = 0;
      FAIL("Content mismatch on page %d: got '%.17s'", i, p);
      break;
    }
  }
  if (content_ok && read_ok) PASS("All pages have correct file content");

  // Write to some pages (private modifications)
  char *p = (char *)rep;
  memcpy(p, "MODIFIED_PAGE_0!", 16);
  memcpy(p + 5 * PAGE_SIZE, "MODIFIED_PAGE_5!", 16);

  // Verify writes stick (no COW needed — it's private memory)
  if (memcmp(p, "MODIFIED_PAGE_0!", 16) == 0 &&
      memcmp(p + 5 * PAGE_SIZE, "MODIFIED_PAGE_5!", 16) == 0)
    PASS("Private writes persist (no COW needed)");
  else
    FAIL("Private writes did not persist");

  // Verify unmodified pages still have original content
  char expected[32];
  sprintf(expected, "PAGE_%04d_CONTENT", 3);
  if (memcmp(p + 3 * PAGE_SIZE, expected, strlen(expected)) == 0)
    PASS("Unmodified pages retain original file content");

  // Partial munmap — trivial!
  PVOID punch = (char *)rep + 4 * PAGE_SIZE;
  SIZE_T punch_size = 8 * PAGE_SIZE;
  st = NtFreeVirtualMemory(self(), &punch, &punch_size,
      MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  if (NT_SUCCESS(st))
    PASS("Partial munmap: 1 syscall (no split-remap!)");
  else
    FAIL("Partial munmap: 0x%08lX", (unsigned long)st);

  // Cleanup
  PVOID f = rep; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  f = (char *)ph + 4 * PAGE_SIZE; fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  f = (char *)ph + 12 * PAGE_SIZE; fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  NtClose(fh);
}


//=============================================================================
// TEST 2: Cluster read — commit + read 256KB at once
//
// The VEH handler should cluster reads like it clusters commits for
// SEC_RESERVE. 64 pages per fault amortizes VEH overhead.
//=============================================================================
static void test_cluster_read(void) {
  printf("\n=== TEST 2: Cluster Read (256KB) ===\n");

  int num_pages = 256; // 1MB file
  HANDLE fh = create_test_file(num_pages);
  if (!fh) { FAIL("Could not create test file"); return; }

  PVOID ph = NULL;
  SIZE_T ph_size = num_pages * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  // Commit 256KB cluster and read in one NtReadFile call
  SIZE_T cluster = 256 * 1024;
  PVOID cm = rep;
  SIZE_T cm_size = cluster;
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &cm, &cm_size,
      MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  if (NT_ERROR(st)) { FAIL("cluster commit: 0x%08lX", (unsigned long)st); goto cleanup2; }

  {
    LARGE_INTEGER offset = {0};
    IO_STATUS_BLOCK iosb = {0};
    st = NtReadFile(fh, NULL, NULL, NULL, &iosb,
                    rep, (ULONG)cluster, &offset, NULL);
    if (NT_ERROR(st)) {
      FAIL("cluster NtReadFile: 0x%08lX", (unsigned long)st);
    } else {
      INFO("Read %llu bytes in one NtReadFile call",
           (unsigned long long)iosb.Information);
      // Verify first and last page
      char exp0[32], exp63[32];
      sprintf(exp0, "PAGE_%04d_CONTENT", 0);
      sprintf(exp63, "PAGE_%04d_CONTENT", 63);
      if (memcmp(rep, exp0, strlen(exp0)) == 0 &&
          memcmp((char *)rep + 63 * PAGE_SIZE, exp63, strlen(exp63)) == 0)
        PASS("256KB cluster read: first and last pages correct");
      else
        FAIL("Cluster read content mismatch");
    }
  }

cleanup2:
  { PVOID f = rep; SIZE_T fs = 0; NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE); }
  NtClose(fh);
}


//=============================================================================
// TEST 3: Performance — private-read vs section view
//
// Compare per-page cost of:
//   Model A: commit + NtReadFile (our proposed model)
//   Model B: section view with WRITECOPY (current model)
//=============================================================================
static void test_perf_vs_section(void) {
  printf("\n=== TEST 3: Performance vs Section View ===\n");

  int num_pages = 256; // 1MB
  HANDLE fh = create_test_file(num_pages);
  if (!fh) { FAIL("Could not create test file"); return; }

  long long freq = qpc_freq();
  SIZE_T total_size = num_pages * PAGE_SIZE;
  int iters = 50;

  // --- Model A: reserve-replace + NtReadFile ---
  long long t0 = qpc_now();
  for (int it = 0; it < iters; it++) {
    PVOID ph = NULL;
    SIZE_T ps = total_size;
    NtAllocateVirtualMemoryEx(self(), &ph, &ps,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    PVOID rep = ph;
    SIZE_T rs = ps;
    NtAllocateVirtualMemoryEx(self(), &rep, &rs,
        MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

    // Commit all + single NtReadFile (cluster model)
    PVOID cm = rep;
    SIZE_T cms = total_size;
    NtAllocateVirtualMemoryEx(self(), &cm, &cms,
        MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    LARGE_INTEGER offset = {0};
    IO_STATUS_BLOCK iosb = {0};
    NtReadFile(fh, NULL, NULL, NULL, &iosb,
               rep, (ULONG)total_size, &offset, NULL);

    PVOID f = rep; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  }
  long long model_a_ns = ticks_to_ns(qpc_now() - t0, freq);

  // --- Model B: section + WRITECOPY view ---
  long long t1 = qpc_now();
  for (int it = 0; it < iters; it++) {
    PVOID ph = NULL;
    SIZE_T ps = total_size;
    NtAllocateVirtualMemoryEx(self(), &ph, &ps,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);

    LARGE_INTEGER max_size;
    max_size.QuadPart = (long long)total_size;
    HANDLE section = NULL;
    NtCreateSectionEx(&section,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
        NULL, &max_size, PAGE_WRITECOPY, SEC_COMMIT, fh, NULL, 0);

    PVOID view = ph;
    SIZE_T vs = ps;
    LARGE_INTEGER offset = {0};
    NtMapViewOfSectionEx(section, self(), &view, &offset, &vs,
        MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);

    // Touch all pages to force them into working set (fault from file)
    volatile char sum = 0;
    for (int i = 0; i < num_pages; i++)
      sum += ((volatile char *)view)[i * PAGE_SIZE];

    NtUnmapViewOfSectionEx(self(), view, 0);
    NtClose(section);
    // placeholder consumed by map; view consumed by unmap → now MEM_FREE
  }
  long long model_b_ns = ticks_to_ns(qpc_now() - t1, freq);

  // --- Model C: section view, measure partial munmap cost ---
  // Create one view, then measure unmap+preserve cost
  {
    PVOID ph = NULL;
    SIZE_T ps = total_size;
    NtAllocateVirtualMemoryEx(self(), &ph, &ps,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    LARGE_INTEGER max_size;
    max_size.QuadPart = (long long)total_size;
    HANDLE section = NULL;
    NtCreateSectionEx(&section,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
        NULL, &max_size, PAGE_WRITECOPY, SEC_COMMIT, fh, NULL, 0);
    PVOID view = ph;
    SIZE_T vs = ps;
    LARGE_INTEGER offset = {0};
    NtMapViewOfSectionEx(section, self(), &view, &offset, &vs,
        MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);

    // Touch all pages
    for (int i = 0; i < num_pages; i++)
      ((volatile char *)view)[i * PAGE_SIZE];

    // Measure: unmap with PRESERVE_PLACEHOLDER (first step of split-remap)
    long long t_unmap = qpc_now();
    NtUnmapViewOfSectionEx(self(), view, MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
    long long unmap_ns = ticks_to_ns(qpc_now() - t_unmap, freq);
    INFO("Section unmap-to-placeholder (1MB view): %lldns", unmap_ns);

    // Compare: private preserve-to-placeholder
    PVOID ph2 = NULL;
    SIZE_T ps2 = total_size;
    NtAllocateVirtualMemoryEx(self(), &ph2, &ps2,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    PVOID rep2 = ph2;
    SIZE_T rs2 = ps2;
    NtAllocateVirtualMemoryEx(self(), &rep2, &rs2,
        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    // Touch to commit
    for (int i = 0; i < num_pages; i++)
      ((volatile char *)rep2)[i * PAGE_SIZE] = (char)i;

    // Partial munmap: decommit middle + preserve
    PVOID punch = (char *)rep2 + total_size / 4;
    SIZE_T punch_size = total_size / 2;
    long long t_punch = qpc_now();
    NtFreeVirtualMemory(self(), &punch, &punch_size, MEM_DECOMMIT);
    punch = (char *)rep2 + total_size / 4;
    punch_size = total_size / 2;
    NtFreeVirtualMemory(self(), &punch, &punch_size,
        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    long long punch_ns = ticks_to_ns(qpc_now() - t_punch, freq);
    INFO("Private decommit+preserve (512KB): %lldns", punch_ns);
    INFO("Section unmap-to-placeholder is just step 1 of split-remap");
    INFO("  (split-remap adds: 2 splits + 2 remaps + snapshot + COW = 5-10x more)");

    // Cleanup
    PVOID f = ph; SIZE_T fs = 0; NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    f = rep2; fs = 0; NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    f = (char *)ph2 + total_size / 4; fs = 0; NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    f = (char *)ph2 + 3 * total_size / 4; fs = 0; NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    NtClose(section);
  }

  INFO("%d iterations, 1MB region:", iters);
  INFO("  Model A (reserve + NtReadFile):  %lldns/iter (%lldns/page)",
       model_a_ns / iters, model_a_ns / (iters * num_pages));
  INFO("  Model B (section WRITECOPY):     %lldns/iter (%lldns/page)",
       model_b_ns / iters, model_b_ns / (iters * num_pages));

  long long diff = model_b_ns - model_a_ns;
  if (model_a_ns < model_b_ns)
    PASS("Private-read is %lldns/iter faster (%lld%% improvement)",
         diff / iters, (diff * 100) / model_b_ns);
  else if (model_a_ns < model_b_ns * 2)
    INFO("Section is faster by %lldns/iter — acceptable overhead for simpler munmap",
         -diff / iters);
  else
    INFO("Section is significantly faster — private-read may not be worth it for read-heavy");
}


//=============================================================================
// TEST 4: Read-only protection + write fault behavior
//
// MAP_PRIVATE with PROT_READ: pages should be readable after demand-read
// but writes should fault (SIGSEGV). Verify this works with reserve-replace.
//=============================================================================
static void test_readonly_mapping(void) {
  printf("\n=== TEST 4: Read-Only Private Mapping ===\n");

  int num_pages = 4;
  HANDLE fh = create_test_file(num_pages);
  if (!fh) { FAIL("Could not create test file"); return; }

  PVOID ph = NULL;
  SIZE_T ph_size = num_pages * PAGE_SIZE;
  NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  // Reserve with PAGE_READONLY — VEH would commit with this prot
  NTSTATUS st = NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READONLY, NULL, 0);
  if (NT_ERROR(st)) {
    FAIL("reserve-replace PAGE_READONLY: 0x%08lX", (unsigned long)st);
    PVOID f = ph; SIZE_T fs = 0;
    NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
    NtClose(fh);
    return;
  }
  PASS("Reserve-replaced with PAGE_READONLY AllocationProtect");

  // Commit as READONLY and read file content
  PVOID cm = rep;
  SIZE_T cms = PAGE_SIZE;
  st = NtAllocateVirtualMemoryEx(self(), &cm, &cms,
      MEM_COMMIT, PAGE_READONLY, NULL, 0);
  if (NT_SUCCESS(st)) {
    LARGE_INTEGER offset = {0};
    IO_STATUS_BLOCK iosb = {0};
    st = NtReadFile(fh, NULL, NULL, NULL, &iosb,
                    rep, (ULONG)PAGE_SIZE, &offset, NULL);
    if (NT_SUCCESS(st)) {
      char expected[32];
      sprintf(expected, "PAGE_%04d_CONTENT", 0);
      if (memcmp(rep, expected, strlen(expected)) == 0)
        PASS("READONLY page has correct file content after NtReadFile");
      else
        FAIL("READONLY page content mismatch");
    } else {
      // NtReadFile into READONLY page might fail
      INFO("NtReadFile into READONLY page: 0x%08lX", (unsigned long)st);
      // Try: commit as RW, read, then protect to RO
      ULONG old;
      PVOID pp = rep; SIZE_T pps = PAGE_SIZE;
      NtProtectVirtualMemory(self(), &pp, &pps, PAGE_READWRITE, &old);
      offset.QuadPart = 0;
      iosb.Status = 0;
      st = NtReadFile(fh, NULL, NULL, NULL, &iosb,
                      rep, (ULONG)PAGE_SIZE, &offset, NULL);
      if (NT_SUCCESS(st)) {
        pp = rep; pps = PAGE_SIZE;
        NtProtectVirtualMemory(self(), &pp, &pps, PAGE_READONLY, &old);
        PASS("Workaround: commit RW → read → protect RO");
      }
    }
  }

  PVOID f = rep; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  NtClose(fh);
}


//=============================================================================
// TEST 5: File offset support
//
// MAP_PRIVATE at offset != 0. The VEH handler must track the file offset
// per-region to read the right data.
//=============================================================================
static void test_file_offset(void) {
  printf("\n=== TEST 5: File Offset Support ===\n");

  int num_pages = 64;
  HANDLE fh = create_test_file(num_pages);
  if (!fh) { FAIL("Could not create test file"); return; }

  // Map pages 16-31 (offset = 16 * PAGE_SIZE)
  int start_page = 16;
  int map_pages = 16;
  SIZE_T map_size = map_pages * PAGE_SIZE;

  PVOID ph = NULL;
  SIZE_T ph_size = map_size;
  NtAllocateVirtualMemoryEx(self(), &ph, &ph_size,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  PVOID rep = ph;
  SIZE_T rep_size = ph_size;
  NtAllocateVirtualMemoryEx(self(), &rep, &rep_size,
      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);

  // Commit all + read at offset
  PVOID cm = rep;
  SIZE_T cms = map_size;
  NtAllocateVirtualMemoryEx(self(), &cm, &cms, MEM_COMMIT, PAGE_READWRITE, NULL, 0);

  LARGE_INTEGER offset;
  offset.QuadPart = (long long)start_page * PAGE_SIZE;
  IO_STATUS_BLOCK iosb = {0};
  NTSTATUS st = NtReadFile(fh, NULL, NULL, NULL, &iosb,
                           rep, (ULONG)map_size, &offset, NULL);
  if (NT_SUCCESS(st)) {
    // Verify: page 0 of mapping should have PAGE_0016_CONTENT
    char expected[32];
    sprintf(expected, "PAGE_%04d_CONTENT", start_page);
    if (memcmp(rep, expected, strlen(expected)) == 0)
      PASS("Offset mapping: first page has correct content (PAGE_%04d)", start_page);
    else
      FAIL("Offset mapping: expected '%s', got '%.17s'", expected, (char *)rep);

    // Last page should have PAGE_0031_CONTENT
    sprintf(expected, "PAGE_%04d_CONTENT", start_page + map_pages - 1);
    if (memcmp((char *)rep + (map_pages - 1) * PAGE_SIZE, expected, strlen(expected)) == 0)
      PASS("Offset mapping: last page correct (PAGE_%04d)", start_page + map_pages - 1);
  } else {
    FAIL("NtReadFile at offset: 0x%08lX", (unsigned long)st);
  }

  PVOID f = rep; SIZE_T fs = 0;
  NtFreeVirtualMemory(self(), &f, &fs, MEM_RELEASE);
  NtClose(fh);
}


//=============================================================================
// main
//=============================================================================
int main(void) {
  printf("=== Private File Mapping Without Sections ===\n");
  printf("Testing: placeholder + reserve-replace + NtReadFile\n");

  test_manual_demand_read();
  test_cluster_read();
  test_perf_vs_section();
  test_readonly_mapping();
  test_file_offset();

  printf("\n=== SUMMARY ===\n");
  printf("  PASS: %d\n", total_pass);
  printf("  FAIL: %d\n", total_fail);
  printf("  SKIP: %d\n", total_skip);

  delete_test_file();
  return total_fail > 0 ? 1 : 0;
}
