//===-- c.dll load/free resource-balance — child helper -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Freestanding (ntdll-only) child for libc_dll_cycle_resource_balance_test.
//
// CRITICAL BUILD CONTRACT:
//   This TU MUST NOT statically import c.dll. If c.dll lands on the
//   helper's IAT, the loader pins it for the process lifetime and the
//   explicit FreeLibrary below becomes a no-op — __libc_dll_fini never
//   runs and the leak is unobservable. CMake must link only
//   windows_ntdll, without c.lib.
//
// What this verifies:
//   One full LoadLibrary/FreeLibrary cycle against c.dll drives a real
//   PROCESS_ATTACH (→ __libc_dll_init → Tier A+B bootstrap) and
//   PROCESS_DETACH (→ __libc_dll_fini → .libcfin reverse-phase walk).
//   After the pair returns, two resource counts must be at their
//   pre-load baseline:
//     1. PEB.TlsBitmap popcount — every tls_alloc in init must have a
//        matching tls_release in fini.
//     2. Process handle count (ProcessHandleCount) — every NtCreate*
//        performed by init must be matched by an NtClose in fini.
//   The first cycle warms loader-internal caches (ETW sessions, loader
//   worker threads, AppContainer probes) and is NOT measured. Only the
//   second cycle's pre/post delta is.
//
// Exit code is a BITMASK so the parent attributes regressions to the
// specific contract that broke:
//   0x00  OK
//   0x01  LoadLibrary failed (first or second attempt)
//   0x02  FreeLibrary failed
//   0x04  TlsBitmap popcount grew above baseline — slot leak in fini
//   0x08  Handle count grew above baseline + kHandleSlack — kernel
//         handle leak in fini
//   0x10  NtQueryInformationProcess failed
//
//===----------------------------------------------------------------------===//

#include <stddef.h>
#include <stdint.h>

extern "C" {
typedef void *HANDLE;
typedef long NTSTATUS;
typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef ULONG *PULONG;
typedef void *PVOID;
typedef wchar_t WCHAR;
typedef WCHAR *PWSTR;

typedef struct _UNICODE_STRING {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

NTSTATUS LdrLoadDll(PWSTR PathToFile, ULONG *Flags,
                              PUNICODE_STRING ModuleFileName,
                              PVOID *ModuleHandle);
NTSTATUS LdrUnloadDll(PVOID ModuleHandle);
NTSTATUS NtQueryInformationProcess(HANDLE ProcessHandle,
                                             ULONG ProcessInformationClass,
                                             PVOID ProcessInformation,
                                             ULONG ProcessInformationLength,
                                             PULONG ReturnLength);
}

namespace {

// ---- TLS bitmap snapshot (PEB+0x78 → PRTL_BITMAP → Buffer[0..]) ---------

struct RtlBitmap {
  ULONG SizeOfBitMap;
  PULONG Buffer;
};

inline void *current_peb() {
#if defined(__x86_64__)
  void *peb;
  __asm__ __volatile__("movq %%gs:0x60, %0" : "=r"(peb));
  return peb;
#elif defined(__aarch64__)
  void *peb;
  __asm__ __volatile__("ldr %0, [x18, #0x60]" : "=r"(peb));
  return peb;
#else
#error "libc_dll_cycle_resource_balance_child: unsupported architecture"
#endif
}

unsigned tls_bitmap_popcount() {
  auto *peb = static_cast<unsigned char *>(current_peb());
  if (!peb)
    return 0;
  auto *bmp = *reinterpret_cast<RtlBitmap **>(peb + 0x78);
  if (!bmp || !bmp->Buffer)
    return 0;
  uint64_t lo = static_cast<uint64_t>(bmp->Buffer[0]);
  uint64_t hi = bmp->SizeOfBitMap > 32 ? static_cast<uint64_t>(bmp->Buffer[1])
                                       : uint64_t{0};
  return static_cast<unsigned>(__builtin_popcountll(lo | (hi << 32)));
}

// ---- Process handle-count snapshot (ProcessHandleCount = 20) ------------

constexpr ULONG kProcessHandleCount = 20;

struct ProcessHandleInformation {
  DWORD HandleCount;
  DWORD HighWatermark;
};

inline HANDLE current_process() {
  return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1));
}

long handle_count() {
  ProcessHandleInformation info{};
  ULONG ret_len = 0;
  NTSTATUS st = NtQueryInformationProcess(current_process(),
                                          kProcessHandleCount, &info,
                                          sizeof(info), &ret_len);
  if (st < 0)
    return -1;
  return static_cast<long>(info.HandleCount);
}

// OS sometimes opens/closes a handful of loader-worker / ETW handles on
// LoadLibrary boundaries; a real per-cycle leak compounds past this in
// one iteration, so any genuine regression still shows up unambiguously.
constexpr long kHandleSlack = 4;

constexpr int kOk = 0;
constexpr int kLoadFailed = 0x01;
constexpr int kFreeFailed = 0x02;
constexpr int kTlsLeak = 0x04;
constexpr int kHandleLeak = 0x08;
constexpr int kQueryFailed = 0x10;

// Build a UNICODE_STRING over a compile-time wide literal. LdrLoadDll
// writes nothing through Buffer; const_cast is safe here.
template <size_t N>
UNICODE_STRING make_ustr(const wchar_t (&s)[N]) {
  UNICODE_STRING u;
  u.Length = static_cast<USHORT>((N - 1) * sizeof(wchar_t));
  u.MaximumLength = static_cast<USHORT>(N * sizeof(wchar_t));
  u.Buffer = const_cast<wchar_t *>(s);
  return u;
}

bool load_c_dll(PVOID *out) {
  UNICODE_STRING name = make_ustr(L"c.dll");
  return LdrLoadDll(nullptr, nullptr, &name, out) >= 0;
}

bool unload_c_dll(PVOID h) { return LdrUnloadDll(h) >= 0; }

} // namespace

extern "C" int main() {
  int result = kOk;

  PVOID h_warm = nullptr;
  if (!load_c_dll(&h_warm))
    return kLoadFailed;
  if (!unload_c_dll(h_warm))
    return kFreeFailed;

  unsigned tls_baseline = tls_bitmap_popcount();
  long handle_baseline = handle_count();
  if (handle_baseline < 0)
    return kQueryFailed;

  PVOID h = nullptr;
  if (!load_c_dll(&h))
    return kLoadFailed;
  if (!unload_c_dll(h))
    return kFreeFailed;

  unsigned tls_now = tls_bitmap_popcount();
  if (tls_now > tls_baseline)
    result |= kTlsLeak;

  long handle_now = handle_count();
  if (handle_now < 0)
    return kQueryFailed;
  if (handle_now > handle_baseline + kHandleSlack)
    result |= kHandleLeak;

  return result;
}
