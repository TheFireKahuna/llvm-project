//===-- Inotify implementation for NT-POSIX --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linux inotify(7) implementation on top of NtNotifyChangeDirectoryFileEx.
//
// Architecture:
//   - Each InotifyWatch opens a directory handle and issues an async
//     NtNotifyChangeDirectoryFileEx with a per-watch NT event handle.
//   - The event handle is bridged to the reactor IOCP via a
//     WaitCompletionPacket (reactor::watch / reactor::rearm).
//   - When a notification completes, the reactor drain thread invokes
//     inotify_watch_callback which parses NT FILE_NOTIFY_INFORMATION
//     entries, translates them to inotify_event records, and appends
//     them to the instance's event buffer.
//   - read() on the inotify fd drains the event buffer.
//   - The inotify fd's NT handle is a NotificationEvent (readable_event)
//     signaled when events are available, cleared when the buffer empties.
//
// Limitations:
//   - IN_OPEN, IN_ACCESS, IN_CLOSE_NOWRITE: no NT equivalent, silently
//     accepted but never fire.
//   - IN_DELETE_SELF / IN_MOVE_SELF: detected via STATUS_DELETE_PENDING
//     on the watched directory (best-effort).
//
//===----------------------------------------------------------------------===//

#include "inotify_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/error_or.h"
#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/inotify_instance.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_path_convert.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

static void inotify_watch_callback(void *context, NTSTATUS status,
                                   ULONG_PTR information);
static int issue_notify(InotifyWatch *w);
static int grow_watch_table(InotifyInstance *inst, uint32_t needed);
static void compact_event_buffer(InotifyInstance *inst);
static bool push_event(InotifyInstance *inst, int wd, uint32_t mask,
                       uint32_t cookie, const char *name, size_t name_len);

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// Retrieve the InotifyInstance from an inotify fd. Returns nullptr if
/// the fd is not a valid inotify descriptor.
static InotifyInstance *get_instance(int fd) {
  auto *ofd = fd_table.get_ofd(fd);
  if (!ofd || !ofd->is_inotify())
    return nullptr;
  return ofd->inotify_inst();
}

/// Issue NtNotifyChangeDirectoryFileEx on a watch. The event_handle is
/// signaled on completion, which triggers the reactor WCP.
/// Returns 0 on success, -errno on failure.
static int issue_notify(InotifyWatch *w) {
  ULONG filter = inotify_mask_to_nt_filter(w->mask);
  if (filter == 0)
    return 0; // No NT-observable events requested.

  w->iosb.Status = STATUS_PENDING;
  w->iosb.Information = 0;

  NTSTATUS status = NtNotifyChangeDirectoryFileEx(
      w->dir_handle,
      w->event_handle,  // Signaled on completion.
      nullptr,          // No APC.
      nullptr,          // No APC context.
      &w->iosb,
      w->notify_buf(),
      InotifyWatch::NOTIFY_BUF_SIZE,
      filter,
      false, // WatchTree = false (inotify is per-directory).
      DirectoryNotifyInformation);

  if (status != STATUS_PENDING && !NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  return 0;
}

/// Grow the watch table to accommodate at least `needed` entries.
/// Must be called with inst->lock held.
static int grow_watch_table(InotifyInstance *inst, uint32_t needed) {
  if (needed <= inst->watch_capacity)
    return 0;

  uint32_t new_cap = inst->watch_capacity;
  while (new_cap < needed)
    new_cap *= 2;

  size_t new_bytes = new_cap * sizeof(InotifyWatch *);
  auto *new_table = static_cast<InotifyWatch **>(page_alloc(new_bytes));
  if (!new_table)
    return -ENOMEM;
  __builtin_memset(new_table, 0, new_bytes);

  // Copy existing entries.
  size_t old_bytes = inst->watch_capacity * sizeof(InotifyWatch *);
  __builtin_memcpy(new_table, inst->watches, old_bytes);

  page_free(inst->watches);
  inst->watches = new_table;
  inst->watch_capacity = new_cap;
  return 0;
}

/// Compact the event buffer by moving unread data to the front.
/// Must be called with inst->lock held.
static void compact_event_buffer(InotifyInstance *inst) {
  if (inst->event_head == 0)
    return;
  uint32_t remaining = inst->event_tail - inst->event_head;
  if (remaining > 0)
    __builtin_memmove(inst->event_buf, inst->event_buf + inst->event_head,
                      remaining);
  uint32_t shift = inst->event_head;
  inst->event_head = 0;
  inst->event_tail = remaining;
  // Adjust event_last to reflect the compacted positions.
  inst->event_last = (inst->event_last >= shift) ? inst->event_last - shift : 0;
}

/// Append a cooked inotify_event to the instance's event buffer.
/// Performs event coalescing: if the last buffered event has the same
/// wd, mask, cookie, and name, the duplicate is dropped (Linux behavior).
/// Must be called with inst->lock held.
/// Returns true if the event was appended (or coalesced), false on overflow.
static bool push_event(InotifyInstance *inst, int wd, uint32_t mask,
                       uint32_t cookie, const char *name, size_t name_len) {
  // Compute padded name length. The len field includes NUL + padding
  // to align the next event record.
  uint32_t padded_name_len = 0;
  if (name && name_len > 0) {
    // Round up (name_len + 1 for NUL) to next multiple of
    // sizeof(inotify_event) for alignment.
    padded_name_len = static_cast<uint32_t>(
        ((name_len + 1) + (INOTIFY_EVENT_ALIGN - 1)) & ~(INOTIFY_EVENT_ALIGN - 1));
  }

  uint32_t event_size =
      static_cast<uint32_t>(sizeof(inotify_event)) + padded_name_len;

  // Coalesce: check if the last buffered event matches (O(1) via event_last).
  if (inst->event_tail > inst->event_head) {
    auto *last = reinterpret_cast<inotify_event *>(
        inst->event_buf + inst->event_last);
    if (last->wd == wd && last->mask == mask && last->cookie == cookie &&
        last->len == padded_name_len) {
      if (padded_name_len == 0)
        return true; // Coalesced (no name).
      const char *last_name =
          reinterpret_cast<const char *>(last) + sizeof(inotify_event);
      if (__builtin_memcmp(last_name, name, name_len) == 0)
        return true; // Coalesced.
    }
  }

  // Check space. Compact if needed.
  if (inst->event_tail + event_size > inst->event_capacity) {
    compact_event_buffer(inst);
    if (inst->event_tail + event_size > inst->event_capacity)
      return false; // Overflow.
  }

  // Write event.
  auto *ev = reinterpret_cast<inotify_event *>(
      inst->event_buf + inst->event_tail);
  ev->wd = wd;
  ev->mask = mask;
  ev->cookie = cookie;
  ev->len = padded_name_len;

  if (padded_name_len > 0) {
    char *dst = reinterpret_cast<char *>(ev) + sizeof(inotify_event);
    // Zero the padded region first, then copy name + NUL.
    __builtin_memset(dst, 0, padded_name_len);
    __builtin_memcpy(dst, name, name_len);
    // NUL is already there from memset.
  }

  inst->event_last = inst->event_tail;
  inst->event_tail += event_size;
  return true;
}

//===----------------------------------------------------------------------===//
// Reactor callback — runs on the drain thread
//===----------------------------------------------------------------------===//

/// Called by the reactor when an NtNotifyChangeDirectoryFileEx completion
/// signals the watch's event handle via the WCP bridge.
static void inotify_watch_callback(void *context, NTSTATUS /*status*/,
                                   ULONG_PTR /*information*/) {
  auto *w = static_cast<InotifyWatch *>(context);
  if (w->removed)
    return;

  InotifyInstance *inst = w->inst;
  if (!inst)
    return;

  inst->lock.lock();

  NTSTATUS notify_status = w->iosb.Status;

  // Helper: evict a watch from the table, push IN_IGNORED, and schedule
  // cleanup. Called with inst->lock held; releases it before returning.
  // Uses reactor::detach (safe from within the callback) instead of
  // reactor::unwatch (which would deadlock on the drain thread).
  auto teardown_watch = [&]() {
    uint32_t idx = static_cast<uint32_t>(w->wd - 1);
    if (idx < inst->watch_capacity && inst->watches[idx] == w) {
      inst->watches[idx] = nullptr;
      inst->watch_count--;
    }
    w->removed = true;
    NtSetEvent(inst->readable_event, nullptr);
    inst->lock.unlock();

    // Detach from reactor (self-unwatch, safe on drain thread).
    reactor::detach(w->reactor_token);
    w->reactor_token = reactor::INVALID_TOKEN;
    // Cancel pending IO and close handles. No reactor callback can
    // fire after detach, so this is safe.
    if (w->dir_handle) {
      IO_STATUS_BLOCK cancel_iosb{};
      NtCancelIoFileEx(w->dir_handle, &w->iosb, &cancel_iosb);
      NtClose(w->dir_handle);
      w->dir_handle = nullptr;
    }
    if (w->event_handle) {
      NtClose(w->event_handle);
      w->event_handle = nullptr;
    }
    page_free(w);
  };

  // Handle error/cleanup statuses.
  if (notify_status == STATUS_DELETE_PENDING ||
      notify_status == STATUS_NOTIFY_CLEANUP) {
    // Directory was deleted or unmounted.
    push_event(inst, w->wd, IN_DELETE_SELF, 0, nullptr, 0);
    push_event(inst, w->wd, IN_IGNORED, 0, nullptr, 0);
    teardown_watch(); // unlocks, detaches, frees w
    return;
  }

  if (!NT_SUCCESS(notify_status)) {
    // Unexpected error — push overflow inline and tear down.
    if (!push_event(inst, -1, IN_Q_OVERFLOW, 0, nullptr, 0))
      inst->overflow = true;
    push_event(inst, w->wd, IN_IGNORED, 0, nullptr, 0);
    teardown_watch();
    return;
  }

  // Parse FILE_NOTIFY_INFORMATION entries.
  ULONG_PTR bytes_returned = w->iosb.Information;
  if (bytes_returned == 0) {
    // NT dropped events (buffer overflow). Push IN_Q_OVERFLOW inline
    // so it appears in-order with surrounding events.
    if (!push_event(inst, -1, IN_Q_OVERFLOW, 0, nullptr, 0))
      inst->overflow = true; // Last resort if buffer is completely full.
    NtSetEvent(inst->readable_event, nullptr);
  } else {
    uint8_t *buf = w->notify_buf();
    uint32_t offset = 0;
    uint32_t rename_cookie = 0;

    for (;;) {
      auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buf + offset);
      uint32_t mask = nt_action_to_inotify_mask(info->Action);

      if (mask != 0) {
        // Generate rename cookie for paired rename events.
        uint32_t cookie = 0;
        if (mask == IN_MOVED_FROM) {
          rename_cookie = inst->next_cookie++;
          if (rename_cookie == 0)
            rename_cookie = inst->next_cookie++; // Skip zero.
          cookie = rename_cookie;
        } else if (mask == IN_MOVED_TO) {
          cookie = rename_cookie;
          rename_cookie = 0;
        }

        // Check if event matches watch mask (plus always-delivered events).
        if (mask & (w->mask | IN_ALWAYS_EVENTS)) {
          // Convert filename from UTF-16LE to UTF-8.
          size_t name_wchars = info->FileNameLength / sizeof(WCHAR);
          char name_utf8[1024];
          size_t name_len = utf16le_to_utf8(
              info->FileName, name_wchars, name_utf8, sizeof(name_utf8));

          if (!push_event(inst, w->wd, mask, cookie, name_utf8, name_len)) {
            // Push overflow inline at the point events were lost.
            if (!push_event(inst, -1, IN_Q_OVERFLOW, 0, nullptr, 0))
              inst->overflow = true;
          }
        }
      }

      if (info->NextEntryOffset == 0)
        break;
      offset += info->NextEntryOffset;
    }
  }

  bool have_events =
      (inst->event_tail > inst->event_head) || inst->overflow;
  if (have_events)
    NtSetEvent(inst->readable_event, nullptr);

  // Handle IN_ONESHOT: stop watching after first batch.
  if (w->oneshot && !w->oneshot_fired) {
    w->oneshot_fired = true;
    push_event(inst, w->wd, IN_IGNORED, 0, nullptr, 0);
    teardown_watch(); // unlocks, detaches, frees w
    return;
  }

  inst->lock.unlock();

  // Re-arm: clear the event and re-issue the notification.
  NtClearEvent(w->event_handle);
  issue_notify(w);
  reactor::rearm(w->reactor_token);
}

//===----------------------------------------------------------------------===//
// inotify_init1
//===----------------------------------------------------------------------===//

intptr_t inotify_init1(int flags) {
  // Validate flags.
  if (flags & ~(IN_NONBLOCK | IN_CLOEXEC))
    return -EINVAL;

  // Create the readable event (manual-reset, initially unsignaled).
  windows::ScopedNtHandle readable_event;
  auto evt_oa = windows::internal_oa();
  NTSTATUS status = NtCreateEvent(readable_event.put(), EVENT_ALL_ACCESS,
                                  &evt_oa, NotificationEvent, false);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  // Allocate the instance.
  InotifyInstance *inst = inotify_instance_alloc();
  if (!inst)
    return -ENOMEM;
  auto free_inst =
      cpp::make_scope_guard([&] { inotify_instance_free(inst); });

  inst->readable_event = readable_event.get();

  // Allocate an fd. The readable_event is the NT handle behind the fd.
  int oflags = O_RDONLY;
  if (flags & IN_NONBLOCK)
    oflags |= O_NONBLOCK;
  auto result =
      fd_table.alloc(readable_event.get(), oflags, 0, FileKind::Inotify);
  if (!result)
    return -EMFILE;

  // All resources committed — dismiss guards.
  free_inst.dismiss();
  (void)readable_event.release();

  // Store the inotify instance in the OFD aux union.
  auto *ofd = fd_table.get_ofd(result.value());
  ofd->set_inotify_inst(inst);

  if (flags & IN_CLOEXEC)
    fd_table.set_fd_cloexec(result.value(), true);

  return result.value();
}

//===----------------------------------------------------------------------===//
// inotify_add_watch
//===----------------------------------------------------------------------===//

intptr_t inotify_add_watch(int fd, const char *pathname, uint32_t mask) {
  if (!pathname)
    return -EFAULT;

  // Validate mask: must have at least one event bit.
  if (!(mask & IN_ALL_EVENTS))
    return -EINVAL;

  // Check for invalid flag combinations.
  if (mask & ~IN_USER_MASK)
    return -EINVAL;

  InotifyInstance *inst = get_instance(fd);
  if (!inst)
    return -EBADF;

  // Separate event bits from control flags.
  uint32_t event_mask = mask & IN_ALL_EVENTS;
  bool oneshot = (mask & IN_ONESHOT) != 0;
  bool dont_follow = (mask & IN_DONT_FOLLOW) != 0;
  bool mask_add = (mask & IN_MASK_ADD) != 0;
  bool mask_create = (mask & IN_MASK_CREATE) != 0;

  // Convert path to NT path.
  using LIBC_NAMESPACE::cpp::string_view;
  WCHAR nt_path[512];
  string_view pathname_sv(pathname);
  auto nt = to_nt_path(pathname_sv, nt_path, 512);
  if (!nt.has_value())
    return -nt.error();
  size_t path_len = nt.value();

  windows::nt_wstring_view path_wsv(nt_path, path_len);

  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(oa);
  oa.RootDirectory = nullptr;
  oa.ObjectName = path_wsv.unicode_string();
  oa.Attributes = OBJ_CASE_INSENSITIVE;
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;

  // Open the directory.
  ULONG open_options = FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT;
  if (dont_follow)
    open_options |= FILE_OPEN_REPARSE_POINT;

  windows::ScopedNtHandle dir_handle;
  IO_STATUS_BLOCK open_iosb{};
  NTSTATUS status = NtCreateFile(
      dir_handle.put(),
      FILE_LIST_DIRECTORY | SYNCHRONIZE,
      &oa, &open_iosb, nullptr,
      0, // FileAttributes
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_OPEN, // Disposition — must already exist.
      open_options,
      nullptr, 0);

  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  inst->lock.lock();

  // Check for existing watch on the same directory by comparing
  // the file ID (avoids path aliasing issues).
  FILE_INTERNAL_INFORMATION file_id_info;
  IO_STATUS_BLOCK id_iosb{};
  status = NtQueryInformationFile(dir_handle.get(), &id_iosb, &file_id_info,
                                  sizeof(file_id_info),
                                  FileInternalInformation);
  LARGE_INTEGER new_file_id{};
  if (NT_SUCCESS(status))
    new_file_id = file_id_info.IndexNumber;

  // Scan existing watches for a match on cached file ID.
  for (uint32_t i = 0; i < inst->watch_capacity; ++i) {
    InotifyWatch *existing = inst->watches[i];
    if (!existing)
      continue;

    if (existing->file_id.QuadPart == new_file_id.QuadPart) {
      // Same directory — modify existing watch.
      if (mask_create) {
        inst->lock.unlock();
        return -EEXIST;
      }

      // Cancel pending IO and re-issue with updated mask.
      IO_STATUS_BLOCK cancel_iosb{};
      NtCancelIoFileEx(existing->dir_handle, &existing->iosb, &cancel_iosb);

      // Unwatch old reactor token and close old event.
      if (existing->reactor_token.valid())
        reactor::unwatch(existing->reactor_token);
      if (existing->event_handle)
        NtClearEvent(existing->event_handle);

      // Update mask.
      if (mask_add)
        existing->mask |= event_mask;
      else
        existing->mask = event_mask;
      existing->oneshot = oneshot;
      existing->oneshot_fired = false;

      // Close the duplicate directory handle — keep the existing one.
      dir_handle.reset();

      // Re-issue notification with new mask.
      int err = issue_notify(existing);
      if (err < 0) {
        inst->lock.unlock();
        return err;
      }

      // Re-register with reactor.
      existing->reactor_token = reactor::watch(
          existing->event_handle, inotify_watch_callback, existing);
      if (!existing->reactor_token.valid()) {
        inst->lock.unlock();
        return -ENOMEM;
      }

      int wd = existing->wd;
      inst->lock.unlock();
      return wd;
    }
  }

  // No existing watch — create a new one.

  // Ensure watch table has room.
  int wd = inst->next_wd;
  uint32_t idx = static_cast<uint32_t>(wd - 1);
  if (idx >= inst->watch_capacity) {
    int err = grow_watch_table(inst, idx + 1);
    if (err < 0) {
      inst->lock.unlock();
      return err;
    }
  }
  inst->next_wd = wd + 1;

  // Allocate watch.
  InotifyWatch *w = inotify_watch_alloc();
  if (!w) {
    inst->lock.unlock();
    return -ENOMEM;
  }

  w->wd = wd;
  w->dir_handle = dir_handle.get();
  w->mask = event_mask;
  w->file_id = new_file_id;
  w->oneshot = oneshot;
  w->dont_follow = dont_follow;
  w->inst = inst;

  // Persist the NT path so fork_reinit_instance can reopen the directory
  // in the child (dir_handle is non-inheritable). page_alloc rounds up to
  // page granularity — cheap compared to the 8KB watch arena itself.
  size_t path_bytes = path_len * sizeof(WCHAR);
  w->saved_path = static_cast<uint16_t *>(page_alloc(path_bytes));
  if (!w->saved_path) {
    page_free(w);
    inst->lock.unlock();
    return -ENOMEM;
  }
  __builtin_memcpy(w->saved_path, nt_path, path_bytes);
  w->saved_path_len = static_cast<uint32_t>(path_len);

  // Create per-watch event handle.
  windows::ScopedNtHandle watch_event;
  auto wevt_oa = windows::internal_oa();
  status = NtCreateEvent(watch_event.put(), EVENT_ALL_ACCESS, &wevt_oa,
                         NotificationEvent, false);
  if (!NT_SUCCESS(status)) {
    page_free(w);
    inst->lock.unlock();
    return -windows_util::ntstatus_to_errno(status);
  }
  w->event_handle = watch_event.get();

  // Issue the first notification.
  int err = issue_notify(w);
  if (err < 0) {
    page_free(w);
    inst->lock.unlock();
    return err;
  }

  // Register with reactor for completion dispatch.
  w->reactor_token =
      reactor::watch(watch_event.get(), inotify_watch_callback, w);
  if (!w->reactor_token.valid()) {
    IO_STATUS_BLOCK cancel_iosb{};
    NtCancelIoFileEx(dir_handle.get(), &w->iosb, &cancel_iosb);
    page_free(w);
    inst->lock.unlock();
    return -ENOMEM;
  }

  // Commit: release ownership of dir_handle and watch_event to the watch.
  (void)dir_handle.release();
  (void)watch_event.release();
  inst->watches[idx] = w;
  inst->watch_count++;

  inst->lock.unlock();
  return wd;
}

//===----------------------------------------------------------------------===//
// inotify_rm_watch
//===----------------------------------------------------------------------===//

intptr_t inotify_rm_watch(int fd, int wd) {
  InotifyInstance *inst = get_instance(fd);
  if (!inst)
    return -EBADF;

  if (wd < 1)
    return -EINVAL;

  inst->lock.lock();

  uint32_t idx = static_cast<uint32_t>(wd - 1);
  if (idx >= inst->watch_capacity || !inst->watches[idx]) {
    inst->lock.unlock();
    return -EINVAL;
  }

  InotifyWatch *w = inst->watches[idx];
  inst->watches[idx] = nullptr;
  inst->watch_count--;

  // Queue IN_IGNORED event before cleanup.
  push_event(inst, wd, IN_IGNORED, 0, nullptr, 0);
  NtSetEvent(inst->readable_event, nullptr);

  // Mark as removed so the reactor callback (if in-flight) is a no-op.
  w->removed = true;

  inst->lock.unlock();

  // Cleanup outside the lock — cancel IO, unwatch reactor, close handles.
  inotify_watch_free(w);

  return 0;
}

//===----------------------------------------------------------------------===//
// inotify_read_ofd — core read logic operating on an already-validated OFD
//===----------------------------------------------------------------------===//
// Called via FileOps inotify_read_impl() in read_write.cpp.

ssize_t inotify_read_ofd(OpenFileDescription *ofd, void *buf, size_t count) {
  if (!buf)
    return -EFAULT;
  if (count < sizeof(inotify_event))
    return -EINVAL;

  InotifyInstance *inst = ofd->inotify_inst();
  if (!inst)
    return -EBADF;

  bool nonblock =
      (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK) != 0;

  for (;;) {
    inst->lock.lock();

    uint32_t avail = inst->event_tail - inst->event_head;
    if (avail > 0) {
      // Copy as many complete events as fit into the user buffer.
      uint32_t copied = 0;
      uint32_t pos = inst->event_head;

      while (pos < inst->event_tail && copied < count) {
        auto *ev = reinterpret_cast<inotify_event *>(
            inst->event_buf + pos);
        uint32_t ev_size =
            static_cast<uint32_t>(sizeof(inotify_event)) + ev->len;

        if (copied + ev_size > count) {
          if (copied == 0) {
            inst->lock.unlock();
            return -EINVAL;
          }
          break;
        }

        __builtin_memcpy(static_cast<uint8_t *>(buf) + copied,
                         inst->event_buf + pos, ev_size);
        copied += ev_size;
        pos += ev_size;
      }

      inst->event_head = pos;

      if (inst->event_head >= inst->event_tail) {
        inst->event_head = 0;
        inst->event_tail = 0;
        inst->event_last = 0;
        if (!inst->overflow)
          NtClearEvent(inst->readable_event);
      }

      inst->lock.unlock();
      return static_cast<ssize_t>(copied);
    }

    if (inst->overflow) {
      inst->overflow = false;
      auto *ev = static_cast<inotify_event *>(buf);
      ev->wd = -1;
      ev->mask = IN_Q_OVERFLOW;
      ev->cookie = 0;
      ev->len = 0;
      NtClearEvent(inst->readable_event);
      inst->lock.unlock();
      return sizeof(inotify_event);
    }

    inst->lock.unlock();

    if (nonblock)
      return -EAGAIN;

    NTSTATUS wait_status =
        NtWaitForSingleObject(inst->readable_event, true, nullptr);

    if (wait_status == STATUS_USER_APC || wait_status == STATUS_ALERTED)
      return -EINTR;
  }
}

bool inotify_release_aux(OpenFileDescription *ofd) {
  inotify_instance_free(ofd->inotify_inst());
  return true;
}

// ---------------------------------------------------------------------------
// Fork reinit: reopen each watch's directory and re-arm notification.
//
// Linux inotify(7): "The file descriptors created with inotify_init(2) are
// preserved across fork(2), as are the watches associated with the inotify
// object."
//
// The parent's dir_handle / event_handle / reactor_token are all
// non-inheritable and dead in the child. The saved NT path (persisted by
// inotify_add_watch) lets us reopen the directory in the child with the
// same semantics (FILE_DIRECTORY_FILE, optional FILE_OPEN_REPARSE_POINT
// when IN_DONT_FOLLOW was set), recreate the event handle, re-issue
// NtNotifyChangeDirectoryFileEx, and re-register with the new reactor.
//
// If a watched directory disappeared between the parent's add_watch and
// fork, or the child lost permission, we drop that watch (matching the
// Linux "IN_IGNORED on vanished directory" semantic at next poll).
// ---------------------------------------------------------------------------

static bool reopen_watch_for_child(InotifyWatch *w) {
  if (!w->saved_path || w->saved_path_len == 0)
    return false;

  windows::nt_wstring_view path_wsv(reinterpret_cast<const WCHAR *>(w->saved_path),
                                 w->saved_path_len);

  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(oa);
  oa.RootDirectory = nullptr;
  oa.ObjectName = path_wsv.unicode_string();
  oa.Attributes = OBJ_CASE_INSENSITIVE;
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;

  ULONG open_options = FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT;
  if (w->dont_follow)
    open_options |= FILE_OPEN_REPARSE_POINT;

  HANDLE dh = nullptr;
  IO_STATUS_BLOCK open_iosb{};
  NTSTATUS st = NtCreateFile(
      &dh, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &open_iosb, nullptr, 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
      open_options, nullptr, 0);
  if (!NT_SUCCESS(st))
    return false;

  HANDLE eh = nullptr;
  auto wevt_oa = windows::internal_oa();
  st = NtCreateEvent(&eh, EVENT_ALL_ACCESS, &wevt_oa, NotificationEvent, false);
  if (!NT_SUCCESS(st)) {
    NtClose(dh);
    return false;
  }

  w->dir_handle = dh;
  w->event_handle = eh;
  w->iosb = {};
  w->oneshot_fired = false;
  w->removed = false;

  if (issue_notify(w) < 0) {
    NtClose(eh);
    NtClose(dh);
    w->dir_handle = nullptr;
    w->event_handle = nullptr;
    return false;
  }

  w->reactor_token = reactor::watch(eh, inotify_watch_callback, w);
  if (!w->reactor_token.valid()) {
    IO_STATUS_BLOCK cancel_iosb{};
    NtCancelIoFileEx(dh, &w->iosb, &cancel_iosb);
    NtClose(eh);
    NtClose(dh);
    w->dir_handle = nullptr;
    w->event_handle = nullptr;
    return false;
  }
  return true;
}

static void fork_reinit_instance(InotifyInstance *inst) {
  // Recreate the per-instance readable_event first so reactor callbacks
  // can signal it when the re-armed watches start producing events.
  inst->readable_event = nullptr;
  {
    auto oa = windows::internal_oa();
    ::NtCreateEvent(&inst->readable_event, EVENT_ALL_ACCESS, &oa,
                    NotificationEvent, 0);
  }

  // Reset the lock (clears both value and stale Treiber wait stack).
  inst->lock.reset_for_fork();

  // Flush the event buffer — parent-side buffered events are not
  // re-delivered; the child starts with a clean slate and will observe
  // changes made after fork.
  inst->event_head = 0;
  inst->event_tail = 0;
  inst->event_last = 0;
  inst->overflow = false;

  // Walk every watch, clear the stale handles, re-arm against the
  // child's new reactor IOCP.
  for (uint32_t i = 0; i < inst->watch_capacity; ++i) {
    auto *w = inst->watches[i];
    if (!w)
      continue;
    // Null stale non-inherited handle pointers before re-arming. The
    // parent's handles are gone; no NtClose is needed or safe.
    w->dir_handle = nullptr;
    w->event_handle = nullptr;
    w->reactor_token = reactor::INVALID_TOKEN;

    if (!reopen_watch_for_child(w)) {
      // Directory vanished or permission lost — drop the slot. saved_path
      // is freed by inotify_watch_free along with the watch arena.
      inotify_watch_free(w);
      inst->watches[i] = nullptr;
      if (inst->watch_count > 0)
        --inst->watch_count;
    }
  }
}

// ---------------------------------------------------------------------------
// FileOps instance — write is EINVAL (inotify is read-only); read drains
// the event buffer; release_aux frees the instance.
// ---------------------------------------------------------------------------

static ssize_t inotify_err_write(OpenFileDescription *, const void *, size_t) {
  return -EINVAL;
}

const FileOps inotify_ops = {
    inotify_read_impl, inotify_err_write, nullptr, nullptr,
    inotify_release_aux,
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Register into .libcops$M. Pulled only when an inotify_init1 / read on
// inotify fd / inotify_add_watch entry point drags this TU.
#include "src/__support/OSUtil/windows/fd/file_ops_section.h"
LIBC_REGISTER_FILE_OPS(Inotify, inotify_ops)

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

void LIBC_NAMESPACE::internal::inotify_fork_reinit() {
  using namespace LIBC_NAMESPACE;
  using namespace LIBC_NAMESPACE::internal;
  // Walk all live fds and reinit any inotify instances.
  fd_table.for_each_live(
      [](int /*fd*/, FdSlot *slot, void * /*ctx*/) {
        auto *ofd = slot->load_ofd(cpp::MemoryOrder::RELAXED);
        if (ofd && ofd->is_inotify())
          fork_reinit_instance(ofd->inotify_inst());
      },
      nullptr);
}

LIBC_REGISTER_FORK_REINIT(inotify,
                          ::LIBC_NAMESPACE::internal::kForkPrioInotify,
                          &::LIBC_NAMESPACE::internal::inotify_fork_reinit)
