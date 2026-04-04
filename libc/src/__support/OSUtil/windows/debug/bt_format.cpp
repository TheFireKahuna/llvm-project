//===-- Backtrace formatting utilities ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Out-of-line implementations of async-signal-safe formatting functions.
// Uses cpp::StringStream + IntegerToString for safe, bounds-checked
// buffer formatting. PE export walks and PEB Ldr walks are non-trivial
// and would bloat text if inlined across multiple TUs.
//
//===----------------------------------------------------------------------===//

#include "bt_format.h"

#include "src/__support/CPP/span.h"
#include "src/__support/CPP/stringstream.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/nt_string_api.h"
#include "src/__support/OSUtil/windows/nt/nt_wchar_converter.h"
#include "src/__support/integer_to_string.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace bt_fmt {

// Type alias for hex pointer formatting: "0x1a2b3c..."
using HexPtr = IntegerToString<uintptr_t, radix::Hex::WithPrefix>;

// -----------------------------------------------------------------------
// Signal-safe write to an NT handle.
// -----------------------------------------------------------------------

void write_to_handle(HANDLE h, cpp::string_view data) {
  const char *ptr = data.data();
  size_t len = data.size();
  while (len > 0) {
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status =
        ::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb,
                      const_cast<char *>(ptr),
                      static_cast<ULONG>(len > 0x7FFFFFFF ? 0x7FFFFFFF : len),
                      nullptr, nullptr);
    if (!NT_SUCCESS(status))
      break;
    size_t written = iosb.Information;
    if (written == 0)
      break;
    ptr += written;
    len -= written;
  }
}

// -----------------------------------------------------------------------
// PE export table walk — find nearest named export <= addr
// -----------------------------------------------------------------------

SymbolInfo find_nearest_symbol(void *base, const void *addr) {
  auto *nt_hdr =
      static_cast<IMAGE_NT_HEADERS64 *>(::RtlImageNtHeader(base));
  if (!nt_hdr)
    return {};

  auto &exp_entry =
      nt_hdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!exp_entry.VirtualAddress || !exp_entry.Size)
    return {};

  auto base_addr = reinterpret_cast<ULONG_PTR>(base);
  auto *exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY *>(
      base_addr + exp_entry.VirtualAddress);

  auto *functions =
      reinterpret_cast<ULONG *>(base_addr + exports->AddressOfFunctions);
  auto *names =
      reinterpret_cast<ULONG *>(base_addr + exports->AddressOfNames);
  auto *ordinals =
      reinterpret_cast<USHORT *>(base_addr + exports->AddressOfNameOrdinals);

  ULONG_PTR target = reinterpret_cast<ULONG_PTR>(addr);
  ULONG_PTR best_addr = 0;
  ULONG best_name_rva = 0;

  for (ULONG i = 0; i < exports->NumberOfNames; ++i) {
    USHORT ordinal = ordinals[i];
    if (ordinal >= exports->NumberOfFunctions)
      continue;
    ULONG_PTR fn_addr = base_addr + functions[ordinal];
    // Skip forwarded exports (RVA within export directory range).
    if (fn_addr >= reinterpret_cast<ULONG_PTR>(exports) &&
        fn_addr < reinterpret_cast<ULONG_PTR>(exports) + exp_entry.Size)
      continue;
    if (fn_addr <= target && fn_addr > best_addr) {
      best_addr = fn_addr;
      best_name_rva = names[i];
    }
  }

  if (!best_addr)
    return {};

  const char *sym_name =
      reinterpret_cast<const char *>(base_addr + best_name_rva);
  return {cpp::string_view(sym_name), reinterpret_cast<void *>(best_addr)};
}

// -----------------------------------------------------------------------
// Module name resolution via PEB Ldr walk
// -----------------------------------------------------------------------

namespace {

// Extract basename from wide path.
const WCHAR *wide_basename(const WCHAR *path, USHORT len_bytes) {
  USHORT len = len_bytes / sizeof(WCHAR);
  const WCHAR *last = path;
  for (USHORT i = 0; i < len; ++i) {
    if (path[i] == u'\\' || path[i] == u'/')
      last = path + i + 1;
  }
  return last;
}

// Convert wide basename to UTF-8 via the self-hosted converter.
int narrow_basename(const WCHAR *wname, const WCHAR *wend,
                    char *out, size_t outlen) {
  size_t wchar_count = static_cast<size_t>(wend - wname);
  int result = windows::utf16_to_utf8(wname, wchar_count, out, outlen - 1);
  return result > 0 ? result : 0;
}

} // namespace

int find_module_name(void *base, char *out, size_t outlen) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
    auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
    if (entry->DllBase == base) {
      const WCHAR *full = entry->FullDllName.Buffer;
      USHORT full_len = entry->FullDllName.Length;
      const WCHAR *bname = wide_basename(full, full_len);
      const WCHAR *end = full + full_len / sizeof(WCHAR);
      return narrow_basename(bname, end, out, outlen);
    }
  }
  return 0;
}

// -----------------------------------------------------------------------
// Format one backtrace frame
// -----------------------------------------------------------------------

int format_frame(void *addr, char *buf, size_t buflen) {
  // Minimum buffer: "module(+0xN) [0xN]\0" needs ~40 chars worst case.
  if (buflen < 64)
    return 0;

  // Reserve last byte for NUL terminator.
  cpp::StringStream ss(cpp::span<char>(buf, buflen - 1));

  // Module name.
  void *base = nullptr;
  ::RtlPcToFileHeader(addr, &base);

  if (base) {
    char mod[64];
    int n = find_module_name(base, mod, sizeof(mod));
    ss << cpp::string_view(mod, static_cast<size_t>(n));
  } else {
    ss << "???";
  }

  ss << '(';

  // Symbol lookup.
  SymbolInfo sym;
  if (base)
    sym = find_nearest_symbol(base, addr);

  if (sym) {
    // Truncate symbol name to leave room for the suffix.
    // Suffix worst case: "+0x<16hex>) [0x<16hex>]" = ~40 chars.
    size_t avail = ss.bufsize() - ss.str().size();
    size_t slen = sym.name.size();
    if (avail > 40)
      slen = slen < (avail - 40) ? slen : (avail - 40);
    else
      slen = 0;

    ss << sym.name.substr(0, slen) << '+';
    uintptr_t offset = reinterpret_cast<uintptr_t>(addr) -
                       reinterpret_cast<uintptr_t>(sym.addr);
    const HexPtr hex(offset);
    ss << hex.view();
  } else {
    ss << '+';
    uintptr_t offset = base ? (reinterpret_cast<uintptr_t>(addr) -
                               reinterpret_cast<uintptr_t>(base))
                            : reinterpret_cast<uintptr_t>(addr);
    const HexPtr hex(offset);
    ss << hex.view();
  }

  // Suffix: ") [0xaddr]"
  ss << ") [";
  const HexPtr addr_hex(reinterpret_cast<uintptr_t>(addr));
  ss << addr_hex.view() << ']';

  size_t written = ss.str().size();
  buf[written] = '\0';
  return static_cast<int>(written);
}

} // namespace bt_fmt
} // namespace LIBC_NAMESPACE_DECL
