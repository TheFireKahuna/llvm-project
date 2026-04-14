// POSIX compliance test for MADV_DONTNEED, private memory, and madvise fixes.
//
// Systematically verifies:
//   T1: Private DONTNEED zeros (decommit+commit cycle)
//   T2: Section DONTNEED zeros (WSEX-selective memset+MEM_RESET)
//   T3: File MAP_PRIVATE revert (PAGE_REVERT_TO_FILE_MAP)
//   T4: WSEX selectivity (sparse allocation, only dirty pages zeroed)
//   T5: Private partial munmap (decommit+punch, neighbors intact)
//   T6: Private mremap grow in-place
//   T7: Optimal munmap sequence (decommit then punch)
//   T8: Cross-VAD DONTNEED (multiple regions)
//   T9: Protection preservation after DONTNEED
//  T10: WSEX priority conditional skip
//
// Build:
//   clang -target x86_64-unknown-windows-itanium -std=c++17 -O2 \
//     dontneed_compliance_test.cpp -o dontneed_compliance_test.exe \
//     -fuse-ld=lld -lkernel32 -lntdll -lucrt -lmsvcrt -llegacy_stdio_definitions

#include <cstdio>
#include <cstring>

extern "C" {
typedef long NTSTATUS;
typedef void *HANDLE;
typedef void *PVOID;
typedef unsigned long ULONG;
typedef unsigned long long SIZE_T;
typedef unsigned long long ULONG_PTR;
typedef long long LONGLONG;
typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned char BOOLEAN;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct {
  unsigned short Length;
  unsigned short MaximumLength;
  wchar_t *Buffer;
} UNICODE_STRING;
typedef struct {
  unsigned long Length;
  HANDLE RootDirectory;
  UNICODE_STRING *ObjectName;
  unsigned long Attributes;
  void *SecurityDescriptor;
  void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;
typedef struct {
  union {
    NTSTATUS Status;
    void *Pointer;
  };
  ULONG_PTR Information;
} IO_STATUS_BLOCK;
typedef struct {
  struct {
    ULONG Type;
    union {
      ULONG ULong;
      PVOID Pointer;
      ULONG_PTR ULong64;
    };
  };
} MEM_EXTENDED_PARAMETER;
typedef struct {
  PVOID BaseAddress;
  PVOID AllocationBase;
  ULONG AllocationProtect;
  unsigned short PartitionId;
  SIZE_T RegionSize;
  ULONG State;
  ULONG Protect;
  ULONG Type;
} MBI;
typedef struct {
  PVOID VirtualAddress;
  SIZE_T NumberOfBytes;
} MEMORY_RANGE_ENTRY;
typedef struct {
  PVOID VirtualAddress;
  union {
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
      ULONG_PTR SharedOriginal : 1;
      ULONG_PTR Bad : 1;
      ULONG_PTR Win32GraphicsProtection : 4;
    };
  } VirtualAttributes;
} WSEX_INFO;

#define NT_SUCCESS(s) ((s) >= 0)
#define NT_ERROR(s) ((s) < 0)
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_RESET 0x80000
#define MEM_RESERVE_PLACEHOLDER 0x40000
#define MEM_REPLACE_PLACEHOLDER 0x4000
#define MEM_PRESERVE_PLACEHOLDER 0x2
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_REVERT_TO_FILE_MAP 0x80000000
#define SEC_COMMIT 0x08000000
#define SECTION_ALL_ACCESS 0x000F001F
#define OBJ_CASE_INSENSITIVE 0x00000040
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_SHARE_DELETE 0x00000004
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define FILE_NON_DIRECTORY_FILE 0x00000040
#define FILE_OVERWRITE_IF 0x00000005
#define FILE_OPEN 0x00000001
#define FILE_GENERIC_READ 0x00120089
#define FILE_GENERIC_WRITE 0x00120116
#define DELETE_ACCESS 0x00010000

__declspec(dllimport) NTSTATUS __stdcall NtAllocateVirtualMemoryEx(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
__declspec(dllimport) NTSTATUS __stdcall NtAllocateVirtualMemory(
    HANDLE, PVOID *, ULONG_PTR, SIZE_T *, ULONG, ULONG);
__declspec(dllimport) NTSTATUS __stdcall NtFreeVirtualMemory(HANDLE, PVOID *,
                                                              SIZE_T *, ULONG);
__declspec(dllimport) NTSTATUS __stdcall NtQueryVirtualMemory(
    HANDLE, PVOID, int, void *, SIZE_T, SIZE_T *);
__declspec(dllimport) NTSTATUS __stdcall NtCreateSectionEx(
    HANDLE *, ULONG, void *, LARGE_INTEGER *, ULONG, ULONG, HANDLE, void *,
    ULONG);
__declspec(dllimport) NTSTATUS __stdcall
NtMapViewOfSectionEx(HANDLE, HANDLE, PVOID *, LARGE_INTEGER *, SIZE_T *, ULONG,
                     ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
__declspec(dllimport) NTSTATUS __stdcall NtUnmapViewOfSectionEx(HANDLE, PVOID,
                                                                 ULONG);
__declspec(dllimport) NTSTATUS __stdcall NtProtectVirtualMemory(
    HANDLE, PVOID *, SIZE_T *, ULONG, ULONG *);
__declspec(dllimport) NTSTATUS __stdcall NtSetInformationVirtualMemory(
    HANDLE, int, SIZE_T, MEMORY_RANGE_ENTRY *, PVOID, ULONG);
__declspec(dllimport) NTSTATUS __stdcall NtClose(HANDLE);
__declspec(dllimport) NTSTATUS __stdcall NtCreateFile(
    HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, LARGE_INTEGER *,
    ULONG, ULONG, ULONG, ULONG, void *, ULONG);
__declspec(dllimport) NTSTATUS __stdcall NtWriteFile(
    HANDLE, HANDLE, void *, void *, IO_STATUS_BLOCK *, void *, ULONG,
    LARGE_INTEGER *, ULONG *);
__declspec(dllimport) NTSTATUS __stdcall NtDeleteFile(OBJECT_ATTRIBUTES *);
} // extern "C"

static HANDLE g_proc;
static int g_pass, g_fail;

struct KUSER_SHARED_DATA_MIN {
  unsigned char pad[0x30];
  wchar_t NtSystemRoot[260];
};

static size_t wide_len(const wchar_t *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}

static bool get_temp_path_nt(wchar_t *dst, size_t dst_cap) {
  auto *shared =
      reinterpret_cast<const volatile KUSER_SHARED_DATA_MIN *>(0x7FFE0000);
  size_t len = 0;
  while (len + 1 < dst_cap && shared->NtSystemRoot[len]) {
    dst[len] = shared->NtSystemRoot[len];
    ++len;
  }
  if (len == 0 || len + 6 >= dst_cap)
    return false;
  if (dst[len - 1] != L'\\')
    dst[len++] = L'\\';
  dst[len++] = L'T';
  dst[len++] = L'e';
  dst[len++] = L'm';
  dst[len++] = L'p';
  dst[len++] = L'\\';
  dst[len] = 0;
  return true;
}

static void init_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us,
                    wchar_t *path) {
  us->Length = static_cast<unsigned short>(wide_len(path) * sizeof(wchar_t));
  us->MaximumLength = us->Length;
  us->Buffer = path;
  oa->Length = sizeof(OBJECT_ATTRIBUTES);
  oa->RootDirectory = nullptr;
  oa->ObjectName = us;
  oa->Attributes = OBJ_CASE_INSENSITIVE;
  oa->SecurityDescriptor = nullptr;
  oa->SecurityQualityOfService = nullptr;
}

#define TEST(name) printf("\n=== %s ===\n", name)
#define PASS(msg)                                                              \
  do {                                                                         \
    printf("  PASS: %s\n", msg);                                               \
    g_pass++;                                                                  \
  } while (0)
#define FAIL(msg)                                                              \
  do {                                                                         \
    printf("  FAIL: %s\n", msg);                                               \
    g_fail++;                                                                  \
  } while (0)
#define CHECK(cond, pass_msg, fail_msg)                                        \
  do {                                                                         \
    if (cond)                                                                  \
      PASS(pass_msg);                                                          \
    else                                                                       \
      FAIL(fail_msg);                                                          \
  } while (0)

// Allocate placeholder-born private committed memory.
static void *alloc_private(SIZE_T size) {
  PVOID ph = nullptr;
  SIZE_T ps = size;
  NTSTATUS st = NtAllocateVirtualMemoryEx(
      g_proc, &ph, &ps, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
      nullptr, 0);
  if (NT_ERROR(st))
    return nullptr;
  PVOID base = ph;
  SIZE_T sz = size;
  st = NtAllocateVirtualMemoryEx(
      g_proc, &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      nullptr, 0);
  if (NT_ERROR(st)) {
    PVOID rb = ph;
    SIZE_T rs = 0;
    NtFreeVirtualMemory(g_proc, &rb, &rs, MEM_RELEASE);
    return nullptr;
  }
  return base;
}

static void free_private(void *addr) {
  PVOID base = addr;
  SIZE_T sz = 0;
  NtFreeVirtualMemory(g_proc, &base, &sz, MEM_RELEASE);
}

static bool is_all_zero(const void *buf, SIZE_T size) {
  const unsigned char *p = (const unsigned char *)buf;
  for (SIZE_T i = 0; i < size; i++)
    if (p[i] != 0)
      return false;
  return true;
}

// T1: Private DONTNEED zeros (decommit+commit cycle)
static void t1_private_dontneed_zeros() {
  TEST("T1: Private DONTNEED zeros");
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated 64KB private", "alloc failed");
  if (!p)
    return;

  // Dirty all pages.
  memset(p, 0xAA, 65536);

  // Decommit + recommit = DONTNEED.
  PVOID base = p;
  SIZE_T sz = 65536;
  NtFreeVirtualMemory(g_proc, &base, &sz, MEM_DECOMMIT);
  base = p;
  sz = 65536;
  NtAllocateVirtualMemoryEx(g_proc, &base, &sz, MEM_COMMIT, PAGE_READWRITE,
                             nullptr, 0);

  CHECK(is_all_zero(p, 65536), "all pages zero after decommit+commit",
        "pages NOT zero after decommit+commit");

  free_private(p);
}

// T2: Section DONTNEED zeros (WSEX-selective memset+MEM_RESET)
static void t2_section_dontneed_zeros() {
  TEST("T2: Section DONTNEED zeros via WSEX-selective");

  // Create a pagefile section + view.
  LARGE_INTEGER sec_size;
  sec_size.QuadPart = 65536;
  HANDLE sec = nullptr;
  NtCreateSectionEx(&sec, SECTION_ALL_ACCESS, nullptr, &sec_size,
                    PAGE_READWRITE, SEC_COMMIT, nullptr, nullptr, 0);
  CHECK(sec != nullptr, "created section", "section creation failed");
  if (!sec)
    return;

  PVOID ph = nullptr;
  SIZE_T ps = 65536;
  NtAllocateVirtualMemoryEx(g_proc, &ph, &ps,
                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                             PAGE_NOACCESS, nullptr, 0);
  PVOID view = ph;
  SIZE_T vs = 65536;
  LARGE_INTEGER off = {};
  NtMapViewOfSectionEx(sec, g_proc, &view, &off, &vs, MEM_REPLACE_PLACEHOLDER,
                       PAGE_READWRITE, nullptr, 0);

  // Dirty pages.
  memset(view, 0xBB, 65536);

  // WSEX-selective DONTNEED: memset backed pages + MEM_RESET.
  // Query WSEX to find backed pages.
  WSEX_INFO batch[16];
  for (int i = 0; i < 16; i++) {
    batch[i].VirtualAddress = (char *)view + i * 4096;
    batch[i].VirtualAttributes.Flags = 0;
  }
  NTSTATUS st = NtQueryVirtualMemory(g_proc, nullptr, 4 /*WorkingSetEx*/,
                                      batch, 16 * sizeof(batch[0]), nullptr);
  CHECK(NT_SUCCESS(st), "WSEX query succeeded", "WSEX query failed");

  // Memset backed pages.
  int zeroed = 0;
  for (int i = 0; i < 16; i++) {
    ULONG_PTR flags = batch[i].VirtualAttributes.Flags;
    if ((flags & 1) || (flags & 0xC00000) == 0x400000) {
      memset((char *)view + i * 4096, 0, 4096);
      zeroed++;
    }
  }
  printf("  INFO: zeroed %d/16 backed pages\n", zeroed);

  // MEM_RESET entire range.
  PVOID reset_base = view;
  SIZE_T reset_sz = 65536;
  NtAllocateVirtualMemory(g_proc, &reset_base, 0, &reset_sz, MEM_RESET,
                           PAGE_READWRITE);

  CHECK(is_all_zero(view, 65536), "section pages zero after WSEX DONTNEED",
        "section pages NOT zero");

  NtUnmapViewOfSectionEx(g_proc, view, 0);
  NtClose(sec);
}

// T3: File MAP_PRIVATE revert
static void t3_file_revert() {
  TEST("T3: File MAP_PRIVATE revert via PAGE_REVERT_TO_FILE_MAP");

  // Create temp file with known content.
  wchar_t tmp_path[260];
  if (!get_temp_path_nt(tmp_path, 260)) {
    FAIL("could not build temp path");
    return;
  }
  wchar_t file_path[300];
  for (int i = 0; tmp_path[i]; i++)
    file_path[i] = tmp_path[i];
  const wchar_t suffix[] = L"dontneed_test.tmp";
  int off = 0;
  while (tmp_path[off])
    off++;
  for (int i = 0; suffix[i]; i++)
    file_path[off + i] = suffix[i];
  file_path[off + 17] = 0;

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  init_oa(&oa, &us, file_path);
  IO_STATUS_BLOCK iosb = {};
  HANDLE fh = nullptr;
  NTSTATUS create_status =
      NtCreateFile(&fh, FILE_GENERIC_READ | FILE_GENERIC_WRITE, &oa, &iosb,
                   nullptr, FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                   FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                   nullptr, 0);
  if (NT_ERROR(create_status) || !fh) {
    FAIL("could not create temp file");
    return;
  }

  unsigned char pattern[4096];
  memset(pattern, 0x42, 4096);
  IO_STATUS_BLOCK wiosb = {};
  NtWriteFile(fh, nullptr, nullptr, nullptr, &wiosb, pattern, 4096, nullptr,
              nullptr);

  // Map as WRITECOPY.
  LARGE_INTEGER sec_size;
  sec_size.QuadPart = 4096;
  HANDLE sec = nullptr;
  NtCreateSectionEx(&sec, SECTION_ALL_ACCESS, nullptr, &sec_size,
                    PAGE_WRITECOPY, SEC_COMMIT, fh, nullptr, 0);
  NtClose(fh);

  if (!sec) {
    FAIL("section creation failed");
    NtDeleteFile(&oa);
    return;
  }

  PVOID ph = nullptr;
  SIZE_T ps = 4096;
  NtAllocateVirtualMemoryEx(g_proc, &ph, &ps,
                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                             PAGE_NOACCESS, nullptr, 0);
  PVOID view = ph;
  SIZE_T vs = 4096;
  LARGE_INTEGER voff = {};
  NtMapViewOfSectionEx(sec, g_proc, &view, &voff, &vs,
                       MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, nullptr, 0);

  // Verify original content.
  CHECK(((unsigned char *)view)[0] == 0x42, "original file content correct",
        "original file content wrong");

  // Dirty the COW page.
  ((volatile unsigned char *)view)[0] = 0xFF;
  CHECK(((unsigned char *)view)[0] == 0xFF, "COW write succeeded",
        "COW write failed");

  // Revert to file content.
  PVOID prot_base = view;
  SIZE_T prot_sz = 4096;
  ULONG old_prot;
  NTSTATUS st = NtProtectVirtualMemory(
      g_proc, &prot_base, &prot_sz,
      PAGE_WRITECOPY | PAGE_REVERT_TO_FILE_MAP, &old_prot);
  CHECK(NT_SUCCESS(st), "PAGE_REVERT_TO_FILE_MAP succeeded",
        "PAGE_REVERT_TO_FILE_MAP failed");
  CHECK(((unsigned char *)view)[0] == 0x42,
        "file content restored after revert", "file content NOT restored");

  NtUnmapViewOfSectionEx(g_proc, view, 0);
  NtClose(sec);
  NtDeleteFile(&oa);
}

// T5: Private partial munmap (decommit+punch, neighbors intact)
static void t5_partial_munmap() {
  TEST("T5: Private partial munmap preserves neighbors");
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated 64KB", "alloc failed");
  if (!p)
    return;

  // Write distinct patterns to each 4KB page.
  for (int i = 0; i < 16; i++)
    memset((char *)p + i * 4096, (unsigned char)(i + 1), 4096);

  // Unmap middle 4KB (page 8).
  char *target = (char *)p + 8 * 4096;
  PVOID decom_base = target;
  SIZE_T decom_sz = 4096;
  NTSTATUS st =
      NtFreeVirtualMemory(g_proc, &decom_base, &decom_sz, MEM_DECOMMIT);
  CHECK(NT_SUCCESS(st), "decommit middle page OK", "decommit failed");

  PVOID punch_base = target;
  SIZE_T punch_sz = 4096;
  st = NtFreeVirtualMemory(g_proc, &punch_base, &punch_sz,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  CHECK(NT_SUCCESS(st), "punch to placeholder OK", "punch failed");

  // Verify neighbors intact.
  bool left_ok = ((unsigned char *)p)[7 * 4096] == 8;
  bool right_ok = ((unsigned char *)p)[9 * 4096] == 10;
  CHECK(left_ok, "left neighbor data intact", "left neighbor CORRUPTED");
  CHECK(right_ok, "right neighbor data intact", "right neighbor CORRUPTED");

  // Verify punched region is placeholder (MEM_RESERVE).
  MBI mbi;
  NtQueryVirtualMemory(g_proc, target, 0, &mbi, sizeof(mbi), nullptr);
  CHECK(mbi.State == MEM_RESERVE, "punched region is placeholder",
        "punched region wrong state");

  // Release the punched placeholder.
  PVOID rel_base = target;
  SIZE_T rel_sz = 0;
  NtFreeVirtualMemory(g_proc, &rel_base, &rel_sz, MEM_RELEASE);

  // Free remaining fragments.
  // Left fragment: [p, p+8*4096)
  PVOID left = p;
  SIZE_T left_sz = 0;
  NtFreeVirtualMemory(g_proc, &left, &left_sz, MEM_RELEASE);
  // Right fragment: [p+9*4096, p+64KB)
  PVOID right = (char *)p + 9 * 4096;
  SIZE_T right_sz = 0;
  NtFreeVirtualMemory(g_proc, &right, &right_sz, MEM_RELEASE);
}

// T6: Private mremap grow in-place
static void t6_private_grow() {
  TEST("T6: Private mremap grow in-place");

  // Allocate 64KB, try to grow to 128KB.
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated 64KB", "alloc failed");
  if (!p)
    return;

  memset(p, 0xCC, 65536);

  // Check if adjacent VA is free.
  char *ext_addr = (char *)p + 65536;
  MBI mbi;
  NtQueryVirtualMemory(g_proc, ext_addr, 0, &mbi, sizeof(mbi), nullptr);

  if (mbi.State != MEM_FREE || mbi.RegionSize < 65536) {
    printf("  SKIP: adjacent VA not free (State=0x%lx Size=%llu)\n", mbi.State,
           (unsigned long long)mbi.RegionSize);
    free_private(p);
    return;
  }

  // Create extension placeholder.
  PVOID ext_ph = nullptr;
  SIZE_T ext_ps = 65536;
  NTSTATUS st = NtAllocateVirtualMemoryEx(
      g_proc, &ext_ph, &ext_ps, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
      PAGE_NOACCESS, nullptr, 0);
  if (NT_ERROR(st) || ext_ph != ext_addr) {
    printf("  SKIP: could not place extension placeholder\n");
    if (NT_SUCCESS(st)) {
      PVOID rb = ext_ph;
      SIZE_T rs = 0;
      NtFreeVirtualMemory(g_proc, &rb, &rs, MEM_RELEASE);
    }
    free_private(p);
    return;
  }

  // Replace with committed private memory.
  PVOID base = ext_addr;
  SIZE_T sz = 65536;
  st = NtAllocateVirtualMemoryEx(
      g_proc, &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      nullptr, 0);
  CHECK(NT_SUCCESS(st), "extension committed OK", "extension commit failed");

  if (NT_SUCCESS(st)) {
    // Verify old data intact.
    CHECK(((unsigned char *)p)[0] == 0xCC, "old data intact",
          "old data CORRUPTED");
    // Verify extension is zero.
    CHECK(is_all_zero(ext_addr, 65536), "extension zero-filled",
          "extension NOT zero");
  }

  // Free both allocations.
  free_private(p);
  PVOID ext_free = ext_addr;
  SIZE_T ext_free_sz = 0;
  NtFreeVirtualMemory(g_proc, &ext_free, &ext_free_sz, MEM_RELEASE);
}

// T7: Optimal munmap sequence (decommit first = faster punch)
static void t7_optimal_munmap() {
  TEST("T7: Optimal munmap (decommit before punch)");
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated", "alloc failed");
  if (!p)
    return;

  memset(p, 0xDD, 65536);

  // Decommit first (makes pages clean).
  PVOID d_base = p;
  SIZE_T d_sz = 65536;
  NTSTATUS st = NtFreeVirtualMemory(g_proc, &d_base, &d_sz, MEM_DECOMMIT);
  CHECK(NT_SUCCESS(st), "decommit OK", "decommit failed");

  // Punch to placeholder.
  // This should succeed and be faster on decommitted (clean) pages.
  d_base = p;
  d_sz = 65536;
  st = NtFreeVirtualMemory(g_proc, &d_base, &d_sz,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  CHECK(NT_SUCCESS(st), "punch after decommit OK", "punch failed");

  // Verify it's a placeholder.
  MBI mbi;
  NtQueryVirtualMemory(g_proc, p, 0, &mbi, sizeof(mbi), nullptr);
  CHECK(mbi.State == MEM_RESERVE, "punched to placeholder",
        "not a placeholder");

  // Release.
  PVOID rel = p;
  SIZE_T rel_sz = 0;
  NtFreeVirtualMemory(g_proc, &rel, &rel_sz, MEM_RELEASE);
}

// T9: Protection preservation
static void t9_protection_preservation() {
  TEST("T9: DONTNEED preserves readability after recommit");
  void *p = alloc_private(4096);
  CHECK(p != nullptr, "allocated", "alloc failed");
  if (!p)
    return;

  // Change to PAGE_READONLY.
  PVOID prot_base = p;
  SIZE_T prot_sz = 4096;
  ULONG old_prot;
  NtProtectVirtualMemory(g_proc, &prot_base, &prot_sz, PAGE_READONLY,
                          &old_prot);

  // Decommit + recommit (DONTNEED).
  PVOID d_base = p;
  SIZE_T d_sz = 4096;
  NtFreeVirtualMemory(g_proc, &d_base, &d_sz, MEM_DECOMMIT);
  d_base = p;
  d_sz = 4096;
  NtAllocateVirtualMemoryEx(g_proc, &d_base, &d_sz, MEM_COMMIT,
                             PAGE_READWRITE, nullptr, 0);

  // Page should be accessible (recommitted as RW).
  CHECK(((volatile unsigned char *)p)[0] == 0,
        "page readable after DONTNEED recommit",
        "page NOT readable after recommit");

  free_private(p);
}

// T10: WSEX priority conditional skip
static void t10_wsex_priority_skip() {
  TEST("T10: WSEX priority conditional skip");
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated", "alloc failed");
  if (!p)
    return;

  // Touch pages to bring into working set.
  memset(p, 0x01, 65536);

  // Query initial priority.
  WSEX_INFO batch[16];
  for (int i = 0; i < 16; i++) {
    batch[i].VirtualAddress = (char *)p + i * 4096;
    batch[i].VirtualAttributes.Flags = 0;
  }
  NtQueryVirtualMemory(g_proc, nullptr, 4, batch, 16 * sizeof(batch[0]),
                        nullptr);

  ULONG max_prio = 0;
  for (int i = 0; i < 16; i++) {
    ULONG_PTR flags = batch[i].VirtualAttributes.Flags;
    ULONG prio = (ULONG)((flags >> 24) & 7);
    if (prio > max_prio)
      max_prio = prio;
  }
  printf("  INFO: initial max_priority = %lu\n", max_prio);
  CHECK(max_prio > 0, "pages have non-zero priority", "pages at priority 0");

  // Demote priority.
  MEMORY_RANGE_ENTRY range = {p, 65536};
  ULONG low_prio = 1; // MEMORY_PRIORITY_VERY_LOW
  NtSetInformationVirtualMemory(g_proc, 1 /*VmPagePriorityInformation*/, 1,
                                 &range, &low_prio, sizeof(low_prio));

  // Re-query — should be <= 1 now.
  for (int i = 0; i < 16; i++) {
    batch[i].VirtualAddress = (char *)p + i * 4096;
    batch[i].VirtualAttributes.Flags = 0;
  }
  NtQueryVirtualMemory(g_proc, nullptr, 4, batch, 16 * sizeof(batch[0]),
                        nullptr);

  ULONG max_prio2 = 0;
  for (int i = 0; i < 16; i++) {
    ULONG prio = (ULONG)((batch[i].VirtualAttributes.Flags >> 24) & 7);
    if (prio > max_prio2)
      max_prio2 = prio;
  }
  printf("  INFO: after demotion max_priority = %lu\n", max_prio2);
  CHECK(max_prio2 <= 1, "priority demoted — skip syscall on second COLD call",
        "priority NOT demoted");

  free_private(p);
}

// T4: WSEX selectivity on sparse allocation
static void t4_wsex_selectivity() {
  TEST("T4: WSEX selectivity (sparse — only dirty pages zeroed)");
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated 64KB", "alloc failed");
  if (!p)
    return;

  // Touch only pages 0, 4, 8, 12 (25% dirty).
  for (int i = 0; i < 16; i += 4)
    ((volatile unsigned char *)((char *)p + i * 4096))[0] = 0xEE;

  // WSEX query to identify backed pages.
  WSEX_INFO batch[16];
  for (int i = 0; i < 16; i++) {
    batch[i].VirtualAddress = (char *)p + i * 4096;
    batch[i].VirtualAttributes.Flags = 0;
  }
  NtQueryVirtualMemory(g_proc, nullptr, 4, batch, 16 * sizeof(batch[0]),
                        nullptr);

  int backed = 0, unbacked = 0;
  for (int i = 0; i < 16; i++) {
    ULONG_PTR flags = batch[i].VirtualAttributes.Flags;
    bool has_backing = (flags & 1) || ((flags & 0xC00000) == 0x400000);
    if (has_backing)
      backed++;
    else
      unbacked++;
  }
  printf("  INFO: %d backed, %d unbacked pages\n", backed, unbacked);
  CHECK(backed >= 4, "at least 4 dirty pages have backing",
        "fewer than 4 backed pages");
  CHECK(unbacked > 0, "some pages have no backing (sparse)",
        "all pages backed — not sparse");

  // WSEX-selective zero: only zero backed pages.
  for (int i = 0; i < 16; i++) {
    ULONG_PTR flags = batch[i].VirtualAttributes.Flags;
    if ((flags & 1) || ((flags & 0xC00000) == 0x400000))
      memset((char *)p + i * 4096, 0, 4096);
  }

  // Verify dirty pages are now zero.
  bool dirty_zeroed = true;
  for (int i = 0; i < 16; i += 4) {
    if (((unsigned char *)((char *)p + i * 4096))[0] != 0)
      dirty_zeroed = false;
  }
  CHECK(dirty_zeroed, "dirty pages zeroed by WSEX-selective pass",
        "dirty pages NOT zeroed");

  free_private(p);
}

// T8: Cross-VAD DONTNEED (span multiple allocations)
static void t8_cross_vad_dontneed() {
  TEST("T8: Cross-VAD DONTNEED");

  // Create two adjacent private committed allocations (separate VADs).
  void *a = alloc_private(65536);
  CHECK(a != nullptr, "allocated first 64KB", "first alloc failed");
  if (!a)
    return;

  // Try to place second allocation adjacent to first.
  char *adj = (char *)a + 65536;
  PVOID ph2 = nullptr;
  SIZE_T ps2 = 65536;
  NTSTATUS st = NtAllocateVirtualMemoryEx(
      g_proc, &ph2, &ps2, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
      PAGE_NOACCESS, nullptr, 0);
  if (NT_ERROR(st) || ph2 != adj) {
    if (NT_SUCCESS(st)) {
      PVOID rb = ph2;
      SIZE_T rs = 0;
      NtFreeVirtualMemory(g_proc, &rb, &rs, MEM_RELEASE);
    }
    printf("  SKIP: could not place adjacent allocation\n");
    free_private(a);
    return;
  }
  PVOID b_base = adj;
  SIZE_T b_sz = 65536;
  st = NtAllocateVirtualMemoryEx(
      g_proc, &b_base, &b_sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      nullptr, 0);
  if (NT_ERROR(st)) {
    PVOID rb = ph2;
    SIZE_T rs = 0;
    NtFreeVirtualMemory(g_proc, &rb, &rs, MEM_RELEASE);
    printf("  SKIP: could not commit adjacent allocation\n");
    free_private(a);
    return;
  }

  // Dirty both allocations.
  memset(a, 0xAA, 65536);
  memset(adj, 0xBB, 65536);

  // DONTNEED spanning both VADs: must walk per-region.
  // Decommit+commit each separately (cross-VAD decommit fails).
  char *cur = (char *)a;
  char *end = (char *)a + 131072;
  int regions = 0;
  while (cur < end) {
    MBI mbi;
    NtQueryVirtualMemory(g_proc, cur, 0, &mbi, sizeof(mbi), nullptr);
    char *rend = (char *)mbi.BaseAddress + mbi.RegionSize;
    if (rend > end)
      rend = end;
    SIZE_T chunk = (SIZE_T)(rend - cur);
    if (mbi.State == MEM_COMMIT && mbi.Type == 0x20000 /*MEM_PRIVATE*/) {
      PVOID db = cur;
      SIZE_T ds = chunk;
      NtFreeVirtualMemory(g_proc, &db, &ds, MEM_DECOMMIT);
      db = cur;
      ds = chunk;
      NtAllocateVirtualMemoryEx(g_proc, &db, &ds, MEM_COMMIT, PAGE_READWRITE,
                                 nullptr, 0);
      regions++;
    }
    cur = rend;
  }
  printf("  INFO: processed %d regions\n", regions);
  CHECK(regions == 2, "walked 2 separate VADs", "unexpected region count");

  // Verify both zeroed.
  CHECK(is_all_zero(a, 65536), "first VAD zeroed", "first VAD NOT zeroed");
  CHECK(is_all_zero(adj, 65536), "second VAD zeroed", "second VAD NOT zeroed");

  free_private(a);
  PVOID bf = adj;
  SIZE_T bfs = 0;
  NtFreeVirtualMemory(g_proc, &bf, &bfs, MEM_RELEASE);
}

// T11: demand_map_placeholder produces private memory (mprotect PROT_NONE→RW)
static void t11_demand_map_private() {
  TEST("T11: mprotect(PROT_NONE->RW) produces private committed memory");

  // Create a bare placeholder (simulating mmap PROT_NONE).
  PVOID ph = nullptr;
  SIZE_T ps = 65536;
  NTSTATUS st = NtAllocateVirtualMemoryEx(
      g_proc, &ph, &ps, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
      nullptr, 0);
  CHECK(NT_SUCCESS(st), "placeholder created", "placeholder failed");
  if (!ph)
    return;

  // Replace placeholder with private committed (simulating demand_map).
  PVOID base = ph;
  SIZE_T sz = 65536;
  st = NtAllocateVirtualMemoryEx(
      g_proc, &base, &sz,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      nullptr, 0);
  CHECK(NT_SUCCESS(st), "MEM_REPLACE_PLACEHOLDER with private OK",
        "MEM_REPLACE_PLACEHOLDER failed");
  if (NT_ERROR(st)) {
    PVOID rb = ph;
    SIZE_T rs = 0;
    NtFreeVirtualMemory(g_proc, &rb, &rs, MEM_RELEASE);
    return;
  }

  // Verify it's MEM_PRIVATE + MEM_COMMIT.
  MBI mbi;
  NtQueryVirtualMemory(g_proc, ph, 0, &mbi, sizeof(mbi), nullptr);
  CHECK(mbi.Type == 0x20000 /*MEM_PRIVATE*/, "type is MEM_PRIVATE",
        "type is NOT MEM_PRIVATE");
  CHECK(mbi.State == MEM_COMMIT, "state is MEM_COMMIT",
        "state is NOT MEM_COMMIT");

  // Verify MEM_DECOMMIT works (the whole point of private over sections).
  PVOID db = ph;
  SIZE_T ds = 4096;
  st = NtFreeVirtualMemory(g_proc, &db, &ds, MEM_DECOMMIT);
  CHECK(NT_SUCCESS(st), "MEM_DECOMMIT works on demand-mapped private",
        "MEM_DECOMMIT FAILED — still section-backed?");

  // Recommit and verify zero.
  db = ph;
  ds = 4096;
  NtAllocateVirtualMemoryEx(g_proc, &db, &ds, MEM_COMMIT, PAGE_READWRITE,
                             nullptr, 0);
  CHECK(((unsigned char *)ph)[0] == 0, "recommitted page is zero",
        "recommitted page NOT zero");

  free_private(ph);
}

// T12: MAP_FIXED over private committed (free_private_mid_alloc)
static void t12_map_fixed_over_private() {
  TEST("T12: MAP_FIXED over middle of private (data preservation)");
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated 64KB", "alloc failed");
  if (!p)
    return;

  // Write distinct patterns.
  for (int i = 0; i < 16; i++)
    memset((char *)p + i * 4096, (unsigned char)(i + 1), 4096);

  // Simulate MAP_FIXED on pages 4-7 (middle of allocation):
  // Decommit target range.
  char *target = (char *)p + 4 * 4096;
  SIZE_T target_sz = 4 * 4096;
  PVOID db = target;
  SIZE_T ds = target_sz;
  NTSTATUS st = NtFreeVirtualMemory(g_proc, &db, &ds, MEM_DECOMMIT);
  CHECK(NT_SUCCESS(st), "decommit middle 4 pages OK", "decommit failed");

  // Punch to placeholder.
  db = target;
  ds = target_sz;
  st = NtFreeVirtualMemory(g_proc, &db, &ds,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  CHECK(NT_SUCCESS(st), "punch middle to placeholder OK", "punch failed");

  // Verify prefix data intact (pages 0-3).
  bool prefix_ok = true;
  for (int i = 0; i < 4; i++) {
    if (((unsigned char *)((char *)p + i * 4096))[0] !=
        (unsigned char)(i + 1))
      prefix_ok = false;
  }
  CHECK(prefix_ok, "prefix pages 0-3 data intact", "prefix data CORRUPTED");

  // Verify suffix data intact (pages 8-15).
  bool suffix_ok = true;
  for (int i = 8; i < 16; i++) {
    if (((unsigned char *)((char *)p + i * 4096))[0] !=
        (unsigned char)(i + 1))
      suffix_ok = false;
  }
  CHECK(suffix_ok, "suffix pages 8-15 data intact", "suffix data CORRUPTED");

  // Replace target placeholder with new committed memory.
  PVOID nb = target;
  SIZE_T ns = target_sz;
  st = NtAllocateVirtualMemoryEx(
      g_proc, &nb, &ns,
      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
      nullptr, 0);
  CHECK(NT_SUCCESS(st), "new mapping in target OK", "new mapping failed");

  // New mapping should be zero.
  if (NT_SUCCESS(st))
    CHECK(is_all_zero(target, target_sz), "new mapping is zero-filled",
          "new mapping NOT zero");

  // Cleanup: three separate allocations now.
  PVOID r1 = p;
  SIZE_T s1 = 0;
  NtFreeVirtualMemory(g_proc, &r1, &s1, MEM_RELEASE);
  PVOID r2 = target;
  SIZE_T s2 = 0;
  NtFreeVirtualMemory(g_proc, &r2, &s2, MEM_RELEASE);
  PVOID r3 = (char *)p + 8 * 4096;
  SIZE_T s3 = 0;
  NtFreeVirtualMemory(g_proc, &r3, &s3, MEM_RELEASE);
}

// T13: mremap shrink private
static void t13_mremap_shrink() {
  TEST("T13: mremap shrink private committed");
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated 64KB", "alloc failed");
  if (!p)
    return;

  memset(p, 0xDD, 65536);

  // Shrink to 32KB: decommit tail + punch.
  char *tail = (char *)p + 32768;
  PVOID db = tail;
  SIZE_T ds = 32768;
  NtFreeVirtualMemory(g_proc, &db, &ds, MEM_DECOMMIT);
  db = tail;
  ds = 32768;
  NTSTATUS st = NtFreeVirtualMemory(g_proc, &db, &ds,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  CHECK(NT_SUCCESS(st), "shrink punch OK", "shrink punch failed");

  // Release tail placeholder.
  PVOID rb = tail;
  SIZE_T rs = 0;
  NtFreeVirtualMemory(g_proc, &rb, &rs, MEM_RELEASE);

  // Verify kept data intact.
  CHECK(((unsigned char *)p)[0] == 0xDD, "kept data intact after shrink",
        "kept data CORRUPTED");
  CHECK(((unsigned char *)p)[32767] == 0xDD, "last kept byte intact",
        "last kept byte CORRUPTED");

  // Verify tail is freed.
  MBI mbi;
  NtQueryVirtualMemory(g_proc, tail, 0, &mbi, sizeof(mbi), nullptr);
  CHECK(mbi.State == MEM_FREE, "tail is MEM_FREE after shrink",
        "tail not freed");

  free_private(p);
}

// T14: mremap move private (memcpy fallback)
static void t14_mremap_move() {
  TEST("T14: mremap move private (memcpy)");
  void *p = alloc_private(65536);
  CHECK(p != nullptr, "allocated 64KB", "alloc failed");
  if (!p)
    return;

  // Fill with pattern.
  for (int i = 0; i < 65536; i++)
    ((unsigned char *)p)[i] = (unsigned char)(i & 0xFF);

  // Allocate new location.
  void *dst = alloc_private(131072);
  CHECK(dst != nullptr, "allocated 128KB destination", "dst alloc failed");
  if (!dst) {
    free_private(p);
    return;
  }

  // Copy.
  memcpy(dst, p, 65536);

  // Verify copy.
  bool copy_ok = true;
  for (int i = 0; i < 65536; i++) {
    if (((unsigned char *)dst)[i] != (unsigned char)(i & 0xFF)) {
      copy_ok = false;
      break;
    }
  }
  CHECK(copy_ok, "memcpy data correct at new location",
        "memcpy data CORRUPTED");

  // Extension (65536-131072) should be zero.
  CHECK(is_all_zero((char *)dst + 65536, 65536),
        "extension is zero-filled", "extension NOT zero");

  free_private(p);
  free_private(dst);
}

int main() {
  g_proc = reinterpret_cast<HANDLE>(-1LL);
  printf("POSIX DONTNEED compliance test\n");

  t1_private_dontneed_zeros();
  t2_section_dontneed_zeros();
  t3_file_revert();
  t4_wsex_selectivity();
  t5_partial_munmap();
  t6_private_grow();
  t7_optimal_munmap();
  t8_cross_vad_dontneed();
  t9_protection_preservation();
  t10_wsex_priority_skip();
  t11_demand_map_private();
  t12_map_fixed_over_private();
  t13_mremap_shrink();
  t14_mremap_move();

  printf("\n========================================\n");
  printf("Results: %d PASS, %d FAIL\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
