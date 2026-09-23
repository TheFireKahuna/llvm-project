//===- ABIContract.cpp - Physical ABI requirements
//-------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ABIContract.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::abi;

namespace {
// Every allocation is bounded by consumed input, in addition to the aggregate
// limit. Traversals below are iterative so recursive type graphs cannot exhaust
// the native stack. Overlong integers have no alternate canonical spelling.
class Reader {
public:
  explicit Reader(StringRef Bytes) : Bytes(Bytes) {}
  bool number(uint64_t &Value) {
    unsigned Size = 0;
    const char *Error = nullptr;
    const auto *Start = reinterpret_cast<const uint8_t *>(Bytes.data());
    Value = decodeULEB128(Start, &Size, Start + Bytes.size(), &Error);
    if (Error || Size != getULEB128Size(Value))
      return false;
    Bytes = Bytes.drop_front(Size);
    return true;
  }
  bool string(std::string &Value) {
    uint64_t Size;
    if (!number(Size) || Size > Bytes.size())
      return false;
    Value = Bytes.take_front(Size).str();
    Bytes = Bytes.drop_front(Size);
    return true;
  }
  bool numbers(SmallVectorImpl<uint64_t> &Values) {
    uint64_t Count;
    if (!number(Count) || Count > Bytes.size())
      return false;
    for (uint64_t I = 0, Value; I != Count; ++I) {
      if (!number(Value))
        return false;
      Values.push_back(Value);
    }
    return true;
  }
  size_t remaining() const { return Bytes.size(); }

private:
  StringRef Bytes;
};

void writeNumbers(raw_ostream &OS, ArrayRef<uint64_t> Values) {
  encodeULEB128(Values.size(), OS);
  for (uint64_t Value : Values)
    encodeULEB128(Value, OS);
}

bool validNode(const Node &N) {
  // Each category has an explicit physical property tuple. Edge-specific
  // tuples are checked separately; no arbitrary tag/value extension is trusted.
  static constexpr uint8_t Counts[] = {0, 2, 2, 3, 3, 3, 3, 4,
                                       6, 2, 8, 9, 5, 2, 2, 1};
  unsigned K = static_cast<unsigned>(N.Category);
  if (K >= std::size(Counts) || N.Properties.size() != Counts[K])
    return false;
  bool Anonymous = N.Category == Kind::Argument ||
                   N.Category == Kind::MachineType ||
                   N.Category == Kind::VTable;
  if (Anonymous != N.Identity.empty() || N.Qualifiers > 7)
    return false;
  if (N.Category == Kind::Identity || N.Category == Kind::Name)
    return N.Edges.empty() && !N.Identity.empty();
  if (N.Category == Kind::Argument && (N.Properties[7] || N.Properties[8]))
    return false;
  return true;
}

bool validEdge(const Edge &E) {
  static constexpr uint8_t Counts[] = {0, 0, 0, 0, 4, 3, 1, 0, 1, 0,
                                       3, 0, 0, 0, 0, 0, 3, 3, 5, 1};
  unsigned R = static_cast<unsigned>(E.Relation);
  return R < std::size(Counts) && E.Properties.size() == Counts[R];
}

bool validRelation(Kind From, Role Relation, Kind To) {
  switch (Relation) {
  case Role::Pointee:
    return From == Kind::Pointer || From == Kind::Reference ||
           From == Kind::MemberPointer;
  case Role::Element:
    return From == Kind::Array || From == Kind::Vector ||
           From == Kind::Complex || From == Kind::Atomic ||
           (From == Kind::MachineType && To == Kind::MachineType);
  case Role::Class:
    return From == Kind::MemberPointer &&
           (To == Kind::Record || To == Kind::Identity);
  case Role::Underlying:
    return From == Kind::Enumeration && To == Kind::Scalar;
  case Role::Field:
    return From == Kind::Record ||
           (From == Kind::MachineType && To == Kind::MachineType);
  case Role::Base:
  case Role::PrimaryBase:
  case Role::VirtualBase:
    return From == Kind::Record && To == Kind::Record;
  case Role::Result:
  case Role::Parameter:
    return From == Kind::Function;
  case Role::ResultABI:
  case Role::ParameterABI:
    return From == Kind::Function && To == Kind::Argument;
  case Role::Coercion:
  case Role::Padding:
  case Role::Expansion:
    return From == Kind::Argument && To == Kind::MachineType;
  case Role::ArgumentFrame:
    return From == Kind::Function && To == Kind::MachineType;
  case Role::VTable:
    return From == Kind::Record && To == Kind::VTable;
  case Role::VTableEntry:
    return From == Kind::VTable && (To == Kind::Record || To == Kind::Function);
  case Role::AddressPoint:
  case Role::Thunk:
    return From == Kind::VTable && To == Kind::Record;
  }
  llvm_unreachable("invalid ABI relation");
}

// Types/functions have semantic names. ABI coercions, argument lowering and
// vtable records are anonymous physical nodes; intern them bottom-up so their
// encoding does not depend on whether a producer shared equivalent nodes.
// Recursive type dependencies end at named nodes, not anonymous machine-type
// cycles. This also removes repeated argument/coercion records from vtables.
bool internNodes(ArrayRef<Node> Nodes, std::vector<uint32_t> &Representatives) {
  Representatives.assign(Nodes.size(), UINT32_MAX);
  StringMap<uint32_t> Named, Anonymous;
  SmallVector<uint8_t, 0> State(Nodes.size(), 0);
  for (uint32_t I = 0; I != Nodes.size(); ++I) {
    const Node &N = Nodes[I];
    if (N.Identity.empty())
      continue;
    std::string Key;
    raw_string_ostream OS(Key);
    encodeULEB128(static_cast<unsigned>(N.Category), OS);
    encodeULEB128(N.Qualifiers, OS);
    OS << N.Identity;
    if (!Named.try_emplace(Key, I).second)
      return false;
    Representatives[I] = I;
  }
  SmallVector<std::pair<uint32_t, size_t>, 16> Stack;
  for (uint32_t Root = 0; Root != Nodes.size(); ++Root) {
    if (Representatives[Root] != UINT32_MAX)
      continue;
    State[Root] = 1;
    Stack.emplace_back(Root, 0);
    while (!Stack.empty()) {
      auto &[Index, Next] = Stack.back();
      const Node &N = Nodes[Index];
      if (Next != N.Edges.size()) {
        uint32_t Target = N.Edges[Next++].Target;
        if (Representatives[Target] != UINT32_MAX)
          continue;
        if (State[Target])
          return false;
        State[Target] = 1;
        Stack.emplace_back(Target, 0);
        continue;
      }
      std::string Key;
      raw_string_ostream OS(Key);
      encodeULEB128(static_cast<unsigned>(N.Category), OS);
      encodeULEB128(N.Qualifiers, OS);
      writeNumbers(OS, N.Properties);
      for (const Edge &E : N.Edges) {
        encodeULEB128(static_cast<unsigned>(E.Relation), OS);
        encodeULEB128(Representatives[E.Target], OS);
        writeNumbers(OS, E.Properties);
      }
      auto [It, Inserted] = Anonymous.try_emplace(Key, Index);
      Representatives[Index] = It->second;
      Stack.pop_back();
    }
  }
  return true;
}
// Union only physical nodes. Matching an Identity edge with a complete edge
// selects the latter at that position, without upgrading other references to
// the same Identity node. Named physical nodes are shared within a contract;
// their dependencies must therefore compose even when reached by different
// paths. Anonymous nodes are united only at matching edge positions.
class ContractMerger {
  std::vector<Node> Nodes;
  std::vector<uint32_t> Parents, Sizes;
  SmallVector<uint32_t, 32> Work;

  uint32_t find(uint32_t I) {
    while (Parents[I] != I) {
      Parents[I] = Parents[Parents[I]];
      I = Parents[I];
    }
    return I;
  }

  Expected<uint32_t> unite(uint32_t A, uint32_t B) {
    A = find(A);
    B = find(B);
    if (A == B)
      return A;
    const Node &Left = Nodes[A];
    const Node &Right = Nodes[B];
    if (Left.Identity != Right.Identity || Left.Qualifiers != Right.Qualifiers)
      return createStringError("conflicting ABI identities or qualifiers");
    if (Left.Category == Kind::Identity && Right.Category != Kind::Identity &&
        Right.Category != Kind::Name)
      return B;
    if (Right.Category == Kind::Identity && Left.Category != Kind::Identity &&
        Left.Category != Kind::Name)
      return A;
    if (Left.Category != Right.Category ||
        Left.Properties != Right.Properties ||
        Left.Edges.size() != Right.Edges.size())
      return createStringError("conflicting physical ABI node properties");
    for (size_t I = 0; I != Left.Edges.size(); ++I)
      if (Left.Edges[I].Relation != Right.Edges[I].Relation ||
          Left.Edges[I].Properties != Right.Edges[I].Properties)
        return createStringError("conflicting physical ABI edge properties");
    if (Sizes[A] < Sizes[B])
      std::swap(A, B);
    Parents[B] = A;
    Sizes[A] += Sizes[B];
    Work.push_back(B);
    return A;
  }

public:
  Expected<Contract> run(const Contract &Left, const Contract &Right) {
    assert(!Left.Nodes.empty() && !Right.Nodes.empty());
    // Each discarded node is visited once. Unlike a product-graph traversal,
    // composition needs at most the sum of the input nodes and edges.
    size_t Count = Left.Nodes.size() + Right.Nodes.size();
    if (Count > 1024 * 1024)
      return createStringError(
          "ABI contract composition exceeds the node limit");
    Nodes = Left.Nodes;
    uint32_t Offset = Nodes.size();
    Nodes.insert(Nodes.end(), Right.Nodes.begin(), Right.Nodes.end());
    for (uint32_t I = Offset; I != Count; ++I)
      for (Edge &E : Nodes[I].Edges)
        E.Target += Offset;
    Parents.reserve(Count);
    Sizes.assign(Count, 1);
    StringMap<uint32_t> Named;
    for (uint32_t I = 0; I != Count; ++I)
      Parents.push_back(I);
    for (uint32_t I = 0; I != Count; ++I) {
      const Node &N = Nodes[I];
      if (N.Identity.empty())
        continue;
      std::string Key;
      raw_string_ostream OS(Key);
      encodeULEB128(static_cast<unsigned>(N.Category), OS);
      encodeULEB128(N.Qualifiers, OS);
      OS << N.Identity;
      auto [It, Inserted] = Named.try_emplace(Key, I);
      if (!Inserted) {
        auto Joined = unite(It->second, I);
        if (!Joined)
          return Joined.takeError();
      }
    }
    auto Root = unite(0, Offset);
    if (!Root)
      return Root.takeError();
    while (!Work.empty()) {
      uint32_t Discarded = Work.pop_back_val();
      for (size_t I = 0; I != Nodes[Discarded].Edges.size(); ++I) {
        uint32_t Kept = find(Discarded);
        auto Target = unite(Nodes[Kept].Edges[I].Target,
                            Nodes[Discarded].Edges[I].Target);
        if (!Target)
          return Target.takeError();
        Nodes[find(Kept)].Edges[I].Target = *Target;
      }
    }

    // Keep only reachable representatives. The ordinary codec interns
    // equivalent anonymous nodes and provides the canonical DFS numbering.
    Contract Result;
    std::vector<uint32_t> IDs(Count, UINT32_MAX);
    SmallVector<uint32_t, 32> Order{find(*Root)};
    IDs[Order.front()] = 0;
    for (size_t I = 0; I != Order.size(); ++I) {
      Node N = Nodes[Order[I]];
      for (Edge &E : N.Edges) {
        uint32_t Target = find(E.Target);
        if (IDs[Target] == UINT32_MAX) {
          IDs[Target] = Order.size();
          Order.push_back(Target);
        }
        E.Target = IDs[Target];
      }
      Result.Nodes.push_back(std::move(N));
    }
    return Contract::decode(Result.encode());
  }
};
} // namespace

Expected<Contract> Contract::mergeWith(const Contract &Other) const {
  return ContractMerger().run(*this, Other);
}

std::string Contract::encode() const {
  assert(!Nodes.empty());
  std::vector<uint32_t> Representatives;
  if (!internNodes(Nodes, Representatives))
    return {};
  // Canonical IDs are first-encounter DFS IDs, independent of a compiler's or
  // linker's intern-table numbering. Back edges retain cycles without
  // recursion.
  uint32_t Root = Representatives[0];
  SmallVector<uint32_t, 32> Order{Root};
  std::vector<uint32_t> IDs(Nodes.size(), UINT32_MAX);
  IDs[Root] = 0;
  SmallVector<std::pair<uint32_t, size_t>, 16> Stack{{Root, 0}};
  while (!Stack.empty()) {
    auto &[Index, Next] = Stack.back();
    if (Next == Nodes[Index].Edges.size()) {
      Stack.pop_back();
      continue;
    }
    uint32_t Target = Representatives[Nodes[Index].Edges[Next++].Target];
    assert(Target < Nodes.size());
    if (IDs[Target] == UINT32_MAX) {
      IDs[Target] = Order.size();
      Order.push_back(Target);
      Stack.emplace_back(Target, 0);
    }
  }
  std::string Bytes;
  raw_string_ostream OS(Bytes);
  encodeULEB128(ContractVersion, OS);
  encodeULEB128(Order.size(), OS);
  for (uint32_t Index : Order) {
    const Node &N = Nodes[Index];
    assert(validNode(N));
    encodeULEB128(static_cast<unsigned>(N.Category), OS);
    encodeULEB128(N.Qualifiers, OS);
    encodeULEB128(N.Identity.size(), OS);
    OS << N.Identity;
    writeNumbers(OS, N.Properties);
    encodeULEB128(N.Edges.size(), OS);
    for (const Edge &E : N.Edges) {
      assert(validEdge(E));
      encodeULEB128(static_cast<unsigned>(E.Relation), OS);
      encodeULEB128(IDs[Representatives[E.Target]], OS);
      writeNumbers(OS, E.Properties);
    }
  }
  return Bytes;
}

Expected<Contract> Contract::decode(StringRef Bytes) {
  if (Bytes.size() > 16 * 1024 * 1024)
    return createStringError("ABI contract exceeds the schema resource limit");
  Reader R(Bytes);
  uint64_t Version, Count;
  if (!R.number(Version) || Version != ContractVersion || !R.number(Count) ||
      !Count || Count > R.remaining() / 5)
    return createStringError("invalid ABI contract header");
  Contract C;
  C.Nodes.reserve(Count);
  for (uint64_t I = 0; I != Count; ++I) {
    uint64_t Category, Qualifiers, Edges;
    Node N;
    if (!R.number(Category) || Category > static_cast<unsigned>(Kind::VTable) ||
        !R.number(Qualifiers) || Qualifiers > UINT32_MAX ||
        !R.string(N.Identity) || !R.numbers(N.Properties) || !R.number(Edges) ||
        Edges > R.remaining() / 3)
      return createStringError("malformed ABI contract node");
    N.Category = static_cast<Kind>(Category);
    N.Qualifiers = Qualifiers;
    for (uint64_t J = 0; J != Edges; ++J) {
      uint64_t Relation, Target;
      Edge E;
      if (!R.number(Relation) ||
          Relation > static_cast<unsigned>(Role::VirtualBase) ||
          !R.number(Target) || Target >= Count || !R.numbers(E.Properties))
        return createStringError("invalid ABI contract dependency");
      E.Relation = static_cast<Role>(Relation);
      E.Target = Target;
      if (!validEdge(E))
        return createStringError("invalid ABI contract edge properties");
      N.Edges.push_back(std::move(E));
    }
    if (!validNode(N))
      return createStringError("invalid ABI contract node properties");
    C.Nodes.push_back(std::move(N));
  }
  if (R.remaining() || C.encode() != Bytes)
    return createStringError("non-canonical ABI contract graph");
  for (const Node &N : C.Nodes)
    for (const Edge &E : N.Edges)
      if (!validRelation(N.Category, E.Relation, C.Nodes[E.Target].Category))
        return createStringError("invalid ABI contract relation");
  return C;
}

bool Contract::isSatisfiedBy(const Contract &Definition) const {
  assert(!Nodes.empty() && !Definition.Nodes.empty());
  DenseSet<std::pair<uint32_t, uint32_t>> Seen;
  SmallVector<std::pair<uint32_t, uint32_t>, 32> Work{{0, 0}};
  while (!Work.empty()) {
    auto Pair = Work.pop_back_val();
    if (!Seen.insert(Pair).second)
      continue;
    // Hostile graphs can arrange a quadratic product traversal despite each
    // individual graph being small. The check is fail-closed and bounded.
    if (Seen.size() > 1024 * 1024)
      return false;
    const Node &Required = Nodes[Pair.first];
    const Node &Offered = Definition.Nodes[Pair.second];
    if (Required.Identity != Offered.Identity ||
        Required.Qualifiers != Offered.Qualifiers)
      return false;
    if (Required.Category == Kind::Identity) {
      if (Offered.Category == Kind::Name || Offered.Identity.empty())
        return false;
      continue;
    }
    if (Required.Category != Offered.Category ||
        Required.Properties != Offered.Properties ||
        Required.Edges.size() != Offered.Edges.size())
      return false;
    for (size_t I = 0; I != Required.Edges.size(); ++I) {
      const Edge &A = Required.Edges[I];
      const Edge &B = Offered.Edges[I];
      if (A.Relation != B.Relation || A.Properties != B.Properties)
        return false;
      Work.emplace_back(A.Target, B.Target);
    }
  }
  return true;
}

std::string llvm::abi::contractDigest(StringRef Bytes) {
  auto Hash = SHA256::hash(arrayRefFromStringRef(Bytes));
  std::string Result = encodeBase64(Hash);
  for (char &C : Result) {
    if (C == '+')
      C = '-';
    else if (C == '/')
      C = '_';
  }
  while (Result.back() == '=')
    Result.pop_back();
  return Result;
}
