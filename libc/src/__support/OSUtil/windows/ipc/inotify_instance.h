//===-- Inotify instance for NT-POSIX ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// InotifyWatch and InotifyInstance types implementing Linux inotify(7) on top
// of NtNotifyChangeDirectoryFileEx. Each watch issues an async directory
// notification via the reactor IOCP; completions are parsed into cooked
// inotify_event records buffered for read().
//
// Allocation: instances and watch pointer tables use page_alloc (demand-
// committed VA). Each watch is a 2-page arena (struct + NT notify buffer).
// The event output buffer is a separate page_alloc'd region that grows on
// demand and is freed on instance destruction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INOTIFY_INSTANCE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INOTIFY_INSTANCE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_file_types.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/raw_mutex.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// inotify constants (Linux-compatible values)
//===----------------------------------------------------------------------===//

// Event mask bits — user-visible.
inline constexpr uint32_t IN_ACCESS        = 0x00000001;
inline constexpr uint32_t IN_MODIFY        = 0x00000002;
inline constexpr uint32_t IN_ATTRIB        = 0x00000004;
inline constexpr uint32_t IN_CLOSE_WRITE   = 0x00000008;
inline constexpr uint32_t IN_CLOSE_NOWRITE = 0x00000010;
inline constexpr uint32_t IN_OPEN          = 0x00000020;
inline constexpr uint32_t IN_MOVED_FROM    = 0x00000040;
inline constexpr uint32_t IN_MOVED_TO      = 0x00000080;
inline constexpr uint32_t IN_CREATE        = 0x00000100;
inline constexpr uint32_t IN_DELETE        = 0x00000200;
inline constexpr uint32_t IN_DELETE_SELF   = 0x00000400;
inline constexpr uint32_t IN_MOVE_SELF     = 0x00000800;

// Convenience combinations.
inline constexpr uint32_t IN_CLOSE = IN_CLOSE_WRITE | IN_CLOSE_NOWRITE;
inline constexpr uint32_t IN_MOVE  = IN_MOVED_FROM | IN_MOVED_TO;
inline constexpr uint32_t IN_ALL_EVENTS =
    IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE | IN_OPEN | IN_MOVE |
    IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF;

// Output-only flags (set by kernel, not user).
inline constexpr uint32_t IN_UNMOUNT    = 0x00002000;
inline constexpr uint32_t IN_Q_OVERFLOW = 0x00004000;
inline constexpr uint32_t IN_IGNORED    = 0x00008000;

// Control flags for inotify_add_watch.
inline constexpr uint32_t IN_ONLYDIR     = 0x01000000;
inline constexpr uint32_t IN_DONT_FOLLOW = 0x02000000;
inline constexpr uint32_t IN_EXCL_UNLINK = 0x04000000;
inline constexpr uint32_t IN_MASK_CREATE = 0x10000000;
inline constexpr uint32_t IN_MASK_ADD    = 0x20000000;
inline constexpr uint32_t IN_ISDIR       = 0x40000000;
inline constexpr uint32_t IN_ONESHOT     = 0x80000000;

// Valid bits for the user mask argument.
inline constexpr uint32_t IN_USER_MASK =
    IN_ALL_EVENTS | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK |
    IN_MASK_CREATE | IN_MASK_ADD | IN_ONESHOT;

// Events that are always delivered regardless of the watch mask.
inline constexpr uint32_t IN_ALWAYS_EVENTS =
    IN_Q_OVERFLOW | IN_IGNORED | IN_UNMOUNT;

// Flags for inotify_init1.
inline constexpr int IN_NONBLOCK = 04000;  // O_NONBLOCK
inline constexpr int IN_CLOEXEC  = 02000000; // O_CLOEXEC

//===----------------------------------------------------------------------===//
// inotify_event — matches Linux struct layout exactly
//===----------------------------------------------------------------------===//

struct inotify_event {
  int wd;
  uint32_t mask;
  uint32_t cookie;
  uint32_t len; // Includes NUL padding to align next event.
  // char name[] follows — variable length, NUL-terminated + padded.
};

static_assert(sizeof(inotify_event) == 16, "inotify_event must be 16 bytes");

// Alignment for the name length padding. Linux pads len to a multiple
// of sizeof(inotify_event) so the next record is naturally aligned.
inline constexpr size_t INOTIFY_EVENT_ALIGN = sizeof(inotify_event);

//===----------------------------------------------------------------------===//
// InotifyWatch — per-directory watch state (2-page arena)
//===----------------------------------------------------------------------===//

struct InotifyWatch {
  // Allocation: struct at the start of a 2-page (8KB) region.
  // The NT notify buffer occupies the remainder.
  static constexpr size_t ALLOC_SIZE = 8192;

  // Struct fields occupy the first portion; notify buffer follows.
  int wd;               // Watch descriptor (1-based, monotonic).
  HANDLE dir_handle;    // NtCreateFile'd directory handle.
  HANDLE event_handle;  // Signaled by NtNotifyChangeDirectoryFileEx.
  reactor::ReactorToken reactor_token; // Reactor WCP token.
  IO_STATUS_BLOCK iosb; // Async IOSB — must stay valid while IO pending.
  uint32_t mask;        // IN_* event mask (user-supplied, without control flags).
  LARGE_INTEGER file_id; // Cached FileInternalInformation for dedup.
  bool oneshot;         // IN_ONESHOT was set.
  bool oneshot_fired;   // First event already delivered for oneshot watch.
  bool removed;         // rm_watch called; pending cleanup.

  // Back-pointer to the owning InotifyInstance. Set at creation, used
  // by the reactor callback to find the event buffer and lock.
  struct InotifyInstance *inst;

  // NT notification buffer — fills the remainder of the 2-page arena.
  // NtNotifyChangeDirectoryFileEx writes FILE_NOTIFY_INFORMATION entries
  // here. Must be DWORD-aligned (guaranteed by page_alloc alignment).
  static constexpr size_t HEADER_SIZE = 128; // Generous padding for fields.
  static constexpr size_t NOTIFY_BUF_SIZE = ALLOC_SIZE - HEADER_SIZE;

  LIBC_INLINE uint8_t *notify_buf() {
    return reinterpret_cast<uint8_t *>(this) + HEADER_SIZE;
  }
};

static_assert(sizeof(InotifyWatch) <= InotifyWatch::HEADER_SIZE,
              "InotifyWatch fields exceed reserved header space");

//===----------------------------------------------------------------------===//
// InotifyInstance — one per inotify_init1() call
//===----------------------------------------------------------------------===//

struct InotifyInstance {
  // NT event handle signaled when cooked events are available for read().
  // This is also the handle stored in the fd_table OFD.
  HANDLE readable_event;

  // Protects watch table, event buffer, and readable_event signaling.
  RawMutex lock;

  // Watch descriptor table — page_alloc'd pointer array, grows by doubling.
  // Index = wd - 1 (wds are 1-based). nullptr = unused slot.
  InotifyWatch **watches;
  uint32_t watch_capacity; // Current array size.
  uint32_t watch_count;    // Number of active (non-null) watches.
  int next_wd;             // Next wd to allocate (monotonic, 1-based).

  // Rename cookie counter — monotonic, never zero.
  uint32_t next_cookie;

  // Cooked event output buffer — page_alloc'd, linear.
  // read() copies from event_head; reactor callback appends at event_tail.
  // Compacted (memmove) when head advances past half capacity.
  uint8_t *event_buf;
  uint32_t event_head;     // Read offset in bytes.
  uint32_t event_tail;     // Write offset in bytes.
  uint32_t event_last;     // Offset of last event (for coalescing).
  uint32_t event_capacity; // Total buffer size in bytes.
  bool overflow;           // IN_Q_OVERFLOW pending.

  // Initial watch table capacity (pointer count).
  static constexpr uint32_t INITIAL_WATCH_CAPACITY = 16;

  // Initial event buffer size.
  static constexpr uint32_t INITIAL_EVENT_CAPACITY = 65536; // 64KB
};

//===----------------------------------------------------------------------===//
// Allocation helpers
//===----------------------------------------------------------------------===//

/// Allocate and zero-initialize an InotifyWatch.
LIBC_INLINE InotifyWatch *inotify_watch_alloc() {
  auto *w = static_cast<InotifyWatch *>(
      page_alloc(InotifyWatch::ALLOC_SIZE));
  if (w)
    __builtin_memset(w, 0, InotifyWatch::ALLOC_SIZE);
  return w;
}

/// Free an InotifyWatch, closing its NT handles first.
/// reactor::unwatch MUST come first — its contract guarantees no callback
/// is executing after it returns, preventing use-after-free races with
/// the drain thread.
LIBC_INLINE void inotify_watch_free(InotifyWatch *w) {
  if (!w)
    return;
  // 1. Unwatch from reactor — blocks until any in-flight callback finishes.
  if (w->reactor_token.slot)
    reactor::unwatch(w->reactor_token);
  // 2. Cancel pending IO now that no callback can re-issue it.
  if (w->dir_handle) {
    IO_STATUS_BLOCK cancel_iosb{};
    NtCancelIoFileEx(w->dir_handle, &w->iosb, &cancel_iosb);
    NtClose(w->dir_handle);
  }
  // 3. Close the event handle.
  if (w->event_handle)
    NtClose(w->event_handle);
  page_free(w);
}

/// Allocate and initialize an InotifyInstance. Returns nullptr on failure.
LIBC_INLINE InotifyInstance *inotify_instance_alloc() {
  // Allocate the instance struct (one page).
  auto *inst = static_cast<InotifyInstance *>(page_alloc(4096));
  if (!inst)
    return nullptr;
  __builtin_memset(inst, 0, 4096);

  // Allocate watch pointer table.
  size_t table_bytes =
      InotifyInstance::INITIAL_WATCH_CAPACITY * sizeof(InotifyWatch *);
  inst->watches = static_cast<InotifyWatch **>(page_alloc(table_bytes));
  if (!inst->watches) {
    page_free(inst);
    return nullptr;
  }
  __builtin_memset(inst->watches, 0, table_bytes);
  inst->watch_capacity = InotifyInstance::INITIAL_WATCH_CAPACITY;

  // Allocate event output buffer.
  inst->event_buf = static_cast<uint8_t *>(
      page_alloc(InotifyInstance::INITIAL_EVENT_CAPACITY));
  if (!inst->event_buf) {
    page_free(inst->watches);
    page_free(inst);
    return nullptr;
  }
  inst->event_capacity = InotifyInstance::INITIAL_EVENT_CAPACITY;

  // First wd is 1; first cookie is 1.
  inst->next_wd = 1;
  inst->next_cookie = 1;

  return inst;
}

/// Free all resources owned by an InotifyInstance.
LIBC_INLINE void inotify_instance_free(InotifyInstance *inst) {
  if (!inst)
    return;

  // Free all active watches.
  for (uint32_t i = 0; i < inst->watch_capacity; ++i) {
    if (inst->watches[i])
      inotify_watch_free(inst->watches[i]);
  }

  if (inst->watches)
    page_free(inst->watches);
  if (inst->event_buf)
    page_free(inst->event_buf);
  page_free(inst);
}

//===----------------------------------------------------------------------===//
// NT action → inotify mask translation
//===----------------------------------------------------------------------===//

/// Map an NT FILE_ACTION_* code to the corresponding IN_* event bit.
LIBC_INLINE uint32_t nt_action_to_inotify_mask(ULONG action) {
  switch (action) {
  case FILE_ACTION_ADDED:
    return IN_CREATE;
  case FILE_ACTION_REMOVED:
  case FILE_ACTION_REMOVED_BY_DELETE:
    return IN_DELETE;
  case FILE_ACTION_MODIFIED:
    return IN_MODIFY;
  case FILE_ACTION_RENAMED_OLD_NAME:
    return IN_MOVED_FROM;
  case FILE_ACTION_RENAMED_NEW_NAME:
    return IN_MOVED_TO;
  default:
    return 0;
  }
}

/// Map an IN_* user mask to NT CompletionFilter bits for
/// NtNotifyChangeDirectoryFileEx.
LIBC_INLINE ULONG inotify_mask_to_nt_filter(uint32_t mask) {
  ULONG filter = 0;

  if (mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO))
    filter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;

  if (mask & IN_MODIFY)
    filter |= FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;

  if (mask & IN_ATTRIB)
    filter |= FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY;

  if (mask & IN_ACCESS)
    filter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;

  if (mask & IN_CLOSE_WRITE)
    filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;

  // IN_OPEN, IN_CLOSE_NOWRITE: no NT equivalent. Silently accepted in
  // the mask but the filter bits are zero — no NT events fire for these.

  // Ensure we always have at least one filter bit if the user asked for
  // any event type, to prevent NtNotifyChangeDirectoryFileEx from failing.
  if (filter == 0 && (mask & IN_ALL_EVENTS))
    filter = FILE_NOTIFY_CHANGE_FILE_NAME;

  return filter;
}

//===----------------------------------------------------------------------===//
// UTF-16LE → UTF-8 converter (dependency-free)
//===----------------------------------------------------------------------===//

/// Convert a UTF-16LE string to UTF-8 in-place into `out`. Returns the
/// number of UTF-8 bytes written (excluding NUL). The output is always
/// NUL-terminated if out_max > 0.
LIBC_INLINE size_t utf16le_to_utf8(const WCHAR *src, size_t src_wchars,
                                   char *out, size_t out_max) {
  size_t j = 0;
  for (size_t i = 0; i < src_wchars && j + 1 < out_max; ++i) {
    uint32_t cp = src[i];

    // Surrogate pair.
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < src_wchars) {
      uint32_t lo = src[i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }

    if (cp < 0x80) {
      out[j++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
      if (j + 2 >= out_max)
        break;
      out[j++] = static_cast<char>(0xC0 | (cp >> 6));
      out[j++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      if (j + 3 >= out_max)
        break;
      out[j++] = static_cast<char>(0xE0 | (cp >> 12));
      out[j++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out[j++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      if (j + 4 >= out_max)
        break;
      out[j++] = static_cast<char>(0xF0 | (cp >> 18));
      out[j++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out[j++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out[j++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  if (out_max > 0)
    out[j] = '\0';
  return j;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_INOTIFY_INSTANCE_H
