//===-- Windows mman test utilities ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared helpers for the libc/test/src/sys/mman/windows test suite. Two
// concerns live here today:
//
//   * make_unique_name(prefix, out, out_len): build a per-invocation
//     unique name in `out`, formatted as "<prefix>_<pid>_<tid>_<seq>".
//     The pid+tid suffix prevents collisions when the libc test runner
//     dispatches `ninja -j 32 check-libc` (or similar parallelism)
//     against multiple test processes — the shm / memfd namespace is
//     process-global on Windows. The local seq counter prevents
//     collisions between successive tests in the same process.
//
//   * ShmUnlinkGuard: RAII guard that unconditionally `shm_unlink`s its
//     name on destruction so that an assertion failure mid-test doesn't
//     leak the named object into the OS kernel namespace and cause
//     follow-on runs of the same test to spuriously hit EEXIST.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TEST_SRC_SYS_MMAN_WINDOWS_TEST_UTILS_H
#define LLVM_LIBC_TEST_SRC_SYS_MMAN_WINDOWS_TEST_UTILS_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/sys/mman/shm_unlink.h"
#include "src/unistd/getpid.h"
#include "src/unistd/gettid.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace mman_test_utils {

inline LIBC_NAMESPACE::cpp::Atomic<uint32_t> &unique_seq_counter() {
  static LIBC_NAMESPACE::cpp::Atomic<uint32_t> seq{0};
  return seq;
}

// Append a hex u64 to `dst` at `pos`; advance `pos`. Returns false if
// the buffer would overflow.
inline bool append_hex(char *dst, size_t cap, size_t &pos, uint64_t v) {
  char tmp[17];
  int n = 0;
  if (v == 0) {
    tmp[n++] = '0';
  } else {
    while (v != 0 && n < 16) {
      uint64_t nib = v & 0xFu;
      tmp[n++] = static_cast<char>(nib < 10 ? '0' + nib : 'a' + (nib - 10));
      v >>= 4;
    }
  }
  if (pos + static_cast<size_t>(n) >= cap)
    return false;
  // Reverse-emit.
  for (int i = n - 1; i >= 0; --i)
    dst[pos++] = tmp[i];
  return true;
}

inline bool append_str(char *dst, size_t cap, size_t &pos, const char *s) {
  while (*s != '\0') {
    if (pos + 1 >= cap)
      return false;
    dst[pos++] = *s++;
  }
  return true;
}

// Build a unique name into `out` of capacity `cap`. Returns false on
// overflow. Format: "<prefix>_<pid>_<tid>_<seq>". `prefix` should
// include any leading '/' the namespace requires (shm_open expects '/').
inline bool make_unique_name(const char *prefix, char *out, size_t cap) {
  if (cap == 0)
    return false;
  size_t pos = 0;
  if (!append_str(out, cap, pos, prefix))
    return false;
  if (!append_str(out, cap, pos, "_"))
    return false;
  uint64_t pid = static_cast<uint64_t>(LIBC_NAMESPACE::getpid());
  if (!append_hex(out, cap, pos, pid))
    return false;
  if (!append_str(out, cap, pos, "_"))
    return false;
  uint64_t tid = static_cast<uint64_t>(LIBC_NAMESPACE::gettid());
  if (!append_hex(out, cap, pos, tid))
    return false;
  if (!append_str(out, cap, pos, "_"))
    return false;
  uint32_t seq = unique_seq_counter().fetch_add(
      1u, LIBC_NAMESPACE::cpp::MemoryOrder::RELAXED);
  if (!append_hex(out, cap, pos, static_cast<uint64_t>(seq)))
    return false;
  if (pos >= cap)
    return false;
  out[pos] = '\0';
  return true;
}

// RAII helper that shm_unlinks its name on destruction. Constructing
// the guard immediately attempts an idempotent unlink so prior-run
// leakage (e.g. a previous run that aborted under ASSERT failure) does
// not block O_CREAT|O_EXCL in the current test.
class ShmUnlinkGuard {
public:
  explicit ShmUnlinkGuard(const char *name) : name_(name) {
    (void)LIBC_NAMESPACE::shm_unlink(name_);
  }
  ~ShmUnlinkGuard() { (void)LIBC_NAMESPACE::shm_unlink(name_); }
  ShmUnlinkGuard(const ShmUnlinkGuard &) = delete;
  ShmUnlinkGuard &operator=(const ShmUnlinkGuard &) = delete;
  const char *name() const { return name_; }

private:
  const char *name_;
};

} // namespace mman_test_utils
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_TEST_SRC_SYS_MMAN_WINDOWS_TEST_UTILS_H
