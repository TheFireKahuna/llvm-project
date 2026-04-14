//===-- Implementation of psiginfo ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/signal/psiginfo.h"

#include "src/signal/signal_print_utils.h"
#include "src/__support/StringUtil/error_to_string.h"
#include "src/__support/StringUtil/signal_to_string.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace {

LIBC_INLINE void append_sender(internal::SignalTextWriter &writer,
                               const siginfo_t &info) {
  writer.write(" pid=");
  writer.write_dec(info.si_pid);
  writer.write(" uid=");
  writer.write_dec(info.si_uid);
}

LIBC_INLINE void append_common_details(internal::SignalTextWriter &writer,
                                       const siginfo_t &info) {
  writer.write(" [code=");
  writer.write_dec(info.si_code);
  if (info.si_errno != 0) {
    writer.write(" errno=");
    writer.write_trimmed(get_error_string(info.si_errno));
  }
}

LIBC_INLINE void append_signal_specific_details(internal::SignalTextWriter &writer,
                                                const siginfo_t &info) {
  switch (info.si_signo) {
  case SIGCHLD:
    append_sender(writer, info);
    writer.write(" status=");
    writer.write_dec(info.si_status);
    return;
  case SIGILL:
  case SIGFPE:
  case SIGSEGV:
  case SIGBUS:
  case SIGTRAP:
    writer.write(" addr=");
    writer.write_ptr(info.si_addr);
    return;
#ifdef SIGSYS
  case SIGSYS:
    writer.write(" addr=");
    writer.write_ptr(info.si_call_addr);
    writer.write(" syscall=");
    writer.write_dec(info.si_syscall);
    writer.write(" arch=");
    writer.write_dec(info.si_arch);
    return;
#endif
#ifdef SIGPOLL
  case SIGPOLL:
    writer.write(" fd=");
    writer.write_dec(info.si_fd);
    writer.write(" band=");
    writer.write_dec(info.si_band);
    return;
#elif defined(SIGIO)
  case SIGIO:
    writer.write(" fd=");
    writer.write_dec(info.si_fd);
    writer.write(" band=");
    writer.write_dec(info.si_band);
    return;
#endif
  default:
    break;
  }

  if (info.si_code <= 0)
    append_sender(writer, info);

#ifdef SI_QUEUE
  if (info.si_code == SI_QUEUE) {
    writer.write(" int=");
    writer.write_dec(info.si_value.sival_int);
    writer.write(" ptr=");
    writer.write_ptr(info.si_value.sival_ptr);
  }
#endif
#ifdef SI_TIMER
  if (info.si_code == SI_TIMER) {
    writer.write(" timer=");
    writer.write_dec(info.si_timerid);
    writer.write(" overrun=");
    writer.write_dec(info.si_overrun);
    writer.write(" int=");
    writer.write_dec(info.si_value.sival_int);
  }
#endif
}

} // namespace

LLVM_LIBC_FUNCTION(void, psiginfo, (const siginfo_t *info, const char *message)) {
  internal::SignalTextWriter writer(stderr);
  internal::write_message_prefix(writer, message);

  if (info == nullptr) {
    writer.write("null siginfo\n");
    writer.finish();
    return;
  }

  writer.write_trimmed(get_signal_string(info->si_signo));
  append_common_details(writer, *info);
  append_signal_specific_details(writer, *info);
  writer.write("]\n");
  writer.finish();
}

} // namespace LIBC_NAMESPACE_DECL
