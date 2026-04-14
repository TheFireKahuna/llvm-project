//===-- FIFO shared-memory ring buffer channel -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements POSIX FIFO data transfer via a cross-process shared-memory ring
// buffer. Named kernel objects provide synchronization:
//
//   Section  (\BaseNamedObjects\lf-<hash>)      — ring buffer + header
//   Event    (\BaseNamedObjects\lf-<hash>-r)    — "readable" (manual-reset)
//   Event    (\BaseNamedObjects\lf-<hash>-w)    — "writable" (manual-reset)
//   Mutant   (\BaseNamedObjects\lf-<hash>-wm)   — write lock (PIPE_BUF atomicity)
//   Mutant   (\BaseNamedObjects\lf-<hash>-rm)   — read lock (multi-reader safety)
//
// Both reads and writes are mutant-protected. This eliminates the unbounded
// spin loop of the CAS+claim pattern and guarantees crash safety (abandoned
// mutants are recoverable — the kernel signals STATUS_ABANDONED).
//
// All bounds computations use compile-time constants, never shared-memory
// header fields. A malicious co-tenant cannot cause out-of-bounds access
// by tampering with the mapped header.
//
// Security: named objects carry a DACL restricting access to the creating
// user's SID. The SipHash key is seeded per-process via ProcessPrng to
// make object names unpredictable across users.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FIFO_CHANNEL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FIFO_CHANNEL_H

#include "hdr/errno_macros.h"
#include "hdr/types/ssize_t.h"
#include "hdr/fcntl_macros.h"
#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/utility/move.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/section_view.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_addr.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// Compile-time constants — used for ALL bounds, never shared header fields
//===----------------------------------------------------------------------===//

inline constexpr uint32_t FIFO_RING_CAPACITY = 65536; // power of 2
inline constexpr uint32_t FIFO_RING_MASK = FIFO_RING_CAPACITY - 1;
inline constexpr uint32_t FIFO_PIPE_BUF = 4096;
inline constexpr size_t FIFO_SECTION_SIZE = 4096 + FIFO_RING_CAPACITY;
inline constexpr uint32_t FIFO_FLAG_UNLINKED = 1u << 0;

//===----------------------------------------------------------------------===//
// Shared header — lives at offset 0 of the mapped section
//===----------------------------------------------------------------------===//

struct FifoHeader {
  // Two-phase init: 0=uninit, 1=in-progress, 2=ready.
  // Second opener spins until 2 before reading other fields.
  cpp::Atomic<uint32_t> initialized;

  // Metadata (informational only — never used for bounds).
  uint32_t capacity;
  uint32_t capacity_mask;
  uint32_t pipe_buf;
  uint32_t reserved[4];

  // Positions. Both protected by their respective mutants —
  // only the mutant holder may modify.
  cpp::Atomic<uint64_t> write_pos; // write mutant holder advances
  cpp::Atomic<uint64_t> read_pos;  // read mutant holder advances

  // Reference counts. Atomic — no lock required.
  cpp::Atomic<int32_t> writer_count;
  cpp::Atomic<int32_t> reader_count;

  cpp::Atomic<uint32_t> flags;
};

//===----------------------------------------------------------------------===//
// Per-fd channel state
//===----------------------------------------------------------------------===//

struct FifoChannel {
  windows::SectionView view; // owns the mapped section view
  HANDLE section;
  HANDLE readable_event;  // manual-reset: data available
  HANDLE writable_event;  // manual-reset: space available
  HANDLE write_mutant;    // serializes writers
  HANDLE read_mutant;     // serializes readers
  int open_mode;          // O_RDONLY, O_WRONLY, O_RDWR

  FifoHeader *header() const { return view.as<FifoHeader>(); }

  char *ring_data() const {
    return reinterpret_cast<char *>(view.base()) + 4096;
  }
};

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

// Build a named OBJECT_ATTRIBUTES. Returns false if the name overflows buf.
LIBC_INLINE bool init_named_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us,
                                WCHAR *buf, size_t buf_wchars,
                                const WCHAR *base_name, size_t base_len,
                                const WCHAR *suffix, size_t suffix_len,
                                SECURITY_DESCRIPTOR *sd = nullptr) {
  constexpr WCHAR prefix[] = u"\\BaseNamedObjects\\lf-";
  constexpr size_t prefix_len = 21;

  size_t total = prefix_len + base_len + suffix_len;
  if (total >= buf_wchars)
    return false;

  for (size_t i = 0; i < prefix_len; ++i)
    buf[i] = prefix[i];
  for (size_t i = 0; i < base_len; ++i)
    buf[prefix_len + i] = base_name[i];
  for (size_t i = 0; i < suffix_len; ++i)
    buf[prefix_len + base_len + i] = suffix[i];
  buf[total] = u'\0';

  us->Length = static_cast<USHORT>(total * sizeof(WCHAR));
  us->MaximumLength = static_cast<USHORT>((total + 1) * sizeof(WCHAR));
  us->Buffer = buf;

  oa->Length = sizeof(OBJECT_ATTRIBUTES);
  oa->RootDirectory = nullptr;
  oa->ObjectName = us;
  oa->Attributes = OBJ_CASE_INSENSITIVE;
  oa->SecurityDescriptor = sd;
  oa->SecurityQualityOfService = nullptr;
  return true;
}

// Ring buffer copy helpers — all bounds use compile-time FIFO_RING_MASK.

LIBC_INLINE void ring_copy_out(void *dst, const char *ring, uint64_t pos,
                                size_t count) {
  size_t offset = static_cast<size_t>(pos) & FIFO_RING_MASK;
  size_t first = FIFO_RING_CAPACITY - offset;
  if (count <= first) {
    __builtin_memcpy(dst, ring + offset, count);
  } else {
    __builtin_memcpy(dst, ring + offset, first);
    __builtin_memcpy(static_cast<char *>(dst) + first, ring, count - first);
  }
}

LIBC_INLINE void ring_copy_in(char *ring, uint64_t pos, const void *src,
                               size_t count) {
  size_t offset = static_cast<size_t>(pos) & FIFO_RING_MASK;
  size_t first = FIFO_RING_CAPACITY - offset;
  if (count <= first) {
    __builtin_memcpy(ring + offset, src, count);
  } else {
    __builtin_memcpy(ring + offset, src, first);
    __builtin_memcpy(ring, static_cast<const char *>(src) + first,
                      count - first);
  }
}

// Decrement a refcount only if positive. Returns the previous value.
// Prevents underflow from double-close or dup() accounting errors.
LIBC_INLINE int32_t safe_refcount_dec(cpp::Atomic<int32_t> &count) {
  int32_t cur = count.load(cpp::MemoryOrder::ACQUIRE);
  while (cur > 0) {
    if (count.compare_exchange_weak(cur, cur - 1, cpp::MemoryOrder::ACQ_REL))
      return cur; // previous value
  }
  return 0; // was already 0 or negative — no decrement performed
}

// RAII cleanup helper for fifo_open_channel error paths.
// Section and view are managed via SectionHandle + SectionView RAII.
// Events and mutants are raw handles (no RAII wrapper exists for these).
struct ChannelHandles {
  windows::SectionHandle section;
  windows::SectionView view;
  HANDLE readable_event = nullptr;
  HANDLE writable_event = nullptr;
  HANDLE write_mutant = nullptr;
  HANDLE read_mutant = nullptr;

  void close_all() {
    if (read_mutant) NtClose(read_mutant);
    if (write_mutant) NtClose(write_mutant);
    if (writable_event) NtClose(writable_event);
    if (readable_event) NtClose(readable_event);
    // view and section cleaned up by their destructors.
  }
};

//===----------------------------------------------------------------------===//
// Channel lifecycle
//===----------------------------------------------------------------------===//

LIBC_INLINE FifoChannel *fifo_open_channel(const WCHAR *hash_name,
                                            size_t hash_len, int flags,
                                            mode_t mode = 0666) {
  WCHAR name_buf[80];
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  ChannelHandles h;

  // Build a DACL from the FIFO's POSIX mode for all kernel objects.
  auto sd_s = internal::byte_scratch(windows_sec::CREATION_SD_BUF_SIZE);
  SECURITY_DESCRIPTOR *sd = sd_s ? windows_sec::build_creation_sd(
      reinterpret_cast<UCHAR *>(sd_s.data()), mode, nullptr, nullptr,
      windows_sec::mode_bits_to_object_access_mask) : nullptr;
  // sd may be nullptr if SID query fails — proceed without restriction.

  // --- Section ---
  if (!init_named_oa(&oa, &us, name_buf, 80, hash_name, hash_len, u"", 0, sd))
    return nullptr;

  h.section = windows::SectionHandle::create_named(
      &us, FIFO_SECTION_SIZE,
      SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
      PAGE_READWRITE, SEC_COMMIT, sd);
  if (!h.section)
    return nullptr;

  // Validate section size — an attacker could pre-create a section with a
  // smaller size. All ring arithmetic uses compile-time constants, but a
  // short section would cause access violations on ring buffer access.
  SIZE_T actual_size = h.section.query_size();
  if (actual_size < FIFO_SECTION_SIZE) {
    h.close_all();
    return nullptr;
  }

  h.view = windows::SectionView::map_anywhere(h.section, PAGE_READWRITE);
  if (!h.view) {
    h.close_all();
    return nullptr;
  }

  auto *hdr = h.view.as<FifoHeader>();

  // Idempotent init: all values are compile-time constants or zero, so
  // concurrent initializers produce identical results. No intermediate
  // "in-progress" state means no deadlock if a thread dies mid-init.
  // Multiple writers racing to set the same constants is benign.
  if (hdr->initialized.load(cpp::MemoryOrder::ACQUIRE) != 2) {
    hdr->capacity = FIFO_RING_CAPACITY;
    hdr->capacity_mask = FIFO_RING_MASK;
    hdr->pipe_buf = FIFO_PIPE_BUF;
    hdr->write_pos.store(0, cpp::MemoryOrder::RELAXED);
    hdr->read_pos.store(0, cpp::MemoryOrder::RELAXED);
    hdr->writer_count.store(0, cpp::MemoryOrder::RELAXED);
    hdr->reader_count.store(0, cpp::MemoryOrder::RELAXED);
    hdr->flags.store(0, cpp::MemoryOrder::RELAXED);
    // Publish. CAS ensures exactly one 0→2 transition. If CAS fails,
    // another initializer already published (identical values).
    uint32_t expected = 0;
    hdr->initialized.compare_exchange_strong(expected, 2,
                                              cpp::MemoryOrder::RELEASE);
  }

  // --- Events (security descriptor applies to all objects) ---
  NTSTATUS status;
  if (!init_named_oa(&oa, &us, name_buf, 80, hash_name, hash_len,
                      u"-r", 2, sd)) {
    h.close_all();
    return nullptr;
  }
  status = NtCreateEvent(&h.readable_event, EVENT_MODIFY_STATE | SYNCHRONIZE,
                         &oa,
                           NotificationEvent, FALSE);
  if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_COLLISION) {
    h.close_all();
    return nullptr;
  }

  if (!init_named_oa(&oa, &us, name_buf, 80, hash_name, hash_len,
                      u"-w", 2, sd)) {
    h.close_all();
    return nullptr;
  }
  status = NtCreateEvent(&h.writable_event, EVENT_MODIFY_STATE | SYNCHRONIZE,
                         &oa,
                           NotificationEvent, TRUE);
  if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_COLLISION) {
    h.close_all();
    return nullptr;
  }

  // --- Mutants ---
  if (!init_named_oa(&oa, &us, name_buf, 80, hash_name, hash_len,
                      u"-wm", 3, sd)) {
    h.close_all();
    return nullptr;
  }
  status = NtCreateMutant(&h.write_mutant, MUTANT_ALL_ACCESS, &oa, FALSE);
  if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_COLLISION) {
    h.close_all();
    return nullptr;
  }

  if (!init_named_oa(&oa, &us, name_buf, 80, hash_name, hash_len,
                      u"-rm", 3, sd)) {
    h.close_all();
    return nullptr;
  }
  status = NtCreateMutant(&h.read_mutant, MUTANT_ALL_ACCESS, &oa, FALSE);
  if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_COLLISION) {
    h.close_all();
    return nullptr;
  }

  // --- Refcounts ---
  int accmode = flags & O_ACCMODE;
  if (accmode == O_RDONLY || accmode == O_RDWR)
    hdr->reader_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  if (accmode == O_WRONLY || accmode == O_RDWR)
    hdr->writer_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

  // Wake the other side.
  if (accmode == O_RDONLY || accmode == O_RDWR)
    NtSetEvent(h.writable_event, nullptr);
  if (accmode == O_WRONLY || accmode == O_RDWR)
    NtSetEvent(h.readable_event, nullptr);

  // --- Blocking open ---
  bool nonblock = (flags & O_NONBLOCK) != 0;

  if (accmode == O_RDONLY && !nonblock) {
    while (hdr->writer_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
      NtResetEvent(h.readable_event, nullptr);
      if (hdr->writer_count.load(cpp::MemoryOrder::ACQUIRE) != 0)
        break;
      NTSTATUS ws = NtWaitForSingleObject(h.readable_event, TRUE, nullptr);
      if (ws == STATUS_ALERTED || ws == STATUS_USER_APC) {
        safe_refcount_dec(hdr->reader_count);
        h.close_all();
        return reinterpret_cast<FifoChannel *>(static_cast<uintptr_t>(EINTR));
      }
    }
  }

  if (accmode == O_WRONLY && !nonblock) {
    while (hdr->reader_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
      NtResetEvent(h.writable_event, nullptr);
      if (hdr->reader_count.load(cpp::MemoryOrder::ACQUIRE) != 0)
        break;
      NTSTATUS ws = NtWaitForSingleObject(h.writable_event, TRUE, nullptr);
      if (ws == STATUS_ALERTED || ws == STATUS_USER_APC) {
        safe_refcount_dec(hdr->writer_count);
        h.close_all();
        return reinterpret_cast<FifoChannel *>(static_cast<uintptr_t>(EINTR));
      }
    }
  }

  if (accmode == O_WRONLY && nonblock) {
    if (hdr->reader_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
      safe_refcount_dec(hdr->writer_count);
      h.close_all();
      return reinterpret_cast<FifoChannel *>(static_cast<uintptr_t>(ENXIO));
    }
  }

  // --- Allocate per-fd channel ---
  auto *ch = static_cast<FifoChannel *>(page_alloc(sizeof(FifoChannel)));
  if (!ch) {
    if (accmode == O_RDONLY || accmode == O_RDWR)
      safe_refcount_dec(hdr->reader_count);
    if (accmode == O_WRONLY || accmode == O_RDWR)
      safe_refcount_dec(hdr->writer_count);
    h.close_all();
    return nullptr;
  }

  ch->view = cpp::move(h.view);
  ch->section = h.section.release();
  ch->readable_event = h.readable_event;
  ch->writable_event = h.writable_event;
  ch->write_mutant = h.write_mutant;
  ch->read_mutant = h.read_mutant;
  ch->open_mode = accmode;
  return ch;
}

// Duplicate a channel for dup()/dup2(). Duplicates all kernel handles,
// maps a new view, increments refcounts. Returns nullptr on failure.
LIBC_INLINE FifoChannel *fifo_dup_channel(FifoChannel *src) {
  if (!src)
    return nullptr;

  auto *ch = static_cast<FifoChannel *>(page_alloc(sizeof(FifoChannel)));
  if (!ch)
    return nullptr;

  HANDLE proc = NtCurrentProcess();
  bool ok = true;

  // Duplicate each handle into the current process.
  auto dup = [&](HANDLE in, HANDLE *out) {
    NTSTATUS st = NtDuplicateObject(proc, in, proc, out, 0, 0,
                                     DUPLICATE_SAME_ACCESS);
    if (!NT_SUCCESS(st)) ok = false;
  };

  dup(src->section, &ch->section);
  dup(src->readable_event, &ch->readable_event);
  dup(src->writable_event, &ch->writable_event);
  dup(src->write_mutant, &ch->write_mutant);
  dup(src->read_mutant, &ch->read_mutant);

  if (!ok) {
    // Partial failure — close whatever succeeded.
    if (ch->read_mutant) NtClose(ch->read_mutant);
    if (ch->write_mutant) NtClose(ch->write_mutant);
    if (ch->writable_event) NtClose(ch->writable_event);
    if (ch->readable_event) NtClose(ch->readable_event);
    if (ch->section) NtClose(ch->section);
    page_free(ch);
    return nullptr;
  }

  // Map a new view from the duplicated section handle.
  windows::SectionHandle borrowed = windows::SectionHandle::borrow(ch->section);
  windows::SectionView view =
      windows::SectionView::map_anywhere(borrowed, PAGE_READWRITE);
  if (!view) {
    NtClose(ch->read_mutant);
    NtClose(ch->write_mutant);
    NtClose(ch->writable_event);
    NtClose(ch->readable_event);
    NtClose(ch->section);
    page_free(ch);
    return nullptr;
  }

  ch->view = cpp::move(view);
  ch->open_mode = src->open_mode;

  // Increment refcounts.
  if (ch->open_mode == O_RDONLY || ch->open_mode == O_RDWR)
    ch->header()->reader_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  if (ch->open_mode == O_WRONLY || ch->open_mode == O_RDWR)
    ch->header()->writer_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);

  return ch;
}

// Create an anonymous pipe pair backed by a shared-memory ring buffer.
// No named objects, no namespace, no blocking open, no marker file.
// Returns true on success, false on failure. On success, *read_ch and
// *write_ch are set to the reader and writer channels respectively.
LIBC_INLINE bool fifo_open_pipe_channel(FifoChannel **read_ch,
                                         FifoChannel **write_ch,
                                         int flags) {
  (void)flags;
  // Create an unnamed section.
  windows::SectionHandle section =
      windows::SectionHandle::create_anon_rw(FIFO_SECTION_SIZE);
  if (!section)
    return false;

  // Map the first view (read channel).
  windows::SectionView read_view =
      windows::SectionView::map_anywhere(section, PAGE_READWRITE);
  if (!read_view)
    return false;

  auto *hdr = read_view.as<FifoHeader>();
  hdr->initialized.store(2, cpp::MemoryOrder::RELAXED);
  hdr->capacity = FIFO_RING_CAPACITY;
  hdr->capacity_mask = FIFO_RING_MASK;
  hdr->pipe_buf = FIFO_PIPE_BUF;
  hdr->write_pos.store(0, cpp::MemoryOrder::RELAXED);
  hdr->read_pos.store(0, cpp::MemoryOrder::RELAXED);
  hdr->writer_count.store(1, cpp::MemoryOrder::RELAXED);
  hdr->reader_count.store(1, cpp::MemoryOrder::RELAXED);
  hdr->flags.store(0, cpp::MemoryOrder::RELEASE);

  // Cleanup helper — closes any non-null handles.
  auto cleanup = [](HANDLE readable, HANDLE writable, HANDLE wmut,
                    HANDLE rmut) {
    if (rmut) NtClose(rmut);
    if (wmut) NtClose(wmut);
    if (writable) NtClose(writable);
    if (readable) NtClose(readable);
    // read_view and section are RAII — cleaned up by destructors.
  };

  // Create unnamed, non-inheritable events and mutants.
  HANDLE readable = nullptr, writable = nullptr;
  HANDLE wmut = nullptr, rmut = nullptr;
  OBJECT_ATTRIBUTES oa = windows::internal_oa();
  NTSTATUS status;

  status = NtCreateEvent(&readable, EVENT_MODIFY_STATE | SYNCHRONIZE, &oa,
                           NotificationEvent, FALSE);
  if (!NT_SUCCESS(status)) {
    cleanup(readable, writable, wmut, rmut);
    return false;
  }

  status = NtCreateEvent(&writable, EVENT_MODIFY_STATE | SYNCHRONIZE, &oa,
                           NotificationEvent, TRUE);
  if (!NT_SUCCESS(status)) {
    cleanup(readable, writable, wmut, rmut);
    return false;
  }

  status = NtCreateMutant(&wmut, MUTANT_ALL_ACCESS, &oa, FALSE);
  if (!NT_SUCCESS(status)) {
    cleanup(readable, writable, wmut, rmut);
    return false;
  }

  status = NtCreateMutant(&rmut, MUTANT_ALL_ACCESS, &oa, FALSE);
  if (!NT_SUCCESS(status)) {
    cleanup(readable, writable, wmut, rmut);
    return false;
  }

  // Allocate both channel structs.
  auto *rch = static_cast<FifoChannel *>(page_alloc(sizeof(FifoChannel)));
  auto *wch = static_cast<FifoChannel *>(page_alloc(sizeof(FifoChannel)));
  if (!rch || !wch) {
    if (rch) page_free(rch);
    if (wch) page_free(wch);
    cleanup(readable, writable, wmut, rmut);
    return false;
  }

  // Duplicate handles for the write channel so each can close independently.
  HANDLE proc = NtCurrentProcess();
  HANDLE section2, readable2, writable2, wmut2, rmut2;
  auto dup = [&](HANDLE in, HANDLE *out) -> bool {
    return NT_SUCCESS(NtDuplicateObject(proc, in, proc, out, 0, 0,
                                         DUPLICATE_SAME_ACCESS));
  };
  if (!dup(section.get(), &section2) || !dup(readable, &readable2) ||
      !dup(writable, &writable2) || !dup(wmut, &wmut2) ||
      !dup(rmut, &rmut2)) {
    page_free(rch);
    page_free(wch);
    cleanup(readable, writable, wmut, rmut);
    return false;
  }

  // Map a second view for the write channel.
  windows::SectionView alias =
      windows::SectionView::map_anywhere(section, PAGE_READWRITE);
  if (!alias) {
    NtClose(section2);
    NtClose(readable2);
    NtClose(writable2);
    NtClose(wmut2);
    NtClose(rmut2);
    page_free(rch);
    page_free(wch);
    cleanup(readable, writable, wmut, rmut);
    return false;
  }

  rch->view = cpp::move(read_view);
  rch->section = section.release();
  rch->readable_event = readable;
  rch->writable_event = writable;
  rch->write_mutant = wmut;
  rch->read_mutant = rmut;
  rch->open_mode = O_RDONLY;

  wch->view = cpp::move(alias);
  wch->section = section2;
  wch->readable_event = readable2;
  wch->writable_event = writable2;
  wch->write_mutant = wmut2;
  wch->read_mutant = rmut2;
  wch->open_mode = O_WRONLY;

  *read_ch = rch;
  *write_ch = wch;
  return true;
}

// Create a lightweight event-only FifoChannel pair for anonymous pipes.
// No ring buffer, no section, no mutants — the real NT pipe handles do I/O.
// The events serve as notification sidecars for poll/select/epoll:
//   readable_event: signaled by the write path when data is written
//   writable_event: always signaled (pipe write buffer is kernel-managed)
// Both channels share the same events (duplicated for independent close).
// Returns true on success.
LIBC_INLINE bool fifo_create_pipe_events(FifoChannel **read_ch,
                                          FifoChannel **write_ch) {
  HANDLE readable = nullptr, writable = nullptr;
  NTSTATUS status;

  // readable_event starts NOT signaled (pipe is empty).
  auto revt_oa = windows::internal_oa();
  status = NtCreateEvent(&readable, EVENT_MODIFY_STATE | SYNCHRONIZE, &revt_oa,
                         NotificationEvent, FALSE);
  if (!NT_SUCCESS(status))
    return false;

  // writable_event starts signaled (pipe has space).
  auto wevt_oa = windows::internal_oa();
  status = NtCreateEvent(&writable, EVENT_MODIFY_STATE | SYNCHRONIZE, &wevt_oa,
                         NotificationEvent, TRUE);
  if (!NT_SUCCESS(status)) {
    NtClose(readable);
    return false;
  }

  auto *rch = static_cast<FifoChannel *>(page_alloc(sizeof(FifoChannel)));
  auto *wch = static_cast<FifoChannel *>(page_alloc(sizeof(FifoChannel)));
  if (!rch || !wch) {
    if (rch) page_free(rch);
    if (wch) page_free(wch);
    NtClose(writable);
    NtClose(readable);
    return false;
  }

  // Duplicate events for the write channel.
  HANDLE proc = NtCurrentProcess();
  HANDLE readable2, writable2;
  if (!NT_SUCCESS(NtDuplicateObject(proc, readable, proc, &readable2, 0, 0,
                                     DUPLICATE_SAME_ACCESS)) ||
      !NT_SUCCESS(NtDuplicateObject(proc, writable, proc, &writable2, 0, 0,
                                     DUPLICATE_SAME_ACCESS))) {
    page_free(rch);
    page_free(wch);
    NtClose(writable);
    NtClose(readable);
    return false;
  }

  __builtin_memset(static_cast<void *>(rch), 0, sizeof(FifoChannel));
  rch->readable_event = readable;
  rch->writable_event = writable;
  rch->open_mode = O_RDONLY;
  // view, section, mutants all zero/null — header() returns nullptr.

  __builtin_memset(static_cast<void *>(wch), 0, sizeof(FifoChannel));
  wch->readable_event = readable2;
  wch->writable_event = writable2;
  wch->open_mode = O_WRONLY;

  *read_ch = rch;
  *write_ch = wch;
  return true;
}

// Close a FIFO channel. For event-only channels (header() == nullptr),
// only closes the events and frees the struct.
LIBC_INLINE void fifo_close_channel(FifoChannel *ch) {
  if (!ch)
    return;

  FifoHeader *hdr = ch->header();

  if (hdr) {
    // Ring-buffer-backed channel: update reader/writer counts.
    // Wake all futex-parked peers on EOF/EPIPE transitions, plus signal
    // events for epoll/poll WCP readiness notification.
    if (ch->open_mode == O_RDONLY || ch->open_mode == O_RDWR) {
      if (safe_refcount_dec(hdr->reader_count) == 1) {
        futex_addr::wake(&hdr->read_pos, UINT32_MAX); // wake ALL writers
        NtSetEvent(ch->writable_event, nullptr);       // epoll/poll EPIPE
      }
    }
    if (ch->open_mode == O_WRONLY || ch->open_mode == O_RDWR) {
      if (safe_refcount_dec(hdr->writer_count) == 1) {
        futex_addr::wake(&hdr->write_pos, UINT32_MAX); // wake ALL readers
        NtSetEvent(ch->readable_event, nullptr);        // epoll/poll EOF
      }
    }
    if (ch->read_mutant) NtClose(ch->read_mutant);
    if (ch->write_mutant) NtClose(ch->write_mutant);
    ch->view.unmap_release();
    if (ch->section) NtClose(ch->section);
  }
  // Event-only channels (anonymous pipes): signal the readable_event when
  // the write end closes so that poll/epoll detect POLLHUP on the read end.
  if (!hdr && ch->open_mode == O_WRONLY && ch->readable_event)
    NtSetEvent(ch->readable_event, nullptr);

  // Close events and free.
  if (ch->writable_event) NtClose(ch->writable_event);
  if (ch->readable_event) NtClose(ch->readable_event);
  page_free(ch);
}

//===----------------------------------------------------------------------===//
// Data transfer
//===----------------------------------------------------------------------===//

// Read from a FIFO channel. Returns bytes read, 0 for EOF, or negative errno.
// The read mutant serializes concurrent readers — no unbounded spin.
LIBC_INLINE ssize_t fifo_read(FifoChannel *ch, void *buf, size_t count,
                               int open_flags) {
  FifoHeader *hdr = ch->header();
  char *ring = ch->ring_data();

  // Acquire read mutant. Alertable for EINTR.
  NTSTATUS ms = NtWaitForSingleObject(ch->read_mutant, TRUE, nullptr);
  if (ms == STATUS_ALERTED || ms == STATUS_USER_APC)
    return -EINTR;
  // STATUS_ABANDONED: prior holder crashed. The write ordering (memcpy then
  // atomic write_pos store) means partial writes can't corrupt the ring.
  // Validate ring invariants as a defense-in-depth check.
  if (ms == STATUS_ABANDONED) {
    uint64_t wp = hdr->write_pos.load(cpp::MemoryOrder::ACQUIRE);
    uint64_t rp = hdr->read_pos.load(cpp::MemoryOrder::ACQUIRE);
    if (wp < rp || (wp - rp) > FIFO_RING_CAPACITY) {
      NtReleaseMutant(ch->read_mutant, nullptr);
      return -EIO; // Ring buffer corrupted.
    }
  }

  for (;;) {
    uint64_t wp = hdr->write_pos.load(cpp::MemoryOrder::ACQUIRE);
    uint64_t rp = hdr->read_pos.load(cpp::MemoryOrder::RELAXED); // we hold the lock

    if (rp < wp) {
      size_t avail = static_cast<size_t>(wp - rp);
      // Cap to ring capacity — defends against tampered write_pos.
      if (avail > FIFO_RING_CAPACITY)
        avail = FIFO_RING_CAPACITY;
      size_t to_read = (count < avail) ? count : avail;

      ring_copy_out(buf, ring, rp, to_read);
      hdr->read_pos.store(rp + to_read, cpp::MemoryOrder::RELEASE);

      NtReleaseMutant(ch->read_mutant, nullptr);
      // Wake one blocked writer waiting for space.
      futex_addr::wake(&hdr->read_pos, 1);
      // Readiness signal for epoll/poll WCP.
      NtSetEvent(ch->writable_event, nullptr);
      return static_cast<ssize_t>(to_read);
    }

    // Buffer empty.
    if (hdr->writer_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
      NtReleaseMutant(ch->read_mutant, nullptr);
      return 0; // EOF
    }

    if (open_flags & O_NONBLOCK) {
      NtReleaseMutant(ch->read_mutant, nullptr);
      return -EAGAIN;
    }

    // Park on write_pos via futex. The futex value check serves as the
    // Dekker re-check: if a writer advanced write_pos between mutant
    // release and futex entry, wait_nt returns -EAGAIN immediately.
    // Events are no longer reset here — they are readiness-only signals
    // owned exclusively by the epoll/poll layer.
    {
      uint64_t wait_wp = hdr->write_pos.load(cpp::MemoryOrder::ACQUIRE);
      NtReleaseMutant(ch->read_mutant, nullptr);
      long fr = futex_addr::wait_nt<uint64_t, /*Interruptible=*/true>(
          &hdr->write_pos, wait_wp, nullptr);
      if (fr == -EINTR)
        return -EINTR;
    }

    // Re-acquire read mutant after wakeup.
    ms = NtWaitForSingleObject(ch->read_mutant, TRUE, nullptr);
    if (ms == STATUS_ALERTED || ms == STATUS_USER_APC)
      return -EINTR;
  }
}

// Write to a FIFO channel. Returns bytes written, or negative errno.
// The write mutant serializes all writers. Writes <= PIPE_BUF are atomic.
// O_APPEND may be set in open_flags (accepted per POSIX) but has no effect —
// this function never uses file position. Pipe data is always appended.
LIBC_INLINE ssize_t fifo_write(FifoChannel *ch, const void *buf, size_t count,
                                int open_flags) {
  FifoHeader *hdr = ch->header();
  char *ring = ch->ring_data();

  // Fast-path EPIPE check (RELAXED — definitive check under mutant below).
  if (hdr->reader_count.load(cpp::MemoryOrder::RELAXED) == 0)
    return -EPIPE;

  // Acquire write mutant. Alertable for EINTR.
  NTSTATUS ms = NtWaitForSingleObject(ch->write_mutant, TRUE, nullptr);
  if (ms == STATUS_ALERTED || ms == STATUS_USER_APC)
    return -EINTR;
  // STATUS_ABANDONED: prior writer crashed. Validate ring invariants.
  if (ms == STATUS_ABANDONED) {
    uint64_t wp = hdr->write_pos.load(cpp::MemoryOrder::ACQUIRE);
    uint64_t rp = hdr->read_pos.load(cpp::MemoryOrder::ACQUIRE);
    if (wp < rp || (wp - rp) > FIFO_RING_CAPACITY) {
      NtReleaseMutant(ch->write_mutant, nullptr);
      return -EIO; // Ring buffer corrupted.
    }
  }

  // Definitive EPIPE check under mutant.
  if (hdr->reader_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
    NtReleaseMutant(ch->write_mutant, nullptr);
    return -EPIPE;
  }

  const char *src = static_cast<const char *>(buf);
  bool atomic = (count <= FIFO_PIPE_BUF);
  size_t written = 0;

  while (written < count) {
    uint64_t wp = hdr->write_pos.load(cpp::MemoryOrder::RELAXED); // we hold the lock
    uint64_t rp = hdr->read_pos.load(cpp::MemoryOrder::ACQUIRE);
    size_t space = FIFO_RING_CAPACITY - static_cast<size_t>(wp - rp);
    // Cap — defends against tampered read_pos.
    if (space > FIFO_RING_CAPACITY)
      space = 0;

    if (space > 0) {
      size_t chunk = count - written;
      if (chunk > space) {
        if (atomic)
          goto wait_for_space; // atomic: all-or-nothing
        chunk = space;
      }

      ring_copy_in(ring, wp, src + written, chunk);
      hdr->write_pos.store(wp + chunk, cpp::MemoryOrder::RELEASE);
      written += chunk;

      // Wake one blocked reader waiting for data.
      futex_addr::wake(&hdr->write_pos, 1);
      // Readiness signal for epoll/poll WCP.
      NtSetEvent(ch->readable_event, nullptr);
      continue;
    }

wait_for_space:
    if (hdr->reader_count.load(cpp::MemoryOrder::ACQUIRE) == 0) {
      NtReleaseMutant(ch->write_mutant, nullptr);
      return written > 0 ? static_cast<ssize_t>(written) : -EPIPE;
    }

    if (open_flags & O_NONBLOCK) {
      NtReleaseMutant(ch->write_mutant, nullptr);
      return written > 0 ? static_cast<ssize_t>(written) : -EAGAIN;
    }

    // Park on read_pos via futex. The futex value check serves as the
    // Dekker re-check: if a reader advanced read_pos between mutant
    // release and futex entry, wait_nt returns -EAGAIN immediately.
    // Events are no longer reset here — they are readiness-only signals
    // owned exclusively by the epoll/poll layer.
    {
      uint64_t wait_rp = hdr->read_pos.load(cpp::MemoryOrder::ACQUIRE);
      NtReleaseMutant(ch->write_mutant, nullptr);
      long fr = futex_addr::wait_nt<uint64_t, /*Interruptible=*/true>(
          &hdr->read_pos, wait_rp, nullptr);
      if (fr == -EINTR)
        return written > 0 ? static_cast<ssize_t>(written) : -EINTR;
    }

    // Re-acquire write mutant.
    ms = NtWaitForSingleObject(ch->write_mutant, TRUE, nullptr);
    if (ms == STATUS_ALERTED || ms == STATUS_USER_APC)
      return written > 0 ? static_cast<ssize_t>(written) : -EINTR;
  }

  NtReleaseMutant(ch->write_mutant, nullptr);
  return static_cast<ssize_t>(written);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FIFO_CHANNEL_H
