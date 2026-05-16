//===-- Windows implementation of misc internal operations -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine code for getentropy, gethostname, sysconf, pathconf, fpathconf.
// All functions live in the internal:: namespace and return -errno on failure
// (or ErrorOr<long> for functions where -1 is a valid success value).
//
//===----------------------------------------------------------------------===//

#include "misc_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "hdr/limits_macros.h"
#include "hdr/unistd_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

//===----------------------------------------------------------------------===//
// getentropy
//===----------------------------------------------------------------------===//

namespace internal {

intptr_t getentropy(void *buf, size_t len) {
  if (len > 256)
    return -EIO;
  if (buf == nullptr && len != 0)
    return -EIO;

  // ProcessPrng always succeeds -- no error handling needed.
  ::ProcessPrng(static_cast<unsigned char *>(buf), len);
  return 0;
}

} // namespace internal

//===----------------------------------------------------------------------===//
// gethostname
//===----------------------------------------------------------------------===//

namespace {

// Max hostname length per POSIX is 255. Cache includes NUL.
constexpr size_t MAX_HOSTNAME = 256;
char cached_hostname[MAX_HOSTNAME];
cpp::Atomic<int> cached_len{-1}; // -1 = not yet queried

int query_hostname() {
  // Open the Tcpip\Parameters key.
  WCHAR key_path[] =
      u"\\Registry\\Machine\\System\\CurrentControlSet"
      u"\\Services\\Tcpip\\Parameters";
  windows::nt_wstring_view key_name(key_path);

  auto oa = windows::named_internal_oa(&key_name);

  windows::ScopedNtHandle key;
  NTSTATUS status = ::NtOpenKeyEx(key.put(), KEY_QUERY_VALUE, &oa, 0);
  if (!NT_SUCCESS(status))
    return -1;

  // Query the "Hostname" value.
  WCHAR value_name_buf[] = u"Hostname";
  windows::nt_wstring_view value_name(value_name_buf);

  // Buffer: KEY_VALUE_PARTIAL_INFORMATION header + up to 255 WCHARs.
  alignas(KEY_VALUE_PARTIAL_INFORMATION) char
      buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + MAX_HOSTNAME * sizeof(WCHAR)];
  ULONG result_len = 0;

  status = ::NtQueryValueKey(key.get(), value_name.unicode_string(), KeyValuePartialInformation, buf,
                             sizeof(buf), &result_len);

  if (!NT_SUCCESS(status))
    return -1;

  auto *info = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION *>(buf);
  if (info->Type != REG_SZ || info->DataLength == 0)
    return -1;

  // Convert UTF-16 hostname to UTF-8.
  PCWCH wide_data = reinterpret_cast<PCWCH>(info->Data);
  ULONG wide_bytes = info->DataLength;
  // Strip trailing NUL if present.
  if (wide_bytes >= sizeof(WCHAR) &&
      wide_data[wide_bytes / sizeof(WCHAR) - 1] == u'\0')
    wide_bytes -= sizeof(WCHAR);

  int utf8_len = windows::utf16_to_utf8(wide_data, wide_bytes / sizeof(WCHAR),
                                        cached_hostname, MAX_HOSTNAME - 1);
  if (utf8_len < 0)
    return -1;

  cached_hostname[utf8_len] = '\0';
  return utf8_len;
}

} // namespace

namespace internal {

intptr_t gethostname(char *name, size_t len) {
  if (name == nullptr)
    return -EFAULT;

  // Fast path: return cached hostname.
  int hostname_len = cached_len.load(cpp::MemoryOrder::ACQUIRE);
  if (hostname_len < 0) {
    // First call -- query registry.
    hostname_len = query_hostname();
    if (hostname_len < 0)
      return -ENOMEM;
    cached_len.store(hostname_len, cpp::MemoryOrder::RELEASE);
  }

  if (static_cast<size_t>(hostname_len) >= len)
    return -ENAMETOOLONG;

  // Copy cached hostname to caller's buffer (including NUL terminator).
  __builtin_memcpy(name, cached_hostname, static_cast<size_t>(hostname_len) + 1);

  return 0;
}

} // namespace internal

//===----------------------------------------------------------------------===//
// sysconf
//===----------------------------------------------------------------------===//

namespace {

// Cached system info -- populated on first sysconf call.
// Page size is not cached here; use windows::get_page_size() which
// reads from g_pcb (populated once by pcb_startup_init()).
struct SysInfo {
  ULONG phys_pages;
  long num_processors;
  bool valid;
};

SysInfo g_sys_info = {};

// Count set bits in an affinity mask.
int popcount_mask(ULONG_PTR mask) {
  int count = 0;
  while (mask) {
    count += mask & 1;
    mask >>= 1;
  }
  return count;
}

// Minimum valid entry: Relationship (4) + Size (4) = 8 bytes.
constexpr DWORD SLPI_EX_MIN_SIZE = 8;

// Query SystemLogicalProcessorInformationEx into a stack buffer.
// Returns the actual data length, or 0 on failure.
// Passes the exact probed size to NT -- not sizeof(buf) -- so the
// returned length matches the packed entries with no trailing padding.
ULONG query_slpi_ex(char *buf, ULONG buf_capacity) {
  ULONG len = 0;
  NtQuerySystemInformation(SystemLogicalProcessorInformationEx, nullptr, 0,
                           &len);
  if (len == 0 || len > buf_capacity)
    return 0;
  NTSTATUS status =
      NtQuerySystemInformation(SystemLogicalProcessorInformationEx,
                               buf, len, &len);
  if (!NT_SUCCESS(status))
    return 0;
  return len;
}

// Advance to the next SLPI_EX entry, with bounds and sanity checks.
// Returns nullptr when the walk is complete or data is corrupt.
const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *
next_slpi_ex(const char *&ptr, const char *end) {
  if (ptr + SLPI_EX_MIN_SIZE > end)
    return nullptr;
  auto *entry =
      reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(ptr);
  if (entry->Size < SLPI_EX_MIN_SIZE || ptr + entry->Size > end)
    return nullptr;
  ptr += entry->Size;
  return entry;
}

// Count logical processors by walking RelationProcessorCore entries.
// Each core entry has one or more GROUP_AFFINITY masks; the set bits
// in each mask represent logical processors (including SMT/HT threads).
// This handles >64 CPUs across multiple processor groups.
long count_logical_processors() {
  auto ss = internal::byte_scratch(8192);
  if (!ss)
    return -1;
  ULONG len = query_slpi_ex(ss.data(), static_cast<ULONG>(ss.size()));
  if (len == 0)
    return -1;

  long total = 0;
  const char *ptr = ss.data();
  const char *end = ss.data() + len;
  while (auto *entry = next_slpi_ex(ptr, end)) {
    if (entry->Relationship == RelationProcessorCore) {
      for (WORD g = 0; g < entry->Processor.GroupCount; ++g)
        total += popcount_mask(entry->Processor.GroupMask[g].Mask);
    }
  }
  return total > 0 ? total : -1;
}

void ensure_sys_info() {
  if (g_sys_info.valid)
    return;
  SYSTEM_BASIC_INFORMATION sbi;
  NTSTATUS status = NtQuerySystemInformation(SystemBasicInformation, &sbi,
                                             sizeof(sbi), nullptr);
  if (NT_SUCCESS(status)) {
    g_sys_info.phys_pages = sbi.NumberOfPhysicalPages;
    g_sys_info.num_processors = count_logical_processors();
    // Fall back to BasicInformation if Ex query failed.
    if (g_sys_info.num_processors <= 0)
      g_sys_info.num_processors = sbi.NumberOfProcessors;
    g_sys_info.valid = true;
  }
}

// Walk SystemLogicalProcessorInformationEx to find cache topology.
// Returns -1 if the requested cache level/type is not found.
long query_cache(int level, PROCESSOR_CACHE_TYPE type, int field) {
  // field: 0 = line size, 1 = cache size, 2 = associativity
  auto ss = internal::byte_scratch(4096);
  if (!ss)
    return -1;
  ULONG len = query_slpi_ex(ss.data(), static_cast<ULONG>(ss.size()));
  if (len == 0)
    return -1;

  const char *ptr = ss.data();
  const char *end = ss.data() + len;
  while (auto *entry = next_slpi_ex(ptr, end)) {
    if (entry->Relationship == RelationCache &&
        entry->Cache.Level == static_cast<BYTE>(level) &&
        entry->Cache.Type == type) {
      switch (field) {
      case 0:
        return entry->Cache.LineSize;
      case 1:
        return entry->Cache.CacheSize;
      case 2:
        return entry->Cache.Associativity;
      }
    }
  }
  return -1;
}

// Feature supported: return _POSIX_VERSION (200809L).
constexpr long SUPPORTED = 200809L;
// Feature not supported: return -1 without setting errno (POSIX semantics
// for "no limit" / "indeterminate" -- distinct from EINVAL).
constexpr long UNSUPPORTED = -1;
// Standard POSIX V7 programming environments supported by this target ABI.
constexpr long V7_ILP32_OFFBIG =
    (sizeof(long) == 4 && sizeof(void *) == 4) ? SUPPORTED : UNSUPPORTED;
constexpr long V7_LP64_OFF64 =
    (sizeof(long) == 8 && sizeof(void *) == 8) ? SUPPORTED : UNSUPPORTED;

} // namespace

namespace internal {

ErrorOr<long> sysconf(int name) {
  ensure_sys_info();

  switch (name) {

  //===--------------------------------------------------------------------===//
  // Resource limits -- backed by NT queries or documented constants.
  //===--------------------------------------------------------------------===//

  case _SC_PAGESIZE:
    return static_cast<long>(windows::get_page_size());

  case _SC_ARG_MAX:
    // CreateProcess command line limit is 32767 UTF-16 code units.
    return 32767L;

  case _SC_CHILD_MAX:
    // No per-user child limit. Return -1 (indeterminate) per POSIX.
    return UNSUPPORTED;

  case _SC_CLK_TCK:
    // Fixed 100 for compatibility with Linux's typical CONFIG_HZ.
    return 100L;

  case _SC_NGROUPS_MAX:
    // No Unix supplementary group model on Windows.
    return 0L;

  case _SC_OPEN_MAX:
    // Our fd table capacity.
    return 1048576L;

  case _SC_STREAM_MAX:
    // Same limit as OPEN_MAX -- one FILE* per fd.
    return 1048576L;

  case _SC_TZNAME_MAX:
    return 255L;

  case _SC_RE_DUP_MAX:
    // POSIX minimum for RE_DUP_MAX.
    return 255L;

  case _SC_LOGIN_NAME_MAX:
    // UNLEN (256) from Windows lmcons.h.
    return 256L;

  case _SC_TTY_NAME_MAX:
    // Conservative value. Windows console/ConPTY device names are short.
    return 32L;

  case _SC_SYMLOOP_MAX:
    // NT I/O Manager reparse point traversal limit.
    return 63L;

  case _SC_HOST_NAME_MAX:
    // DNS hostname max.
    return 255L;

  case _SC_LINE_MAX:
    return 2048L;

  case _SC_ATEXIT_MAX:
    // Block-based allocator, no practical limit.
    return 2147483647L;

  case _SC_IOV_MAX:
    // Our readv/writev implementation limit.
    return 1024L;

  case _SC_NSIG:
    // Our signal implementation supports 64 signals.
    return 64L;

  //===--------------------------------------------------------------------===//
  // Processor / memory topology -- backed by SystemBasicInformation.
  //===--------------------------------------------------------------------===//

  case _SC_NPROCESSORS_CONF:
  case _SC_NPROCESSORS_ONLN:
    return g_sys_info.num_processors;

  case _SC_PHYS_PAGES:
    return static_cast<long>(g_sys_info.phys_pages);

  case _SC_AVPHYS_PAGES: {
    SYSTEM_PERFORMANCE_INFORMATION spi;
    NTSTATUS status = NtQuerySystemInformation(SystemPerformanceInformation,
                                               &spi, sizeof(spi), nullptr);
    if (NT_SUCCESS(status))
      return static_cast<long>(spi.AvailablePages);
    return UNSUPPORTED;
  }

  //===--------------------------------------------------------------------===//
  // Thread limits.
  //===--------------------------------------------------------------------===//

  case _SC_THREAD_DESTRUCTOR_ITERATIONS:
    // POSIX minimum. Our FLS-based impl runs all destructors in one pass.
    return 4L;

  case _SC_THREAD_KEYS_MAX:
    // Windows FLS slot limit.
    return 128L;

  case _SC_THREAD_STACK_MIN:
    return static_cast<long>(windows::get_page_size());

  case _SC_THREAD_THREADS_MAX:
    // No fixed limit -- bounded by memory. Return -1 (indeterminate).
    return UNSUPPORTED;

  //===--------------------------------------------------------------------===//
  // Cache topology -- backed by SystemLogicalProcessorInformationEx.
  //===--------------------------------------------------------------------===//

  case _SC_LEVEL1_DCACHE_LINESIZE:
    return query_cache(1, CacheData, 0);
  case _SC_LEVEL1_DCACHE_SIZE:
    return query_cache(1, CacheData, 1);
  case _SC_LEVEL1_DCACHE_ASSOC:
    return query_cache(1, CacheData, 2);

  case _SC_LEVEL1_ICACHE_LINESIZE:
    return query_cache(1, CacheInstruction, 0);
  case _SC_LEVEL1_ICACHE_SIZE:
    return query_cache(1, CacheInstruction, 1);
  case _SC_LEVEL1_ICACHE_ASSOC:
    return query_cache(1, CacheInstruction, 2);

  case _SC_LEVEL2_CACHE_LINESIZE:
    return query_cache(2, CacheUnified, 0);
  case _SC_LEVEL2_CACHE_SIZE:
    return query_cache(2, CacheUnified, 1);
  case _SC_LEVEL2_CACHE_ASSOC:
    return query_cache(2, CacheUnified, 2);

  case _SC_LEVEL3_CACHE_LINESIZE:
    return query_cache(3, CacheUnified, 0);
  case _SC_LEVEL3_CACHE_SIZE:
    return query_cache(3, CacheUnified, 1);
  case _SC_LEVEL3_CACHE_ASSOC:
    return query_cache(3, CacheUnified, 2);

  case _SC_LEVEL4_CACHE_LINESIZE:
    return query_cache(4, CacheUnified, 0);
  case _SC_LEVEL4_CACHE_SIZE:
    return query_cache(4, CacheUnified, 1);
  case _SC_LEVEL4_CACHE_ASSOC:
    return query_cache(4, CacheUnified, 2);

  //===--------------------------------------------------------------------===//
  // POSIX version.
  //===--------------------------------------------------------------------===//

  case _SC_VERSION:
    return 200809L;

  //===--------------------------------------------------------------------===//
  // POSIX.2 version / option queries.
  //===--------------------------------------------------------------------===//

  case _SC_2_VERSION:
    return _POSIX2_VERSION;
  case _SC_2_C_BIND:
    return _POSIX2_C_BIND;
  case _SC_2_C_DEV:
    return _POSIX2_C_DEV;
  case _SC_2_CHAR_TERM:
    return _POSIX2_CHAR_TERM;
  case _SC_2_FORT_DEV:
    return _POSIX2_FORT_DEV;
  case _SC_2_FORT_RUN:
    return _POSIX2_FORT_RUN;
  case _SC_2_LOCALEDEF:
    return _POSIX2_LOCALEDEF;
  case _SC_2_PBS:
    return _POSIX2_PBS;
  case _SC_2_PBS_ACCOUNTING:
    return _POSIX2_PBS_ACCOUNTING;
  case _SC_2_PBS_CHECKPOINT:
    return _POSIX2_PBS_CHECKPOINT;
  case _SC_2_PBS_LOCATE:
    return _POSIX2_PBS_LOCATE;
  case _SC_2_PBS_MESSAGE:
    return _POSIX2_PBS_MESSAGE;
  case _SC_2_PBS_TRACK:
    return _POSIX2_PBS_TRACK;
  case _SC_2_SW_DEV:
    return _POSIX2_SW_DEV;
  case _SC_2_UPE:
    return _POSIX2_UPE;

  //===--------------------------------------------------------------------===//
  // POSIX.1-2008 programming environment queries.
  //===--------------------------------------------------------------------===//

  case _SC_V7_ILP32_OFF32:
  case _SC_V7_LPBIG_OFFBIG:
    return UNSUPPORTED;
  case _SC_V7_ILP32_OFFBIG:
    return V7_ILP32_OFFBIG;
  case _SC_V7_LP64_OFF64:
    return V7_LP64_OFF64;
  case _SC_V8_ILP32_OFF32:
  case _SC_V8_ILP32_OFFBIG:
  case _SC_V8_LP64_OFF64:
  case _SC_V8_LPBIG_OFFBIG:
    return UNSUPPORTED;

  //===--------------------------------------------------------------------===//
  // Feature-test queries -- 200809L if we implement it, -1 if not.
  // Each claim is backed by a working implementation in this libc.
  //===--------------------------------------------------------------------===//

  // Threading -- full pthread API implemented.
  case _SC_THREADS:
  case _SC_THREAD_SAFE_FUNCTIONS:
  case _SC_THREAD_ATTR_STACKADDR:
  case _SC_THREAD_ATTR_STACKSIZE:
  case _SC_BARRIERS:
  case _SC_READER_WRITER_LOCKS:
  case _SC_SPIN_LOCKS:
  case _SC_TIMEOUTS:
    return SUPPORTED;

  // Memory -- mmap, mprotect, mlock, shm_open all implemented.
  case _SC_MAPPED_FILES:
  case _SC_MEMORY_PROTECTION:
  case _SC_MEMLOCK:
  case _SC_MEMLOCK_RANGE:
  case _SC_SHARED_MEMORY_OBJECTS:
    return SUPPORTED;

  // Time -- clock_gettime with MONOTONIC, REALTIME, CPUTIME implemented.
  case _SC_MONOTONIC_CLOCK:
  case _SC_CLOCK_SELECTION:
  case _SC_CPUTIME:
  case _SC_THREAD_CPUTIME:
    return SUPPORTED;

  // I/O -- fsync, fdatasync implemented.
  case _SC_FSYNC:
  case _SC_SYNCHRONIZED_IO:
    return SUPPORTED;

  // Process -- posix_spawn implemented.
  case _SC_SPAWN:
    return SUPPORTED;

  // X/Open / XSI feature queries.
  case _SC_XOPEN_CRYPT:
    return _XOPEN_CRYPT;
  case _SC_XOPEN_ENH_I18N:
    return _XOPEN_ENH_I18N;
  case _SC_XOPEN_REALTIME:
    return _XOPEN_REALTIME;
  case _SC_XOPEN_REALTIME_THREADS:
    return _XOPEN_REALTIME_THREADS;
  case _SC_XOPEN_SHM:
    return _XOPEN_SHM;
  case _SC_XOPEN_UNIX:
    return _XOPEN_UNIX;
  case _SC_XOPEN_UUCP:
    return _XOPEN_UUCP;
  case _SC_XOPEN_VERSION:
    return _XOPEN_VERSION;

  // Not implemented -- honest -1.
  case _SC_AIO_LISTIO_MAX:
  case _SC_AIO_MAX:
  case _SC_AIO_PRIO_DELTA_MAX:
  case _SC_BC_BASE_MAX:
  case _SC_BC_DIM_MAX:
  case _SC_BC_SCALE_MAX:
  case _SC_BC_STRING_MAX:
  case _SC_COLL_WEIGHTS_MAX:
  case _SC_DELAYTIMER_MAX:
  case _SC_DEVICE_CONTROL:
  case _SC_EXPR_NEST_MAX:
  case _SC_GETGR_R_SIZE_MAX:
  case _SC_GETPW_R_SIZE_MAX:
  case _SC_MQ_OPEN_MAX:
  case _SC_MQ_PRIO_MAX:
  case _SC_PRIORITIZED_IO:
  case _SC_RTSIG_MAX:
  case _SC_SEM_NSEMS_MAX:
  case _SC_SEM_VALUE_MAX:
  case _SC_SIGQUEUE_MAX:
  case _SC_SPORADIC_SERVER:
  case _SC_SS_REPL_MAX:
  case _SC_TIMER_MAX:
  case _SC_THREAD_PROCESS_SHARED:
  case _SC_THREAD_PRIO_INHERIT:
  case _SC_THREAD_PRIO_PROTECT:
  case _SC_THREAD_PRIORITY_SCHEDULING:
  case _SC_THREAD_ROBUST_PRIO_INHERIT:
  case _SC_THREAD_ROBUST_PRIO_PROTECT:
  case _SC_THREAD_SPORADIC_SERVER:
  case _SC_TIMERS:
  case _SC_TYPED_MEMORY_OBJECTS:
  case _SC_SEMAPHORES:
  case _SC_JOB_CONTROL:
  case _SC_SAVED_IDS:
  case _SC_REALTIME_SIGNALS:
  case _SC_MESSAGE_PASSING:
  case _SC_ASYNCHRONOUS_IO:
  case _SC_REGEXP:
  case _SC_SHELL:
  case _SC_ADVISORY_INFO:
  case _SC_PRIORITY_SCHEDULING:
  case _SC_IPV6:
  case _SC_RAW_SOCKETS:
    return UNSUPPORTED;

  default:
    return Error(EINVAL);
  }
}

} // namespace internal

//===----------------------------------------------------------------------===//
// pathconf / fpathconf
//===----------------------------------------------------------------------===//

namespace {

// Shared pathconf implementation operating on an open NT handle.
// Returns ErrorOr<long>: the pathconf value on success, Error(errno) on error.
ErrorOr<long> pathconf_handle_internal(HANDLE file_handle, int name) {
  // Query filesystem attributes. The buffer must be large enough for the
  // variable-length FileSystemName (NTFS = 8 bytes, ReFS = 8, FAT32 = 10).
  alignas(8) char attr_buf[128];
  IO_STATUS_BLOCK iosb;
  NTSTATUS status = NtQueryVolumeInformationFile(
      file_handle, &iosb, attr_buf, sizeof(attr_buf),
      FileFsAttributeInformation);

  FILE_FS_ATTRIBUTE_INFORMATION *attr = nullptr;
  if (NT_SUCCESS(status))
    attr = reinterpret_cast<FILE_FS_ATTRIBUTE_INFORMATION *>(attr_buf);

  // Query block size information for transfer-size queries.
  FILE_FS_SIZE_INFORMATION size_info = {};
  bool have_size = false;
  if (name == _PC_REC_MIN_XFER_SIZE || name == _PC_ALLOC_SIZE_MIN ||
      name == _PC_REC_XFER_ALIGN) {
    status = NtQueryVolumeInformationFile(file_handle, &iosb, &size_info,
                                          sizeof(size_info),
                                          FileFsSizeInformation);
    have_size = NT_SUCCESS(status);
  }

  switch (name) {
  case _PC_NAME_MAX:
    // Filesystem-reported max component name. NTFS/ReFS = 255.
    if (attr)
      return static_cast<long>(attr->MaximumComponentNameLength);
    return 255L; // safe default

  case _PC_PATH_MAX:
    // NT Object Manager limit for \??\ paths. Always 32767 UTF-16 code
    // units regardless of registry settings -- NtCreateFile bypasses the
    // Win32 260-char restriction.
    return 32767L;

  case _PC_LINK_MAX:
    // NTFS supports 1023 hard links per file. Check the capability flag
    // to handle FAT32 (no hard links, return 1).
    if (attr && (attr->FileSystemAttributes & FILE_SUPPORTS_HARD_LINKS))
      return 1023L;
    return 1L;

  case _PC_FILESIZEBITS:
    // NTFS/ReFS support 64-bit file sizes. FAT32 is 32-bit.
    if (attr) {
      // FAT32's FileSystemName is u"FAT32" (10 bytes).
      ULONG fs_len = attr->FileSystemNameLength / sizeof(WCHAR);
      if (fs_len >= 3 && attr->FileSystemName[0] == u'F' &&
          attr->FileSystemName[1] == u'A' &&
          attr->FileSystemName[2] == u'T')
        return 32L;
    }
    return 64L;

  case _PC_2_SYMLINKS:
    // NTFS/ReFS support reparse points (symlinks).
    if (attr && (attr->FileSystemAttributes & FILE_SUPPORTS_REPARSE_POINTS))
      return 1L;
    return 0L;

  case _PC_PIPE_BUF:
    // Named pipe atomic write guarantee. Windows named pipes guarantee
    // atomic writes up to the pipe buffer size, defaulting to 4096.
    return 4096L;

  case _PC_REC_MIN_XFER_SIZE:
    // Minimum recommended transfer size = allocation unit (cluster size).
    if (have_size)
      return static_cast<long>(size_info.SectorsPerAllocationUnit *
                               size_info.BytesPerSector);
    return 4096L;

  case _PC_ALLOC_SIZE_MIN:
  case _PC_REC_XFER_ALIGN:
    // Minimum allocation and recommended alignment = sector size.
    if (have_size)
      return static_cast<long>(size_info.BytesPerSector);
    return 512L;

  case _PC_MAX_CANON:
    return static_cast<long>(_POSIX_MAX_CANON);

  case _PC_MAX_INPUT:
    return static_cast<long>(_POSIX_MAX_INPUT);

  case _PC_CHOWN_RESTRICTED:
    // Windows ACL model always restricts ownership changes.
    return static_cast<long>(_POSIX_CHOWN_RESTRICTED);

  case _PC_NO_TRUNC:
    // NTFS/ReFS never silently truncate filenames.
    return static_cast<long>(_POSIX_NO_TRUNC);

  case _PC_VDISABLE:
    return static_cast<long>(_POSIX_VDISABLE);

  case _PC_SYMLINK_MAX:
    // NT I/O Manager reparse point traversal limit.
    return 63L;

  case _PC_TIMESTAMP_RESOLUTION:
    // Resolution in nanoseconds. Varies by filesystem:
    //   NTFS/ReFS: 100 ns (FILETIME granularity)
    //   exFAT:     10,000,000 ns (10 ms)
    //   FAT32:     2,000,000,000 ns (2 s for write time)
    if (attr) {
      ULONG fs_len = attr->FileSystemNameLength / sizeof(WCHAR);
      if (fs_len >= 5 && attr->FileSystemName[0] == u'e' &&
          attr->FileSystemName[1] == u'x' &&
          attr->FileSystemName[2] == u'F' &&
          attr->FileSystemName[3] == u'A' &&
          attr->FileSystemName[4] == u'T')
        return 10000000L; // exFAT: 10 ms
      if (fs_len >= 3 && attr->FileSystemName[0] == u'F' &&
          attr->FileSystemName[1] == u'A' &&
          attr->FileSystemName[2] == u'T')
        return 2000000000L; // FAT32/FAT16: 2 s
    }
    return 100L; // NTFS/ReFS default

  case _PC_TEXTDOMAIN_MAX:
    // Domain name becomes filename "domainname.mo" -- bounded by NAME_MAX
    // minus the ".mo" suffix (3 chars). Derived from the filesystem query.
    if (attr)
      return static_cast<long>(attr->MaximumComponentNameLength) - 3;
    return 252L; // 255 - 3

  case _PC_FALLOC:
    // posix_fallocate is implemented via FileAllocationInformation.
    return 1L;

  case _PC_ASYNC_IO:
    // IO Ring provides true asynchronous I/O for disk files.
    return 1L;

  case _PC_SYNC_IO:
    // fsync/fdatasync are fully implemented via NtFlushBuffersFileEx.
    return 1L;

  case _PC_PRIO_IO:
  case _PC_REC_INCR_XFER_SIZE:
  case _PC_REC_MAX_XFER_SIZE:
    return -1L;

  default:
    return Error(EINVAL);
  }
}

} // namespace

namespace internal {

ErrorOr<long> pathconf(const char *path, int name) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (!path)
    return Error(EFAULT);
  string_view sv(path);

  auto path_buf_s = path_scratch();
  if (!path_buf_s) return Error(ENOMEM);
  WCHAR *path_buf = path_buf_s.data();
  auto nt = to_nt_path(sv, path_buf, path_buf_s.size());
  if (!nt.has_value())
    return Error(nt.error());
  size_t nt_len = nt.value();

  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view object_name(path_buf, nt_len);
  init_object_attributes(&oa, &object_name);

  IO_STATUS_BLOCK iosb;
  HANDLE handle;

  // Open with minimal access. FILE_OPEN_FOR_BACKUP_INTENT allows directories.
  NTSTATUS status = NtOpenFile(
      &handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT);

  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));

  ErrorOr<long> result = pathconf_handle_internal(handle, name);
  NtClose(handle);
  return result;
}

ErrorOr<long> fpathconf(int fd, int name) {
  auto handle = internal::fd_table.get(fd);
  if (!handle)
    return Error(handle.error());
  return pathconf_handle_internal(*handle, name);
}

} // namespace internal

} // namespace LIBC_NAMESPACE_DECL
