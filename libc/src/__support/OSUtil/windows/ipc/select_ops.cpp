//===-- Internal select/pselect engine implementation ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// pselect is the primary engine: converts fd_sets to pollfds, delegates to
// ppoll() for readiness (including atomic signal masking), then reconstructs
// the caller's fd_sets. select() converts timeval → timespec and delegates.
//
// Returns non-negative ready count on success, -errno on failure.
// No libc_errno references.
//
//===----------------------------------------------------------------------===//

#include "select_ops.h"

#include "hdr/types/struct_pollfd.h"
#include "include/llvm-libc-macros/poll-macros.h"
#include "include/llvm-libc-macros/sys-select-macros.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/poll_ops.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

//===----------------------------------------------------------------------===//
// pselect — primary engine
//===----------------------------------------------------------------------===//

intptr_t pselect(int nfds, fd_set *__restrict read_set,
                 fd_set *__restrict write_set, fd_set *__restrict error_set,
                 const struct timespec *__restrict timeout,
                 const sigset_t *__restrict sigmask) {
  if (nfds < 0 || nfds > FD_SETSIZE)
    return -EINVAL;

  fd_set r_out, w_out, e_out;
  FD_ZERO(&r_out);
  FD_ZERO(&w_out);
  FD_ZERO(&e_out);

  struct pollfd pfds[FD_SETSIZE];
  int poll_to_fd[FD_SETSIZE];
  nfds_t poll_count = 0;

  for (int fd = 0; fd < nfds; ++fd) {
    bool watch_read = read_set && FD_ISSET(fd, read_set);
    bool watch_write = write_set && FD_ISSET(fd, write_set);
    bool watch_error = error_set && FD_ISSET(fd, error_set);
    if (!watch_read && !watch_write && !watch_error)
      continue;

    if (!fd_table.get_ofd(fd))
      return -EBADF;

    short events = 0;
    if (watch_read)
      events |= POLLIN | POLLRDNORM;
    if (watch_write)
      events |= POLLOUT | POLLWRNORM;
    if (watch_error)
      events |= POLLPRI;

    pfds[poll_count].fd = fd;
    pfds[poll_count].events = events;
    pfds[poll_count].revents = 0;
    poll_to_fd[poll_count] = fd;
    ++poll_count;
  }

  intptr_t poll_result = internal::ppoll(pfds, poll_count, timeout, sigmask);
  if (poll_result < 0)
    return poll_result;

  int ready = 0;
  for (nfds_t i = 0; i < poll_count; ++i) {
    int fd = poll_to_fd[i];
    bool fd_ready = false;
    bool watch_read = read_set && FD_ISSET(fd, read_set);
    bool watch_write = write_set && FD_ISSET(fd, write_set);
    bool watch_error = error_set && FD_ISSET(fd, error_set);
    short revents = pfds[i].revents;

    if (watch_read && (revents & (POLLIN | POLLRDNORM | POLLHUP))) {
      FD_SET(fd, &r_out);
      fd_ready = true;
    }
    if (watch_write && (revents & (POLLOUT | POLLWRNORM))) {
      FD_SET(fd, &w_out);
      fd_ready = true;
    }
    if (watch_error && (revents & (POLLPRI | POLLERR | POLLHUP | POLLNVAL))) {
      FD_SET(fd, &e_out);
      fd_ready = true;
    }
    if (fd_ready)
      ++ready;
  }

  if (read_set)
    *read_set = r_out;
  if (write_set)
    *write_set = w_out;
  if (error_set)
    *error_set = e_out;
  return ready;
}

//===----------------------------------------------------------------------===//
// select — thin wrapper over pselect
//===----------------------------------------------------------------------===//

intptr_t select(int nfds, fd_set *__restrict read_set,
                fd_set *__restrict write_set, fd_set *__restrict error_set,
                struct timeval *__restrict timeout) {
  const struct timespec *ts_ptr = nullptr;
  struct timespec ts;
  if (timeout) {
    if (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
        timeout->tv_usec >= 1000000)
      return -EINVAL;
    ts.tv_sec = timeout->tv_sec;
    ts.tv_nsec = static_cast<long>(timeout->tv_usec) * 1000L;
    ts_ptr = &ts;
  }
  return pselect(nfds, read_set, write_set, error_set, ts_ptr, nullptr);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
