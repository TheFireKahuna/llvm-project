/*===-- ThreadPointer.inc - The owned thread-local model's ABI --*- C++ -*-===*\
|*
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
|* See https://llvm.org/LICENSE.txt for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
\*===----------------------------------------------------------------------===*/
/*
 * The thread-local model that NT-POSIX and Windows Itanium share, as the
 * compiler, the linker, the fill and the runtime hold it: a thread's thread
 * pointer names its block, found from the thread's client id through the
 * runtime's TID map, and an image's thread-locals are at its place from the
 * thread pointer, or, for a wide image, in the slot its encoded wide word
 * names.
 *
 * The file has two identical copies: this one, and compiler-rt's in
 * include/tls. Change this one and copy it there.
 *
 * Defining TP_EXPORT(Id, Name, Sealed) or TP_SYMBOL(Id, Name) before
 * including the file expands the list of names; including it with neither
 * defined introduces the constants.
 */

/* The runtime's exports, at the conventions its specification states. An
 * image of the model reads each through the import symbol "__imp_" #Name
 * Sealed: its word, a sealed value, for "$val"; what it answers for the
 * image's base, the image's place word, for "$init"; the import itself for
 * "". */
#ifndef TP_EXPORT
#define TP_EXPORT(Id, Name, Sealed)
#endif
TP_EXPORT(Map, __tp_map, "$val")
TP_EXPORT(Base, __tp_base, "$val")
TP_EXPORT(Key, __tp_key, "$val")
TP_EXPORT(Register, __tp_register, "$init")
TP_EXPORT(None, __tp_none, "")
TP_EXPORT(Slow, __tp_slow, "")
TP_EXPORT(Try, __tp_try, "")
TP_EXPORT(Probe, __tp_probe, "")
#undef TP_EXPORT

/* The link's: the absolute symbol that states an image's own place, and the
 * export of an image whose form is direct and whose records are named. */
#ifndef TP_SYMBOL
#define TP_SYMBOL(Id, Name)
#endif
TP_SYMBOL(Place, __tp_place)
TP_SYMBOL(Unit, __tp_unit)
#undef TP_SYMBOL

#ifndef TP_INC
#define TP_INC

/* The derivation. An entry of the TID map is the u32 at map + the client id,
 * which the TEB holds at TP_CLIENT_ID; it names a block exactly when a bit
 * of TP_SLOT_TEST is set, and with i = e & TP_MASK the block is at
 *   base + ((i >> 6) << TP_CHUNK_SHIFT) + ((i & 63) << TP_BLOCK_SHIFT).
 * TP_TOMBSTONE is the entry of a released thread, and the closed thread
 * pointer the block of slot TP_CLOSED_SLOT of chunk 0, never committed. */
#define TP_CLIENT_ID 0x48
#define TP_SLOT_TEST 0x30
#define TP_MASK 0x1FFFFF
#define TP_BLOCK_SHIFT 10
#define TP_CHUNK_SHIFT 18
#define TP_TOMBSTONE 1
#define TP_CLOSED_SLOT 16

/* The block: the header's words that code outside the runtime addresses, the
 * header's size H, then the wide area's TP_WIDE_WORDS words, the
 * executable's first, then the executable's thread-locals where they fit the
 * block. The planes follow the blocks, a thread's slot of plane p at
 * p * TP_PLANE from its thread pointer: the executable's thread-locals in
 * plane 1 where they fit a slot and not the block, the runtime's in
 * TP_PLANES. An image's thread-locals of at most TP_BLOCK bytes are
 * direct, and of more wide. */
#define TP_BLOCK 0x400
#define TP_TCB_SELF 0x00
#define TP_SELF 0x10
#define TP_STACK_GUARD 0x28
#define TP_POINTER_GUARD 0x30
#define TP_H 0x100
#define TP_WIDE_WORDS 8
#define TP_EXE_PLACE (TP_H + 8 * TP_WIDE_WORDS)
#define TP_PLANE 0xC000
#define TP_PLANES 4

/* A word whose value the fill computes holds this until the fill writes it,
 * which no pointer holds on either machine. */
#define TP_UNFILLED 0xF1F1F1F1F1F1F1F1ULL

/* The place word, __imp___tp_register$init, TP_PLACE_IN_SEALED bytes
 * into .sealed, after the fill's done word: the image's place, its form and,
 * for a wide image, the key. */
#define TP_PLACE_IN_SEALED 8
#define TP_PLACE_WORD_SIZE 24
#define TP_PLACE_AT 0
#define TP_FORM_AT 8
#define TP_KEY_AT 16
#define TP_FORM_DIRECT 0
#define TP_FORM_WIDE 1

/* A thread-local's record, X$tls: the address of its image's place word, its
 * offset in the image's template, and its guard byte's. */
#define TP_RECORD_SIZE 16
#define TP_RECORD_PLACE_AT 0
#define TP_RECORD_OFFSET_AT 8
#define TP_RECORD_GUARD_AT 12

/* A wide word holds rol(slot ^ key, TP_ROTATE). The key has the bits of
 * TP_KEY_SET set and those of TP_KEY_CLEAR clear, so that a zero word,
 * or one decoded with an unfilled key, is no user address on either machine. */
#define TP_ROTATE 17
#define TP_KEY_SET 0x8080000000000000ULL
#define TP_KEY_CLEAR 0x4040000000000000ULL

#endif /* TP_INC */
