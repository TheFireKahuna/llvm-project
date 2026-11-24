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
public:
  X86WinCOFFObjectWriter(bool Is64Bit);
  ~X86WinCOFFObjectWriter() override = default;

  unsigned getRelocType(MCContext &Ctx, const MCValue &Target,
                        const MCFixup &Fixup, bool IsCrossSection,
                        const MCAsmBackend &MAB) const override;
};

} // end anonymous namespace

X86WinCOFFObjectWriter::X86WinCOFFObjectWriter(bool Is64Bit)
    : MCWinCOFFObjectTargetWriter(Is64Bit ? COFF::IMAGE_FILE_MACHINE_AMD64
                                          : COFF::IMAGE_FILE_MACHINE_I386) {}

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

  // A specifier COFF has no relocation for, such as ELF's @GOTPCREL, would
  // change what the instruction reaches if it were dropped. @PLT on a call
  // reaches the function either way.
  auto Spec = Target.getSpecifier();
  if (Spec && Spec != X86::S_PLT && Spec != X86::S_COFF_SECREL &&
      Spec != MCSymbolRefExpr::VK_COFF_IMGREL32 &&
      Spec != MCSymbolRefExpr::VK_COFF_IMGREL32_CALL &&
      !(Spec == X86::S_COFF_REL32_CALL && FixupKind == FK_Data_4) &&
      !((Spec == X86::S_COFF_FUSED || Spec == X86::S_TPOFF) && Is64Bit &&
        Ctx.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment())) {
    Ctx.reportError(Fixup.getLoc(),
                    "relocation specifier @" +
                        Ctx.getAsmInfo().getSpecifierName(Spec) +
                        " unsupported on COFF targets");
    return COFF::IMAGE_REL_AMD64_ABSOLUTE;
  }
  if (Is64Bit) {
    // The relocation types of Windows Itanium and NT-POSIX objects state the
    // form of the instruction, or that a data field's target is only called.
    bool HasSiteTypes =
        Ctx.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment();
    // A fused read is the load of a pointer that MEM_LOAD or MEM_LOAD_REX2
    // would state.
    if (Spec == X86::S_COFF_FUSED) {
      if (FixupKind == X86::reloc_riprel_4byte_movq_load)
        return COFF::IMAGE_REL_LLVM_FUSED_READ;
      if (FixupKind == X86::reloc_riprel_4byte_movq_load_rex2)
        return COFF::IMAGE_REL_LLVM_FUSED_READ_REX2;
      Ctx.reportError(Fixup.getLoc(),
                      "fused read is not a 64-bit mov of a pointer");
      return COFF::IMAGE_REL_AMD64_ABSOLUTE;
    }
    // A thread-local's offset from the thread pointer is a displacement or
    // an immediate.
    if (Spec == X86::S_TPOFF) {
      if (!PCRel &&
          (FixupKind == FK_Data_4 || FixupKind == X86::reloc_signed_4byte ||
           FixupKind == X86::reloc_signed_4byte_relax))
        return COFF::IMAGE_REL_LLVM_TPOFF32;
      Ctx.reportError(Fixup.getLoc(), "@TPOFF is not a 32-bit absolute field");
      return COFF::IMAGE_REL_AMD64_ABSOLUTE;
    }
    switch (FixupKind) {
    case X86::reloc_riprel_4byte_imm1:
      return COFF::IMAGE_REL_AMD64_REL32_1;
    case X86::reloc_riprel_4byte_imm2:
      return COFF::IMAGE_REL_AMD64_REL32_2;
    case X86::reloc_riprel_4byte_imm4:
      return COFF::IMAGE_REL_AMD64_REL32_4;
    case X86::reloc_riprel_4byte_call:
      return COFF::IMAGE_REL_LLVM_MEM_CALL;
    case X86::reloc_riprel_4byte_jump:
      return COFF::IMAGE_REL_LLVM_MEM_JUMP;
    case X86::reloc_riprel_4byte_jump_p1:
      return COFF::IMAGE_REL_LLVM_MEM_JUMP_P1;
    case X86::reloc_riprel_4byte_tail:
      return COFF::IMAGE_REL_LLVM_MEM_JUMP_TAIL;
    case X86::reloc_riprel_4byte_lea:
      // The target's constant is the offset less the field's four bytes.
      return Target.getConstant() == -4 ? COFF::IMAGE_REL_LLVM_LEA
                                        : COFF::IMAGE_REL_LLVM_LEA_OFFSET;
    case X86::reloc_jump_4byte_pcrel:
      return COFF::IMAGE_REL_LLVM_JUMP32;
    case X86::reloc_jump_1byte_pcrel:
      return COFF::IMAGE_REL_LLVM_JUMP8;
    case X86::reloc_tail_4byte_pcrel:
      return COFF::IMAGE_REL_LLVM_BRANCH_TAIL;
    case X86::reloc_riprel_4byte_movq_load:
      if (HasSiteTypes)
        return COFF::IMAGE_REL_LLVM_MEM_LOAD;
      return COFF::IMAGE_REL_AMD64_REL32;
    case X86::reloc_riprel_4byte_movq_load_rex2:
      if (HasSiteTypes)
        return COFF::IMAGE_REL_LLVM_MEM_LOAD_REX2;
      return COFF::IMAGE_REL_AMD64_REL32;
    case X86::reloc_branch_4byte_pcrel:
      if (HasSiteTypes)
        return COFF::IMAGE_REL_LLVM_BRANCH32;
      return COFF::IMAGE_REL_AMD64_REL32;
    case X86::reloc_riprel_4byte:
    case X86::reloc_riprel_4byte_relax:
    case X86::reloc_riprel_4byte_relax_rex:
    case X86::reloc_riprel_4byte_relax_rex2:
    case X86::reloc_riprel_4byte_relax_evex:
      return COFF::IMAGE_REL_AMD64_REL32;
    case FK_Data_4:
      if (PCRel || Spec == X86::S_COFF_REL32_CALL)
        return HasSiteTypes &&
                       (Spec == X86::S_PLT || Spec == X86::S_COFF_REL32_CALL)
                   ? COFF::IMAGE_REL_LLVM_REL32_CALL
                   : COFF::IMAGE_REL_AMD64_REL32;
      [[fallthrough]];
    case X86::reloc_signed_4byte:
    case X86::reloc_signed_4byte_relax:
      if (Spec == MCSymbolRefExpr::VK_COFF_IMGREL32_CALL)
        return HasSiteTypes ? COFF::IMAGE_REL_LLVM_ADDR32NB_CALL
                            : COFF::IMAGE_REL_AMD64_ADDR32NB;
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
      if (PCRel || Spec == X86::S_COFF_REL32_CALL)
        return COFF::IMAGE_REL_I386_REL32;
      [[fallthrough]];
    case X86::reloc_signed_4byte:
    case X86::reloc_signed_4byte_relax:
      if (Spec == MCSymbolRefExpr::VK_COFF_IMGREL32 ||
          Spec == MCSymbolRefExpr::VK_COFF_IMGREL32_CALL)
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
llvm::createX86WinCOFFObjectWriter(bool Is64Bit) {
  return std::make_unique<X86WinCOFFObjectWriter>(Is64Bit);
}
