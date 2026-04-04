//===-- Custom async-signal-safe stack walker for x64 PE ----------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Table-driven x64 stack walker. Reads .pdata/.xdata directly from mapped
// PE images — no RtlVirtualUnwind, no KiUserInvertedFunctionTable, no locks,
// no allocations, no syscalls.
//
// Safety mechanisms:
//   - FaultGuard: catches access violations from corrupt stack reads.
//   - RSP monotonicity: each frame's RSP must exceed the previous.
//   - Frame cap: hard limit of MAX_WALK_FRAMES iterations.
//   - Chain depth cap: MAX_CHAIN_DEPTH for UNW_FLAG_CHAININFO.
//
//===----------------------------------------------------------------------===//

#include "stack_walker.h"

#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/nt_string_api.h"
#include "src/__support/OSUtil/windows/nt/nt_unwind_types.h"
#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/macros/config.h"

#ifdef __x86_64__

namespace LIBC_NAMESPACE_DECL {
namespace {

// -----------------------------------------------------------------------
// Module lookup via PEB Ldr
// -----------------------------------------------------------------------

struct ModuleInfo {
  uintptr_t base;
  const RUNTIME_FUNCTION *pdata;
  ULONG pdata_count;
};

// Find the module containing \p pc and locate its .pdata section.
// Returns false if no module contains \p pc or it has no exception table.
bool find_module(uintptr_t pc, ModuleInfo &out) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr)
    return false;

  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
    auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
    auto mod_base = reinterpret_cast<uintptr_t>(entry->DllBase);
    auto mod_end = mod_base + entry->SizeOfImage;
    if (pc < mod_base || pc >= mod_end)
      continue;

    // Found the module. Read its PE header for the exception directory.
    auto *nt_hdr =
        static_cast<IMAGE_NT_HEADERS64 *>(::RtlImageNtHeader(entry->DllBase));
    if (!nt_hdr)
      return false;

    auto &exc = nt_hdr->OptionalHeader
                    .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!exc.VirtualAddress || !exc.Size)
      return false;

    out.base = mod_base;
    out.pdata = reinterpret_cast<const RUNTIME_FUNCTION *>(
        mod_base + exc.VirtualAddress);
    out.pdata_count = exc.Size / sizeof(RUNTIME_FUNCTION);
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------
// .pdata binary search
// -----------------------------------------------------------------------

// Binary search the sorted RUNTIME_FUNCTION array for the entry containing
// the given RVA. \p module_base is needed for indirect entry resolution.
// Returns nullptr for leaf functions (no entry).
const RUNTIME_FUNCTION *find_function_entry(uintptr_t module_base,
                                            const RUNTIME_FUNCTION *table,
                                            ULONG count, ULONG rva) {
  ULONG lo = 0, hi = count;
  while (lo < hi) {
    ULONG mid = lo + (hi - lo) / 2;
    if (rva < table[mid].BeginAddress) {
      hi = mid;
    } else if (rva >= table[mid].EndAddress) {
      lo = mid + 1;
    } else {
      const RUNTIME_FUNCTION *result = &table[mid];
      // Bit 0 of UnwindData set = indirect reference to another
      // RUNTIME_FUNCTION at (module_base + (UnwindData & ~1)).
      if (result->UnwindData & 1) {
        result = reinterpret_cast<const RUNTIME_FUNCTION *>(
            module_base + (result->UnwindData & ~1u));
      }
      return result;
    }
  }
  return nullptr;
}

// -----------------------------------------------------------------------
// Single-frame unwind via .xdata interpretation
// -----------------------------------------------------------------------

// Process the unwind codes for one RUNTIME_FUNCTION (and any chained
// entries). Updates \p rsp and \p gprs to reflect the caller's state.
// If \p machframe_rip is non-null, a kPushMachFrame wrote the RIP there.
// Returns true on success, false on corrupt/unsupported metadata.
bool process_unwind_codes(uintptr_t module_base,
                          const RUNTIME_FUNCTION *entry,
                          ULONG ip_offset, uintptr_t &rsp,
                          uintptr_t *gprs, uintptr_t *machframe_rip) {
  int chain_depth = 0;
  const RUNTIME_FUNCTION *cur = entry;

  while (cur && chain_depth < unwind::MAX_CHAIN_DEPTH) {
    // Resolve indirect entries (bit 0 of UnwindData).
    ULONG unwind_rva = cur->UnwindData;
    if (unwind_rva & 1) {
      // Indirect: points to another RUNTIME_FUNCTION. Rare.
      // For safety, just bail.
      return false;
    }

    auto *info = reinterpret_cast<const unwind::Info *>(
        module_base + unwind_rva);

    const unwind::Code *codes = info->codes();
    int count = info->code_count;

    // Pre-compute the save base for SAVE_NONVOL/SAVE_XMM128.
    // If a frame register is used, the base is derived from it.
    // Otherwise, the base is the current RSP (post-allocation).
    uintptr_t save_base = rsp;
    if (info->frame_register() != 0) {
      save_base = gprs[info->frame_register()] -
                  info->frame_offset_scaled();
    }

    for (int i = 0; i < count;) {
      unsigned char op = codes[i].opcode();
      unsigned char op_info = codes[i].info();
      int slots = unwind::slots_for(op, op_info);

      // Skip codes for prolog points we haven't reached yet.
      // Only relevant for the first entry (not chained); chained entries
      // describe the outer prolog which has fully executed.
      if (chain_depth == 0 && codes[i].offset > ip_offset) {
        i += slots;
        continue;
      }

      switch (op) {
      case unwind::kPushNonVol:
        gprs[op_info] = *reinterpret_cast<const uintptr_t *>(rsp);
        rsp += 8;
        break;

      case unwind::kAllocLarge:
        if (op_info == 0)
          rsp += static_cast<uintptr_t>(codes[i].next_u16()) * 8;
        else
          rsp += codes[i].next_u32();
        break;

      case unwind::kAllocSmall:
        rsp += static_cast<uintptr_t>(op_info) * 8 + 8;
        break;

      case unwind::kSetFpReg:
        rsp = gprs[info->frame_register()] -
              info->frame_offset_scaled();
        break;

      case unwind::kSaveNonVol:
        gprs[op_info] = *reinterpret_cast<const uintptr_t *>(
            save_base + static_cast<uintptr_t>(codes[i].next_u16()) * 8);
        break;

      case unwind::kSaveNonVolFar:
        gprs[op_info] = *reinterpret_cast<const uintptr_t *>(
            save_base + codes[i].next_u32());
        break;

      case unwind::kPushMachFrame:
        // Machine frame (interrupt/exception entry).
        if (op_info != 0)
          rsp += 8; // skip error code
        // Machine frame layout: [RIP, CS, RFLAGS, RSP, SS]
        if (machframe_rip)
          *machframe_rip = *reinterpret_cast<const uintptr_t *>(rsp);
        rsp = *reinterpret_cast<const uintptr_t *>(rsp + 24); // saved RSP
        return true; // RIP extracted from machine frame

      case unwind::kSaveXmm128:
      case unwind::kSaveXmm128Far:
      case unwind::kEpilog:
      case unwind::kSpareCode:
        // XMM/epilog: skip (irrelevant for stack walking).
        break;

      default:
        return false; // unknown opcode
      }

      i += slots;
    }

    // Follow chain if UNW_FLAG_CHAININFO is set.
    if (info->flags() & UNW_FLAG_CHAININFO) {
      cur = info->chained_entry();
      ++chain_depth;
      // Chained entries describe the outer prolog — all codes apply
      // (ip_offset is irrelevant for chained entries; handled by the
      // chain_depth == 0 check above).
      continue;
    }
    break;
  }

  return true;
}

// Unwind one frame. Updates \p state to reflect the caller's register state.
// Returns true on success, false if the walk should terminate.
bool unwind_one_frame(internal::WalkState &state, const ModuleInfo &mod) {
  ULONG rva = static_cast<ULONG>(state.rip - mod.base);
  const RUNTIME_FUNCTION *entry =
      find_function_entry(mod.base, mod.pdata, mod.pdata_count, rva);

  if (!entry) {
    // Leaf function: no prolog, return address is at [RSP].
    state.rip = *reinterpret_cast<const uintptr_t *>(state.rsp);
    state.rsp += 8;
    return state.rip != 0;
  }

  ULONG ip_offset = rva - entry->BeginAddress;
  uintptr_t rsp = state.rsp;
  uintptr_t mf_rip = 0;

  if (!process_unwind_codes(mod.base, entry, ip_offset, rsp, state.gprs,
                            &mf_rip))
    return false;

  if (mf_rip != 0) {
    // kPushMachFrame extracted RIP from the machine frame.
    state.rip = mf_rip;
    state.rsp = rsp;
  } else {
    // Normal function: return address is at the new [RSP].
    state.rip = *reinterpret_cast<const uintptr_t *>(rsp);
    state.rsp = rsp + 8;
  }
  return state.rip != 0;
}

} // anonymous namespace

namespace internal {

int posix_stack_walk(void **buffer, int max_frames, int skip,
                     const WalkState &initial) {
  if (max_frames <= 0)
    return 0;

  WalkState state = initial;
  // volatile: survives longjmp from FaultGuard.
  volatile int count = 0;
  int total = 0;

  // FaultGuard: if any memory read during the walk faults (corrupt stack,
  // unmapped PE metadata), we catch the access violation and return
  // whatever frames we captured before the fault.
  windows::FaultGuard guard;
  if (windows::fault_guard_enter(&guard, windows::FAULT_GUARD_MEMORY)) {
    // Faulted — guard already popped by VEH handler.
    return count;
  }

  while (total < MAX_WALK_FRAMES && count < max_frames) {
    // Record this frame (after skipping the first `skip` frames).
    if (total >= skip) {
      buffer[count] = reinterpret_cast<void *>(state.rip);
      ++count;
    }
    ++total;

    uintptr_t prev_rsp = state.rsp;

    // Find module for current RIP.
    ModuleInfo mod;
    if (!find_module(state.rip, mod))
      break;

    // Unwind one frame.
    if (!unwind_one_frame(state, mod))
      break;

    // RSP must strictly increase (prevents infinite loops on corruption).
    if (state.rsp <= prev_rsp)
      break;

    // Zero RIP = end of call chain (thread start frame).
    if (state.rip == 0)
      break;
  }

  windows::fault_guard_leave(&guard);
  return count;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // __x86_64__
