//===-- PCB Zone 0 / Zone 0b seal AV death tests --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The PCB design contract says Zone 0 (page 0 of g_pcb) is sealed
// PAGE_READONLY at the end of Tier A and stays sealed for the rest of the
// process lifetime, and Zone 0b (page 1) is sealed PAGE_READONLY outside
// the narrow fork-reinit / veh-fini unseal windows. That guarantee is
// load-bearing — the master VEH dispatch table, NT capability bitmask,
// /GS-style security cookie and zone canary all live there, and a
// successful attacker write on any of those fields breaks downstream
// hardening.
//
// These tests verify that the seals *actually apply*, not merely that the
// code calls NtProtectVirtualMemory. By the time any unit test runs,
// __libc_bootstrap() has completed and both zones are sealed; a write to
// either page from a non-VEH context must raise EXCEPTION_ACCESS_VIOLATION
// and — because the master VEH does not catch AVs on sealed PCB pages —
// terminate the process by signal.
//
// Pattern mirrors slab_pool_death_test.cpp: the parent forks; the child
// performs the illegal write through a `volatile unsigned char *` obtained
// by const-casting a Zone 0 / Zone 0b byte pointer; the parent waitpid's
// and asserts WIFSIGNALED with WTERMSIG == SIGSEGV. If the write ever
// completes and the child reports REACHED_END, the seal regressed.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/fork.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include "include/llvm-libc-macros/windows/signal-macros.h"
#include "include/llvm-libc-macros/windows/sys-wait-macros.h"

namespace {

// Sentinels so a regression (write completed without a fault) surfaces as
// a distinct exit code rather than as a plain zero.
enum : int {
  REACHED_END = 77,
};

// Touch the first byte of the target zone via a volatile unsigned char *
// obtained by const_cast'ing away the const-ness of the Zone 0 field
// address. The compiler cannot prove the write is dead so it is emitted;
// on a correctly sealed page the store raises EXCEPTION_ACCESS_VIOLATION.
//
// We deliberately pick `&g_pcb.zone0.page_size()` (returns a const ref
// logically, but we take the address through the raw zone pointer) and
// `&g_pcb.zone0b` as the page bases — both fields are at offset 0 inside
// their respective pages, so a one-byte store there is the cleanest
// possible probe for "is this page writable?".
[[gnu::noinline]] void poke_zone0_first_byte() {
  auto *zone = const_cast<LIBC_NAMESPACE::PcbZone0 *>(&LIBC_NAMESPACE::g_pcb.zone0);
  auto *probe = reinterpret_cast<volatile unsigned char *>(zone);
  *probe = 0xAB; // must AV if Zone 0 seal is live
}

[[gnu::noinline]] void poke_zone0b_first_byte() {
  auto *zone = const_cast<LIBC_NAMESPACE::PcbZone0b *>(&LIBC_NAMESPACE::g_pcb.zone0b);
  auto *probe = reinterpret_cast<volatile unsigned char *>(zone);
  *probe = 0xCD; // must AV if Zone 0b seal is live
}

} // namespace

// ---------------------------------------------------------------------------
// Zone 0 — sealed for process lifetime. Writing ANY Zone 0 byte must AV.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZone0SealDeath, WriteToZone0FaultsWithSigsegv) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Force the compiler to treat the page_size read as live so nothing
    // silently optimises the whole probe away ahead of the write.
    (void)LIBC_NAMESPACE::g_pcb.zone0.page_size();
    poke_zone0_first_byte();
    // Must not reach here — the sealed page makes the write AV.
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  // Expected: killed by a signal, WTERMSIG == SIGSEGV. Anything else is a
  // regression of the Zone 0 seal (or of the signal mapping for AV).
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}

// ---------------------------------------------------------------------------
// Zone 0b — sealed whenever we are not inside libc_fork_reinit() or
// veh_core fini. Test-body context is neither, so the page must be RO.
// Writing any Zone 0b byte must AV with SIGSEGV.
// ---------------------------------------------------------------------------

TEST(LlvmLibcPcbZone0bSealDeath, WriteToZone0bFaultsWithSigsegv) {
  // Belt-and-braces: confirm Zone 0b is not currently in an unseal window.
  // If a future refactor leaves the zone unsealed during test bring-up the
  // death check below would spuriously "fail" (the write would succeed and
  // the child would exit normally) — catch that here with a clear signal
  // instead of chasing it through WIFSIGNALED.
  EXPECT_FALSE(LIBC_NAMESPACE::zone0b_writable_now());

  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    // Child inherits the sealed PTE state across fork (Zone 0b is RO in
    // the parent at fork time; the CoW clone inherits that protection).
    (void)LIBC_NAMESPACE::g_pcb.zone0b.pid();
    poke_zone0b_first_byte();
    ::NtTerminateProcess(NtCurrentProcess(), REACHED_END);
  }
  int status = 0;
  LIBC_NAMESPACE::waitpid(pid, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_FALSE(WIFEXITED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}
