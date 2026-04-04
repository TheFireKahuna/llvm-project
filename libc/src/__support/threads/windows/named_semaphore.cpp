//===-- Named semaphore implementation for Windows -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Named semaphore support backed by NT kernel semaphore objects.
//
// sem_open:   marker file in <temp-root>\llvm_sem\ + NtCreateSemaphore
// sem_close:  NtClose + return sem_t to pool
// sem_unlink: NtDeleteFile on marker (cross-process, immediate)
// sem_post:   NtReleaseSemaphore
// sem_wait:   alertable NtWaitForSingleObject with signal/cancel support
//
// POSIX names (leading '/') use per-user SipHash-2-4 keyed from the process
// token's owner SID. NT object names live under
// \BaseNamedObjects\ls-<hash>-<gen>. The generation counter (stored in the
// marker file) ensures sem_unlink + sem_open(O_CREAT) creates a genuinely new
// kernel semaphore.
//
// Slashless names are an NTPOSIX extension used to open an existing native NT
// named semaphore by leaf name under \BaseNamedObjects. This is intentionally
// open-only today so POSIX unlinkable names and native object-lifetime names
// stay distinct. Future work: add an explicit native API if callers need raw
// NT paths or native create semantics.
//
// Marker file format (16 bytes):
//   [0-7]:   magic "LLVMSEMA"
//   [8-11]:  generation (uint32_t LE, starts at 1)
//   [12-15]: mode (uint32_t LE)
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/windows/named_semaphore.h"
#include "hdr/errno_macros.h"
#include "src/__support/error_or.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt/session_bno.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/lazy_init.h"
#include "src/__support/OSUtil/windows/lazy_init_reset.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/temp_path.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/CPP/span.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/stringstream.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/threads/windows/semaphore.h" // SEM_KIND_*
#include "src/__support/OSUtil/windows/signal/signal.h"

#include <limits.h> // INT_MAX
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace {

//===----------------------------------------------------------------------===//
// SipHash-2-4 — deterministic name derivation per user
//===----------------------------------------------------------------------===//

inline uint64_t rotl(uint64_t x, int b) {
  return (x << b) | (x >> (64 - b));
}

inline void sipround(uint64_t &v0, uint64_t &v1, uint64_t &v2, uint64_t &v3) {
  v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
  v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
  v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
  v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
}

cpp::Atomic<uint64_t> g_k0{0};
cpp::Atomic<uint64_t> g_k1{0};
cpp::Atomic<uint32_t> g_init{0};

inline constexpr uint32_t KEY_INIT_UNINITIALIZED = 0;
inline constexpr uint32_t KEY_INIT_IN_PROGRESS = 1;
inline constexpr uint32_t KEY_INIT_READY = 2;

void ensure_key() {
  for (;;) {
    uint32_t state = g_init.load(cpp::MemoryOrder::ACQUIRE);
    if (state == KEY_INIT_READY)
      return;

    if (state == KEY_INIT_UNINITIALIZED) {
      uint32_t expected = KEY_INIT_UNINITIALIZED;
      if (!g_init.compare_exchange_strong(expected, KEY_INIT_IN_PROGRESS,
                                          cpp::MemoryOrder::ACQ_REL,
                                          cpp::MemoryOrder::ACQUIRE))
        continue;

      alignas(8) UCHAR sid_buf[256], grp_buf[256];
      SID *owner = nullptr;
      SID *grp = nullptr;
      uint64_t k0 = 0x6C6C766D6C696263ULL; // "llvmlibc"
      uint64_t k1 = 0x73656D6170686F72ULL; // "semaphor"
      NTSTATUS st = windows_sec::get_token_sids(
          sid_buf, sizeof(sid_buf), &owner, grp_buf, sizeof(grp_buf), &grp);
      if (NT_SUCCESS(st) && owner) {
        ULONG sid_len = ::RtlLengthSid(owner);
        const auto *bytes = reinterpret_cast<const uint8_t *>(owner);
        k0 = 0;
        k1 = 0;
        for (ULONG i = 0; i < sid_len; ++i) {
          if (i < 8)
            k0 |= static_cast<uint64_t>(bytes[i]) << (i * 8);
          else
            k1 |= static_cast<uint64_t>(bytes[i]) << ((i - 8) * 8);
        }
        k0 ^= 0x736970686173686BULL; // "siphashK"
        k1 ^= 0x6C6C766D73656D21ULL; // "llvmsem!"
      }
      g_k0.store(k0, cpp::MemoryOrder::RELAXED);
      g_k1.store(k1, cpp::MemoryOrder::RELAXED);
      g_init.store(KEY_INIT_READY, cpp::MemoryOrder::RELEASE);
      futex_addr::wake(&g_init, static_cast<uint32_t>(-1));
      return;
    }

    futex_addr::wait(&g_init, KEY_INIT_IN_PROGRESS, nullptr);
  }
}

uint64_t siphash(const char *data, size_t len) {
  ensure_key();
  uint64_t k0 = g_k0.load(cpp::MemoryOrder::RELAXED);
  uint64_t k1 = g_k1.load(cpp::MemoryOrder::RELAXED);

  uint64_t v0 = k0 ^ 0x736F6D6570736575ULL;
  uint64_t v1 = k1 ^ 0x646F72616E646F6DULL;
  uint64_t v2 = k0 ^ 0x6C7967656E657261ULL;
  uint64_t v3 = k1 ^ 0x7465646279746573ULL;

  const auto *p = reinterpret_cast<const uint8_t *>(data);
  size_t blocks = len / 8;
  for (size_t i = 0; i < blocks; ++i) {
    uint64_t m = 0;
    for (int j = 0; j < 8; ++j)
      m |= static_cast<uint64_t>(p[i * 8 + j]) << (j * 8);
    v3 ^= m;
    sipround(v0, v1, v2, v3);
    sipround(v0, v1, v2, v3);
    v0 ^= m;
  }

  uint64_t m = static_cast<uint64_t>(len & 0xFF) << 56;
  const uint8_t *tail = p + blocks * 8;
  size_t rem = len & 7;
  for (size_t i = 0; i < rem; ++i)
    m |= static_cast<uint64_t>(tail[i]) << (i * 8);
  v3 ^= m;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  v0 ^= m;

  v2 ^= 0xFF;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  return v0 ^ v1 ^ v2 ^ v3;
}

//===----------------------------------------------------------------------===//
// Marker file — filesystem-backed name registry for cross-process unlink
//===----------------------------------------------------------------------===//

constexpr char SEM_MAGIC[8] = {'L', 'L', 'V', 'M', 'S', 'E', 'M', 'A'};
constexpr size_t SEM_MARKER_SIZE = 16;
constexpr int WIN_MAX_PATH = 260;

struct SemMarker {
  char magic[8];
  uint32_t generation;
  uint32_t mode;
};

// Build marker file path: <temp-root>\llvm_sem\<name-without-slash>
// Returns length excluding NUL, or -1 on error.
int build_marker_path(const char *name, char *buf, int buf_size) {
  if (!name)
    return -1;
  // Skip leading '/' if present — the body is the part after it.
  // Slashless names are accepted (POSIX recommends '/' but doesn't require it).
  cpp::string_view body((name[0] == '/') ? name + 1 : name);
  if (body.size() == 0 || body[0] == '.')
    return -1;
  if (body.contains('/'))
    return -1;

  auto temp_wide_s = windows::path_scratch();
  if (!temp_wide_s)
    return -1;
  WCHAR *temp_wide = temp_wide_s.data();
  size_t temp_len = windows::get_temp_path_w(temp_wide, temp_wide_s.size());
  if (temp_len == 0 || temp_len > WIN_MAX_PATH)
    return -1;

  constexpr int TEMP_UTF8_CAP = WIN_MAX_PATH * 3 + 1;
  auto temp_utf8_s = windows::byte_scratch(TEMP_UTF8_CAP);
  if (!temp_utf8_s)
    return -1;
  char *temp_utf8 = temp_utf8_s.data();
  int utf8_len = windows::wide_to_utf8_n(temp_wide, temp_len, temp_utf8,
                                         TEMP_UTF8_CAP);
  if (utf8_len < 0)
    return -1;

  // Reserve last byte for NUL.
  cpp::StringStream ss(cpp::span<char>(buf, buf_size - 1));
  ss << cpp::string_view(temp_utf8, static_cast<size_t>(utf8_len))
     << "llvm_sem\\"
     << body;

  if (ss.overflow())
    return -1;

  size_t written = ss.str().size();
  buf[written] = '\0';
  return static_cast<int>(written);
}

// Ensure <temp-root>\llvm_sem\ exists with hardened permissions.
// Returns true if the directory is safe to use.
bool ensure_sem_dir(const char *marker_path) {
  const char *last_sep = nullptr;
  for (const char *p = marker_path; *p; ++p) {
    if (*p == '\\')
      last_sep = p;
  }
  if (!last_sep)
    return false;

  // Extract directory portion and convert to NT path.
  int dir_len = static_cast<int>(last_sep - marker_path);

  auto nt_path_s = windows::path_scratch();
  if (!nt_path_s)
    return false;
  WCHAR *nt_path = nt_path_s.data();
  using LIBC_NAMESPACE::cpp::string_view;
  string_view dir_sv(marker_path, static_cast<size_t>(dir_len));
  auto nt = to_nt_path(dir_sv, nt_path, nt_path_s.size());
  if (!nt.has_value())
    return false;
  size_t nt_len = nt.value();

  return windows_sec::ensure_secure_ipc_dir(nt_path, nt_len);
}

// Open marker file via NtCreateFile. disposition controls create-vs-open.
// When creating (FILE_CREATE), builds a DACL from mode for the marker file.
// Returns the handle, or nullptr on failure. Sets *status_out.
HANDLE open_marker_file(const char *marker_path, ULONG disposition,
                         mode_t mode, NTSTATUS *status_out) {
  using LIBC_NAMESPACE::cpp::string_view;
  auto nt_path_s = windows::path_scratch();
  if (!nt_path_s) {
    *status_out = STATUS_NO_MEMORY;
    return nullptr;
  }
  WCHAR *nt_path = nt_path_s.data();
  string_view marker_sv(marker_path);
  auto nt = to_nt_path(marker_sv, nt_path, nt_path_s.size());
  if (!nt.has_value()) {
    *status_out = STATUS_OBJECT_NAME_INVALID;
    return nullptr;
  }
  size_t nt_len = nt.value();

  windows::nt_wstring_view name(nt_path, nt_len);
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &name);

  // Apply file DACL on creation so the marker inherits POSIX permissions.
  alignas(8) UCHAR sd_buf[windows_sec::CREATION_SD_BUF_SIZE];
  if (disposition == FILE_CREATE)
    oa.SecurityDescriptor = windows_sec::build_creation_sd(sd_buf, mode);

  HANDLE h = nullptr;
  IO_STATUS_BLOCK iosb = {};
  *status_out = ::NtCreateFile(
      &h, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE_ACCESS, &oa, &iosb,
      nullptr, FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN, // hidden marker
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      disposition,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
  return NT_SUCCESS(*status_out) ? h : nullptr;
}

// Write marker contents to a newly created file.
NTSTATUS write_marker(HANDLE h, uint32_t generation, mode_t mode) {
  SemMarker marker = {};
  __builtin_memcpy(marker.magic, SEM_MAGIC, 8);
  marker.generation = generation;
  marker.mode = static_cast<uint32_t>(mode);
  IO_STATUS_BLOCK iosb = {};
  LARGE_INTEGER offset = {};
  return ::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, &marker,
                       SEM_MARKER_SIZE, &offset, nullptr);
}

// Read marker contents from an open file. Returns false on failure.
bool read_marker(HANDLE h, SemMarker *out) {
  IO_STATUS_BLOCK iosb = {};
  LARGE_INTEGER offset = {};
  NTSTATUS st = ::NtReadFile(h, nullptr, nullptr, nullptr, &iosb, out,
                              SEM_MARKER_SIZE, &offset, nullptr);
  if (!NT_SUCCESS(st) || iosb.Information < SEM_MARKER_SIZE)
    return false;
  return cpp::string_view(out->magic, 8) == cpp::string_view(SEM_MAGIC, 8);
}

//===----------------------------------------------------------------------===//
// NT object name formatting — includes generation for unlink/recreate safety
//===----------------------------------------------------------------------===//

// \Sessions\<N>\BaseNamedObjects\ls-<16 hex>-<decimal gen>

// Build the NT semaphore object name with generation.
// Returns length in WCHARs, or 0 on failure.
size_t build_nt_name(const char *name, uint32_t generation, WCHAR *out,
                     size_t max_wchars) {
  if (!name)
    return 0;
  cpp::string_view body((name[0] == '/') ? name + 1 : name);
  if (body.size() == 0)
    return 0;

  uint64_t h = siphash(body.data(), body.size());

  windows::WStringStream ss(cpp::span<WCHAR>(out, max_wchars - 1));
  windows::write_session_bno_prefix(ss);
  ss << u"ls-";
  ss.write_int<radix::Hex::WithWidth<16>>(h);
  ss << u'-' << generation;

  if (ss.overflow())
    return 0;
  ss.null_terminate();
  return ss.str().size();
}

//===----------------------------------------------------------------------===//
// sem_t pool (SlabPool-backed, demand-committed)
//===----------------------------------------------------------------------===//

internal::SlabPool g_sem_pool;

} // namespace (anonymous)

// First-use gate for g_sem_pool. pool.init()/init_tls() are idempotent,
// but the gate makes the "subsystem not touched" case observable via
// was_initialized() (used by fork and exec paths to skip repair work) and
// gives us a single point to register for `.libclzr` reset.
//
// Kept at `LIBC_NAMESPACE::internal` scope (not inside the file's anonymous
// namespace) so the thunk emitted by `LIBC_REGISTER_LAZY_RESET` at the
// bottom of this TU, which resolves `::LIBC_NAMESPACE::internal::g_..._init`
// via fully-qualified lookup, can see it.
namespace internal {
static int named_semaphore_init_impl() {
  constexpr size_t kSlotSize =
      (sizeof(sem_t) + alignof(sem_t) - 1) & ~(alignof(sem_t) - 1);
  LIBC_NAMESPACE::g_sem_pool.init(kSlotSize, alignof(sem_t));
  LIBC_NAMESPACE::g_sem_pool.init_tls();
  return 0;
}
LazyInit<&named_semaphore_init_impl> g_named_semaphore_init;
} // namespace internal

namespace {

sem_t *alloc_sem() {
  internal::g_named_semaphore_init.ensure();
  void *slot = g_sem_pool.tls_alloc();
  if (slot)
    __builtin_memset(slot, 0, sizeof(sem_t));
  return static_cast<sem_t *>(slot);
}

void free_sem(sem_t *s) { internal::SlabPool::free(s); }

//===----------------------------------------------------------------------===//
// Named sem_t layout — dedup metadata packed into the 7 padding bytes.
//
//   byte  0:      kind (SEM_KIND_NAMED = 1)
//   bytes 1-2:    refcount (uint16_t)
//   bytes 3-6:    name_gen_hash (uint32_t) — siphash of name + generation
//   byte  7:      reserved (zero)
//   bytes 8-15:   HANDLE (kernel semaphore)
//
// POSIX requires duplicate sem_open() to return the same sem_t*.
// find_open() scans live pool slots via for_each_live_all, matching on
// (kind==NAMED, refcount>0, name_gen_hash). Generation is folded into
// the hash so post-unlink re-creates don't match stale slots.
//===----------------------------------------------------------------------===//

HANDLE get_handle(sem_t *s) {
  HANDLE h;
  __builtin_memcpy(&h, &s->__data[8], sizeof(HANDLE));
  return h;
}

void set_handle(sem_t *s, HANDLE h) {
  s->__data[0] = SEM_KIND_NAMED;
  __builtin_memcpy(&s->__data[8], &h, sizeof(HANDLE));
}

uint16_t get_refcount(sem_t *s) {
  uint16_t rc;
  __builtin_memcpy(&rc, &s->__data[1], sizeof(rc));
  return rc;
}

void set_refcount(sem_t *s, uint16_t rc) {
  __builtin_memcpy(&s->__data[1], &rc, sizeof(rc));
}

uint32_t get_name_gen_hash(sem_t *s) {
  uint32_t h;
  __builtin_memcpy(&h, &s->__data[3], sizeof(h));
  return h;
}

void set_name_gen_hash(sem_t *s, uint32_t h) {
  __builtin_memcpy(&s->__data[3], &h, sizeof(h));
}

// Hash name + generation into a 32-bit tag for dedup matching.
// Non-zero guaranteed (so we never match a zeroed slot).
uint32_t make_name_gen_hash(const char *name, uint32_t generation) {
  // Mix generation into the name hash via XOR with a rotated generation.
  cpp::string_view name_sv(name);
  uint64_t h = siphash(name_sv.data(), name_sv.size());
  h ^= static_cast<uint64_t>(generation) * 0x9E3779B97F4A7C15ULL;
  uint32_t h32 = static_cast<uint32_t>(h ^ (h >> 32));
  return h32 | 1; // ensure non-zero
}

// for_each_live_all callback: find a named sem_t with matching hash.
struct FindCtx {
  uint32_t target_hash;
  sem_t *result;
};

void find_cb(void *slot, void *ctx) {
  auto *fc = static_cast<FindCtx *>(ctx);
  if (fc->result)
    return;
  auto *s = static_cast<sem_t *>(slot);
  if (s->__data[0] == SEM_KIND_NAMED &&
      get_refcount(s) > 0 &&
      get_name_gen_hash(s) == fc->target_hash)
    fc->result = s;
}

// Find an existing open for name+gen. Bumps refcount if found.
// Returns nullptr if not found or if the slot was concurrently freed.
// Thread safety: the pool's all-slabs lock serializes the scan.
// Refcount is only modified under this lock (both here and in close),
// so no CAS needed — the lock provides mutual exclusion.
sem_t *find_open(uint32_t ngh) {
  // Gate the pool. If the subsystem was never touched, for_each_live_all
  // on an uninit pool would be valid (just empty), but gating here keeps
  // the "which subsystems have state?" picture clean for fork/exec audits.
  internal::g_named_semaphore_init.ensure();
  FindCtx fc{ngh, nullptr};
  g_sem_pool.for_each_live_all(find_cb, &fc);
  if (fc.result) {
    uint16_t rc = get_refcount(fc.result);
    if (rc == 0)
      return nullptr; // raced with close
    set_refcount(fc.result, rc + 1);
  }
  return fc.result;
}

sem_t *open_native_leaf(const char *name, int oflag, int *err) {
  if ((oflag & (O_CREAT | O_EXCL)) != 0) {
    *err = EINVAL;
    return nullptr;
  }

  uint32_t ngh = make_name_gen_hash(name, 0);

  sem_t *existing = find_open(ngh);
  if (existing)
    return existing;

  auto nt_name_s = windows::path_scratch();
  if (!nt_name_s) {
    *err = ENOMEM;
    return nullptr;
  }
  WCHAR *nt_name = nt_name_s.data();
  cpp::string_view name_sv(name);
  size_t nt_name_len =
      to_base_named_object_path(name_sv, nt_name, nt_name_s.size());
  if (nt_name_len == 0) {
    *err = EINVAL;
    return nullptr;
  }

  windows::nt_wstring_view sem_name(nt_name, nt_name_len);
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &sem_name);

  windows::ScopedNtHandle sem_h;
  constexpr ACCESS_MASK kSemOpenAccess_ =
      SYNCHRONIZE | READ_CONTROL | 0x0001 | 0x0002;
  NTSTATUS st = ::NtOpenSemaphore(sem_h.put(), kSemOpenAccess_, &oa);
  if (!NT_SUCCESS(st)) {
    *err = windows_util::ntstatus_to_errno(st);
    return nullptr;
  }

  sem_t *sem = alloc_sem();
  if (!sem) {
    *err = ENOMEM;
    return nullptr;
  }

  set_name_gen_hash(sem, ngh);
  // Re-check before publishing (our slot has refcount=0, invisible).
  existing = find_open(ngh);
  if (existing) {
    free_sem(sem);
    return existing;
  }
  set_refcount(sem, 1);
  set_handle(sem, sem_h.release());
  return sem;
}

//===----------------------------------------------------------------------===//
// NT timeout conversion
//===----------------------------------------------------------------------===//

LARGE_INTEGER to_nt_timeout(const internal::AbsTimeout &timeout) {
  LARGE_INTEGER li;
  const timespec &ts = timeout.get_timespec();
  if (timeout.is_realtime()) {
    li.QuadPart = static_cast<LONGLONG>(ts.tv_sec) * 10000000LL +
                  ts.tv_nsec / 100LL + 116444736000000000LL;
  } else {
    ULONGLONG now_100ns = 0;
    ::RtlQueryUnbiasedInterruptTime(&now_100ns);
    LONGLONG target_100ns =
        static_cast<LONGLONG>(ts.tv_sec) * 10000000LL + ts.tv_nsec / 100LL;
    LONGLONG delta = target_100ns - static_cast<LONGLONG>(now_100ns);
    li.QuadPart = delta <= 0 ? 0 : -delta;
  }
  return li;
}

} // namespace

void named_semaphore_fork_reinit() {
  if (g_init.load(cpp::MemoryOrder::ACQUIRE) == KEY_INIT_IN_PROGRESS)
    g_init.store(KEY_INIT_UNINITIALIZED, cpp::MemoryOrder::RELAXED);
}

//===----------------------------------------------------------------------===//
// NamedSemaphore public API
//===----------------------------------------------------------------------===//

sem_t *NamedSemaphore::open(const char *name, int oflag, mode_t mode,
                             unsigned value, int *err) {
  using LIBC_NAMESPACE::cpp::string_view;
  // Slashless names prefixed with '\\' are treated as native NT leaf names
  // (open existing kernel semaphore by raw name under \BaseNamedObjects).
  // All other names go through the POSIX marker path, with a '/' prepended
  // if missing. This matches POSIX semantics where the leading '/' is
  // recommended but many programs omit it.
  if (name && name[0] == '\\')
    return open_native_leaf(name + 1, oflag, err);

  // Build marker file path.
  constexpr int kPathMax = WIN_MAX_PATH * 3 + 255 + 32;
  auto marker_path_s = windows::byte_scratch(kPathMax);
  if (!marker_path_s) {
    *err = ENOMEM;
    return nullptr;
  }
  char *marker_path = marker_path_s.data();
  if (build_marker_path(name, marker_path, kPathMax) < 0) {
    *err = EINVAL;
    return nullptr;
  }

  NTSTATUS st;
  SemMarker marker = {};
  bool created_new = false;

  // Apply umask before storing or using the mode.
  mode_t effective_mode = mode & ~windows_sec::get_umask();

  if (oflag & O_CREAT) {
    if (value > static_cast<unsigned>(INT_MAX)) {
      *err = EINVAL;
      return nullptr;
    }

    if (!ensure_sem_dir(marker_path)) {
      *err = EACCES;
      return nullptr;
    }

    // Try to create the marker file exclusively.
    {
      windows::ScopedNtHandle marker_h(
          open_marker_file(marker_path, FILE_CREATE, effective_mode, &st));
      if (marker_h) {
        // New semaphore — write marker with generation 1.
        created_new = true;
        marker.generation = 1;
        marker.mode = static_cast<uint32_t>(effective_mode);
        NTSTATUS wst =
            write_marker(marker_h.get(), marker.generation, effective_mode);
        marker_h.reset(); // close before checking write result
        if (!NT_SUCCESS(wst)) {
          // Clean up the empty marker on write failure.
          auto nt_cleanup_s = windows::path_scratch();
          if (nt_cleanup_s) {
            WCHAR *nt_path = nt_cleanup_s.data();
            string_view marker_cleanup_sv(marker_path);
            auto nt = to_nt_path(marker_cleanup_sv, nt_path,
                                  nt_cleanup_s.size());
            if (nt.has_value()) {
              size_t nt_len = nt.value();
              windows::nt_wstring_view cleanup_name(nt_path, nt_len);
              OBJECT_ATTRIBUTES oa;
              init_object_attributes(&oa, &cleanup_name);
              ::NtDeleteFile(&oa);
            }
          }
          *err = EIO;
          return nullptr;
        }
      } else if (st == STATUS_OBJECT_NAME_COLLISION) {
        // Marker already exists.
        if (oflag & O_EXCL) {
          *err = EEXIST;
          return nullptr;
        }
        // Open existing marker and read generation.
        marker_h.reset(
            open_marker_file(marker_path, FILE_OPEN, 0, &st));
        if (!marker_h) {
          *err = ENOENT;
          return nullptr;
        }
        if (!read_marker(marker_h.get(), &marker)) {
          *err = EINVAL;
          return nullptr;
        }
        // marker_h closed by destructor at end of scope.
      } else {
        *err = (st == STATUS_ACCESS_DENIED) ? EACCES : EINVAL;
        return nullptr;
      }
    }
  } else {
    // Open existing only.
    windows::ScopedNtHandle marker_h(
        open_marker_file(marker_path, FILE_OPEN, 0, &st));
    if (!marker_h) {
      *err = ENOENT;
      return nullptr;
    }
    if (!read_marker(marker_h.get(), &marker)) {
      *err = EINVAL;
      return nullptr;
    }
    // marker_h closed by destructor at end of scope.
  }

  // Dedup: now that we know the generation, check for an existing open.
  // Skip for newly created markers (no prior open possible).
  uint32_t ngh = make_name_gen_hash(name, marker.generation);
  if (!created_new) {
    sem_t *existing = find_open(ngh);
    if (existing)
      return existing;
  }

  // Build NT semaphore name with generation.
  auto nt_name_s = windows::path_scratch();
  if (!nt_name_s) {
    *err = ENOMEM;
    return nullptr;
  }
  size_t nt_name_len = build_nt_name(name, marker.generation,
                                      nt_name_s.data(), nt_name_s.size());
  if (nt_name_len == 0) {
    *err = EINVAL;
    return nullptr;
  }

  windows::nt_wstring_view sem_wsv(nt_name_s.data(), nt_name_len);

  OBJECT_ATTRIBUTES oa;
  __builtin_memset(&oa, 0, sizeof(oa));
  oa.Length = sizeof(oa);
  oa.ObjectName = sem_wsv.unicode_string();
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  // Build a DACL for the kernel semaphore from the POSIX mode.
  mode_t sem_mode = static_cast<mode_t>(marker.mode);
  alignas(8) UCHAR sem_sd_buf[windows_sec::CREATION_SD_BUF_SIZE];
  if (created_new) {
    oa.SecurityDescriptor = windows_sec::build_creation_sd(
        sem_sd_buf, sem_mode, nullptr, nullptr,
        windows_sec::mode_bits_to_object_access_mask);
  }

  constexpr ACCESS_MASK kSemOpenAccess =
      SYNCHRONIZE | READ_CONTROL | 0x0001 /*QUERY_STATE*/ | 0x0002 /*MODIFY_STATE*/;

  windows::ScopedNtHandle sem_h;
  if (created_new) {
    st = ::NtCreateSemaphore(sem_h.put(), SEMAPHORE_ALL_ACCESS, &oa,
                              static_cast<LONG>(value), INT_MAX);
  } else {
    oa.Attributes |= OBJ_OPENIF;
    st = ::NtCreateSemaphore(sem_h.put(), kSemOpenAccess, &oa,
                              static_cast<LONG>(value), INT_MAX);
  }

  if (!NT_SUCCESS(st)) {
    if (st == STATUS_OBJECT_NAME_NOT_FOUND)
      *err = ENOENT;
    else if (st == STATUS_ACCESS_DENIED)
      *err = EACCES;
    else
      *err = EINVAL;
    return nullptr;
  }

  sem_t *sem = alloc_sem();
  if (!sem) {
    *err = ENOMEM;
    return nullptr;
  }

  set_name_gen_hash(sem, ngh);
  // Re-check before publishing (our slot has refcount=0, invisible).
  sem_t *winner = find_open(ngh);
  if (winner) {
    free_sem(sem);
    return winner;
  }
  set_refcount(sem, 1);
  set_handle(sem, sem_h.release());
  return sem;
}

int NamedSemaphore::close(sem_t *sem) {
  // Decrement refcount under the pool's all-slabs lock (same lock that
  // find_open holds during its scan+bump, so no races).
  uint16_t rc = get_refcount(sem);
  if (rc > 1) {
    set_refcount(sem, rc - 1);
    return 0;
  }

  // Last reference — zero the tag, close handle, free to pool.
  set_name_gen_hash(sem, 0);
  set_refcount(sem, 0);
  { windows::ScopedNtHandle h(get_handle(sem)); }
  free_sem(sem);
  return 0;
}

int NamedSemaphore::unlink(const char *name) {
  constexpr int kPathMax = WIN_MAX_PATH * 3 + 255 + 32;
  auto marker_path_s = windows::byte_scratch(kPathMax);
  if (!marker_path_s)
    return ENOMEM;
  char *marker_path = marker_path_s.data();
  if (build_marker_path(name, marker_path, kPathMax) < 0)
    return ENOENT;

  auto nt_path_s = windows::path_scratch();
  if (!nt_path_s)
    return ENOMEM;
  WCHAR *nt_path = nt_path_s.data();
  using LIBC_NAMESPACE::cpp::string_view;
  string_view marker_sv(marker_path);
  auto nt = to_nt_path(marker_sv, nt_path, nt_path_s.size());
  if (!nt.has_value())
    return nt.error();
  size_t nt_len = nt.value();

  windows::nt_wstring_view marker_name(nt_path, nt_len);
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &marker_name);

  // NtDeleteFile removes the directory entry immediately. The marker file
  // was created with FILE_SHARE_DELETE, so this succeeds even while other
  // processes have it open. POSIX unlink semantics: name gone, data persists
  // until last handle closes.
  NTSTATUS st = ::NtDeleteFile(&oa);
  if (!NT_SUCCESS(st))
    return ENOENT;
  return 0;
}

int NamedSemaphore::post(sem_t *sem) {
  HANDLE h = get_handle(sem);
  NTSTATUS st = ::NtReleaseSemaphore(h, 1, nullptr);
  if (st == STATUS_SEMAPHORE_LIMIT_EXCEEDED)
    return EOVERFLOW;
  if (!NT_SUCCESS(st))
    return EINVAL;
  return 0;
}

int NamedSemaphore::wait(sem_t *sem) {
  HANDLE h = get_handle(sem);
  for (;;) {
    NTSTATUS st = ::NtWaitForSingleObject(h, /*Alertable=*/TRUE, nullptr);
    if (st == STATUS_SUCCESS)
      return 0;
    if (st == STATUS_USER_APC || st == STATUS_ALERTED) {
      // APC fired — signal handlers pended. Dispatch them and decide
      // whether to restart or return EINTR.
      if (signal_state::should_restart_syscall())
        continue;
      return EINTR;
    }
    return EINVAL;
  }
}

int NamedSemaphore::trywait(sem_t *sem) {
  HANDLE h = get_handle(sem);
  LARGE_INTEGER zero;
  zero.QuadPart = 0;
  NTSTATUS st = ::NtWaitForSingleObject(h, FALSE, &zero);
  if (st == STATUS_TIMEOUT)
    return EAGAIN;
  if (!NT_SUCCESS(st))
    return EINVAL;
  return 0;
}

int NamedSemaphore::timedwait(sem_t *sem,
                               const internal::AbsTimeout &timeout) {
  HANDLE h = get_handle(sem);
  for (;;) {
    // Recompute timeout on each iteration — time spent in signal handlers
    // during a previous APC wake counts against the deadline.
    LARGE_INTEGER nt_timeout = to_nt_timeout(timeout);
    if (nt_timeout.QuadPart == 0 && !timeout.is_realtime()) {
      // Monotonic deadline already passed (to_nt_timeout returned 0).
      // One last non-blocking try before reporting timeout.
      LARGE_INTEGER zero;
      zero.QuadPart = 0;
      NTSTATUS st = ::NtWaitForSingleObject(h, FALSE, &zero);
      if (st == STATUS_SUCCESS)
        return 0;
      return ETIMEDOUT;
    }

    NTSTATUS st = ::NtWaitForSingleObject(h, /*Alertable=*/TRUE, &nt_timeout);
    if (st == STATUS_SUCCESS)
      return 0;
    if (st == STATUS_TIMEOUT)
      return ETIMEDOUT;
    if (st == STATUS_USER_APC || st == STATUS_ALERTED) {
      if (signal_state::should_restart_syscall())
        continue;
      return EINTR;
    }
    return EINVAL;
  }
}

int NamedSemaphore::getvalue(sem_t *sem, int *sval) {
  HANDLE h = get_handle(sem);
  SEMAPHORE_BASIC_INFORMATION info;
  NTSTATUS st = ::NtQuerySemaphore(h, /*SemaphoreBasicInformation=*/0, &info,
                                   sizeof(info), nullptr);
  if (!NT_SUCCESS(st))
    return EINVAL;
  *sval = static_cast<int>(info.CurrentCount);
  return 0;
}

namespace internal {
static void named_semaphore_fini() {
  // Only destroy if we actually brought the pool up.
  if (g_named_semaphore_init.was_initialized())
    ::LIBC_NAMESPACE::g_sem_pool.destroy();
}
} // namespace internal

} // namespace LIBC_NAMESPACE_DECL

void LIBC_NAMESPACE::internal::named_semaphore_fork_reinit() {
  // Parent never used sem_open/sem_close → no pool state to repair and no
  // SipHash key to reset. Skip both.
  if (!LIBC_NAMESPACE::internal::g_named_semaphore_init.was_initialized())
    return;
  // SipHash key reset (process-token-keyed; parent's key would collide
  // with any other child of the same parent).
  LIBC_NAMESPACE::named_semaphore_fork_reinit();
  // Clear the gate so the next sem_* call in the child re-runs
  // named_semaphore_init_impl (idempotent pool.init + init_tls).
  LIBC_NAMESPACE::internal::g_named_semaphore_init.reset_for_fork();
}

LIBC_REGISTER_FINI(4, named_semaphore,
                   &::LIBC_NAMESPACE::internal::named_semaphore_fini)

// Register into `.libclzr$M` so exec_self_hollow() clears the gate
// during image swap. The pool's slab mappings, TLS slot index, and SipHash
// key are all process-state the new image must re-acquire.
LIBC_REGISTER_LAZY_RESET(named_semaphore,
                         ::LIBC_NAMESPACE::internal::g_named_semaphore_init)

LIBC_REGISTER_FORK_REINIT(named_semaphore,
                          ::LIBC_NAMESPACE::internal::kForkPrioNamedSemaphore,
                          &::LIBC_NAMESPACE::internal::named_semaphore_fork_reinit)
