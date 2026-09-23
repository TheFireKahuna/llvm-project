//===- ABIContract.h - Physical ABI requirements -----------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_ABICONTRACT_H
#define LLVM_SUPPORT_ABICONTRACT_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <string>
#include <vector>

namespace llvm::abi {

// These are wire values, not Clang TypeClass, LLVM TypeID or ABIArgInfo values.
// Change the schema version when the meaning or ordering of a field changes.
inline constexpr unsigned ContractVersion = 1;
enum class Kind : uint8_t {
  Identity,
  Name,
  Scalar,
  Pointer,
  Reference,
  MemberPointer,
  Array,
  Vector,
  Record,
  Enumeration,
  Function,
  Argument,
  MachineType,
  Complex,
  Atomic,
  VTable,
};

enum class Role : uint8_t {
  Pointee,
  Element,
  Class,
  Underlying,
  Field,
  Base,
  PrimaryBase,
  Result,
  Parameter,
  ResultABI,
  ParameterABI,
  Coercion,
  Padding,
  Expansion,
  ArgumentFrame,
  VTable,
  VTableEntry,
  AddressPoint,
  Thunk,
  VirtualBase,
};

struct Edge {
  Role Relation;
  uint32_t Target;
  SmallVector<uint64_t, 2> Properties;
};

struct Node {
  Kind Category;
  uint32_t Qualifiers = 0;
  std::string Identity;
  SmallVector<uint64_t, 6> Properties;
  SmallVector<Edge, 4> Edges;
};

// Property tuples, in wire order. Sizes/alignments are bits except Record,
// whose CharUnits fields are target bytes, and Name, whose length is bytes.
//   Identity:     none
//   Name/Scalar:  size, alignment
//   Pointer:      size, alignment, target address space
//   Reference:    size, alignment, rvalue-reference flag
//   MemberPointer: size, alignment, function-member flag
//   Array:        size, alignment, element count
//   Vector:       size, alignment, element count, extended-vector flag
//   Record:       size, alignment, data size, nonvirtual size/alignment, flags
//   Enumeration/Complex/Atomic: size, alignment
//   Function:     LLVM CC, required count, variadic, instance method, regparm+1
//                 (zero when absent), flags, inalloca byte alignment, vector
//                 width
//   Argument:     lowering kind, direct byte offset, byte alignment, address
//                 space, flags, inalloca field, padding-inreg, two reserved
//                 zeros
//   MachineType:  kind, size, alignment, element count/bit width, flags
//   VTable:       relative-component flag
// Edge tuples are also fixed: field (index, bit offset, width or UINT64_MAX,
// flags), base (byte offset, virtual, access), primary base (virtual),
// parameter (index), parameter ABI (index, parameter ABI kind, flags), vtable
// entry (index, component kind, signed offset), address point (table offset,
// table index, address-point index), thunk (index,
// this/virtual-this/return/virtual- return adjustments), virtual base (byte
// offset). Other edges have no tuple. Signed offsets use their uint64_t
// two's-complement representation. Reserved fields must stay zero; changing a
// tuple requires a new ContractVersion.

// Nodes describe physical facts, never source bodies or debug-only queries.
// Identity nodes intentionally carry no layout knowledge. An edge to one must
// not acquire completeness merely because another use knows the definition.
// The root is node zero. Edges preserve ABI order (fields, parameters, slots),
// and shared/cyclic dependencies use indices instead of copying their bodies.
struct Contract {
  std::vector<Node> Nodes;

  LLVM_ABI std::string encode() const;
  LLVM_ABI static Expected<Contract> decode(StringRef Bytes);
  LLVM_ABI bool isSatisfiedBy(const Contract &Definition) const;

  // Compose compatible requirements without acquiring layout knowledge at
  // unrelated Identity edges. Both inputs must be valid contracts. Conflicting
  // physical facts or an exceeded resource bound return an error.
  LLVM_ABI Expected<Contract> mergeWith(const Contract &Other) const;
};

// Return the full SHA-256 in unpadded base64url form. This names a contract;
// it does not authenticate a provider. Callers intern records before hashing.
LLVM_ABI std::string contractDigest(StringRef CanonicalBytes);

} // namespace llvm::abi

#endif
