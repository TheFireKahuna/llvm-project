//===--- concurrent_fuzz siphash impl ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "test/src/__support/concurrent_fuzz/siphash.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent_fuzz {

namespace {

LIBC_INLINE uint64_t rotl(uint64_t x, unsigned r) {
  return (x << r) | (x >> ((64 - r) & 63));
}

LIBC_INLINE void sipround(uint64_t &v0, uint64_t &v1, uint64_t &v2,
                          uint64_t &v3) {
  v0 += v1;
  v1 = rotl(v1, 13);
  v1 ^= v0;
  v0 = rotl(v0, 32);
  v2 += v3;
  v3 = rotl(v3, 16);
  v3 ^= v2;
  v0 += v3;
  v3 = rotl(v3, 21);
  v3 ^= v0;
  v2 += v1;
  v1 = rotl(v1, 17);
  v1 ^= v2;
  v2 = rotl(v2, 32);
}

} // namespace

void SipHasher::reset(uint64_t k0, uint64_t k1) {
  v0 = k0 ^ 0x736F6D6570736575ULL;
  v1 = k1 ^ 0x646F72616E646F6DULL;
  v2 = k0 ^ 0x6C7967656E657261ULL;
  v3 = k1 ^ 0x7465646279746573ULL;
}

void SipHasher::absorb_u64(uint64_t word) {
  v3 ^= word;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  v0 ^= word;
}

uint64_t SipHasher::finalize(uint8_t tail_byte_count, uint64_t tail) {
  uint64_t b = (static_cast<uint64_t>(tail_byte_count) << 56) | tail;
  v3 ^= b;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  v0 ^= b;
  v2 ^= 0xFFu;
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  sipround(v0, v1, v2, v3);
  return v0 ^ v1 ^ v2 ^ v3;
}

uint64_t siphash_u64s(uint64_t k0, uint64_t k1, const uint64_t *words,
                      size_t count) {
  SipHasher h;
  h.reset(k0, k1);
  for (size_t i = 0; i < count; ++i)
    h.absorb_u64(words[i]);
  // Tail byte count = (count * 8) mod 256 — only the low byte is used in
  // SipHash. We hash whole u64 words so the per-byte tail is empty.
  return h.finalize(static_cast<uint8_t>((count * 8u) & 0xFFu), 0);
}

} // namespace concurrent_fuzz
} // namespace LIBC_NAMESPACE_DECL
