//===-- File descriptor table implementation ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/fcntl_lock_table.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/File/windows/windows_file_ops_section.h"
#include "src/__support/OSUtil/windows/fd/file_ops_section.h"
#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/stdio_macros.h"
#include "src/__support/File/file.h"
#include "src/__support/File/windows/file.h"
#include "src/__support/OSUtil/windows/ipc/fifo_channel.h"
#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/OSUtil/windows/ipc/sockpair_channel.h"
#include "src/__support/OSUtil/windows/ipc/socket_state.h"
#include "src/__support/OSUtil/windows/fd/file_pool.h"
#include "src/__support/OSUtil/windows/io/file_type.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/fd/ofd_pool.h"
#include "src/__support/OSUtil/windows/process/spawn_runtime_data.h"
#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/OSUtil/windows/resource/rlimit_query.h"
#include "src/__support/macros/config.h"

#include "hdr/types/FILE.h"
#include "src/__support/CPP/new.h"
#include "src/__support/alloc-checker.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Process-wide singleton. Zero-initialized; subsystem init runs from fd_table_startup_init() (Phase 6).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wglobal-constructors"
FdTable fd_table;
#pragma clang diagnostic pop

// --- OpenFileDescription methods ---

static constexpr WCHAR CONDRV_PREFIX[] = u"\\Device\\ConDrv";

LIBC_INLINE bool is_wchar_ascii_ci_equal(WCHAR lhs, WCHAR rhs) {
  if (lhs >= u'A' && lhs <= u'Z')
    lhs += 32;
  if (rhs >= u'A' && rhs <= u'Z')
    rhs += 32;
  return lhs == rhs;
}

// Match "\Device\ConDrv" exactly or as a prefix followed by '\'.
LIBC_INLINE bool has_condrv_namespace_name(const UNICODE_STRING &name) {
  if (!name.Buffer || name.Length == 0)
    return false;

  constexpr USHORT PREFIX_CHARS = sizeof(CONDRV_PREFIX) / sizeof(WCHAR) - 1;
  const USHORT name_chars = name.Length / sizeof(WCHAR);
  if (name_chars < PREFIX_CHARS)
    return false;

  for (USHORT i = 0; i < PREFIX_CHARS; ++i) {
    if (!is_wchar_ascii_ci_equal(name.Buffer[i], CONDRV_PREFIX[i]))
      return false;
  }

  // Exact match or prefix followed by a path separator.
  return name_chars == PREFIX_CHARS || name.Buffer[PREFIX_CHARS] == u'\\';
}

static bool probe_condrv_object_name(HANDLE h) {
  // NtQueryObject(ObjectNameInformation) returns the NT device path.
  // Stack buffer sized for typical console paths (~60 chars + header).
  alignas(8) uint8_t buf[256];
  ULONG ret_len = 0;
  NTSTATUS s = NtQueryObject(h, ObjectNameInformation, buf, sizeof(buf),
                              &ret_len);
  if (!NT_SUCCESS(s))
    return false;

  auto *oni = reinterpret_cast<OBJECT_NAME_INFORMATION *>(buf);
  return has_condrv_namespace_name(oni->Name);
}

static bool probe_condrv_console_mode(HANDLE h) {
  DWORD mode = 0;
  return NT_SUCCESS(condrv::get_console_mode_on(h, nullptr, &mode));
}

// Probe whether a handle refers to a ConDrv console device.
// The object-name check (NtQueryObject) is authoritative for FILE_TYPE_CHAR
// handles: if the path isn't \Device\ConDrv\..., it's a serial port, NUL, or
// other char device — no need for the expensive console-mode IPC roundtrip.
// For FILE_TYPE_UNKNOWN handles the name query may fail due to restricted
// access, so we fall back to console-mode probing as secondary evidence.
static bool probe_condrv(HANDLE h, DWORD ftype) {
  if (probe_condrv_object_name(h))
    return true;
  // Only fall through to console-mode probe when the file type doesn't already
  // rule out a console. FILE_TYPE_CHAR with a non-ConDrv name is definitively
  // not a console; FILE_TYPE_UNKNOWN may be a console with restricted queries.
  if (ftype == FILE_TYPE_CHAR)
    return false;
  return probe_condrv_console_mode(h);
}

void OpenFileDescription::init_common(HANDLE h, int open_flags, FileKind k,
                                      const FileOps *o) {
  handle = h;
  ops = o;
  refcount.store(1, cpp::MemoryOrder::RELAXED);
  // Strip O_PATH from status_flags -- it's immutable and tracked separately
  // in immutable_flags. status_flags only holds mutable flags (O_APPEND,
  // O_NONBLOCK) that can be changed via fcntl(F_SETFL).
  status_flags.store(open_flags & ~(O_ACCMODE | O_PATH),
                     cpp::MemoryOrder::RELAXED);
  kind = k;
  access_mode = static_cast<uint8_t>(open_flags & O_ACCMODE);
  immutable_flags = (open_flags & O_PATH) ? IMMUTABLE_O_PATH : 0;
  aux.raw[0] = 0;
  aux.raw[1] = 0;
  file_ptr = nullptr;

  // Disk fds need explicit atomic initialization of position and section_handle
  // (zero-init via aux.raw is bitwise correct for these atomics on all targets,
  // but be explicit for clarity and to satisfy sanitizers).
  if (k == FileKind::Disk) {
    aux.disk.position.store(0, cpp::MemoryOrder::RELAXED);
    aux.disk.section_handle.store(nullptr, cpp::MemoryOrder::RELAXED);
  }
}

void OpenFileDescription::reinit_for_reopen(HANDLE h, int open_flags,
                                             FileKind k, const FileOps *o) {
  // Preserve refcount and file_ptr — the OFD is still live.
  handle = h;
  ops = o;
  // RELEASE (not RELAXED): this OFD is live — concurrent threads holding
  // dup'd fds may read status_flags via fcntl(F_GETFL). The release store
  // ensures they see a consistent flag set after the reinit. init_common()
  // uses RELAXED because a freshly allocated OFD is not yet visible to
  // other threads.
  status_flags.store(open_flags & ~(O_ACCMODE | O_PATH),
                     cpp::MemoryOrder::RELEASE);
  kind = k;
  access_mode = static_cast<uint8_t>(open_flags & O_ACCMODE);
  immutable_flags = (open_flags & O_PATH) ? IMMUTABLE_O_PATH : 0;
  aux.raw[0] = 0;
  aux.raw[1] = 0;

  if (k == FileKind::Disk) {
    aux.disk.position.store(0, cpp::MemoryOrder::RELAXED);
    aux.disk.section_handle.store(nullptr, cpp::MemoryOrder::RELAXED);
  }
}

void OpenFileDescription::release() {
  uint32_t old = refcount.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
  if (old != 1)
    return; // Not the last reference.

  // Last reference -- kind-specific cleanup via ops table.
  // release_aux returns true if the caller should NtClose the handle,
  // false if the subsystem owns the handle lifetime (e.g., PTY master).
  bool close_handle = true;
  if (ops && ops->release_aux)
    close_handle = ops->release_aux(this);

  if (close_handle && handle)
    NtClose(handle);
  handle = nullptr;

  file_ptr = nullptr;
  ofd_pool::free(this);
}

// --- Handle classification ---

FileKind FdTable::classify_handle(HANDLE h, DWORD ftype) {
  switch (ftype) {
  case FILE_TYPE_DISK:
    return FileKind::Disk;
  case FILE_TYPE_CHAR:
    if (probe_condrv(h, ftype))
      return FileKind::ConDrv;
    return FileKind::Char;
  case FILE_TYPE_PIPE:
    return FileKind::Pipe;
  default: // FILE_TYPE_UNKNOWN
    if (probe_condrv(h, ftype))
      return FileKind::ConDrv;
    return FileKind::Char;
  }
}

// --- File type / kind helpers ---

DWORD FdTable::query_file_type(HANDLE handle) {
  return windows::query_file_type(handle);
}

// kind_to_ops() is defined in file_ops_table.cpp.

// --- Internal slot helpers ---

ErrorOr<int> FdTable::alloc_slot(OpenFileDescription *ofd, int min_fd,
                                 bool cloexec) {
  int hint = alloc_hint_.load(cpp::MemoryOrder::RELAXED);
  int start = (min_fd > hint) ? min_fd : hint;

  // Bit-as-claim-lock scan. The occupancy bitmap bit doubles as a
  // per-slot acquire token: a winning fetch_or (bit 0→1) grants exclusive
  // permission to install into the slot. Close's unconditional
  // fetch_and (mark_dead → bitmap.clear) is the release; an installer
  // that races ahead of release will observe prev=1 on its fetch_or and
  // retry, so no install can commit a slot CAS until close's bit-clear
  // has actually landed. That closes the old (slot CAS-null → install
  // CAS-ofd → sibling bit-clear) race without any extra atomics — the
  // fetch_or just moves before the slot CAS instead of after.
  //
  // Walk: tzcnt over ~bits gives the next free candidate; blsr advances.
  // try_acquire on each candidate filters out bits another thread has
  // already claimed.
  //
  // The slot.ptr()!=null verification remains as defense-in-depth. If an
  // install_fd/dup2 path on the same slot is mid-sequence (it acquired
  // the bit but hasn't yet CAS'd the slot), we relinquish our claim
  // (bitmap.clear) so the winner can finish.
  for (int fd = start; fd >= 0;) {
    unsigned ufd = static_cast<unsigned>(fd);
    unsigned ci = ufd >> CHUNK_SHIFT;
    unsigned si_start = ufd & CHUNK_MASK;

    // Pin the chunk: ensure committed + increment live_count. Our pin
    // becomes the slot's live reference on successful install.
    FdSlot *chunk = pool_.acquire_for_scan(ci);
    if (!chunk)
      return Error(ENOMEM); // Chunk allocation failed.

    auto *meta = Pool::meta_for(chunk);
    bool found = false;

    unsigned w_start = si_start / 64;
    unsigned b_start = si_start & 63u;
    for (unsigned w = w_start; w < Pool::BITMAP_WORDS && !found; ++w) {
      uint64_t bits =
          meta->bitmap.template word_at<cpp::MemoryOrder::ACQUIRE>(w);
      uint64_t free_bits = ~bits;
      // Clamp the first word to start at or above si_start.
      if (w == w_start)
        free_bits &= ~((1ULL << b_start) - 1ULL);

      while (free_bits) {
        unsigned b = static_cast<unsigned>(__builtin_ctzll(free_bits));
        unsigned si = w * 64 + b;
        FdSlot *slot = &chunk[si];

        // Acquire the claim lock on this slot's bit. If another thread
        // beat us (fetch_or returned prev=1), the bit state we saw in
        // `bits` was stale; move on.
        if (!meta->bitmap.try_acquire(si)) {
          free_bits &= free_bits - 1;
          continue;
        }

        // We hold the claim. Load the slot. If ptr is non-null, a
        // sibling install_fd/dup2 on this same index acquired the bit
        // before us and is mid-CAS. Release our claim and move on.
        TaggedOfd current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
        if (current.ptr() != nullptr) {
          meta->bitmap.clear(si);
          free_bits &= free_bits - 1;
          continue;
        }

        // CAS: preserve generation, install ptr + CLOEXEC. On failure,
        // release claim and keep scanning.
        TaggedOfd desired = current.with_ptr(ofd).with_cloexec(cloexec);
        if (!slot->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
          meta->bitmap.clear(si);
          free_bits &= free_bits - 1;
          continue;
        }

        fd = static_cast<int>((ci << CHUNK_SHIFT) | si);
        // bit already set by try_acquire; live_count already +1 from pin.

        // Advance hint past this fd.
        int old_hint = alloc_hint_.load(cpp::MemoryOrder::RELAXED);
        while (old_hint <= fd) {
          if (alloc_hint_.compare_exchange_weak(old_hint, fd + 1,
                                                cpp::MemoryOrder::RELAXED))
            break;
        }

        // Advance high-water mark.
        int hw = high_water_.load(cpp::MemoryOrder::RELAXED);
        while (fd + 1 > hw) {
          if (high_water_.compare_exchange_weak(hw, fd + 1,
                                                cpp::MemoryOrder::RELAXED))
            break;
        }

        found = true;
        break;
      }
    }

    if (found)
      return fd;

    // Exhausted this chunk. Release the scan pin (may trigger decommit).
    pool_.release_scan_ref(ci);

    // Move to the start of the next chunk.
    fd = static_cast<int>(static_cast<unsigned>(ci + 1) << CHUNK_SHIFT);
  }
  // int overflow (fd < 0) -- effectively out of address space.
  return Error(ENOMEM);
}

OpenFileDescription *FdTable::release_slot(int fd) {
  FdSlot *slot = slot_for(fd);
  if (!slot)
    return nullptr;

  // Atomically clear the pointer, CLOEXEC, and bump the generation in one CAS.
  TaggedOfd current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
  OpenFileDescription *old = current.ptr();
  if (!old)
    return nullptr;

  // CAS loop: bump generation and null the pointer+CLOEXEC atomically.
  TaggedOfd desired = current.next_gen_null();
  while (!slot->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
    old = current.ptr();
    if (!old)
      return nullptr; // Another thread released it first.
    desired = current.next_gen_null();
  }

  // Update pool bitmap + live count. May trigger chunk decommit if this
  // was the last live slot in the chunk.
  pool_.mark_dead(static_cast<unsigned>(fd));

  // Retreat alloc hint.
  int old_hint = alloc_hint_.load(cpp::MemoryOrder::RELAXED);
  while (fd < old_hint) {
    if (alloc_hint_.compare_exchange_weak(old_hint, fd,
                                           cpp::MemoryOrder::RELAXED))
      break;
  }

  return old;
}

// --- Init ---

void FdTable::init() {
  pool_.init();
  alloc_hint_.store(0, cpp::MemoryOrder::RELAXED);
  high_water_.store(0, cpp::MemoryOrder::RELAXED);

  // Pre-allocate chunk 0 for the standard fds.
  pool_.ensure_chunk(0);
}

// --- Alloc ---

ErrorOr<int> FdTable::alloc(HANDLE h, int open_flags, int min_fd,
                            FileKind kind_hint) {
  // RLIMIT_NOFILE: reject early if min_fd already exceeds the soft limit.
  rlim_t nofile = windows::get_nofile_limit();
  if (nofile != RLIM_INFINITY && min_fd >= static_cast<int>(nofile))
    return Error(EMFILE);

  // Pool-alloc before classify_handle — classify issues an NT syscall
  // (query_file_type), so avoid wasting it if the pool is exhausted.
  OpenFileDescription *ofd = ofd_pool::alloc();
  if (!ofd)
    return Error(ENOMEM);

  FileKind k = kind_hint;
  if (k == FileKind::Auto) {
    DWORD ftype = query_file_type(h);
    k = classify_handle(h, ftype);
  }

  ofd->init_common(h, open_flags, k, kind_to_ops(k));

  auto result = alloc_slot(ofd, min_fd);
  if (!result.has_value()) {
    ofd->refcount.store(0, cpp::MemoryOrder::RELAXED);
    ofd->handle = nullptr; // Don't close caller's handle.
    ofd_pool::free(ofd);
    return result;
  }

  // RLIMIT_NOFILE: authoritative post-check — alloc_slot scans upward,
  // so the actual fd may exceed the limit even if min_fd was below it.
  // Use release_slot's return to avoid double-free if a concurrent close
  // raced us (pathological but possible under fd iteration).
  int fd = result.value();
  if (nofile != RLIM_INFINITY && fd >= static_cast<int>(nofile)) {
    OpenFileDescription *released = release_slot(fd);
    if (released) {
      released->handle = nullptr; // Don't close caller's handle.
      released->release();
    }
    return Error(EMFILE);
  }

  return result;
}

ErrorOr<int> FdTable::alloc_synthetic(FileKind kind, int open_flags,
                                      int min_fd, uint8_t aux_byte) {
  rlim_t nofile = windows::get_nofile_limit();
  if (nofile != RLIM_INFINITY && min_fd >= static_cast<int>(nofile))
    return Error(EMFILE);

  OpenFileDescription *ofd = ofd_pool::alloc();
  if (!ofd)
    return Error(ENOMEM);

  ofd->init_common(nullptr, open_flags, kind, kind_to_ops(kind));
  // Write aux_byte before alloc_slot publishes the fd to other threads.
  // Used by VirtualDir to set dir_kind atomically with slot creation.
  if (aux_byte)
    reinterpret_cast<uint8_t *>(&ofd->aux)[0] = aux_byte;

  auto result = alloc_slot(ofd, min_fd);
  if (!result.has_value()) {
    ofd->refcount.store(0, cpp::MemoryOrder::RELAXED);
    ofd_pool::free(ofd);
    return result;
  }

  // RLIMIT_NOFILE: authoritative post-check.
  int fd = result.value();
  if (nofile != RLIM_INFINITY && fd >= static_cast<int>(nofile)) {
    OpenFileDescription *released = release_slot(fd);
    if (released)
      released->release();
    return Error(EMFILE);
  }

  return result;
}

ErrorOr<int> FdTable::alloc_at(int fd, HANDLE h, int open_flags) {
  if (fd < 0)
    return Error(EBADF);

  // RLIMIT_NOFILE: the target fd must be below the soft limit.
  rlim_t nofile = windows::get_nofile_limit();
  if (nofile != RLIM_INFINITY && fd >= static_cast<int>(nofile))
    return Error(EMFILE);

  OpenFileDescription *ofd = ofd_pool::alloc();
  if (!ofd)
    return Error(ENOMEM);

  DWORD ftype = query_file_type(h);
  FileKind k = classify_handle(h, ftype);
  ofd->init_common(h, open_flags, k, kind_to_ops(k));

  // Pin the chunk to prevent concurrent decommit during the CAS.
  unsigned ufd = static_cast<unsigned>(fd);
  unsigned ci = ufd >> CHUNK_SHIFT;
  unsigned local = ufd & CHUNK_MASK;
  FdSlot *chunk = pool_.acquire_for_scan(ci);
  if (!chunk) {
    ofd->refcount.store(0, cpp::MemoryOrder::RELAXED);
    ofd->handle = nullptr;
    ofd_pool::free(ofd);
    return Error(ENOMEM);
  }
  auto *meta = Pool::meta_for(chunk);
  FdSlot *slot = &chunk[local];

  // Install the new ofd, either as an in-place eviction (slot currently
  // live → bit stays 1, no bitmap transition needed) or as a fresh fill
  // (slot currently null → acquire the bit claim, then slot CAS). The
  // try_acquire gate on the fill path is what keeps this race-free: a
  // concurrent close's bitmap.clear acts as the release of the prior
  // occupant's claim, and any installer that races ahead of the clear
  // sees prev=1 on try_acquire and re-loops until the close's clear lands.
  TaggedOfd current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    if (current.ptr() != nullptr) {
      // Eviction-in-place: slot stays continuously live. Bit stays 1.
      TaggedOfd desired = TaggedOfd::make(ofd, current.gen() + 1);
      if (slot->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
        OpenFileDescription *old = current.ptr();
        pool_.release_scan_ref(ci); // pin redundant; old's live ref carries
        old->release();
        break;
      }
      current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
      continue;
    }

    // Fresh fill: acquire the bit lock before the slot CAS. If a racing
    // close hasn't yet cleared the bit, try_acquire returns false and we
    // re-loop; the close's upcoming bitmap.clear will release for us.
    if (!meta->bitmap.try_acquire(local)) {
      current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
      continue;
    }
    TaggedOfd desired = current.with_ptr(ofd);
    if (slot->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
      // Bit set by try_acquire; pin's live_count+1 becomes slot's live ref.
      break;
    }
    // Slot CAS lost the race. Release our claim and re-examine.
    meta->bitmap.clear(local);
    current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
  }

  // Advance high-water mark.
  int hw = high_water_.load(cpp::MemoryOrder::RELAXED);
  while (fd + 1 > hw) {
    if (high_water_.compare_exchange_weak(hw, fd + 1,
                                           cpp::MemoryOrder::RELAXED))
      break;
  }

  return fd;
}

// --- Release ---

ErrorOr<int> FdTable::release(int fd) {
  OpenFileDescription *old = release_slot(fd);
  if (!old)
    return Error(EBADF);

  // POSIX: close() releases ALL byte-range locks on the same file.
  // Only check disk fds (locks don't apply to pipes/sockets/devices).
  // The lock_table_empty() fast path avoids syscalls when no locks are held.
  if (old->kind == FileKind::Disk && old->handle && !lock_table_empty())
    lock_table_release_file(old->handle);

  old->release();
  return 0;
}

void FdTable::free_slot(int fd) {
  // Used by fclose: resources already closed by platform_close.
  // Release the slot but don't call ofd->release() -- the OFD's resources
  // were already cleaned up. We still decrement the refcount so the OFD
  // is freed back to the pool.
  OpenFileDescription *old = release_slot(fd);
  if (old) {
    // Handle already closed by platform_close. Null it out
    // so release() doesn't double-close.
    old->handle = nullptr;
    old->release();
  }
}

// --- Dup ---

// Safely acquire a reference to the OFD in a slot, returning the OFD pointer
// and the tagged snapshot on success. Uses a CAS loop on the refcount that
// rejects refcount==0 (OFD is being freed), then re-checks the slot to detect
// concurrent close/reopen. This eliminates the race window in the old
// load-bump-recheck pattern where fetch_add could operate on freed/reused
// pool memory.
//
// Returns nullptr if the slot is empty, concurrently closed, or the OFD is
// mid-teardown. Caller should retry or return EBADF as appropriate.
static OpenFileDescription *try_acquire_ofd(FdSlot *slot, TaggedOfd &out_tag) {
  // Outer loop: reload the slot if the OFD is mid-teardown (refcount==0)
  // or the slot changed between the refcount CAS and the re-check.
  for (;;) {
    TaggedOfd tagged = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
    OpenFileDescription *ofd = tagged.ptr();
    if (!ofd)
      return nullptr;

    // CAS loop: atomically bump refcount only if > 0.
    // If refcount==0, the OFD is being freed (release() has decremented it
    // to 0 but may not have returned it to the pool yet). We must not touch
    // it — reload the slot and retry, since the slot will soon be nulled by
    // release_slot or already has been.
    uint32_t cur = ofd->refcount.load(cpp::MemoryOrder::ACQUIRE);
    for (;;) {
      if (cur == 0)
        break; // OFD is dying — retry from slot load.
      if (ofd->refcount.compare_exchange_weak(cur, cur + 1,
                                              cpp::MemoryOrder::ACQ_REL,
                                              cpp::MemoryOrder::ACQUIRE))
        break;
    }
    if (cur == 0)
      continue; // Retry from slot load.

    // We now hold a live reference (refcount was > 0 and we incremented it).
    // Re-check the slot: if the tagged word changed (pointer, generation, or
    // CLOEXEC), the fd was closed/reopened concurrently. Our reference is to
    // a still-live OFD (refcount > 1 when we CAS'd), so release() is safe.
    if (slot->load_tagged(cpp::MemoryOrder::ACQUIRE) != tagged) {
      ofd->release();
      return nullptr;
    }

    out_tag = tagged;
    return ofd;
  }
}

ErrorOr<int> FdTable::dup(int old_fd, int min_fd, int new_fd_flags) {
  // RLIMIT_NOFILE: reject early if min_fd already exceeds the soft limit.
  rlim_t nofile = windows::get_nofile_limit();
  if (nofile != RLIM_INFINITY && min_fd >= static_cast<int>(nofile))
    return Error(EMFILE);

  FdSlot *slot = get_slot(old_fd);
  if (!slot)
    return Error(EBADF);

  TaggedOfd tagged;
  OpenFileDescription *ofd = try_acquire_ofd(slot, tagged);
  if (!ofd)
    return Error(EBADF);

  bool cloexec = (new_fd_flags & FD_CLOEXEC) != 0;
  auto result = alloc_slot(ofd, min_fd, cloexec);
  if (!result.has_value()) {
    ofd->release(); // Undo the refcount bump.
    return result;
  }

  // RLIMIT_NOFILE: authoritative post-check.
  int fd = result.value();
  if (nofile != RLIM_INFINITY && fd >= static_cast<int>(nofile)) {
    if (release_slot(fd))
      ofd->release();
    return Error(EMFILE);
  }

  return result;
}

ErrorOr<int> FdTable::dup_to(int old_fd, int new_fd, int new_fd_flags) {
  // Validate new_fd before touching the source slot to avoid a wasted
  // refcount bump-then-release on clearly invalid arguments.
  if (new_fd < 0)
    return Error(EBADF);

  // RLIMIT_NOFILE: the target fd must be below the soft limit.
  rlim_t nofile = windows::get_nofile_limit();
  if (nofile != RLIM_INFINITY && new_fd >= static_cast<int>(nofile))
    return Error(EMFILE);

  FdSlot *slot = get_slot(old_fd);
  if (!slot)
    return Error(EBADF);

  TaggedOfd tagged;
  OpenFileDescription *ofd = try_acquire_ofd(slot, tagged);
  if (!ofd)
    return Error(EBADF);

  // Pin the target chunk to prevent concurrent decommit during the CAS.
  unsigned unfd = static_cast<unsigned>(new_fd);
  unsigned ci = unfd >> CHUNK_SHIFT;
  unsigned local = unfd & CHUNK_MASK;
  FdSlot *chunk = pool_.acquire_for_scan(ci);
  if (!chunk) {
    ofd->release();
    return Error(ENOMEM);
  }
  auto *meta = Pool::meta_for(chunk);
  FdSlot *target = &chunk[local];

  bool cloexec = (new_fd_flags & FD_CLOEXEC) != 0;

  // Same claim-lock protocol as alloc_at: eviction-in-place keeps bit at
  // 1 and needs no bitmap op; fresh fill acquires via try_acquire before
  // the slot CAS, blocking behind a racing close's bitmap.clear.
  TaggedOfd current = target->load_tagged(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    if (current.ptr() != nullptr) {
      // Eviction-in-place.
      TaggedOfd desired = TaggedOfd::make(ofd, current.gen() + 1, cloexec);
      if (target->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
        OpenFileDescription *old = current.ptr();
        pool_.release_scan_ref(ci);
        old->release();
        break;
      }
      current = target->load_tagged(cpp::MemoryOrder::ACQUIRE);
      continue;
    }

    // Fresh fill.
    if (!meta->bitmap.try_acquire(local)) {
      current = target->load_tagged(cpp::MemoryOrder::ACQUIRE);
      continue;
    }
    TaggedOfd desired = current.with_ptr(ofd).with_cloexec(cloexec);
    if (target->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
      break;
    }
    meta->bitmap.clear(local);
    current = target->load_tagged(cpp::MemoryOrder::ACQUIRE);
    continue;
  }

  // Advance high-water mark.
  int hw = high_water_.load(cpp::MemoryOrder::RELAXED);
  while (new_fd + 1 > hw) {
    if (high_water_.compare_exchange_weak(hw, new_fd + 1,
                                           cpp::MemoryOrder::RELAXED))
      break;
  }

  // Only republish stdio globals when a standard fd (0-2) was actually
  // The global stdin/stdout/stderr pointers are compile-time-stable to the
  // StdStreamStorage union address (see src/stdio/windows/std{in,out,err}.cpp)
  // — dup2 onto fd 0/1/2 migrates the same file_ptr to the new OFD but does
  // not move the FILE * visible to consumers. No publish step required.

  return new_fd;
}

// --- Lookup ---

ErrorOr<HANDLE> FdTable::get(int fd) {
  OpenFileDescription *ofd = get_ofd(fd);
  if (!ofd)
    return Error(EBADF);
  return ofd->handle;
}

OpenFileDescription *FdTable::get_ofd(int fd) {
  FdSlot *slot = slot_for(fd);
  if (!slot)
    return nullptr;
  return slot->load_ofd(cpp::MemoryOrder::ACQUIRE);
}

FdSlot *FdTable::get_slot(int fd) { return slot_for(fd); }

// --- Bitmap-based iteration ---

void FdTable::for_each_live(FdCallback cb, void *ctx) {
  // Thunk: IndexedPool callback signature uses unsigned idx + T*,
  // FdTable callback uses int fd + FdSlot*. The cast is safe because
  // fd values are always non-negative (unsigned -> int is well-defined
  // for values within int range, which all valid fds are).
  struct Ctx {
    FdCallback cb;
    void *user_ctx;
  };
  Ctx thunk_ctx{cb, ctx};
  pool_.for_each_live(
      [](unsigned idx, FdSlot *slot, void *opaque) {
        auto *c = static_cast<Ctx *>(opaque);
        c->cb(static_cast<int>(idx), slot, c->user_ctx);
      },
      &thunk_ctx);
}

// --- Standard fd initialization ---

// stdin/stdout/stderr are backed by union-typed static storage defined in
// src/stdio/windows/std{in,out,err}.cpp. The unions are constexpr-init'd
// with a WindowsFile(sync, handle=nullptr, buffer, ...) at link time; the
// FILE * pointers (LIBC_NAMESPACE::stdin etc.) point at those addresses
// directly, compile-time-stable. bind_std_fd wires each OFD's file_ptr
// at the matching union storage (no file_pool allocation) and late-binds
// the PEB handle via WindowsFile::set_handle. See
// libc/src/__support/File/windows/file.h::StdStreamStorage for layout
// rationale.
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
extern uint8_t stdin_buffer[];
extern uint8_t stdout_buffer[];
extern uint8_t stderr_buffer[];
extern StdStreamStorage stdin_storage;
extern StdStreamStorage stdout_storage;
extern StdStreamStorage stderr_storage;
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct StdFdInfo {
  int flags;
  uint8_t *buf;
  size_t buf_size;
  int buf_mode;
  File::ModeFlags modeflags;
  StdStreamStorage *storage;
};

static const StdFdInfo STD_FD_LAYOUT[3] = {
    {O_RDONLY, ::LIBC_NAMESPACE::stdin_buffer, 512, _IOFBF,
     File::ModeFlags(File::OpenMode::READ), &::LIBC_NAMESPACE::stdin_storage},
    {O_WRONLY, ::LIBC_NAMESPACE::stdout_buffer, 1024, _IOLBF,
     File::ModeFlags(File::OpenMode::APPEND),
     &::LIBC_NAMESPACE::stdout_storage},
    {O_WRONLY, ::LIBC_NAMESPACE::stderr_buffer, 1, _IONBF,
     File::ModeFlags(File::OpenMode::APPEND),
     &::LIBC_NAMESPACE::stderr_storage},
};

// Transitions a StdStreamStorage union from its current active member to
// the one implied by the resolved kind. Only fd==0 + FileKind::Disk opts
// into IoRingFile (the async read pipeline wins for large disk-redirected
// input); stdout / stderr stay Sync because IoRingFile cannot preserve
// _IOLBF / _IONBF buffer-mode semantics. If the 16KB IoRingFile buffer
// allocation fails, falls back to sync — correctness preserved, perf lost.
LIBC_INLINE void bind_std_storage(const StdFdInfo &info, HANDLE handle,
                                  FileKind kind, int fd) {
  auto *storage = info.storage;
  bool want_ioring = (fd == 0 && kind == FileKind::Disk);
  storage->destruct_active();
  if (want_ioring) {
    AllocChecker ac;
    uint8_t *io_buf = new (ac) uint8_t[IoRingFile::IORING_BUFFER_SIZE];
    if (ac) {
      storage->construct_ioring(handle, io_buf,
                                IoRingFile::IORING_BUFFER_SIZE, _IOFBF,
                                info.modeflags, fd);
      return;
    }
    // Fall through to sync on allocation failure.
  }
  storage->construct_sync(handle, info.buf, info.buf_size, info.buf_mode,
                          info.modeflags, fd);
}

void FdTable::bind_std_fd(int fd, HANDLE handle) {
  bind_std_fd_known_type(fd, handle, FileKind::Auto);
}

void FdTable::bind_std_fd_known_type(int fd, HANDLE handle,
                                     FileKind known_kind) {
  if (fd < 0)
    return;
  unsigned ufd = static_cast<unsigned>(fd);
  unsigned ci = ufd >> CHUNK_SHIFT;
  unsigned local = ufd & CHUNK_MASK;

  // Allocate and populate the new OFD up front. On any failure path before
  // a successful slot CAS, we roll back the OFD to avoid leaks.
  OpenFileDescription *new_ofd = ofd_pool::alloc();
  if (!new_ofd)
    return;

  bool invalidate = (!handle || handle == INVALID_HANDLE_VALUE);
  if (invalidate) {
    new_ofd->init_common(nullptr, 0, FileKind::Char,
                         kind_to_ops(FileKind::Char));
  } else {
    FileKind k = known_kind;
    if (k == FileKind::Auto) {
      DWORD ftype = query_file_type(handle);
      k = classify_handle(handle, ftype);
    }
    new_ofd->init_common(handle, STD_FD_LAYOUT[fd].flags, k, kind_to_ops(k));
  }

  FdSlot *chunk = pool_.acquire_for_scan(ci);
  if (!chunk) {
    new_ofd->refcount.store(0, cpp::MemoryOrder::RELAXED);
    new_ofd->handle = nullptr;
    ofd_pool::free(new_ofd);
    return;
  }
  auto *meta = Pool::meta_for(chunk);
  FdSlot *slot = &chunk[local];

  // Claim-lock OFD publication, same shape as alloc_at / dup_to.
  //
  // stdio FILE* identity preservation: on eviction, new_ofd tentatively
  // takes over old_ofd's file_ptr (same pointer value). A concurrent
  // reader who observed old_ofd's FILE* before rebind still has a valid
  // pointer — the File object at that address survives the OFD swap and
  // gets reconstructed in place post-CAS following the freopen_impl
  // pattern (stdio_file_ops.cpp:207). Identity is the SAME address; the
  // state behind it transitions.
  OpenFileDescription *old_ofd = nullptr;
  TaggedOfd current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    if (current.ptr() != nullptr) {
      // Eviction. Snapshot the file_ptr so identity migrates with new_ofd.
      OpenFileDescription *candidate_old = current.ptr();
      new_ofd->file_ptr = candidate_old->file_ptr;

      TaggedOfd desired = TaggedOfd::make(new_ofd, current.gen() + 1);
      if (slot->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
        old_ofd = candidate_old;
        pool_.release_scan_ref(ci); // pin redundant; old's live ref carries
        break;
      }
      // CAS lost; clear tentative file_ptr so next iteration re-evaluates.
      new_ofd->file_ptr = nullptr;
      current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
      continue;
    }
    // Fresh fill.
    if (!meta->bitmap.try_acquire(local)) {
      current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
      continue;
    }
    TaggedOfd desired = current.with_ptr(new_ofd);
    if (slot->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
      // Bit set by try_acquire; pin's live_count+1 becomes slot's live ref.
      break;
    }
    meta->bitmap.clear(local);
    current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
  }

  // File reconstruction mirrors freopen_impl (stdio_file_ops.cpp:259-398):
  //   flush → remove_file → placement-new → add_file.
  //
  // flush() acquires File::mutex and drains in-flight I/O so any reader
  // that entered File methods before us completes before we reconstruct.
  // remove_file unlinks from the global stdio list while the placement-new
  // overwrites internal fields (preserving prev/next via remove/add is
  // safer than save/restore because a concurrent global-list walker can't
  // observe the File mid-construction).
  //
  // close_nt_resources is deliberately skipped: the console handles behind
  // stdio fds are owned by the console subsystem, not by the File, so the
  // File has no NT resources to tear down on rebind. The new WindowsFile
  // just picks up the new handle.
  //
  // Residual race window: between flush's mutex release and our placement-
  // new, a NEW reader can acquire the mutex and observe the old state.
  // Identical to freopen_impl and governed by the same POSIX §7.21.5.4
  // contract — callers serialize stdio rebind with concurrent I/O on the
  // affected stream.
  if (old_ofd && new_ofd->file_ptr) {
    old_ofd->file_ptr = nullptr; // handoff — prevent old->release() freeing

    if (!invalidate) {
      new_ofd->file_ptr->flush();
      File::remove_file(new_ofd->file_ptr);
      if (fd <= 2) {
        // std-fd rebind: placement-new into the StdStreamStorage union,
        // respecting the currently-active member for correct destructor
        // dispatch. Kind-swap to IoRing activates only for fd==0 &&
        // FileKind::Disk — preserves stdout/stderr's expected buffer
        // semantics (_IOLBF / _IONBF) which IoRingFile cannot honor.
        bind_std_storage(STD_FD_LAYOUT[fd], handle, new_ofd->kind, fd);
        new_ofd->file_ptr = STD_FD_LAYOUT[fd].storage->active();
      } else {
        new (new_ofd->file_ptr) WindowsFile(
            handle, STD_FD_LAYOUT[fd].buf, STD_FD_LAYOUT[fd].buf_size,
            STD_FD_LAYOUT[fd].buf_mode, /* owned */ false,
            STD_FD_LAYOUT[fd].modeflags, fd);
      }
      File::add_file(new_ofd->file_ptr);
    }
    // Invalidate path: file_ptr handoff only — no reconstruction. The
    // migrated File retains its prior WindowsFile state; the new OFD has
    // a null handle so POSIX-level I/O through this fd fails. Matches
    // the pre-migration invalidate branch's behaviour.
  } else if (!invalidate) {
    // Fresh fill. For std fds 0/1/2, transition the static StdStreamStorage
    // to the kind implied by the new OFD (Sync by default; IoRing only for
    // disk-redirected stdin). The constexpr-init default is sync, so the
    // first bind typically lands as a destruct-trivial-sync + construct-sync
    // (with real handle) or a destruct-trivial-sync + construct-ioring swap.
    // For non-std fds, allocate from file_pool as before.
    if (fd <= 2) {
      bind_std_storage(STD_FD_LAYOUT[fd], handle, new_ofd->kind, fd);
      new_ofd->file_ptr = STD_FD_LAYOUT[fd].storage->active();
      File::add_file(new_ofd->file_ptr);
    } else {
      void *fslot = file_pool::alloc();
      if (fslot) {
        new_ofd->file_ptr = reinterpret_cast<File *>(fslot);
        new (new_ofd->file_ptr) WindowsFile(handle, STD_FD_LAYOUT[fd].buf,
                                            STD_FD_LAYOUT[fd].buf_size,
                                            STD_FD_LAYOUT[fd].buf_mode,
                                            /* owned */ false,
                                            STD_FD_LAYOUT[fd].modeflags, fd);
        File::add_file(new_ofd->file_ptr);
      }
    }
  }

  // Advance high-water mark.
  int hw = high_water_.load(cpp::MemoryOrder::RELAXED);
  while (fd + 1 > hw) {
    if (high_water_.compare_exchange_weak(hw, fd + 1,
                                          cpp::MemoryOrder::RELAXED))
      break;
  }

  // Release the old OFD. Handle is NOT closed (console-owned); refcount
  // drops and pool storage is reclaimed. file_ptr was already nulled above
  // if we transferred ownership, so old->release() won't touch the File.
  if (old_ofd) {
    old_ofd->handle = nullptr; // Don't close the displaced handle.
    old_ofd->release();
  }
}

void FdTable::init_std_fds() {
  RTL_USER_PROCESS_PARAMETERS *params = NtCurrentPeb()->ProcessParameters;
  HANDLE peb_handles[3] = {params->StandardInput, params->StandardOutput,
                           params->StandardError};

  for (int fd = 0; fd < 3; ++fd)
    bind_std_fd(fd, peb_handles[fd]);

  // Parse inherited runtime data (MSVC CRT fd inheritance protocol) for
  // fds > 2. Native NTPOSIX spawns carry this through ProcessParameters
  // RuntimeData; Win32-compatible launches may project it via lpReserved2.
  process_utils::InheritedRuntimeDataView inherited = {};
  if (process_utils::get_inherited_runtime_data(&inherited)) {
    DWORD count = *reinterpret_cast<const DWORD *>(inherited.data);
    SIZE_T expected = sizeof(DWORD) + count * (1 + sizeof(HANDLE));
    if (inherited.size >= expected) {
      auto *flags_ptr = inherited.data + sizeof(DWORD);
      auto *handles =
          reinterpret_cast<const HANDLE *>(flags_ptr + count);
      for (DWORD i = 3; i < count; ++i) {
        if (!(flags_ptr[i] & 0x01)) // FOPEN flag
          continue;
        HANDLE h = handles[i];
        if (!h || h == INVALID_HANDLE_VALUE)
          continue;
        int ifd = static_cast<int>(i);
        unsigned uifd = static_cast<unsigned>(ifd);
        unsigned ci = uifd >> CHUNK_SHIFT;
        unsigned local = uifd & CHUNK_MASK;

        int oflags = 0;
        if (flags_ptr[i] & 0x08)
          oflags |= O_APPEND;

        OBJECT_BASIC_INFORMATION obj_info = {};
        NTSTATUS qst = NtQueryObject(h, ObjectBasicInformation, &obj_info,
                                     sizeof(obj_info), nullptr);
        if (NT_SUCCESS(qst)) {
          bool can_read = (obj_info.GrantedAccess & FILE_READ_DATA) != 0;
          bool can_write = (obj_info.GrantedAccess &
                            (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
          if (can_read && can_write)
            oflags |= O_RDWR;
          else if (can_write)
            oflags |= O_WRONLY;
        }

        DWORD ftype = query_file_type(h);
        FileKind k = classify_handle(h, ftype);

        OpenFileDescription *ofd = ofd_pool::alloc();
        if (!ofd)
          continue;
        ofd->init_common(h, oflags, k, kind_to_ops(k));

        // init_std_fds runs single-threaded at libc init, but route through
        // the claim-lock protocol anyway so every FdTable publish path
        // follows the same invariant: try_acquire precedes the slot CAS on
        // fresh fills. Inherited fds are always fresh (slot was never
        // occupied pre-init), so eviction will never fire here.
        FdSlot *chunk = pool_.acquire_for_scan(ci);
        if (!chunk) {
          ofd->refcount.store(0, cpp::MemoryOrder::RELAXED);
          ofd->handle = nullptr;
          ofd_pool::free(ofd);
          continue;
        }
        auto *meta = Pool::meta_for(chunk);
        FdSlot *slot = &chunk[local];

        bool installed = false;
        TaggedOfd current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
        for (;;) {
          if (current.ptr() != nullptr) {
            TaggedOfd desired = TaggedOfd::make(ofd, current.gen() + 1);
            if (slot->cas_tagged(current, desired,
                                 cpp::MemoryOrder::ACQ_REL)) {
              OpenFileDescription *old = current.ptr();
              pool_.release_scan_ref(ci);
              old->handle = nullptr;
              old->release();
              installed = true;
              break;
            }
            current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
            continue;
          }
          if (!meta->bitmap.try_acquire(local)) {
            current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
            continue;
          }
          TaggedOfd desired = current.with_ptr(ofd);
          if (slot->cas_tagged(current, desired, cpp::MemoryOrder::ACQ_REL)) {
            installed = true;
            break;
          }
          meta->bitmap.clear(local);
          current = slot->load_tagged(cpp::MemoryOrder::ACQUIRE);
        }

        if (!installed) {
          ofd->refcount.store(0, cpp::MemoryOrder::RELAXED);
          ofd->handle = nullptr;
          ofd_pool::free(ofd);
          continue;
        }

        // Update high-water mark.
        int hw = high_water_.load(cpp::MemoryOrder::RELAXED);
        while (ifd + 1 > hw) {
          if (high_water_.compare_exchange_weak(hw, ifd + 1,
                                                cpp::MemoryOrder::RELAXED))
            break;
        }
      }
    }
  }
}

void FdTable::rebind_std_fds(HANDLE std_input, HANDLE std_output,
                             HANDLE std_error) {
  bind_std_fd(0, std_input);
  bind_std_fd(1, std_output);
  bind_std_fd(2, std_error);
}

void FdTable::rebind_std_fds_as_console(HANDLE std_input, HANDLE std_output,
                                        HANDLE std_error) {
  bind_std_fd_known_type(0, std_input, FileKind::ConDrv);
  bind_std_fd_known_type(1, std_output, FileKind::ConDrv);
  bind_std_fd_known_type(2, std_error, FileKind::ConDrv);
}

void FdTable::set_fd_cloexec(int fd, bool cloexec) {
  FdSlot *slot = get_slot(fd);
  if (!slot)
    return;
  OpenFileDescription *ofd = slot->load_ofd(cpp::MemoryOrder::ACQUIRE);
  if (!ofd)
    return;

  slot->set_cloexec(cloexec);

  // Synchronize the kernel handle's inheritance flag.
  // OBJ_INHERIT = !CLOEXEC: inheritable handles survive fork/exec,
  // non-inheritable handles do not.
  if (ofd->handle) {
    OBJECT_HANDLE_FLAG_INFORMATION flags = {
        static_cast<BOOLEAN>(cloexec ? FALSE : TRUE), FALSE};
    ::NtSetInformationObject(ofd->handle, ObjectHandleFlagInformation,
                             &flags, sizeof(flags));
  }
}

void FdTable::fork_reinit() {
  // Reset pool locks, decommit empty chunks. Per-thread IO rings are
  // per-process kernel objects -- they don't survive fork. The child's
  // first I/O will lazily create a new ring via get_thread_ring().
  // No per-OFD lock state to reset (SEEK_END is lock-free CAS, no mutex).
  pool_.fork_reinit();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

namespace {
void release_live_fd(int fd, LIBC_NAMESPACE::internal::FdSlot * /*slot*/,
                     void * /*ctx*/) {
  // FdTable::release decrements the OFD refcount; when the last reference
  // drops it issues NtClose on the underlying kernel handle (file/console/
  // pipe/etc) which releases all NT byte-range locks held against it.
  // Failures here are unrecoverable at process teardown — we ignore the
  // ErrorOr because no caller can act on it.
  (void)LIBC_NAMESPACE::internal::fd_table.release(fd);
}
} // namespace

int LIBC_NAMESPACE::internal::fd_table_startup_init() {
  // Populate kind → FileOps* dispatch table from hard-coded always-pulled
  // kinds plus .libcops$* section registrations. Must run before any
  // kind_to_ops() call — which init_std_fds() triggers for std fds.
  LIBC_NAMESPACE::internal::init_file_ops_table();
  // Populate WindowsFileKind → WindowsFileOps* table from .libcfio$*
  // registrations. Must run before any WindowsFile/IoRingFile ctor —
  // init_std_fds() (Phase 8) placement-constructs WindowsFile for fds
  // 0/1/2, and each ctor calls kind_to_windows_file_ops().
  LIBC_NAMESPACE::internal::init_windows_file_ops_table();
  LIBC_NAMESPACE::internal::fd_table.init();
  return 0;
}

int LIBC_NAMESPACE::internal::fd_table_std_fds_startup_init() {
  LIBC_NAMESPACE::internal::fd_table.init_std_fds();
  return 0;
}

void LIBC_NAMESPACE::internal::fd_table_fork_reinit() {
  LIBC_NAMESPACE::internal::fd_table.fork_reinit();
}

namespace LIBC_NAMESPACE_DECL {
namespace internal {
// Walk every live slot (fd 0/1/2 included) and release. The bitmap-based
// iterator skips dormant chunks, so this is O(live fds), not O(highest fd).
// Std fd handles came from PEB into our handle table — closing our copies
// is correct and does not affect the parent process's std handles.
static void fd_table_fini() { fd_table.for_each_live(release_live_fd, nullptr); }
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(6, fd_table, &::LIBC_NAMESPACE::internal::fd_table_fini)

LIBC_REGISTER_FORK_REINIT(fd_table,
                          ::LIBC_NAMESPACE::internal::kForkPrioFdTable,
                          &::LIBC_NAMESPACE::internal::fd_table_fork_reinit)
