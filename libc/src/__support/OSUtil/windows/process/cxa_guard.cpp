//===-- __cxa_guard for Itanium C++ ABI on Windows ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread-safe static local variable guard for the Itanium C++ ABI on COFF.
// c.dll loads before libc++abi, so it carries its own guard implementation.
// Uses RtlWaitOnAddress (ntdll) for contention — no libc deps.
//
// Guard variable layout (Itanium ABI, 64-bit COFF):
//   Byte 0: init flag (0 = pending, 1 = complete)
//   Byte 1: lock flag (0 = free, 1 = held)
//   Bytes 2-7: unused
//
//===----------------------------------------------------------------------===//

// Minimal ntdll declarations — keep this freestanding.
extern "C" {
using NTSTATUS = long;
using PVOID = void *;
using SIZE_T = unsigned long long;
#define NT_SUCCESS(s) ((s) >= 0)

__declspec(dllimport) NTSTATUS __stdcall RtlWaitOnAddress(
    volatile void *Address, void *CompareAddress, SIZE_T AddressSize,
    void *Timeout);
__declspec(dllimport) void __stdcall RtlWakeAddressSingle(PVOID Address);
__declspec(dllimport) void __stdcall RtlWakeAddressAll(PVOID Address);
} // extern "C"

using GuardType = unsigned long long;

extern "C" {

int __cxa_guard_acquire(GuardType *guard) {
  auto *init_byte = reinterpret_cast<unsigned char *>(guard);
  auto *lock_byte = init_byte + 1;

  // Fast path: already initialized.
  if (__atomic_load_n(init_byte, __ATOMIC_ACQUIRE))
    return 0;

  for (;;) {
    unsigned char expected = 0;
    if (__atomic_compare_exchange_n(lock_byte, &expected, 1u,
                                    /*weak=*/false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      // Acquired. Double-check init flag (may have completed between
      // the fast-path check and lock acquisition).
      if (__atomic_load_n(init_byte, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(lock_byte, 0u, __ATOMIC_RELEASE);
        RtlWakeAddressSingle(lock_byte);
        return 0;
      }
      return 1; // Caller initializes the guarded variable.
    }

    // Contention — park until the lock byte changes.
    unsigned char cmp = 1;
    RtlWaitOnAddress(lock_byte, &cmp, sizeof(cmp), nullptr);

    // Check init flag before retrying the lock.
    if (__atomic_load_n(init_byte, __ATOMIC_ACQUIRE))
      return 0;
  }
}

void __cxa_guard_release(GuardType *guard) {
  auto *init_byte = reinterpret_cast<unsigned char *>(guard);
  auto *lock_byte = init_byte + 1;

  __atomic_store_n(init_byte, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(lock_byte, 0u, __ATOMIC_RELEASE);
  // Wake all — waiters re-check the init flag and return immediately.
  RtlWakeAddressAll(lock_byte);
}

void __cxa_guard_abort(GuardType *guard) {
  auto *lock_byte = reinterpret_cast<unsigned char *>(guard) + 1;

  __atomic_store_n(lock_byte, 0u, __ATOMIC_RELEASE);
  RtlWakeAddressAll(lock_byte);
}

} // extern "C"
