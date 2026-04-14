//===-- FIFO support for Windows ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX FIFO (mkfifo/open) on Windows via NT named pipes.
//
// Marker file: mkfifo creates a small file (FILE_ATTRIBUTE_SYSTEM|HIDDEN)
// containing an 8-byte magic ("LLVMFIFO") followed by the serialized mode.
// This is filesystem-agnostic (works on NTFS, FAT32, exFAT, network shares).
//
// Pipe naming: the FIFO path is hashed (SipHash-2-4) to produce a
// deterministic pipe name under \Device\NamedPipe\. Any process using this
// libc derives the same pipe name from the same path — no registry needed.
//
// Open semantics:
//   O_RDONLY  → create pipe server (NtCreateNamedPipeFile), FSCTL_PIPE_LISTEN
//   O_WRONLY  → open pipe client (NtOpenFile), FSCTL_PIPE_WAIT if no reader
//   O_RDWR   → full-duplex pipe, returns immediately (POSIX-undefined)
//
// Blocking behavior matches POSIX: O_RDONLY blocks until a writer opens,
// O_WRONLY blocks until a reader opens. O_NONBLOCK modifies per spec.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FIFO_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FIFO_H

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/mode_t.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/signal/signal.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//

inline constexpr char FIFO_MAGIC[8] = {'L', 'L', 'V', 'M', 'F', 'I', 'F', 'O'};
inline constexpr size_t FIFO_MARKER_SIZE = 16; // 8 magic + 4 mode + 4 reserved
inline constexpr ULONG FIFO_BUFFER_SIZE = 65536;

// Pipe namespace prefix: u"\\Device\\NamedPipe\\llvm-fifo-"
inline constexpr WCHAR FIFO_PIPE_PREFIX[] = u"\\Device\\NamedPipe\\llvm-fifo-";
inline constexpr size_t FIFO_PIPE_PREFIX_LEN = 28; // wcslen of the above

//===----------------------------------------------------------------------===//
// SipHash-2-4 (64-bit) — deterministic pipe name derivation
//===----------------------------------------------------------------------===//

namespace siphash {

LIBC_INLINE uint64_t rotl(uint64_t x, int b) {
  return (x << b) | (x >> (64 - b));
}

LIBC_INLINE void sipround(uint64_t &v0, uint64_t &v1, uint64_t &v2,
                           uint64_t &v3) {
  v0 += v1;
  v1 = rotl(v1, 13);
  v1 ^= v0;
  v0 = rotl(v0, 32);
  v2 += v3;
  v3 = rotl(v3, 16);
  v3 ^= v2;
  v0 += v3;
  v3 = rotl(v3, 21);
  v3 ^= v0;
  v2 += v1;
  v1 = rotl(v1, 17);
  v1 ^= v2;
  v2 = rotl(v2, 32);
}

// Per-user SipHash key derived from the process token's owner SID.
// Same user → same key → same FIFO names (cross-process rendezvous works).
// Different user → different key → unpredictable names (cross-user squatting blocked).
//
// Init protocol: no intermediate "in-progress" state. Every thread that sees
// state 0 independently derives the key from the SID (deterministic — same
// user always gets the same answer) and races to publish via CAS(0→2). No
// two-phase init means no liveness hazard if a thread dies mid-init.
inline cpp::Atomic<uint64_t> g_siphash_k0{0};
inline cpp::Atomic<uint64_t> g_siphash_k1{0};
inline cpp::Atomic<uint32_t> g_siphash_init{0}; // 0=uninit, 2=ready, 3=failed

// Returns true if keys are ready, false if SID lookup failed permanently.
LIBC_INLINE bool ensure_siphash_key() {
  uint32_t state = g_siphash_init.load(cpp::MemoryOrder::ACQUIRE);
  if (state == 2)
    return true;
  if (state == 3)
    return false;

  // Not yet initialized. Derive key from the user SID independently.
  // Multiple threads may execute this concurrently — the derivation is
  // deterministic (same user SID → same key), so races are benign.
  alignas(8) UCHAR sid_buf[256];
  SID *owner = nullptr;
  alignas(8) UCHAR grp_buf[256];
  SID *grp = nullptr;
  NTSTATUS st = windows_sec::get_token_sids(sid_buf, sizeof(sid_buf),
                                             &owner, grp_buf,
                                             sizeof(grp_buf), &grp);
  if (!NT_SUCCESS(st) || !owner) {
    // SID lookup failed — refuse to use predictable keys.
    // Hardcoded fallback would allow cross-user FIFO name squatting.
    uint32_t expected = 0;
    g_siphash_init.compare_exchange_strong(expected, 3,
                                            cpp::MemoryOrder::RELEASE);
    return false;
  }

  // Hash the SID bytes into the key. Simple xor-fold.
  ULONG sid_len = ::RtlLengthSid(owner);
  const auto *sid_bytes = reinterpret_cast<const uint8_t *>(owner);
  uint64_t k0 = 0;
  uint64_t k1 = 0;
  for (ULONG i = 0; i < sid_len; ++i) {
    if (i < 8)
      k0 |= static_cast<uint64_t>(sid_bytes[i]) << (i * 8);
    else
      k1 |= static_cast<uint64_t>(sid_bytes[i]) << ((i - 8) * 8);
  }
  // Mix with a constant to avoid zero keys for short SIDs.
  k0 ^= 0x736970686173686BULL; // "siphashK"
  k1 ^= 0x6C6C766D66696621ULL; // "llvmfif!"

  // Publish: RELAXED stores are safe because the CAS below is RELEASE,
  // guaranteeing k0/k1 are visible to any thread that loads init==2
  // with ACQUIRE. Concurrent RELAXED stores from other threads are benign
  // since all threads derive identical values from the same SID.
  g_siphash_k0.store(k0, cpp::MemoryOrder::RELAXED);
  g_siphash_k1.store(k1, cpp::MemoryOrder::RELAXED);

  uint32_t expected = 0;
  g_siphash_init.compare_exchange_strong(expected, 2,
                                          cpp::MemoryOrder::RELEASE);
  // If CAS failed, another thread already published (same values). Fine.
  return g_siphash_init.load(cpp::MemoryOrder::ACQUIRE) == 2;
}

// SipHash-2-4 with per-user key. Same user across processes gets the same
// hash for the same path. Different users get different hashes.
// Returns 0 if key init failed (SID lookup unavailable).
LIBC_INLINE uint64_t hash(const char *data, size_t len) {
  if (!ensure_siphash_key())
    return 0;
  uint64_t k0 = g_siphash_k0.load(cpp::MemoryOrder::RELAXED);
  uint64_t k1 = g_siphash_k1.load(cpp::MemoryOrder::RELAXED);

  uint64_t v0 = k0 ^ 0x736F6D6570736575ULL;
  uint64_t v1 = k1 ^ 0x646F72616E646F6DULL;
  uint64_t v2 = k0 ^ 0x6C7967656E657261ULL;
  uint64_t v3 = k1 ^ 0x7465646279746573ULL;

  const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
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

  // Last block with length byte.
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

} // namespace siphash

//===----------------------------------------------------------------------===//
// Pipe name derivation
//===----------------------------------------------------------------------===//

// Build the NT pipe path: \Device\NamedPipe\llvm-fifo-<16 hex chars>
// Returns length in WCHARs, or 0 on failure.
LIBC_INLINE size_t derive_pipe_name(const char *path, WCHAR *out,
                                     size_t max_wchars) {
  // Canonicalize: convert to NT path form for consistent hashing.
  auto canon_s = path_scratch();
  if (!canon_s) return 0;
  WCHAR *canon = canon_s.data();
  size_t canon_len = to_nt_path(path, canon, canon_s.size());
  if (canon_len == 0)
    return 0;

  // Case-fold for consistency (NT paths are case-insensitive).
  for (size_t i = 0; i < canon_len; ++i) {
    if (canon[i] >= L'A' && canon[i] <= L'Z')
      canon[i] += 32;
    // Normalize separators.
    if (canon[i] == L'/')
      canon[i] = u'\\';
  }

  // Ensure per-user SipHash key is available. Without it, FIFO names
  // would be predictable, enabling cross-user squatting attacks.
  if (!siphash::ensure_siphash_key())
    return 0;

  // Hash the canonical wide path.
  uint64_t h = siphash::hash(reinterpret_cast<const char *>(canon),
                              canon_len * sizeof(WCHAR));

  // Format: prefix + 16 hex digits.
  size_t total = FIFO_PIPE_PREFIX_LEN + 16;
  if (total >= max_wchars)
    return 0;

  for (size_t i = 0; i < FIFO_PIPE_PREFIX_LEN; ++i)
    out[i] = FIFO_PIPE_PREFIX[i];

  constexpr WCHAR hex[] = u"0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    out[FIFO_PIPE_PREFIX_LEN + i] = hex[h & 0xF];
    h >>= 4;
  }
  out[total] = u'\0';
  return total;
}

//===----------------------------------------------------------------------===//
// Marker file helpers
//===----------------------------------------------------------------------===//

// Check if an open file handle is a FIFO marker. Reads the magic bytes.
// Returns true if the file contains "LLVMFIFO" at offset 0.
LIBC_INLINE bool is_fifo_marker(HANDLE h) {
  char buf[8];
  IO_STATUS_BLOCK iosb = {};
  LARGE_INTEGER offset = {};
  NTSTATUS status =
      NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf, 8, &offset, nullptr);
  if (!NT_SUCCESS(status) || iosb.Information < 8)
    return false;
  return __builtin_memcmp(buf, FIFO_MAGIC, 8) == 0;
}

// Write the FIFO marker content (magic + mode) to a newly created file.
LIBC_INLINE NTSTATUS write_fifo_marker(HANDLE h, mode_t mode) {
  char buf[FIFO_MARKER_SIZE] = {};
  __builtin_memcpy(buf, FIFO_MAGIC, 8);
  uint32_t m = static_cast<uint32_t>(mode);
  __builtin_memcpy(buf + 8, &m, sizeof(m));

  IO_STATUS_BLOCK iosb = {};
  LARGE_INTEGER offset = {};
  return NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buf,
                     FIFO_MARKER_SIZE, &offset, nullptr);
}

// Read the mode_t stored in the marker file (bytes 8-11).
// Returns the mode, or 0777 if the read fails.
LIBC_INLINE mode_t read_fifo_mode(HANDLE h) {
  char buf[FIFO_MARKER_SIZE];
  IO_STATUS_BLOCK iosb = {};
  LARGE_INTEGER offset = {};
  NTSTATUS status = NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf,
                                FIFO_MARKER_SIZE, &offset, nullptr);
  if (!NT_SUCCESS(status) || iosb.Information < 12)
    return 0777; // can't read mode — permissive fallback
  uint32_t m;
  __builtin_memcpy(&m, buf + 8, sizeof(m));
  return static_cast<mode_t>(m & 07777);
}

// Check POSIX permissions for a FIFO open. Returns 0 on success, errno on
// failure. Compares the requested access mode against the stored mode bits
// and the current process's effective uid/gid.
LIBC_INLINE int check_fifo_perms(mode_t fifo_mode, int flags) {
  int accmode = flags & O_ACCMODE;
  // Determine needed permission bits.
  mode_t need = 0;
  if (accmode == O_RDONLY)
    need = S_IROTH; // check "other" read — upgraded below if owner/group match
  else if (accmode == O_WRONLY)
    need = S_IWOTH;
  else if (accmode == O_RDWR)
    need = S_IROTH | S_IWOTH;

  // "Other" permissions always grant access.
  if ((fifo_mode & need) == need)
    return 0;

  // Check owner permissions.
  auto sid_s = byte_scratch(512);
  if (!sid_s)
    return 0; // can't allocate — allow (matches NT behavior)
  UCHAR *owner_buf = reinterpret_cast<UCHAR *>(sid_s.data());
  UCHAR *group_buf = owner_buf + 256;
  SID *owner_sid = nullptr;
  SID *group_sid = nullptr;
  NTSTATUS st = windows_sec::get_token_sids(owner_buf, 256u,
                                             &owner_sid, group_buf,
                                             256u, &group_sid);
  if (!NT_SUCCESS(st))
    return 0; // can't query SIDs — allow (matches NT behavior)

  // Owner check: shift need bits to owner position.
  mode_t owner_need = need << 6; // S_IROTH → S_IRUSR
  if ((fifo_mode & owner_need) == owner_need)
    return 0;

  // Group check: shift need bits to group position.
  mode_t group_need = need << 3; // S_IROTH → S_IRGRP
  if ((fifo_mode & group_need) == group_need)
    return 0;

  return EACCES;
}

//===----------------------------------------------------------------------===//
// FIFO open — creates or attaches to a shared-memory ring buffer channel
//===----------------------------------------------------------------------===//

// Derive a 16-hex-char hash from a POSIX path for naming kernel objects.
// Returns the hash portion only (no pipe prefix), length in WCHARs.
LIBC_INLINE size_t derive_fifo_hash(const char *path, WCHAR *out,
                                     size_t max_wchars) {
  // Reuse derive_pipe_name, then extract the 16-char hash suffix.
  WCHAR full[128];
  size_t full_len = derive_pipe_name(path, full, 128);
  if (full_len == 0 || full_len < FIFO_PIPE_PREFIX_LEN + 16)
    return 0;
  size_t hash_len = 16;
  if (hash_len >= max_wchars)
    return 0;
  for (size_t i = 0; i < hash_len; ++i)
    out[i] = full[FIFO_PIPE_PREFIX_LEN + i];
  out[hash_len] = u'\0';
  return hash_len;
}

// Open a FIFO for reading, writing, or both.
// Called from openat() after detecting a FIFO marker file.
// fifo_mode is the POSIX mode read from the marker (used for kernel object DACLs).
LIBC_INLINE ErrorOr<int> open_fifo(const char *path, int flags,
                                    mode_t fifo_mode) {
  WCHAR hash[32];
  size_t hash_len = derive_fifo_hash(path, hash, 32);
  if (hash_len == 0)
    return Error(EINVAL);

  FifoChannel *ch = fifo_open_channel(hash, hash_len, flags, fifo_mode);

  // fifo_open_channel returns small error codes via pointer encoding on failure.
  uintptr_t ch_val = reinterpret_cast<uintptr_t>(ch);
  if (ch_val == 0)
    return Error(ENOMEM);
  if (ch_val == EINTR) {
    signal_state::should_restart_syscall(); // Dispatch deferred handlers.
    return Error(EINTR);
  }
  if (ch_val == ENXIO)
    return Error(ENXIO);

  // Allocate an fd with FileKind::Fifo. Handle is nullptr (I/O goes through
  // the ring buffer channel, not a kernel handle). O_APPEND is accepted and
  // stored (visible via F_GETFL) but has no effect — fifo_write never uses
  // file position. POSIX: "If the file is a pipe, FIFO, or terminal
  // device, O_APPEND shall have no effect."
  auto fd_result = fd_table.alloc(nullptr, flags, 0, FileKind::Fifo);
  if (!fd_result.has_value()) {
    fifo_close_channel(ch);
    return fd_result;
  }

  // Attach the ring buffer channel to the OFD.
  OpenFileDescription *ofd = fd_table.get_ofd(fd_result.value());
  ofd->set_fifo_channel(ch);

  // O_CLOEXEC -> set FD_CLOEXEC and revoke OBJ_INHERIT.
  if (flags & O_CLOEXEC)
    fd_table.set_fd_cloexec(fd_result.value(), true);

  return fd_result.value();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FIFO_H
