//===-- Hardware breakpoint / watchpoint impl (x86-64) ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// DR7 layout (Intel SDM Vol.3, §17.2.4), bits numbered low→high:
//
//   [0]      L0   — local enable DR0        [2]  L1    [4]  L2    [6]  L3
//   [1]      G0   — global enable (unused)  [3]  G1    [5]  G2    [7]  G3
//   [8]      LE   — local exact data trap (recommended on; P6+ ignores anyway)
//   [9]      GE   — global exact (unused)
//   [10]     1    — architecturally reserved must-be-one
//   [11]     RTM  — RTM-aware debug (zero)
//   [12]     0    — reserved, zero
//   [13]     GD   — general detect (zero; don't trap on DR access)
//   [14..15] 0    — reserved
//   [16..17] R/W0 [18..19] LEN0  (per-slot access + size)
//   [20..21] R/W1 [22..23] LEN1
//   [24..25] R/W2 [26..27] LEN2
//   [28..29] R/W3 [30..31] LEN3
//
// LEN encoding: 00=1, 01=2, 11=4, 10=8  (Intel's length-2 quirk is baked in)
// R/W  encoding: 00=exec, 01=write, 10=io (user mode: disallow), 11=rw
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/debug/hw_breakpoint.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_context.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace hw_bp {

namespace {

// ---------------------------------------------------------------------------
// DR7 bit fields
// ---------------------------------------------------------------------------

constexpr unsigned DR7_RESERVED_SET = 1u << 10;   // must-be-one
constexpr unsigned DR7_LE = 1u << 8;              // local exact
constexpr uint64_t DR7_PERMANENT_MASK =
    DR7_RESERVED_SET | DR7_LE;

constexpr unsigned local_enable_bit(unsigned slot) { return slot * 2; }

// Clear the L{slot} + R/W{slot} + LEN{slot} fields, leaving others intact.
uint64_t dr7_clear_slot(uint64_t dr7, unsigned slot) {
  uint64_t mask = 0;
  mask |= 1ull << local_enable_bit(slot);      // Ln
  mask |= 1ull << (local_enable_bit(slot) + 1); // Gn (zeroed regardless)
  mask |= 0xFull << (16 + slot * 4);            // R/Wn + LENn (4 bits)
  return dr7 & ~mask;
}

// Apply L{slot} + R/W{slot} + LEN{slot}.
uint64_t dr7_set_slot(uint64_t dr7, unsigned slot, unsigned rw, unsigned len_enc) {
  dr7 = dr7_clear_slot(dr7, slot);
  dr7 |= 1ull << local_enable_bit(slot);
  dr7 |= (uint64_t(rw) & 0x3) << (16 + slot * 4);
  dr7 |= (uint64_t(len_enc) & 0x3) << (16 + slot * 4 + 2);
  return dr7 | DR7_PERMANENT_MASK;
}

bool dr7_slot_enabled(uint64_t dr7, unsigned slot) {
  return (dr7 >> local_enable_bit(slot)) & 1ull;
}

unsigned dr7_slot_rw(uint64_t dr7, unsigned slot) {
  return unsigned((dr7 >> (16 + slot * 4)) & 0x3);
}

unsigned dr7_slot_len_enc(uint64_t dr7, unsigned slot) {
  return unsigned((dr7 >> (16 + slot * 4 + 2)) & 0x3);
}

// LEN encoding table — hardware quirk: 10 = 8, 11 = 4.
bool encode_len(unsigned bytes, unsigned access, unsigned *enc_out) {
  // Execute breakpoints must use len=1 per Intel SDM.
  if (access == ACCESS_EXEC) {
    if (bytes != 1)
      return false;
    *enc_out = 0;
    return true;
  }
  switch (bytes) {
  case 1: *enc_out = 0; return true;
  case 2: *enc_out = 1; return true;
  case 8: *enc_out = 2; return true;
  case 4: *enc_out = 3; return true;
  default: return false;
  }
}

unsigned decode_len(unsigned enc) {
  static constexpr unsigned table[4] = {1, 2, 8, 4};
  return table[enc & 0x3];
}

bool validate_access(unsigned access) {
  return access == ACCESS_EXEC || access == ACCESS_WRITE || access == ACCESS_RW;
}

bool validate_alignment(void *addr, unsigned bytes) {
  // len==1 has no alignment requirement; others must be naturally aligned.
  uintptr_t v = reinterpret_cast<uintptr_t>(addr);
  switch (bytes) {
  case 1: return true;
  case 2: return (v & 1) == 0;
  case 4: return (v & 3) == 0;
  case 8: return (v & 7) == 0;
  default: return false;
  }
}

// ---------------------------------------------------------------------------
// CONTEXT get/set with debug-register scope
// ---------------------------------------------------------------------------

// Minimal stack-allocated CONTEXT. Sized for the full struct because
// NtGetContextThread validates buffer size against architectural flags; we
// only ever set/read the debug-register fields but must present the whole.
struct alignas(16) ContextBuffer {
  CONTEXT ctx;
};

NTSTATUS read_debug_regs(HANDLE thread, CONTEXT *ctx) {
  ctx->ContextFlags = CONTEXT_DEBUG_REGISTERS;
  return ::NtGetContextThread(thread, ctx);
}

NTSTATUS write_debug_regs(HANDLE thread, CONTEXT *ctx) {
  ctx->ContextFlags = CONTEXT_DEBUG_REGISTERS;
  return ::NtSetContextThread(thread, ctx);
}

int nt_to_errno(NTSTATUS st) {
  if (NT_SUCCESS(st))
    return 0;
  // 0xC0000022 = STATUS_ACCESS_DENIED.
  if (static_cast<uint32_t>(st) == 0xC0000022u)
    return -EPERM;
  return -EIO;
}

// ---------------------------------------------------------------------------
// Self-thread fast path — no suspend needed.
// ---------------------------------------------------------------------------
//
// NtCurrentThread() is a constant pseudo-handle (-2). NtSetContextThread on
// the calling thread stages DR values into the kernel's saved context and
// installs them on return-from-syscall; no state-change dance required.
// Detect by pointer equality since the pseudo-handle value is architectural.

bool is_self(HANDLE thread) {
  return thread == ::NtCurrentThread();
}

// ---------------------------------------------------------------------------
// Cross-thread brackets — Win11 crash-safe suspend pair.
// ---------------------------------------------------------------------------

struct StateChangeBracket {
  HANDLE sc = nullptr;
  HANDLE target = nullptr;
  bool suspended = false;

  NTSTATUS open(HANDLE t) {
    target = t;
    OBJECT_ATTRIBUTES oa{};
    oa.Length = sizeof(oa);
    NTSTATUS st = ::NtCreateThreadStateChange(&sc, THREAD_STATE_ALL_ACCESS,
                                              &oa, t, 0);
    if (!NT_SUCCESS(st))
      return st;
    st = ::NtChangeThreadState(sc, t, ThreadStateSuspend, nullptr, 0, 0);
    if (!NT_SUCCESS(st)) {
      ::NtClose(sc);
      sc = nullptr;
      return st;
    }
    suspended = true;
    return 0;
  }

  ~StateChangeBracket() {
    if (suspended)
      ::NtChangeThreadState(sc, target, ThreadStateResume, nullptr, 0, 0);
    if (sc)
      ::NtClose(sc);
  }

  StateChangeBracket() = default;
  StateChangeBracket(const StateChangeBracket &) = delete;
  StateChangeBracket &operator=(const StateChangeBracket &) = delete;
};

// ---------------------------------------------------------------------------
// Read-modify-write of the debug-register subset under the appropriate
// brack. Keeps arm/disarm/disarm_all symmetric.
// ---------------------------------------------------------------------------

template <typename Mutator>
int rmw_debug_regs(HANDLE thread, Mutator &&mutate) {
  ContextBuffer buf{};

  if (is_self(thread)) {
    NTSTATUS st = read_debug_regs(thread, &buf.ctx);
    if (!NT_SUCCESS(st))
      return nt_to_errno(st);
    int rc = mutate(&buf.ctx);
    if (rc < 0)
      return rc;
    st = write_debug_regs(thread, &buf.ctx);
    return NT_SUCCESS(st) ? rc : nt_to_errno(st);
  }

  StateChangeBracket br;
  NTSTATUS st = br.open(thread);
  if (!NT_SUCCESS(st))
    return nt_to_errno(st);

  st = read_debug_regs(thread, &buf.ctx);
  if (!NT_SUCCESS(st))
    return nt_to_errno(st);

  int rc = mutate(&buf.ctx);
  if (rc < 0)
    return rc;

  st = write_debug_regs(thread, &buf.ctx);
  return NT_SUCCESS(st) ? rc : nt_to_errno(st);
}

// Address slot selector — index into {Dr0, Dr1, Dr2, Dr3}.
void set_dr_addr(CONTEXT *ctx, unsigned slot, void *addr) {
  uint64_t v = reinterpret_cast<uint64_t>(addr);
  switch (slot) {
  case 0: ctx->Dr0 = v; break;
  case 1: ctx->Dr1 = v; break;
  case 2: ctx->Dr2 = v; break;
  case 3: ctx->Dr3 = v; break;
  }
}

void *get_dr_addr(const CONTEXT *ctx, unsigned slot) {
  switch (slot) {
  case 0: return reinterpret_cast<void *>(ctx->Dr0);
  case 1: return reinterpret_cast<void *>(ctx->Dr1);
  case 2: return reinterpret_cast<void *>(ctx->Dr2);
  case 3: return reinterpret_cast<void *>(ctx->Dr3);
  }
  return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int arm(HANDLE thread, void *addr, unsigned len, unsigned access) {
  if (LIBC_UNLIKELY(!validate_access(access)))
    return -EINVAL;
  if (LIBC_UNLIKELY(!validate_alignment(addr, len)))
    return -EINVAL;

  unsigned len_enc;
  if (LIBC_UNLIKELY(!encode_len(len, access, &len_enc)))
    return -EINVAL;

  int assigned = -1;
  int rc = rmw_debug_regs(thread, [&](CONTEXT *ctx) -> int {
    // First-fit over the four slots. DR7 is the single source of truth;
    // no shadow state to stay in sync with.
    for (unsigned slot = 0; slot < NUM_SLOTS; ++slot) {
      if (!dr7_slot_enabled(ctx->Dr7, slot)) {
        set_dr_addr(ctx, slot, addr);
        ctx->Dr7 = dr7_set_slot(ctx->Dr7, slot, access, len_enc);
        // Clear any stale status bit on this slot.
        ctx->Dr6 &= ~(1ull << slot);
        assigned = int(slot);
        return 0;
      }
    }
    return -EAGAIN;
  });
  if (rc < 0)
    return rc;
  return assigned;
}

int disarm(HANDLE thread, unsigned slot) {
  if (LIBC_UNLIKELY(slot >= NUM_SLOTS))
    return -EINVAL;

  return rmw_debug_regs(thread, [&](CONTEXT *ctx) -> int {
    set_dr_addr(ctx, slot, nullptr);
    ctx->Dr7 = dr7_clear_slot(ctx->Dr7, slot) | DR7_PERMANENT_MASK;
    ctx->Dr6 &= ~(1ull << slot);
    return 0;
  });
}

int disarm_all(HANDLE thread) {
  return rmw_debug_regs(thread, [&](CONTEXT *ctx) -> int {
    ctx->Dr0 = ctx->Dr1 = ctx->Dr2 = ctx->Dr3 = 0;
    // Keep only the permanent reserved bits; drop all Ln/Gn/R_W/LEN.
    ctx->Dr7 = DR7_PERMANENT_MASK;
    // Clear the four per-slot status bits; leave BS (bit 14) for any
    // in-flight single-step.
    ctx->Dr6 &= ~0xFull;
    return 0;
  });
}

int query(HANDLE thread, Watchpoint out[NUM_SLOTS]) {
  ContextBuffer buf{};
  NTSTATUS st;
  if (is_self(thread)) {
    st = read_debug_regs(thread, &buf.ctx);
  } else {
    StateChangeBracket br;
    st = br.open(thread);
    if (!NT_SUCCESS(st))
      return nt_to_errno(st);
    st = read_debug_regs(thread, &buf.ctx);
  }
  if (!NT_SUCCESS(st))
    return nt_to_errno(st);

  for (unsigned slot = 0; slot < NUM_SLOTS; ++slot) {
    out[slot].enabled = dr7_slot_enabled(buf.ctx.Dr7, slot);
    if (out[slot].enabled) {
      out[slot].addr = get_dr_addr(&buf.ctx, slot);
      out[slot].access = dr7_slot_rw(buf.ctx.Dr7, slot);
      out[slot].len = decode_len(dr7_slot_len_enc(buf.ctx.Dr7, slot));
    } else {
      out[slot].addr = nullptr;
      out[slot].access = 0;
      out[slot].len = 0;
    }
  }
  return 0;
}

void clear_dr6_status(CONTEXT *ctx) {
  if (!ctx)
    return;
  // B0..B3 = bits 0..3 (hardware breakpoint condition detected).
  // BS     = bit 14    (EFlags.TF single-step).
  // BD     = bit 13    (DR access detect — we don't use GD, so shouldn't
  //                     fire, but clear defensively).
  // BT     = bit 15    (task-switch trap — legacy 32-bit, clear defensively).
  // RTM    = bit 16    (RTM debug — not used).
  constexpr uint64_t STATUS_MASK =
      0xFull | (1ull << 13) | (1ull << 14) | (1ull << 15) | (1ull << 16);
  ctx->Dr6 &= ~STATUS_MASK;
}

} // namespace hw_bp
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
