//===-- X86WinCOFFObjectWriter.cpp - X86 Win COFF Writer ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/X86FixupKinds.h"
#include "MCTargetDesc/X86MCAsmInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class X86WinCOFFObjectWriter : public MCWinCOFFObjectTargetWriter {
  bool DescribeSites;

public:
  X86WinCOFFObjectWriter(bool Is64Bit, bool DescribeSites);
  ~X86WinCOFFObjectWriter() override = default;

  uint64_t getLinkRecordCapabilities() const override {
    return DescribeSites ? COFF::LinkRecordsX86_64Sites : 0;
  }
  std::optional<unsigned> getLinkSiteForm(const MCFixup &Fixup,
                                          unsigned Type) const override;

  unsigned getRelocType(MCContext &Ctx, const MCValue &Target,
                        const MCFixup &Fixup, bool IsCrossSection,
                        const MCAsmBackend &MAB) const override;
};

} // end anonymous namespace

X86WinCOFFObjectWriter::X86WinCOFFObjectWriter(bool Is64Bit,
                                               bool DescribeSites)
    : MCWinCOFFObjectTargetWriter(Is64Bit ? COFF::IMAGE_FILE_MACHINE_AMD64
                                          : COFF::IMAGE_FILE_MACHINE_I386),
      DescribeSites(Is64Bit && DescribeSites) {}

// Every REL32 that is not a branch has a site, since a linker takes a
// qualifying relocation without one as a branch. The fixup kind says whether
// the instruction is a branch, and the code emitter how it uses the address.
// A jump's prefixes are part of its form: a linker that makes it a direct jump
// rewrites it from its first byte, so that the Windows unwinder, which finds
// an epilogue by decoding its last instruction, still recognises it.
std::optional<unsigned>
X86WinCOFFObjectWriter::getLinkSiteForm(const MCFixup &Fixup,
                                        unsigned Type) const {
  if (!DescribeSites || Type != COFF::IMAGE_REL_AMD64_REL32 ||
      Fixup.getKind() == X86::reloc_branch_4byte_pcrel)
    return std::nullopt;
  switch (Fixup.getUse()) {
  case MCFixupUse::Unknown:
    return COFF::LinkSiteOther;
  case MCFixupUse::Call:
    return COFF::LinkSiteCall;
  case MCFixupUse::Jump:
    // The opcode and the ModRM byte precede the displacement.
    switch (Fixup.getInstOffset()) {
    case 2:
      return COFF::LinkSiteJump;
    case 3:
      return COFF::LinkSiteJumpOnePrefix;
    default:
      return COFF::LinkSiteOther;
    }
  case MCFixupUse::Load:
    return COFF::LinkSiteLoad;
  case MCFixupUse::Address:
    return COFF::LinkSiteAddress;
  }
  llvm_unreachable("unknown fixup use");
}

unsigned X86WinCOFFObjectWriter::getRelocType(MCContext &Ctx,
                                              const MCValue &Target,
                                              const MCFixup &Fixup,
                                              bool IsCrossSection,
                                              const MCAsmBackend &MAB) const {
  const bool Is64Bit = getMachine() == COFF::IMAGE_FILE_MACHINE_AMD64;
  unsigned FixupKind = Fixup.getKind();
  bool PCRel = Fixup.isPCRel();
  if (IsCrossSection) {
    // IMAGE_REL_AMD64_REL64 does not exist. We treat FK_Data_8 as FK_PCRel_4 so
    // that .quad a-b can lower to IMAGE_REL_AMD64_REL32. This allows generic
    // instrumentation to not bother with the COFF limitation. A negative value
    // needs attention.
    if (!PCRel &&
        (FixupKind == FK_Data_4 || FixupKind == llvm::X86::reloc_signed_4byte ||
         (FixupKind == FK_Data_8 && Is64Bit))) {
      FixupKind = FK_Data_4;
      PCRel = true;
    } else {
      Ctx.reportError(Fixup.getLoc(), "Cannot represent this expression");
      return COFF::IMAGE_REL_AMD64_ADDR32;
    }
  }

  auto Spec = Target.getSpecifier();
  if (Is64Bit) {
    switch (FixupKind) {
    case X86::reloc_riprel_4byte:
    case X86::reloc_riprel_4byte_movq_load:
    case X86::reloc_riprel_4byte_movq_load_rex2:
    case X86::reloc_riprel_4byte_relax:
    case X86::reloc_riprel_4byte_relax_rex:
    case X86::reloc_riprel_4byte_relax_rex2:
    case X86::reloc_riprel_4byte_relax_evex:
    case X86::reloc_branch_4byte_pcrel:
      return COFF::IMAGE_REL_AMD64_REL32;
    case FK_Data_4:
      if (PCRel)
        return COFF::IMAGE_REL_AMD64_REL32;
      [[fallthrough]];
    case X86::reloc_signed_4byte:
    case X86::reloc_signed_4byte_relax:
      if (Spec == MCSymbolRefExpr::VK_COFF_IMGREL32)
        return COFF::IMAGE_REL_AMD64_ADDR32NB;
      if (Spec == X86::S_COFF_SECREL)
        return COFF::IMAGE_REL_AMD64_SECREL;
      return COFF::IMAGE_REL_AMD64_ADDR32;
    case FK_Data_8:
      return COFF::IMAGE_REL_AMD64_ADDR64;
    case FK_SecRel_2:
      return COFF::IMAGE_REL_AMD64_SECTION;
    case FK_SecRel_4:
      return COFF::IMAGE_REL_AMD64_SECREL;
    default:
      Ctx.reportError(Fixup.getLoc(), "unsupported relocation type");
      return COFF::IMAGE_REL_AMD64_ADDR32;
    }
  } else if (getMachine() == COFF::IMAGE_FILE_MACHINE_I386) {
    switch (FixupKind) {
    case X86::reloc_riprel_4byte:
    case X86::reloc_riprel_4byte_movq_load:
      return COFF::IMAGE_REL_I386_REL32;
    case FK_Data_4:
      if (PCRel)
        return COFF::IMAGE_REL_I386_REL32;
      [[fallthrough]];
    case X86::reloc_signed_4byte:
    case X86::reloc_signed_4byte_relax:
      if (Spec == MCSymbolRefExpr::VK_COFF_IMGREL32)
        return COFF::IMAGE_REL_I386_DIR32NB;
      if (Spec == X86::S_COFF_SECREL)
        return COFF::IMAGE_REL_I386_SECREL;
      return COFF::IMAGE_REL_I386_DIR32;
    case FK_SecRel_2:
      return COFF::IMAGE_REL_I386_SECTION;
    case FK_SecRel_4:
      return COFF::IMAGE_REL_I386_SECREL;
    default:
      Ctx.reportError(Fixup.getLoc(), "unsupported relocation type");
      return COFF::IMAGE_REL_I386_DIR32;
    }
  } else
    llvm_unreachable("Unsupported COFF machine type.");
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createX86WinCOFFObjectWriter(bool Is64Bit, bool DescribeSites) {
  return std::make_unique<X86WinCOFFObjectWriter>(Is64Bit, DescribeSites);
}
