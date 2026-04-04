//===-- Classes to capture properties of Windows applications ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_CONFIG_WINDOWS_APP_H
#define LLVM_LIBC_CONFIG_WINDOWS_APP_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// Data structure which captures properties of a Windows application.
struct AppProperties {
  // Page size used for the application (typically 4096 on x64).
  uintptr_t page_size;

  // Allocation granularity (typically 65536 on x64).
  uintptr_t alloc_granularity;

  // Module handle for the main executable.
  void *module_handle;

  // TLS index for thread-local atexit callbacks.
  unsigned long tls_atexit_index;

  // DLL notification cookie for __cxa_finalize on DLL unload.
  void *dll_notify_cookie;

  // Command line arguments.
  int argc;
  char **argv;

  // Environment pointer.
  char **env_ptr;
};

extern AppProperties app;

// The descriptor of a thread's TLS area.
// On Windows, we use direct TEB TLS slots with .CRT$XLC cleanup callbacks.
struct TLSDescriptor {
  // The size of the TLS area (not used on Windows, TLS handles this).
  uintptr_t size = 0;

  // The TLS index for this thread's storage.
  unsigned long tls_index = 0xFFFFFFFF; // TLS_OUT_OF_INDEXES

  // Reserved for compatibility with Linux TLSDescriptor.
  uintptr_t tp = 0;
};

// Initialize the TLS area for the current thread.
void init_tls(TLSDescriptor &tls);

// Cleanup the TLS area as described in |tls_descriptor|.
void cleanup_tls(uintptr_t tls_addr, uintptr_t tls_size);

// Set the thread pointer for the current thread.
// On Windows, this is a no-op as we use TLS.
bool set_thread_ptr(uintptr_t val);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_CONFIG_WINDOWS_APP_H
