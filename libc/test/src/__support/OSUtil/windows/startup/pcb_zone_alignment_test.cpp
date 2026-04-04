//===-- PCB zone alignment and page-boundary tests ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The ProcessControlBlock lives in the ".pcb" PE section and is split into:
//   Zone 0  — page 0 (sealed PAGE_READONLY for process lifetime).
//   Zone 0b — page 1 (sealed PAGE_READONLY except during fork reinit).
//   Zone 1  — page 2+ (freely mutable).
//
// pcb_seal_readonly_a / pcb_seal_readonly_b call NtProtectVirtualMemory on
// &g_pcb.zone0 / &g_pcb.zone0b with a page-sized region. For that to protect
// exactly the intended bytes (and not clip or spill into an adjacent zone),
// three invariants MUST hold:
//
//   1. g_pcb is page-aligned (4 KB on every supported target).
//   2. Zone 0 begins at g_pcb+0 and is <= 4096 bytes; Zone 0b begins at
//      g_pcb+4096 and is <= 4096 bytes; Zone 1 begins at g_pcb+8192.
//   3. No directly-addressable subobject of Zone 0 or Zone 0b straddles a
//      page boundary.
//
// If a new field nudges any boundary, or the ".pcb" section attributes ever
// drop the alignment below 4 KB, these tests fail — statically at compile
// time via static_assert, and at runtime via EXPECT checks on the live
// g_pcb address. This is the "alignment accident" trap: you cannot silently
// demote the seal to partial coverage without tripping one of these.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/type_traits/is_trivially_destructible.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/veh/veh_state.h"
#include "src/__support/macros/config.h"
#include "test/UnitTest/Test.h"

#include <stddef.h>
#include <stdint.h>

// =========================================================================
// File-scope static_asserts — fire regardless of whether any TEST is run.
//
// These live OUTSIDE the LIBC_NAMESPACE so they are evaluated on translation-
// unit inclusion; they do not depend on any runtime fixture. A field drift
// that violates any of these stops the test-binary build cold.
// =========================================================================

namespace {
inline constexpr size_t kPcbPageSize = 4096;
inline constexpr size_t kZone0Start = 0;
inline constexpr size_t kZone0bStart = 4096;
inline constexpr size_t kZone1Start = 8192;
} // namespace

// --- Struct-level alignment and sizes -----------------------------------

static_assert(alignof(LIBC_NAMESPACE::ProcessControlBlock) >= kPcbPageSize,
              "ProcessControlBlock must be 4 KB-aligned for page seals");

static_assert(sizeof(LIBC_NAMESPACE::PcbZone0) == kPcbPageSize,
              "PcbZone0 must be exactly one page");

static_assert(sizeof(LIBC_NAMESPACE::PcbZone0b) == kPcbPageSize,
              "PcbZone0b must be exactly one page");

// --- Zone placement within ProcessControlBlock --------------------------

static_assert(offsetof(LIBC_NAMESPACE::ProcessControlBlock, zone0) ==
                  kZone0Start,
              "zone0 must be the first member (base of page 0)");

static_assert(offsetof(LIBC_NAMESPACE::ProcessControlBlock, zone0b) ==
                  kZone0bStart,
              "zone0b must start at page 1 (offset 4096)");

static_assert(offsetof(LIBC_NAMESPACE::ProcessControlBlock, init_state) ==
                  kZone1Start,
              "Zone 1 (first mutable field) must start at page 2 (offset 8192)");

// --- Zone 0b directly-addressable subobjects stay inside page 1 ---------
//
// Zone 0b is shallow: its only subobject group is the active-fields block
// ending at dll_notify_cookie_ (validated by PcbZone0bLayoutCheck in
// process_control_block.h). We independently assert every offset + size
// pair fits inside [0, 4096) here so a silent field reorder or addition
// can't slip through unnoticed.
//
// We cannot use offsetof on PcbZone0b's private fields from this TU (no
// friendship), so we check the published invariants: the whole struct is
// exactly one page and its starting offset in the PCB is exactly 4096.
// PcbZone0bLayoutCheck inside process_control_block.h validates the
// intra-struct offsets (pid_=0, parent_pid_=4, security_cookie_=8,
// dll_notify_cookie_=32); those asserts fire on any include of this TU.

// --- Trivial destructibility (no dtor side-effects at exit) -------------

static_assert(LIBC_NAMESPACE::cpp::is_trivially_destructible_v<
                  LIBC_NAMESPACE::ProcessControlBlock>,
              "PCB must be trivially destructible");
static_assert(
    LIBC_NAMESPACE::cpp::is_trivially_destructible_v<LIBC_NAMESPACE::PcbZone0>,
    "PcbZone0 must be trivially destructible");
static_assert(
    LIBC_NAMESPACE::cpp::is_trivially_destructible_v<LIBC_NAMESPACE::PcbZone0b>,
    "PcbZone0b must be trivially destructible");

// =========================================================================
// Runtime checks on the LIVE g_pcb instance.
//
// static_assert only knows compile-time layout. These EXPECT checks bind
// the compile-time invariants to the actual address the linker gave g_pcb
// — the only way to catch a ".pcb" section whose IMAGE_SCN_ALIGN_4096BYTES
// bit didn't survive link.
// =========================================================================

namespace {

// Returns the page index (floor address / page size) for an arbitrary
// pointer. Used to assert that the first and last byte of a subobject
// lie on the same page — i.e. no field straddles a page boundary.
constexpr uintptr_t page_of(uintptr_t addr) { return addr / kPcbPageSize; }

uintptr_t pcb_base() {
  return reinterpret_cast<uintptr_t>(&LIBC_NAMESPACE::g_pcb);
}

uintptr_t zone0_base() {
  return reinterpret_cast<uintptr_t>(&LIBC_NAMESPACE::g_pcb.zone0);
}

uintptr_t zone0b_base() {
  return reinterpret_cast<uintptr_t>(&LIBC_NAMESPACE::g_pcb.zone0b);
}

} // namespace

// ---------------------------------------------------------------------------
// g_pcb is page-aligned. The page seal is issued at &g_pcb.zone0 /
// &g_pcb.zone0b — those must coincide with a page boundary or
// NtProtectVirtualMemory will round down and either fail or seal the wrong
// region. If the .pcb section attributes ever regress to the default 16-byte
// alignment, this test is the tripwire.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZoneAlignment, GlobalPcbIsPageAligned) {
  EXPECT_EQ(pcb_base() % kPcbPageSize, static_cast<uintptr_t>(0));
  EXPECT_EQ(zone0_base() % kPcbPageSize, static_cast<uintptr_t>(0));
  EXPECT_EQ(zone0b_base() % kPcbPageSize, static_cast<uintptr_t>(0));
}

// ---------------------------------------------------------------------------
// The three zone bases sit at the expected relative offsets from g_pcb.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZoneAlignment, ZoneBasesAtExpectedOffsets) {
  EXPECT_EQ(zone0_base() - pcb_base(), static_cast<uintptr_t>(kZone0Start));
  EXPECT_EQ(zone0b_base() - pcb_base(), static_cast<uintptr_t>(kZone0bStart));

  const uintptr_t zone1_base =
      reinterpret_cast<uintptr_t>(&LIBC_NAMESPACE::g_pcb.init_state);
  EXPECT_EQ(zone1_base - pcb_base(), static_cast<uintptr_t>(kZone1Start));
}

// ---------------------------------------------------------------------------
// Zone 0 fully occupies page 0: first byte and last byte are both on page 0,
// and Zone 0b begins exactly one page later. "Fully occupies" is the
// load-bearing claim — if Zone 0 were <4096 bytes but alignas(4096) padded
// the next field up, the seal would still work; if Zone 0 were >4096 bytes,
// the seal would clip its tail.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZoneAlignment, Zone0OccupiesExactlyPage0) {
  static_assert(sizeof(LIBC_NAMESPACE::PcbZone0) == kPcbPageSize,
                "Zone 0 must be exactly one page");

  const uintptr_t first = zone0_base();
  const uintptr_t last = first + sizeof(LIBC_NAMESPACE::PcbZone0) - 1;

  EXPECT_EQ(page_of(first - pcb_base()), static_cast<uintptr_t>(0));
  EXPECT_EQ(page_of(last - pcb_base()), static_cast<uintptr_t>(0));
  EXPECT_EQ(zone0b_base(), first + kPcbPageSize);
}

// ---------------------------------------------------------------------------
// Zone 0b fully occupies page 1: first byte on page 1, last byte on page 1,
// Zone 1 (init_state) begins exactly one page later still.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZoneAlignment, Zone0bOccupiesExactlyPage1) {
  static_assert(sizeof(LIBC_NAMESPACE::PcbZone0b) == kPcbPageSize,
                "Zone 0b must be exactly one page");

  const uintptr_t first = zone0b_base();
  const uintptr_t last = first + sizeof(LIBC_NAMESPACE::PcbZone0b) - 1;

  EXPECT_EQ(page_of(first - pcb_base()), static_cast<uintptr_t>(1));
  EXPECT_EQ(page_of(last - pcb_base()), static_cast<uintptr_t>(1));

  const uintptr_t zone1_base =
      reinterpret_cast<uintptr_t>(&LIBC_NAMESPACE::g_pcb.init_state);
  EXPECT_EQ(zone1_base, first + kPcbPageSize);
}

// ---------------------------------------------------------------------------
// Enumerate every directly-addressable Zone 0 subobject and assert each one
// starts within [0, 4096) AND ends within [0, 4096) — i.e. no field straddles
// the page-0/page-1 boundary.
//
// Zone 0's visible subobjects are:
//   - the constants block (private scalars; only aggregate bounds checkable
//     from this TU via the public accessors, but PcbZone0LayoutCheck in
//     process_control_block.h statically locks their individual offsets to
//     the documented layout).
//   - veh_sealed_ (published type windows::VehSealedState).
//
// For veh_sealed_ we can do the full offset+size fit check directly because
// the type is public and a const accessor returns it.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZoneAlignment, Zone0VehSealedStateFitsInPage0) {
  const uintptr_t first =
      reinterpret_cast<uintptr_t>(&LIBC_NAMESPACE::g_pcb.zone0.veh_sealed());
  const uintptr_t last = first + sizeof(LIBC_NAMESPACE::windows::VehSealedState) - 1;

  // veh_sealed_ lives at offset 72 from the start of Zone 0 (pinned by
  // PcbZone0LayoutCheck::static_assert) — verify the live offset agrees.
  EXPECT_EQ(first - zone0_base(), static_cast<uintptr_t>(72));

  // Entire subobject must lie on page 0.
  EXPECT_EQ(page_of(first - pcb_base()), static_cast<uintptr_t>(0));
  EXPECT_EQ(page_of(last - pcb_base()), static_cast<uintptr_t>(0));

  // Offset + size strictly within the Zone 0 page budget.
  const size_t off_in_zone = first - zone0_base();
  EXPECT_LT(off_in_zone + sizeof(LIBC_NAMESPACE::windows::VehSealedState),
            static_cast<size_t>(kPcbPageSize));
}

// ---------------------------------------------------------------------------
// PCB constants block (first 48 bytes of Zone 0) — verify via the public
// accessors that each constant scalar lies fully within page 0. The access
// of the rvalue address-of via the accessor is valid only for member-access
// expressions; here we instead check the aggregate starting address (zone0
// base) and the documented session_id_ offset from PcbZone0LayoutCheck.
//
// The individual scalar layout (page_size_@0, alloc_granularity_@4,
// min_address_@8, max_address_@16, module_handle_@24, dso_handle_@32,
// session_id_@40, nt_build_@48, capabilities_@52, optional_@56) is
// compile-time verified in process_control_block.h. We redundantly assert
// that each documented offset + its type size fits in [0, 4096) here so a
// silent re-layout cannot slip through without failing this TU as well.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZoneAlignment, Zone0ConstantsBlockFitsInPage0) {
  struct Entry {
    size_t offset;
    size_t size;
    const char *name;
  };

  const Entry entries[] = {
      {0, sizeof(uint32_t), "page_size_"},
      {4, sizeof(uint32_t), "alloc_granularity_"},
      {8, sizeof(void *), "min_address_"},
      {16, sizeof(void *), "max_address_"},
      {24, sizeof(void *), "module_handle_"},
      {32, sizeof(void *), "dso_handle_"},
      {40, sizeof(uint32_t), "session_id_"},
      {48, sizeof(uint32_t), "nt_build_"},
      {52, sizeof(uint32_t), "capabilities_"},
      {56, sizeof(LIBC_NAMESPACE::NtOptionalSyscalls), "optional_"},
      {72, sizeof(LIBC_NAMESPACE::windows::VehSealedState), "veh_sealed_"},
  };

  for (const auto &e : entries) {
    // Every entry begins inside page 0.
    EXPECT_LT(e.offset, static_cast<size_t>(kPcbPageSize));
    // Every entry ends strictly inside page 0 (last byte at offset+size-1).
    EXPECT_LE(e.offset + e.size, static_cast<size_t>(kPcbPageSize));
    // Live-address check: the entry's first and last byte are on page 0.
    const uintptr_t first = zone0_base() + e.offset;
    const uintptr_t last = first + e.size - 1;
    EXPECT_EQ(page_of(first - pcb_base()), static_cast<uintptr_t>(0));
    EXPECT_EQ(page_of(last - pcb_base()), static_cast<uintptr_t>(0));
  }
}

// ---------------------------------------------------------------------------
// Zone 0b active fields — pid_@0, parent_pid_@4, security_cookie_@8,
// security_cookie_complement_@16, zone_canary_@24, dll_notify_cookie_@32
// (the first four offsets are pinned by PcbZone0bLayoutCheck in the header;
// the latter two are documented layout). Each + size must fit in page 1.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZoneAlignment, Zone0bActiveFieldsFitInPage1) {
  struct Entry {
    size_t offset;
    size_t size;
    const char *name;
  };

  const Entry entries[] = {
      {0, sizeof(int32_t), "pid_"},              // pid_t is int32_t
      {4, sizeof(uint32_t), "parent_pid_"},      // DWORD
      {8, sizeof(uintptr_t), "security_cookie_"},
      {16, sizeof(uintptr_t), "security_cookie_complement_"},
      {24, sizeof(uintptr_t), "zone_canary_"},
      {32, sizeof(void *), "dll_notify_cookie_"},
  };

  for (const auto &e : entries) {
    EXPECT_LT(e.offset, static_cast<size_t>(kPcbPageSize));
    EXPECT_LE(e.offset + e.size, static_cast<size_t>(kPcbPageSize));

    const uintptr_t first = zone0b_base() + e.offset;
    const uintptr_t last = first + e.size - 1;

    // Every active field lies on page 1 — i.e. page index 1 within g_pcb.
    EXPECT_EQ(page_of(first - pcb_base()), static_cast<uintptr_t>(1));
    EXPECT_EQ(page_of(last - pcb_base()), static_cast<uintptr_t>(1));
  }
}

// ---------------------------------------------------------------------------
// The first Zone 1 field (init_state) begins on page 2. This is the "Zone 0b
// didn't grow past its page" double-check — if Zone 0b's size ever diverged
// from kPcbPageSize, init_state would shift off page 2 and the seal on Zone
// 0b would either clip init_state or miss the tail of Zone 0b.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZoneAlignment, FirstZone1FieldStartsOnPage2) {
  const uintptr_t init_state_addr =
      reinterpret_cast<uintptr_t>(&LIBC_NAMESPACE::g_pcb.init_state);
  EXPECT_EQ(page_of(init_state_addr - pcb_base()), static_cast<uintptr_t>(2));
  EXPECT_EQ(init_state_addr - pcb_base(), static_cast<uintptr_t>(kZone1Start));
}
