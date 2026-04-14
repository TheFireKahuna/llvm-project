//===-- Named semaphore for Windows ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Named semaphore support for Windows-backed libc.
//
// There are currently two name domains:
//   - POSIX names (leading '/') use marker files plus hashed NT names to
//     provide sem_unlink semantics.
//   - Slashless names are an NTPOSIX extension used to open an existing native
//     NT named semaphore by leaf name under \BaseNamedObjects.
//
// Cross-process name lifecycle uses filesystem marker files in libc's temp
// root under llvm_sem\.
// The marker stores a generation counter — sem_unlink deletes the marker
// (visible to all processes immediately), and a subsequent sem_open(O_CREAT)
// creates a fresh kernel semaphore with a new generation in its NT name.
//
// NT object name format: \BaseNamedObjects\ls-<16 hex chars>-<generation>
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_NAMED_SEMAPHORE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_NAMED_SEMAPHORE_H

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/mode_t.h"
#include "include/llvm-libc-types/sem_t.h"
#include "src/__support/macros/config.h"
#include "src/__support/time/abs_timeout.h"

namespace LIBC_NAMESPACE_DECL {

class NamedSemaphore {
public:
  // sem_open: create or open a named semaphore.
  // Names with leading '/' (or slashless) go through the POSIX marker path.
  // Slashless names get a '/' prepended internally — POSIX recommends but
  // does not require the leading slash.
  // Names with leading '\\' are an NTPOSIX extension: open-only against a
  // native NT leaf name under \BaseNamedObjects (no O_CREAT/O_EXCL).
  // mode and value are used only when O_CREAT is set.
  // On failure, returns nullptr and sets *err to the POSIX errno value.
  static sem_t *open(const char *name, int oflag, mode_t mode, unsigned value,
                     int *err);

  // sem_close: close a named semaphore handle.
  static int close(sem_t *sem);

  // sem_unlink: remove the name from the namespace (cross-process).
  // Deletes the marker file; the NT kernel semaphore persists until all
  // handles are closed. Matches POSIX unlink semantics.
  static int unlink(const char *name);

  // Operations on an open named semaphore.
  // wait/timedwait are alertable — they handle signals and cancellation.
  static int post(sem_t *sem);
  static int wait(sem_t *sem);
  static int trywait(sem_t *sem);
  static int timedwait(sem_t *sem, const internal::AbsTimeout &timeout);
  static int getvalue(sem_t *sem, int *sval);
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_NAMED_SEMAPHORE_H
