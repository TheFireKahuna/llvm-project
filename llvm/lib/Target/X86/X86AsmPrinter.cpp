//===-- X86AsmPrinter.cpp - Convert X86 LLVM code to AT&T assembly --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to X86 machine code.
//
//===----------------------------------------------------------------------===//

#include "X86AsmPrinter.h"
#include "MCTargetDesc/X86ATTInstPrinter.h"
#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "MCTargetDesc/X86TargetStreamer.h"
#include "TargetInfo/X86TargetInfo.h"
#include "X86.h"
#include "X86InstrInfo.h"
#include "X86MachineFunctionInfo.h"
#include "X86Subtarget.h"
#include "llvm-c/Visibility.h"
#include "llvm/Analysis/StaticDataProfileInfo.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGenTypes/MachineValueType.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/KCFIHash.h"

using namespace llvm;

X86AsmPrinter::X86AsmPrinter(TargetMachine &TM,
                             std::unique_ptr<MCStreamer> Streamer)
    : AsmPrinter(TM, std::move(Streamer), ID), FM(*this) {
  GetPSI = [this](Module &M) -> ProfileSummaryInfo * {
    if (auto *PSIW = getAnalysisIfAvailable<ProfileSummaryInfoWrapperPass>())
      return &PSIW->getPSI();
    return nullptr;
  };
  GetSDPI = [this](Module &M) -> StaticDataProfileInfo * {
    if (auto *SDPIW =
            getAnalysisIfAvailable<StaticDataProfileInfoWrapperPass>())
      return &SDPIW->getStaticDataProfileInfo();
    return nullptr;
  };
}

//===----------------------------------------------------------------------===//
// Primitive Helper Functions.
//===----------------------------------------------------------------------===//

/// runOnMachineFunction - Emit the function body.
///
bool X86AsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  PSI = GetPSI(*MF.getFunction().getParent());
  SDPI = GetSDPI(*MF.getFunction().getParent());

  Subtarget = &MF.getSubtarget<X86Subtarget>();

  SMShadowTracker.startFunction(MF);
  CodeEmitter.reset(TM.getTarget().createMCCodeEmitter(
      *Subtarget->getInstrInfo(), MF.getContext()));

  const Module *M = MF.getFunction().getParent();
  EmitFPOData = Subtarget->isTargetWin32() && M->getCodeViewFlag();

  IndCSPrefix = M->getModuleFlag("indirect_branch_cs_prefix");

  SetupMachineFunction(MF);

  if (Subtarget->isTargetCOFF()) {
    bool Local = MF.getFunction().hasLocalLinkage();
    OutStreamer->beginCOFFSymbolDef(CurrentFnSym);
    OutStreamer->emitCOFFSymbolStorageClass(
        Local ? COFF::IMAGE_SYM_CLASS_STATIC : COFF::IMAGE_SYM_CLASS_EXTERNAL);
    OutStreamer->emitCOFFSymbolType(COFF::IMAGE_SYM_DTYPE_FUNCTION
                                    << COFF::SCT_COMPLEX_TYPE_SHIFT);
    OutStreamer->endCOFFSymbolDef();
  }

  // Emit the rest of the function body.
  emitFunctionBody();

  // Emit the XRay table for this function.
  emitXRayTable();

  EmitFPOData = false;

  IndCSPrefix = false;

  // We didn't modify anything.
  return false;
}

void X86AsmPrinter::emitFunctionBodyStart() {
  if (EmitFPOData) {
    auto *XTS =
        static_cast<X86TargetStreamer *>(OutStreamer->getTargetStreamer());
    XTS->emitFPOProc(
        CurrentFnSym,
        MF->getInfo<X86MachineFunctionInfo>()->getArgumentStackSize());
  }
}

void X86AsmPrinter::emitFunctionBodyEnd() {
  if (EmitFPOData) {
    auto *XTS =
        static_cast<X86TargetStreamer *>(OutStreamer->getTargetStreamer());
    XTS->emitFPOEndProc();
  }
}

uint32_t X86AsmPrinter::MaskKCFIType(uint32_t Value) {
  // If the type hash matches an invalid pattern, mask the value.
  const uint32_t InvalidValues[] = {
      0xFA1E0FF3, /* ENDBR64 */
      0xFB1E0FF3, /* ENDBR32 */
  };
  for (uint32_t N : InvalidValues) {
    // LowerKCFI_CHECK emits -Value for indirect call checks, so we must also
    // mask that. Note that -(Value + 1) == ~Value.
    if (N == Value || -N == Value)
      return Value + 1;
  }
  return Value;
}

void X86AsmPrinter::EmitKCFITypePadding(const MachineFunction &MF,
                                        unsigned TypeBytes) {
  // Keep the function entry aligned, taking patchable-function-prefix into
  // account if set.
  int64_t PrefixBytes = MF.getFunction().getFnAttributeAsParsedInteger(
      "patchable-function-prefix");

  // Also take the type identifier into account if we're emitting
  // one. Otherwise, just pad with nops.
  PrefixBytes += TypeBytes;

  emitNops(offsetToAlignment(PrefixBytes, MF.getPreferredAlignment()));
}

/// emitKCFITypeId - Emit the KCFI type information in architecture specific
/// format.
void X86AsmPrinter::emitKCFITypeId(const MachineFunction &MF) {
  const Function &F = MF.getFunction();
  // A module with a marker gives every typed function a prefix, whether or
  // not it checks its own indirect calls.
  const ConstantInt *Marker = mdconst::extract_or_null<ConstantInt>(
      F.getParent()->getModuleFlag("kcfi-marker"));
  if (!F.getParent()->getModuleFlag("kcfi") && !Marker)
    return;

  ConstantInt *Type = nullptr;
  if (const MDNode *MD = F.getMetadata(LLVMContext::MD_kcfi_type))
    Type = mdconst::extract<ConstantInt>(MD->getOperand(0));

  // If we don't have a type to emit, just emit padding if needed to maintain
  // the same alignment for all functions.
  if (!Type) {
    EmitKCFITypePadding(MF, /*TypeBytes=*/0);
    return;
  }

  // Emit a function symbol for the type data to avoid unreachable instruction
  // warnings from binary validation tools, and use the same linkage as the
  // parent function. Note that using local linkage would result in duplicate
  // symbols for weak parent functions. On COFF, however, a linker treats an
  // external symbol that only a discarded COMDAT copy defines as undefined, so
  // keep the symbol local there, which lets a copy without type data prevail.
  MCSymbol *FnSym = OutContext.getOrCreateSymbol("__cfi_" + MF.getName());
  if (!TM.getTargetTriple().isOSBinFormatCOFF())
    emitLinkage(&MF.getFunction(), FnSym);
  if (MAI.hasDotTypeDotSizeDirective())
    OutStreamer->emitSymbolAttribute(FnSym, MCSA_ELF_TypeFunction);
  if (!Marker)
    OutStreamer->emitLabel(FnSym);

  // Embed the type hash in the X86::MOV32ri instruction to avoid special
  // casing object file parsers. The instruction is 5 bytes long.
  unsigned TypeBytes = 5;
  // The marker is the displacement of a 7-byte nopl, so that the 8 bytes
  // before the hash are a fixed pattern: 0F 1F 80, the marker, and the B8 of
  // the move. A second type the function carries, which a call through a
  // member function pointer checks, precedes it.
  ConstantInt *VfnType = nullptr;
  if (Marker) {
    TypeBytes += 7;
    if (const MDNode *MD = F.getMetadata("kcfi_vfn_type")) {
      VfnType = mdconst::extract<ConstantInt>(MD->getOperand(0));
      TypeBytes += 4;
    }
  }
  EmitKCFITypePadding(MF, TypeBytes);
  // With a marker, the symbol follows the padding and marks the first type
  // word, as on targets that emit the type as data, so that the layout after
  // it tells a linker whether the prefix has a second type.
  if (Marker)
    OutStreamer->emitLabel(FnSym);
  if (VfnType)
    OutStreamer->emitInt32(VfnType->getZExtValue());
  if (Marker) {
    MCInst Nop = MCInstBuilder(X86::NOOPL)
                     .addReg(X86::RAX)
                     .addImm(1)
                     .addReg(X86::NoRegister)
                     .addImm(static_cast<int32_t>(Marker->getZExtValue()))
                     .addReg(X86::NoRegister);
    Nop.setFlags(X86::IP_USE_DISP32);
    EmitAndCountInstruction(Nop);
  }
  unsigned DestReg = X86::EAX;

  if (F.getParent()->getModuleFlag("kcfi-arity")) {
    // The ArityToRegMap assumes the 64-bit SysV ABI.
    [[maybe_unused]] const auto &Triple = MF.getTarget().getTargetTriple();
    assert(Triple.isX86_64() && !Triple.isOSWindows());

    // Determine the function's arity (i.e., the number of arguments) at the ABI
    // level by counting the number of parameters that are passed
    // as registers, such as pointers and 64-bit (or smaller) integers. The
    // Linux x86-64 ABI allows up to 6 integer parameters to be passed in GPRs.
    // Additional parameters or parameters larger than 64 bits may be passed on
    // the stack, in which case the arity is denoted as 7. Floating-point
    // arguments passed in XMM0-XMM7 are not counted toward arity because
    // floating-point values are not relevant to enforcing kCFI at this time.
    const unsigned ArityToRegMap[8] = {X86::EAX, X86::ECX, X86::EDX, X86::EBX,
                                       X86::ESP, X86::EBP, X86::ESI, X86::EDI};
    int Arity;
    if (MF.getInfo<X86MachineFunctionInfo>()->getArgumentStackSize() > 0) {
      Arity = 7;
    } else {
      Arity = 0;
      for (const auto &LI : MF.getRegInfo().liveins()) {
        auto Reg = LI.first;
        if (X86::GR8RegClass.contains(Reg) || X86::GR16RegClass.contains(Reg) ||
            X86::GR32RegClass.contains(Reg) ||
            X86::GR64RegClass.contains(Reg)) {
          ++Arity;
        }
      }
    }
    DestReg = ArityToRegMap[Arity];
  }

  EmitAndCountInstruction(MCInstBuilder(X86::MOV32ri)
                              .addReg(DestReg)
                              .addImm(MaskKCFIType(Type->getZExtValue())));

  if (MAI.hasDotTypeDotSizeDirective()) {
    MCSymbol *EndSym = OutContext.createTempSymbol("cfi_func_end");
    OutStreamer->emitLabel(EndSym);

    const MCExpr *SizeExp = MCBinaryExpr::createSub(
        MCSymbolRefExpr::create(EndSym, OutContext),
        MCSymbolRefExpr::create(FnSym, OutContext), OutContext);
    OutStreamer->emitELFSize(FnSym, SizeExp);
  }
}

/// emitKCFIThunks - Emit the per-type thunks that the CFGuard pass routes
/// indirect calls with a KCFI type through when the prefixes carry a marker.
/// Each is a COMDAT, kept once per image, that compares the 8 bytes before the
/// target, the end of the marker, the opcode byte and the type, with the
/// call's, so that a type that occurs in code by chance does not pass. On a
/// mismatch it continues into the type's mismatch routine, a weak alias whose
/// default, shared by every type, fails fast if the target carries the marker,
/// since the target is then a function of another type, and continues into
/// the guard function otherwise, since the target was built without KCFI.
///
/// A thunk tests the target against [__llvm_code_start, __llvm_code_end)
/// first. A matching target inside it is taken directly, and a matching
/// target outside it continues into the guard function the image defines. A
/// linker defines the range only for an image in which every function that no
/// pointer may reach has had its type overwritten, so that a match inside the
/// image proves what the guard function would. Each object references the
/// bounds as weak aliases of one byte in a COMDAT, which leaves the range
/// empty otherwise. A target outside the range is compared only when its
/// prefix lies on the target's own page: code at the start of an allocation,
/// such as JIT code, may follow an unmapped page, so a target in a page's
/// first bytes is treated as having no prefix and goes to the mismatch
/// routine unread. A target inside the range is in this image's mapped code,
/// so it takes no such test. A dispatch thunk takes the target in RAX and
/// jumps to it, and a check thunk takes it in RCX and returns; both may clobber
/// R10 and R11, as the guard functions do.
void X86AsmPrinter::emitKCFIThunks(Module &M) {
  const ConstantInt *Marker =
      mdconst::extract_or_null<ConstantInt>(M.getModuleFlag("kcfi-marker"));
  if (!Marker)
    return;
  int64_t PrefixNops = 0;
  if (auto *MD =
          mdconst::extract_or_null<ConstantInt>(M.getModuleFlag("kcfi-offset")))
    PrefixNops = MD->getZExtValue();

  const MCSubtargetInfo &STI = TM.getMCSubtargetInfo();
  auto EmitFunctionStart = [&](MCSymbol *Sym) {
    OutStreamer->switchSection(OutContext.getCOFFSection(
        ".text",
        COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE |
            COFF::IMAGE_SCN_MEM_READ | COFF::IMAGE_SCN_LNK_COMDAT,
        Sym->getName(), COFF::IMAGE_COMDAT_SELECT_ANY));
    OutStreamer->beginCOFFSymbolDef(Sym);
    OutStreamer->emitCOFFSymbolStorageClass(COFF::IMAGE_SYM_CLASS_EXTERNAL);
    OutStreamer->emitCOFFSymbolType(COFF::IMAGE_SYM_DTYPE_FUNCTION
                                    << COFF::SCT_COMPLEX_TYPE_SHIFT);
    OutStreamer->endCOFFSymbolDef();
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Global);
    OutStreamer->emitCodeAlignment(Align(16), STI);
    OutStreamer->emitLabel(Sym);
  };
  auto EmitGuardJump = [&](StringRef GuardFn) {
    OutStreamer->emitInstruction(
        MCInstBuilder(X86::JMP64m)
            .addReg(X86::RIP)
            .addImm(1)
            .addReg(X86::NoRegister)
            .addExpr(MCSymbolRefExpr::create(
                OutContext.getOrCreateSymbol(GuardFn), OutContext))
            .addReg(X86::NoRegister),
        STI);
  };

  auto EmitLea = [&](unsigned Reg, MCSymbol *Sym) {
    OutStreamer->emitInstruction(
        MCInstBuilder(X86::LEA64r)
            .addReg(Reg)
            .addReg(X86::RIP)
            .addImm(1)
            .addReg(X86::NoRegister)
            .addExpr(MCSymbolRefExpr::create(Sym, OutContext))
            .addReg(X86::NoRegister),
        STI);
  };
  auto EmitCmp = [&](unsigned LHS, unsigned RHS) {
    OutStreamer->emitInstruction(
        MCInstBuilder(X86::CMP64rr).addReg(LHS).addReg(RHS), STI);
  };
  auto EmitJcc = [&](MCSymbol *Target, X86::CondCode Cond) {
    OutStreamer->emitInstruction(
        MCInstBuilder(X86::JCC_1)
            .addExpr(MCSymbolRefExpr::create(Target, OutContext))
            .addImm(Cond),
        STI);
  };
  // Makes both range bounds weak aliases of __llvm_code_empty, a byte of
  // read-only data in a COMDAT, so that an image whose linker does not define
  // them has an empty range. The default is defined in a section, so that
  // references stay against the bounds, which a linker can then define; those
  // to an alias of an undefined symbol are relocated against that symbol.
  auto EmitCodeRangeDefault = [&](MCSymbol *Start, MCSymbol *End) {
    MCSymbol *Empty = OutContext.getOrCreateSymbol("__llvm_code_empty");
    for (MCSymbol *Sym : {Start, End}) {
      OutStreamer->emitSymbolAttribute(Sym, MCSA_Weak);
      OutStreamer->emitAssignment(Sym,
                                  MCSymbolRefExpr::create(Empty, OutContext));
    }
    OutStreamer->switchSection(OutContext.getCOFFSection(
        ".rdata",
        COFF::IMAGE_SCN_CNT_INITIALIZED_DATA | COFF::IMAGE_SCN_MEM_READ |
            COFF::IMAGE_SCN_LNK_COMDAT,
        Empty->getName(), COFF::IMAGE_COMDAT_SELECT_ANY));
    OutStreamer->emitSymbolAttribute(Empty, MCSA_Global);
    OutStreamer->emitLabel(Empty);
    OutStreamer->emitIntValue(0, 1);
  };
  // The bits of a target's page offset that are zero when the prefix read
  // before it, at most PrefixNops + 12 bytes, could start on the page before.
  uint32_t PageTestMask = 0xFFF & ~(PowerOf2Ceil(PrefixNops + 12) - 1);
  // testl $mask, %reg32; jz target
  auto EmitPageTest = [&](unsigned Reg, MCSymbol *Target) {
    if (Reg == X86::RAX)
      OutStreamer->emitInstruction(
          MCInstBuilder(X86::TEST32i32).addImm(PageTestMask), STI);
    else
      OutStreamer->emitInstruction(MCInstBuilder(X86::TEST32ri)
                                       .addReg(getX86SubSuperRegister(Reg, 32))
                                       .addImm(PageTestMask),
                                   STI);
    EmitJcc(Target, X86::COND_E);
  };
  // movl $FAST_FAIL_GUARD_ICALL_CHECK_FAILURE_XFG, %ecx; int $0x29
  auto EmitFastFail = [&] {
    OutStreamer->emitInstruction(
        MCInstBuilder(X86::MOV32ri).addReg(X86::ECX).addImm(64), STI);
    OutStreamer->emitInstruction(MCInstBuilder(X86::INT).addImm(0x29), STI);
  };

  // A local thunk serves a type salted by a class with internal linkage,
  // whose functions are all in this image: a matching target outside the
  // range fails fast, unless the range is empty because the image was not
  // sealed.
  struct ThunkKind {
    StringRef Prefix;
    StringRef MismatchPrefix;
    StringRef Open;
    StringRef GuardFn;
    unsigned TargetReg;
    bool Local;
  };
  const ThunkKind Kinds[] = {
      {"__llvm_kcfi_dispatch_", "__llvm_kcfi_mismatch_", "__llvm_kcfi_open",
       "__guard_dispatch_icall_fptr", X86::RAX, false},
      {"__llvm_kcfi_check_", "__llvm_kcfi_check_mismatch_",
       "__llvm_kcfi_check_open", "__guard_check_icall_fptr", X86::RCX, false},
      {"__llvm_kcfi_local_dispatch_", "__llvm_kcfi_mismatch_",
       "__llvm_kcfi_open", "__guard_dispatch_icall_fptr", X86::RAX, true},
      {"__llvm_kcfi_local_check_", "__llvm_kcfi_check_mismatch_",
       "__llvm_kcfi_check_open", "__guard_check_icall_fptr", X86::RCX, true}};
  MCSymbol *CodeStart = nullptr;
  MCSymbol *CodeEnd = nullptr;
  SmallPtrSet<MCSymbol *, 2> EmittedOpens;
  for (const ThunkKind &Kind : Kinds) {
    MCSymbol *Open = nullptr;
    for (const Function &F : M) {
      StringRef TypeName = F.getName();
      uint32_t Type;
      if (!F.isDeclaration() || F.use_empty() ||
          !TypeName.consume_front(Kind.Prefix) || TypeName.size() != 8 ||
          TypeName.getAsInteger(16, Type))
        continue;
      if (!Open)
        Open = OutContext.getOrCreateSymbol(Kind.Open);
      if (!CodeStart) {
        CodeStart = OutContext.getOrCreateSymbol("__llvm_code_start");
        CodeEnd = OutContext.getOrCreateSymbol("__llvm_code_end");
        EmitCodeRangeDefault(CodeStart, CodeEnd);
      }

      // A type's local and ordinary thunks share its mismatch routine.
      MCSymbol *Mismatch =
          OutContext.getOrCreateSymbol(Kind.MismatchPrefix + TypeName);
      if (!Mismatch->isVariable()) {
        OutStreamer->emitSymbolAttribute(Mismatch, MCSA_Weak);
        OutStreamer->emitAssignment(Mismatch,
                                    MCSymbolRefExpr::create(Open, OutContext));
      }

      // leaq __llvm_code_start(%rip), %r10; cmpq %r10, %reg; jb 1f
      // leaq __llvm_code_end(%rip), %r10; cmpq %r10, %reg; jae 1f
      // movabsq $expected, %r11; cmpq %r11, -8(%reg); jne mismatch
      // jmpq *%reg (dispatch) or retq (check)
      // 1: testl $mask, %reg32; jz mismatch
      // movabsq $expected, %r11; cmpq %r11, -8(%reg); jne mismatch
      // jmpq *guard(%rip)
      //
      // A local thunk loads both bounds first, into R10 and R11, and at 1:
      // cmpq %r11, %r10; jne 2f before the page test, where 2: fails fast.
      uint64_t Expected = getKCFIMarkerPattern(Marker->getZExtValue()) >> 32 |
                          uint64_t(MaskKCFIType(Type)) << 32;
      auto EmitCompare = [&] {
        OutStreamer->emitInstruction(
            MCInstBuilder(X86::MOV64ri).addReg(X86::R11).addImm(Expected), STI);
        OutStreamer->emitInstruction(MCInstBuilder(X86::CMP64mr)
                                         .addReg(Kind.TargetReg)
                                         .addImm(1)
                                         .addReg(X86::NoRegister)
                                         .addImm(-(PrefixNops + 8))
                                         .addReg(X86::NoRegister)
                                         .addReg(X86::R11),
                                     STI);
        EmitJcc(Mismatch, X86::COND_NE);
      };
      EmitFunctionStart(getSymbol(&F));
      MCSymbol *Outside = OutContext.createTempSymbol();
      if (Kind.Local) {
        EmitLea(X86::R10, CodeStart);
        EmitLea(X86::R11, CodeEnd);
        EmitCmp(Kind.TargetReg, X86::R10);
        EmitJcc(Outside, X86::COND_B);
        EmitCmp(Kind.TargetReg, X86::R11);
        EmitJcc(Outside, X86::COND_AE);
      } else {
        EmitLea(X86::R10, CodeStart);
        EmitCmp(Kind.TargetReg, X86::R10);
        EmitJcc(Outside, X86::COND_B);
        EmitLea(X86::R10, CodeEnd);
        EmitCmp(Kind.TargetReg, X86::R10);
        EmitJcc(Outside, X86::COND_AE);
      }
      EmitCompare();
      if (Kind.TargetReg == X86::RAX)
        OutStreamer->emitInstruction(
            MCInstBuilder(X86::JMP64r).addReg(X86::RAX), STI);
      else
        OutStreamer->emitInstruction(MCInstBuilder(X86::RET64), STI);
      OutStreamer->emitLabel(Outside);
      MCSymbol *Trap = nullptr;
      if (Kind.Local) {
        // The bounds are equal in an image that was not sealed, where no
        // target is in the range and the guard function decides.
        Trap = OutContext.createTempSymbol();
        EmitCmp(X86::R10, X86::R11);
        EmitJcc(Trap, X86::COND_NE);
      }
      EmitPageTest(Kind.TargetReg, Mismatch);
      EmitCompare();
      EmitGuardJump(Kind.GuardFn);
      if (Trap) {
        OutStreamer->emitLabel(Trap);
        EmitFastFail();
      }
    }
    if (!Open || !EmittedOpens.insert(Open).second)
      continue;

    // testl $mask, %reg32; jz 1f
    // movabsq $pattern, %r11; cmpq %r11, -12(%reg); je 2f
    // 1: jmpq *guard(%rip)
    // 2: movl $FAST_FAIL_GUARD_ICALL_CHECK_FAILURE_XFG, %ecx; int $0x29
    //
    // A target in a page's first bytes has no prefix that can be read, so it
    // is foreign.
    uint64_t Pattern = getKCFIMarkerPattern(Marker->getZExtValue());
    EmitFunctionStart(Open);
    MCSymbol *Foreign = OutContext.createTempSymbol();
    EmitPageTest(Kind.TargetReg, Foreign);
    OutStreamer->emitInstruction(
        MCInstBuilder(X86::MOV64ri).addReg(X86::R11).addImm(Pattern), STI);
    OutStreamer->emitInstruction(MCInstBuilder(X86::CMP64mr)
                                     .addReg(Kind.TargetReg)
                                     .addImm(1)
                                     .addReg(X86::NoRegister)
                                     .addImm(-(PrefixNops + 12))
                                     .addReg(X86::NoRegister)
                                     .addReg(X86::R11),
                                 STI);
    MCSymbol *Trap = OutContext.createTempSymbol();
    OutStreamer->emitInstruction(
        MCInstBuilder(X86::JCC_1)
            .addExpr(MCSymbolRefExpr::create(Trap, OutContext))
            .addImm(X86::COND_E),
        STI);
    OutStreamer->emitLabel(Foreign);
    EmitGuardJump(Kind.GuardFn);
    OutStreamer->emitLabel(Trap);
    EmitFastFail();
  }
}

/// PrintSymbolOperand - Print a raw symbol reference operand.  This handles
/// jump tables, constant pools, global address and external symbols, all of
/// which print to a label with various suffixes for relocation types etc.
void X86AsmPrinter::PrintSymbolOperand(const MachineOperand &MO,
                                       raw_ostream &O) {
  switch (MO.getType()) {
  default: llvm_unreachable("unknown symbol type!");
  case MachineOperand::MO_ConstantPoolIndex:
    GetCPISymbol(MO.getIndex())->print(O, MAI);
    printOffset(MO.getOffset(), O);
    break;
  case MachineOperand::MO_GlobalAddress: {
    const GlobalValue *GV = MO.getGlobal();

    MCSymbol *GVSym;
    if (MO.getTargetFlags() == X86II::MO_DARWIN_NONLAZY ||
        MO.getTargetFlags() == X86II::MO_DARWIN_NONLAZY_PIC_BASE)
      GVSym = getSymbolWithGlobalValueBase(GV, "$non_lazy_ptr");
    else
      GVSym = getSymbolPreferLocal(*GV);

    // Handle dllimport linkage.
    if (MO.getTargetFlags() == X86II::MO_DLLIMPORT)
      GVSym = OutContext.getOrCreateSymbol(Twine("__imp_") + GVSym->getName());
    else if (MO.getTargetFlags() == X86II::MO_COFFSTUB)
      GVSym =
          OutContext.getOrCreateSymbol(Twine(".refptr.") + GVSym->getName());

    if (MO.getTargetFlags() == X86II::MO_DARWIN_NONLAZY ||
        MO.getTargetFlags() == X86II::MO_DARWIN_NONLAZY_PIC_BASE) {
      MCSymbol *Sym = getSymbolWithGlobalValueBase(GV, "$non_lazy_ptr");
      MachineModuleInfoImpl::StubValueTy &StubSym =
          MMI->getObjFileInfo<MachineModuleInfoMachO>().getGVStubEntry(Sym);
      if (!StubSym.getPointer())
        StubSym = MachineModuleInfoImpl::StubValueTy(getSymbol(GV),
                                                     !GV->hasInternalLinkage());
    }

    // If the name begins with a dollar-sign, enclose it in parens.  We do this
    // to avoid having it look like an integer immediate to the assembler.
    if (GVSym->getName()[0] != '$')
      GVSym->print(O, MAI);
    else {
      O << '(';
      GVSym->print(O, MAI);
      O << ')';
    }
    printOffset(MO.getOffset(), O);
    break;
  }
  }

  switch (MO.getTargetFlags()) {
  default:
    llvm_unreachable("Unknown target flag on GV operand");
  case X86II::MO_NO_FLAG:    // No flag.
    break;
  case X86II::MO_DARWIN_NONLAZY:
  case X86II::MO_DLLIMPORT:
  case X86II::MO_COFFSTUB:
    // These affect the name of the symbol, not any suffix.
    break;
  case X86II::MO_GOT_ABSOLUTE_ADDRESS:
    O << " + [.-";
    MF->getPICBaseSymbol()->print(O, MAI);
    O << ']';
    break;
  case X86II::MO_PIC_BASE_OFFSET:
  case X86II::MO_DARWIN_NONLAZY_PIC_BASE:
    O << '-';
    MF->getPICBaseSymbol()->print(O, MAI);
    break;
  case X86II::MO_TLSGD:     O << "@TLSGD";     break;
  case X86II::MO_TLSLD:     O << "@TLSLD";     break;
  case X86II::MO_TLSLDM:    O << "@TLSLDM";    break;
  case X86II::MO_GOTTPOFF:  O << "@GOTTPOFF";  break;
  case X86II::MO_INDNTPOFF: O << "@INDNTPOFF"; break;
  case X86II::MO_TPOFF:     O << "@TPOFF";     break;
  case X86II::MO_DTPOFF:    O << "@DTPOFF";    break;
  case X86II::MO_NTPOFF:    O << "@NTPOFF";    break;
  case X86II::MO_GOTNTPOFF: O << "@GOTNTPOFF"; break;
  case X86II::MO_GOTPCREL:  O << "@GOTPCREL";  break;
  case X86II::MO_GOTPCREL_NORELAX: O << "@GOTPCREL_NORELAX"; break;
  case X86II::MO_GOT:       O << "@GOT";       break;
  case X86II::MO_GOTOFF:    O << "@GOTOFF";    break;
  case X86II::MO_PLT:       O << "@PLT";       break;
  case X86II::MO_TLVP:      O << "@TLVP";      break;
  case X86II::MO_TLVP_PIC_BASE:
    O << "@TLVP" << '-';
    MF->getPICBaseSymbol()->print(O, MAI);
    break;
  case X86II::MO_SECREL:    O << "@SECREL32";  break;
  }
}

void X86AsmPrinter::PrintOperand(const MachineInstr *MI, unsigned OpNo,
                                 raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNo);
  const bool IsATT = MI->getInlineAsmDialect() == InlineAsm::AD_ATT;
  switch (MO.getType()) {
  default: llvm_unreachable("unknown operand type!");
  case MachineOperand::MO_Register: {
    if (IsATT)
      O << '%';
    O << X86ATTInstPrinter::getRegisterName(MO.getReg());
    return;
  }

  case MachineOperand::MO_Immediate:
    if (IsATT)
      O << '$';
    O << MO.getImm();
    return;

  case MachineOperand::MO_ConstantPoolIndex:
  case MachineOperand::MO_GlobalAddress: {
    switch (MI->getInlineAsmDialect()) {
    case InlineAsm::AD_ATT:
      O << '$';
      break;
    case InlineAsm::AD_Intel:
      O << "offset ";
      break;
    }
    PrintSymbolOperand(MO, O);
    break;
  }
  case MachineOperand::MO_BlockAddress: {
    MCSymbol *Sym = GetBlockAddressSymbol(MO.getBlockAddress());
    Sym->print(O, MAI);
    break;
  }
  }
}

/// PrintModifiedOperand - Print subregisters based on supplied modifier,
/// deferring to PrintOperand() if no modifier was supplied or if operand is not
/// a register.
void X86AsmPrinter::PrintModifiedOperand(const MachineInstr *MI, unsigned OpNo,
                                         raw_ostream &O, StringRef Modifier) {
  const MachineOperand &MO = MI->getOperand(OpNo);
  if (Modifier.empty() || !MO.isReg())
    return PrintOperand(MI, OpNo, O);
  if (MI->getInlineAsmDialect() == InlineAsm::AD_ATT)
    O << '%';
  Register Reg = MO.getReg();
  if (Modifier.consume_front("subreg")) {
    unsigned Size = (Modifier == "64")   ? 64
                    : (Modifier == "32") ? 32
                    : (Modifier == "16") ? 16
                                         : 8;
    Reg = getX86SubSuperRegister(Reg, Size);
  }
  O << X86ATTInstPrinter::getRegisterName(Reg);
}

/// PrintPCRelImm - This is used to print an immediate value that ends up
/// being encoded as a pc-relative value.  These print slightly differently, for
/// example, a $ is not emitted.
void X86AsmPrinter::PrintPCRelImm(const MachineInstr *MI, unsigned OpNo,
                                  raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  default: llvm_unreachable("Unknown pcrel immediate operand");
  case MachineOperand::MO_Register:
    // pc-relativeness was handled when computing the value in the reg.
    PrintOperand(MI, OpNo, O);
    return;
  case MachineOperand::MO_Immediate:
    O << MO.getImm();
    return;
  case MachineOperand::MO_GlobalAddress:
    PrintSymbolOperand(MO, O);
    return;
  }
}

void X86AsmPrinter::PrintLeaMemReference(const MachineInstr *MI, unsigned OpNo,
                                         raw_ostream &O, StringRef Modifier) {
  const MachineOperand &BaseReg = MI->getOperand(OpNo + X86::AddrBaseReg);
  const MachineOperand &IndexReg = MI->getOperand(OpNo + X86::AddrIndexReg);
  const MachineOperand &DispSpec = MI->getOperand(OpNo + X86::AddrDisp);

  // If we really don't want to print out (rip), don't.
  bool HasBaseReg = BaseReg.getReg() != 0;
  if (HasBaseReg && Modifier == "no-rip" && BaseReg.getReg() == X86::RIP)
    HasBaseReg = false;

  // If we really just want to print out displacement.
  if ((DispSpec.isGlobal() || DispSpec.isSymbol()) && Modifier == "disp-only")
    HasBaseReg = false;

  // HasParenPart - True if we will print out the () part of the mem ref.
  bool HasParenPart = IndexReg.getReg() || HasBaseReg;

  switch (DispSpec.getType()) {
  default:
    llvm_unreachable("unknown operand type!");
  case MachineOperand::MO_Immediate: {
    int DispVal = DispSpec.getImm();
    if (DispVal || !HasParenPart)
      O << DispVal;
    break;
  }
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_ConstantPoolIndex:
    PrintSymbolOperand(DispSpec, O);
    break;
  }

  if (Modifier == "H")
    O << "+8";

  if (HasParenPart) {
    assert(IndexReg.getReg() != X86::ESP &&
           "X86 doesn't allow scaling by ESP");

    O << '(';
    if (HasBaseReg)
      PrintModifiedOperand(MI, OpNo + X86::AddrBaseReg, O, Modifier);

    if (IndexReg.getReg()) {
      O << ',';
      PrintModifiedOperand(MI, OpNo + X86::AddrIndexReg, O, Modifier);
      unsigned ScaleVal = MI->getOperand(OpNo + X86::AddrScaleAmt).getImm();
      if (ScaleVal != 1)
        O << ',' << ScaleVal;
    }
    O << ')';
  }
}

static bool isSimpleReturn(const MachineInstr &MI) {
  // We exclude all tail calls here which set both isReturn and isCall.
  return MI.getDesc().isReturn() && !MI.getDesc().isCall();
}

static bool isIndirectBranchOrTailCall(const MachineInstr &MI) {
  unsigned Opc = MI.getOpcode();
  return MI.getDesc().isIndirectBranch() /*Make below code in a good shape*/ ||
         Opc == X86::TAILJMPr || Opc == X86::TAILJMPm ||
         Opc == X86::TAILJMPr64 || Opc == X86::TAILJMPm64 ||
         Opc == X86::TCRETURNri || Opc == X86::TCRETURN_WIN64ri ||
         Opc == X86::TCRETURN_HIPE32ri || Opc == X86::TCRETURNmi ||
         Opc == X86::TCRETURN_WINmi64 || Opc == X86::TCRETURNri64 ||
         Opc == X86::TCRETURNmi64 || Opc == X86::TCRETURNri64_ImpCall ||
         Opc == X86::TAILJMPr64_REX || Opc == X86::TAILJMPm64_REX;
}

void X86AsmPrinter::emitBasicBlockEnd(const MachineBasicBlock &MBB) {
  if (Subtarget->hardenSlsRet() || Subtarget->hardenSlsIJmp()) {
    auto I = MBB.getLastNonDebugInstr();
    if (I != MBB.end()) {
      if ((Subtarget->hardenSlsRet() && isSimpleReturn(*I)) ||
          (Subtarget->hardenSlsIJmp() && isIndirectBranchOrTailCall(*I))) {
        MCInst TmpInst;
        TmpInst.setOpcode(X86::INT3);
        EmitToStreamer(*OutStreamer, TmpInst);
      }
    }
  }
  if (SplitChainedAtEndOfBlock) {
    OutStreamer->emitWinCFISplitChained();
    // Splitting into a new unwind info implicitly starts a prolog. We have no
    // instructions to add to the prolog, so immediately end it.
    OutStreamer->emitWinCFIEndProlog();
    SplitChainedAtEndOfBlock = false;
  }
  AsmPrinter::emitBasicBlockEnd(MBB);
  SMShadowTracker.emitShadowPadding(*OutStreamer, getSubtargetInfo());
}

void X86AsmPrinter::PrintMemReference(const MachineInstr *MI, unsigned OpNo,
                                      raw_ostream &O, StringRef Modifier) {
  assert(isMem(*MI, OpNo) && "Invalid memory reference!");
  const MachineOperand &Segment = MI->getOperand(OpNo + X86::AddrSegmentReg);
  if (Segment.getReg()) {
    PrintModifiedOperand(MI, OpNo + X86::AddrSegmentReg, O, Modifier);
    O << ':';
  }
  PrintLeaMemReference(MI, OpNo, O, Modifier);
}

void X86AsmPrinter::PrintIntelMemReference(const MachineInstr *MI,
                                           unsigned OpNo, raw_ostream &O,
                                           StringRef Modifier) {
  const MachineOperand &BaseReg = MI->getOperand(OpNo + X86::AddrBaseReg);
  unsigned ScaleVal = MI->getOperand(OpNo + X86::AddrScaleAmt).getImm();
  const MachineOperand &IndexReg = MI->getOperand(OpNo + X86::AddrIndexReg);
  const MachineOperand &DispSpec = MI->getOperand(OpNo + X86::AddrDisp);
  const MachineOperand &SegReg = MI->getOperand(OpNo + X86::AddrSegmentReg);

  // If we really don't want to print out (rip), don't.
  bool HasBaseReg = BaseReg.getReg() != 0;
  if (HasBaseReg && Modifier == "no-rip" && BaseReg.getReg() == X86::RIP)
    HasBaseReg = false;

  // If we really just want to print out displacement.
  if ((DispSpec.isGlobal() || DispSpec.isSymbol()) && Modifier == "disp-only") {
    HasBaseReg = false;
  }

  // If this has a segment register, print it.
  if (SegReg.getReg()) {
    PrintOperand(MI, OpNo + X86::AddrSegmentReg, O);
    O << ':';
  }

  O << '[';

  bool NeedPlus = false;
  if (HasBaseReg) {
    PrintOperand(MI, OpNo + X86::AddrBaseReg, O);
    NeedPlus = true;
  }

  if (IndexReg.getReg()) {
    if (NeedPlus) O << " + ";
    if (ScaleVal != 1)
      O << ScaleVal << '*';
    PrintOperand(MI, OpNo + X86::AddrIndexReg, O);
    NeedPlus = true;
  }

  if (!DispSpec.isImm()) {
    if (NeedPlus) O << " + ";
    // Do not add `offset` operator. Matches the behaviour of
    // X86IntelInstPrinter::printMemReference.
    PrintSymbolOperand(DispSpec, O);
  } else {
    int64_t DispVal = DispSpec.getImm();
    if (DispVal || (!IndexReg.getReg() && !HasBaseReg)) {
      if (NeedPlus) {
        if (DispVal > 0)
          O << " + ";
        else {
          O << " - ";
          DispVal = -DispVal;
        }
      }
      O << DispVal;
    }
  }
  O << ']';
}

const MCSubtargetInfo *X86AsmPrinter::getIFuncMCSubtargetInfo() const {
  assert(Subtarget);
  return Subtarget;
}

void X86AsmPrinter::emitMachOIFuncStubBody(Module &M, const GlobalIFunc &GI,
                                           MCSymbol *LazyPointer) {
  // _ifunc:
  //   jmpq *lazy_pointer(%rip)

  OutStreamer->emitInstruction(
      MCInstBuilder(X86::JMP32m)
          .addReg(X86::RIP)
          .addImm(1)
          .addReg(0)
          .addOperand(MCOperand::createExpr(
              MCSymbolRefExpr::create(LazyPointer, OutContext)))
          .addReg(0),
      *Subtarget);
}

void X86AsmPrinter::emitMachOIFuncStubHelperBody(Module &M,
                                                 const GlobalIFunc &GI,
                                                 MCSymbol *LazyPointer) {
  // _ifunc.stub_helper:
  //   push %rax
  //   push %rdi
  //   push %rsi
  //   push %rdx
  //   push %rcx
  //   push %r8
  //   push %r9
  //   callq foo
  //   movq %rax,lazy_pointer(%rip)
  //   pop %r9
  //   pop %r8
  //   pop %rcx
  //   pop %rdx
  //   pop %rsi
  //   pop %rdi
  //   pop %rax
  //   jmpq *lazy_pointer(%rip)

  for (int Reg :
       {X86::RAX, X86::RDI, X86::RSI, X86::RDX, X86::RCX, X86::R8, X86::R9})
    OutStreamer->emitInstruction(MCInstBuilder(X86::PUSH64r).addReg(Reg),
                                 *Subtarget);

  OutStreamer->emitInstruction(
      MCInstBuilder(X86::CALL64pcrel32)
          .addOperand(MCOperand::createExpr(lowerConstant(GI.getResolver()))),
      *Subtarget);

  OutStreamer->emitInstruction(
      MCInstBuilder(X86::MOV64mr)
          .addReg(X86::RIP)
          .addImm(1)
          .addReg(0)
          .addOperand(MCOperand::createExpr(
              MCSymbolRefExpr::create(LazyPointer, OutContext)))
          .addReg(0)
          .addReg(X86::RAX),
      *Subtarget);

  for (int Reg :
       {X86::R9, X86::R8, X86::RCX, X86::RDX, X86::RSI, X86::RDI, X86::RAX})
    OutStreamer->emitInstruction(MCInstBuilder(X86::POP64r).addReg(Reg),
                                 *Subtarget);

  OutStreamer->emitInstruction(
      MCInstBuilder(X86::JMP32m)
          .addReg(X86::RIP)
          .addImm(1)
          .addReg(0)
          .addOperand(MCOperand::createExpr(
              MCSymbolRefExpr::create(LazyPointer, OutContext)))
          .addReg(0),
      *Subtarget);
}

static bool printAsmMRegister(const X86AsmPrinter &P, const MachineInstr &MI,
                              const MachineOperand &MO, char Mode,
                              raw_ostream &O) {
  Register Reg = MO.getReg();
  bool EmitPercent = MI.getInlineAsmDialect() == InlineAsm::AD_ATT;

  if (!X86::GR8RegClass.contains(Reg) &&
      !X86::GR16RegClass.contains(Reg) &&
      !X86::GR32RegClass.contains(Reg) &&
      !X86::GR64RegClass.contains(Reg))
    return true;

  switch (Mode) {
  default: return true;  // Unknown mode.
  case 'b': // Print QImode register
    Reg = getX86SubSuperRegister(Reg, 8);
    break;
  case 'h': // Print QImode high register
    Reg = getX86SubSuperRegister(Reg, 8, true);
    if (!Reg.isValid())
      return true;
    break;
  case 'w': // Print HImode register
    Reg = getX86SubSuperRegister(Reg, 16);
    break;
  case 'k': // Print SImode register
    Reg = getX86SubSuperRegister(Reg, 32);
    break;
  case 'V':
    EmitPercent = false;
    [[fallthrough]];
  case 'q':
    // Print 64-bit register names if 64-bit integer registers are available.
    // Otherwise, print 32-bit register names.
    Reg = getX86SubSuperRegister(Reg, P.getSubtarget().is64Bit() ? 64 : 32);
    break;
  }

  if (EmitPercent)
    O << '%';

  O << X86ATTInstPrinter::getRegisterName(Reg);
  return false;
}

static bool printAsmVRegister(const MachineInstr &MI, const MachineOperand &MO,
                              char Mode, raw_ostream &O) {
  Register Reg = MO.getReg();
  bool EmitPercent = MI.getInlineAsmDialect() == InlineAsm::AD_ATT;

  unsigned Index;
  if (X86::VR128XRegClass.contains(Reg))
    Index = Reg - X86::XMM0;
  else if (X86::VR256XRegClass.contains(Reg))
    Index = Reg - X86::YMM0;
  else if (X86::VR512RegClass.contains(Reg))
    Index = Reg - X86::ZMM0;
  else
    return true;

  switch (Mode) {
  default: // Unknown mode.
    return true;
  case 'x': // Print V4SFmode register
    Reg = X86::XMM0 + Index;
    break;
  case 't': // Print V8SFmode register
    Reg = X86::YMM0 + Index;
    break;
  case 'g': // Print V16SFmode register
    Reg = X86::ZMM0 + Index;
    break;
  }

  if (EmitPercent)
    O << '%';

  O << X86ATTInstPrinter::getRegisterName(Reg);
  return false;
}

/// PrintAsmOperand - Print out an operand for an inline asm expression.
///
bool X86AsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                    const char *ExtraCode, raw_ostream &O) {
  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0) return true; // Unknown modifier.

    const MachineOperand &MO = MI->getOperand(OpNo);
    const bool IsIntel = MI->getInlineAsmDialect() == InlineAsm::AD_Intel;

    switch (ExtraCode[0]) {
    default:
      // See if this is a generic print operand
      return AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, O);
    case 'a': // This is an address.  Currently only 'i' and 'r' are expected.
      switch (MO.getType()) {
      default:
        return true;
      case MachineOperand::MO_Immediate:
        O << MO.getImm();
        return false;
      case MachineOperand::MO_ConstantPoolIndex:
      case MachineOperand::MO_JumpTableIndex:
      case MachineOperand::MO_ExternalSymbol:
        llvm_unreachable("unexpected operand type!");
      case MachineOperand::MO_GlobalAddress:
        PrintSymbolOperand(MO, O);
        if (Subtarget->is64Bit())
          O << "(%rip)";
        return false;
      case MachineOperand::MO_Register:
        O << (IsIntel ? '[' : '(');
        PrintOperand(MI, OpNo, O);
        O << (IsIntel ? ']' : ')');
        return false;
      }

    case 'c': // Don't print "$" before a global var name or constant.
      switch (MO.getType()) {
      default:
        PrintOperand(MI, OpNo, O);
        break;
      case MachineOperand::MO_Immediate:
        O << MO.getImm();
        break;
      case MachineOperand::MO_ConstantPoolIndex:
      case MachineOperand::MO_JumpTableIndex:
      case MachineOperand::MO_ExternalSymbol:
        llvm_unreachable("unexpected operand type!");
      case MachineOperand::MO_GlobalAddress:
        PrintSymbolOperand(MO, O);
        break;
      }
      return false;

    case 'A': // Print '*' before a register (it must be a register)
      if (MO.isReg()) {
        if (!IsIntel)
          O << '*';
        PrintOperand(MI, OpNo, O);
        return false;
      }
      return true;

    case 'b': // Print QImode register
    case 'h': // Print QImode high register
    case 'w': // Print HImode register
    case 'k': // Print SImode register
    case 'q': // Print DImode register
    case 'V': // Print native register without '%'
      if (MO.isReg())
        return printAsmMRegister(*this, *MI, MO, ExtraCode[0], O);
      PrintOperand(MI, OpNo, O);
      return false;

    case 'x': // Print V4SFmode register
    case 't': // Print V8SFmode register
    case 'g': // Print V16SFmode register
      if (MO.isReg())
        return printAsmVRegister(*MI, MO, ExtraCode[0], O);
      PrintOperand(MI, OpNo, O);
      return false;

    case 'p': {
      const MachineOperand &MO = MI->getOperand(OpNo);
      if (MO.getType() != MachineOperand::MO_GlobalAddress)
        return true;
      PrintSymbolOperand(MO, O);
      return false;
    }

    case 'P': // This is the operand of a call, treat specially.
      PrintPCRelImm(MI, OpNo, O);
      return false;

    case 'n': // Negate the immediate or print a '-' before the operand.
      // Note: this is a temporary solution. It should be handled target
      // independently as part of the 'MC' work.
      if (MO.isImm()) {
        O << -MO.getImm();
        return false;
      }
      O << '-';
    }
  }

  PrintOperand(MI, OpNo, O);
  return false;
}

bool X86AsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                                          const char *ExtraCode,
                                          raw_ostream &O) {
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0) return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default: return true;  // Unknown modifier.
    case 'a': {
      // Print as address — only valid with 'p' constraint.
      const InlineAsm::Flag Flags(MI->getOperand(OpNo - 1).getImm());
      if (Flags.getMemoryConstraintID() != InlineAsm::ConstraintCode::p)
        return true;
      break;
    }
    case 'b': // Print QImode register
    case 'h': // Print QImode high register
    case 'w': // Print HImode register
    case 'k': // Print SImode register
    case 'q': // Print SImode register
      // These only apply to registers, ignore on mem.
      break;
    case 'H':
      if (MI->getInlineAsmDialect() == InlineAsm::AD_Intel) {
        return true;  // Unsupported modifier in Intel inline assembly.
      } else {
        PrintMemReference(MI, OpNo, O, "H");
      }
      return false;
   // Print memory only with displacement. The Modifer 'P' is used in inline
   // asm to present a call symbol or a global symbol which can not use base
   // reg or index reg.
    case 'P':
      if (MI->getInlineAsmDialect() == InlineAsm::AD_Intel) {
        PrintIntelMemReference(MI, OpNo, O, "disp-only");
      } else {
        PrintMemReference(MI, OpNo, O, "disp-only");
      }
      return false;
    }
  } else {
    // Constraint 'p' requires modifier 'a'.
    const InlineAsm::Flag Flags(MI->getOperand(OpNo - 1).getImm());
    if (Flags.getMemoryConstraintID() == InlineAsm::ConstraintCode::p)
      return true;
  }
  if (MI->getInlineAsmDialect() == InlineAsm::AD_Intel) {
    PrintIntelMemReference(MI, OpNo, O);
  } else {
    PrintMemReference(MI, OpNo, O);
  }
  return false;
}

void X86AsmPrinter::emitStartOfAsmFile(Module &M) {
  const Triple &TT = TM.getTargetTriple();

  if (TT.isOSBinFormatELF()) {
    // Assemble feature flags that may require creation of a note section.
    unsigned FeatureFlagsAnd = 0;
    if (M.getModuleFlag("cf-protection-branch"))
      FeatureFlagsAnd |= ELF::GNU_PROPERTY_X86_FEATURE_1_IBT;
    if (M.getModuleFlag("cf-protection-return"))
      FeatureFlagsAnd |= ELF::GNU_PROPERTY_X86_FEATURE_1_SHSTK;

    if (FeatureFlagsAnd) {
      // Emit a .note.gnu.property section with the flags.
      assert((TT.isX86_32() || TT.isX86_64()) &&
             "CFProtection used on invalid architecture!");
      MCSection *Cur = OutStreamer->getCurrentSectionOnly();
      MCSection *Nt = MMI->getContext().getELFSection(
          ".note.gnu.property", ELF::SHT_NOTE, ELF::SHF_ALLOC);
      OutStreamer->switchSection(Nt);

      // Emitting note header.
      const int WordSize = TT.isX86_64() && !TT.isX32() ? 8 : 4;
      emitAlignment(WordSize == 4 ? Align(4) : Align(8));
      OutStreamer->emitIntValue(4, 4 /*size*/); // data size for "GNU\0"
      OutStreamer->emitIntValue(8 + WordSize, 4 /*size*/); // Elf_Prop size
      OutStreamer->emitIntValue(ELF::NT_GNU_PROPERTY_TYPE_0, 4 /*size*/);
      OutStreamer->emitBytes(StringRef("GNU", 4)); // note name

      // Emitting an Elf_Prop for the CET properties.
      OutStreamer->emitInt32(ELF::GNU_PROPERTY_X86_FEATURE_1_AND);
      OutStreamer->emitInt32(4);                          // data size
      OutStreamer->emitInt32(FeatureFlagsAnd);            // data
      emitAlignment(WordSize == 4 ? Align(4) : Align(8)); // padding

      OutStreamer->switchSection(Cur);
    }
  }

  if (TT.isOSBinFormatMachO())
    OutStreamer->switchSection(getObjFileLowering().getTextSection());

  if (TT.isOSBinFormatCOFF()) {
    emitCOFFFeatureSymbol(M);
    emitCOFFReplaceableFunctionData(M);

    if (M.getModuleFlag("import-call-optimization"))
      EnableImportCallOptimization = true;

    // Unwind v3 is set for the entire module, not just individual functions.
    if (M.getWinX64EHUnwindMode() == WinX64EHUnwindMode::V3)
      OutStreamer->emitWinCFIUnwindVersion(3);
  }

  // TODO: Support prefixed registers for the Intel syntax.
  const bool IntelSyntax =
      MAI.getOutputAssemblerDialect() == InlineAsm::AD_Intel;
  OutStreamer->emitSyntaxDirective(IntelSyntax ? "intel" : "att",
                                   IntelSyntax ? "noprefix" : "");

  // If this is not inline asm and we're in 16-bit
  // mode prefix assembly with .code16.
  bool is16 = TT.getEnvironment() == Triple::CODE16;
  if (M.getModuleInlineAsm().empty() && is16) {
    auto *XTS =
        static_cast<X86TargetStreamer *>(OutStreamer->getTargetStreamer());
    XTS->emitCode16();
  }
}

static void
emitNonLazySymbolPointer(MCStreamer &OutStreamer, MCSymbol *StubLabel,
                         MachineModuleInfoImpl::StubValueTy &MCSym) {
  // L_foo$stub:
  OutStreamer.emitLabel(StubLabel);
  //   .indirect_symbol _foo
  OutStreamer.emitSymbolAttribute(MCSym.getPointer(), MCSA_IndirectSymbol);

  if (MCSym.getInt())
    // External to current translation unit.
    OutStreamer.emitIntValue(0, 4/*size*/);
  else
    // Internal to current translation unit.
    //
    // When we place the LSDA into the TEXT section, the type info
    // pointers need to be indirect and pc-rel. We accomplish this by
    // using NLPs; however, sometimes the types are local to the file.
    // We need to fill in the value for the NLP in those cases.
    OutStreamer.emitValue(
        MCSymbolRefExpr::create(MCSym.getPointer(), OutStreamer.getContext()),
        4 /*size*/);
}

static void emitNonLazyStubs(MachineModuleInfo *MMI, MCStreamer &OutStreamer) {

  MachineModuleInfoMachO &MMIMacho =
      MMI->getObjFileInfo<MachineModuleInfoMachO>();

  // Output stubs for dynamically-linked functions.
  MachineModuleInfoMachO::SymbolListTy Stubs;

  // Output stubs for external and common global variables.
  Stubs = MMIMacho.GetGVStubList();
  if (!Stubs.empty()) {
    OutStreamer.switchSection(MMI->getContext().getMachOSection(
        "__IMPORT", "__pointers", MachO::S_NON_LAZY_SYMBOL_POINTERS,
        SectionKind::getMetadata()));

    for (auto &Stub : Stubs)
      emitNonLazySymbolPointer(OutStreamer, Stub.first, Stub.second);

    Stubs.clear();
    OutStreamer.addBlankLine();
  }
}

/// True if this module is being built for windows/msvc, and uses floating
/// point. This is used to emit an undefined reference to _fltused. This is
/// needed in Windows kernel or driver contexts to find and prevent code from
/// modifying non-GPR registers.
///
/// TODO: It would be better if this was computed from MIR by looking for
/// selected floating-point instructions.
static bool usesMSVCFloatingPoint(const Triple &TT, const Module &M) {
  // Only needed for MSVC
  if (!TT.isWindowsMSVCEnvironment())
    return false;

  for (const Function &F : M) {
    for (const Instruction &I : instructions(F)) {
      if (I.getType()->isFloatingPointTy())
        return true;

      for (const auto &Op : I.operands()) {
        if (Op->getType()->isFloatingPointTy())
          return true;
      }
    }
  }

  return false;
}

void X86AsmPrinter::emitEndOfAsmFile(Module &M) {
  const Triple &TT = TM.getTargetTriple();

  if (TT.isOSBinFormatMachO()) {
    // Mach-O uses non-lazy symbol stubs to encode per-TU information into
    // global table for symbol lookup.
    emitNonLazyStubs(MMI, *OutStreamer);

    // Emit fault map information.
    FM.serializeToFaultMapSection();

    // This flag tells the linker that no global symbols contain code that fall
    // through to other global symbols (e.g. an implementation of multiple entry
    // points). If this doesn't occur, the linker can safely perform dead code
    // stripping. Since LLVM never generates code that does this, it is always
    // safe to set.
    OutStreamer->emitSubsectionsViaSymbols();
  } else if (TT.isOSBinFormatCOFF()) {
    emitKCFIThunks(M);

    // If import call optimization is enabled, emit the appropriate section.
    // We do this whether or not we recorded any items.
    if (EnableImportCallOptimization) {
      OutStreamer->switchSection(getObjFileLowering().getImportCallSection());

      // Section always starts with some magic.
      constexpr char ImpCallMagic[12] = "RetpolineV1";
      OutStreamer->emitBytes(StringRef{ImpCallMagic, sizeof(ImpCallMagic)});

      // Layout of this section is:
      // Per section that contains an item to record:
      //  uint32_t SectionSize: Size in bytes for information in this section.
      //  uint32_t Section Number
      //  Per call to imported function in section:
      //    uint32_t Kind: the kind of item.
      //    uint32_t InstOffset: the offset of the instr in its parent section.
      for (auto &[Section, CallsToImportedFuncs] :
           SectionToImportedFunctionCalls) {
        unsigned SectionSize =
            sizeof(uint32_t) * (2 + 2 * CallsToImportedFuncs.size());
        OutStreamer->emitInt32(SectionSize);
        OutStreamer->emitCOFFSecNumber(Section->getBeginSymbol());
        for (auto &[CallsiteSymbol, Kind] : CallsToImportedFuncs) {
          OutStreamer->emitInt32(Kind);
          OutStreamer->emitCOFFSecOffset(CallsiteSymbol);
        }
      }
    }

    if (usesMSVCFloatingPoint(TT, M)) {
      // In Windows' libcmt.lib, there is a file which is linked in only if the
      // symbol _fltused is referenced. Linking this in causes some
      // side-effects:
      //
      // 1. For x86-32, it will set the x87 rounding mode to 53-bit instead of
      // 64-bit mantissas at program start.
      //
      // 2. It links in support routines for floating-point in scanf and printf.
      //
      // MSVC emits an undefined reference to _fltused when there are any
      // floating point operations in the program (including calls). A program
      // that only has: `scanf("%f", &global_float);` may fail to trigger this,
      // but oh well...that's a documented issue.
      StringRef SymbolName =
          (TT.getArch() == Triple::x86) ? "__fltused" : "_fltused";
      MCSymbol *S = MMI->getContext().getOrCreateSymbol(SymbolName);
      OutStreamer->emitSymbolAttribute(S, MCSA_Global);
      return;
    }
  } else if (TT.isOSBinFormatELF()) {
    FM.serializeToFaultMapSection();
  }

  // Emit __morestack address if needed for indirect calls.
  if (TT.isX86_64() && TM.getCodeModel() == CodeModel::Large) {
    if (MCSymbol *AddrSymbol = OutContext.lookupSymbol("__morestack_addr")) {
      Align Alignment(1);
      MCSection *ReadOnlySection = getObjFileLowering().getSectionForConstant(
          getDataLayout(), SectionKind::getReadOnly(),
          /*C=*/nullptr, Alignment, /*F=*/nullptr);
      OutStreamer->switchSection(ReadOnlySection);
      OutStreamer->emitLabel(AddrSymbol);

      unsigned PtrSize = MAI.getCodePointerSize();
      OutStreamer->emitSymbolValue(GetExternalSymbolSymbol("__morestack"),
                                   PtrSize);
    }
  }
}

char X86AsmPrinter::ID = 0;

INITIALIZE_PASS(X86AsmPrinter, "x86-asm-printer", "X86 Assembly Printer", false,
                false)

//===----------------------------------------------------------------------===//
// Target Registry Stuff
//===----------------------------------------------------------------------===//

// Force static initialization.
extern "C" LLVM_C_ABI void LLVMInitializeX86AsmPrinter() {
  RegisterAsmPrinter<X86AsmPrinter> X(getTheX86_32Target());
  RegisterAsmPrinter<X86AsmPrinter> Y(getTheX86_64Target());
}

PreservedAnalyses X86AsmPrinterBeginPass::run(Module &M,
                                              ModuleAnalysisManager &MAM) {
  // Force the computation of SDPI so that it is available for the
  // actual pass, where it cannot be explicitly requested.
  MAM.getResult<StaticDataProfileInfoAnalysis>(M);
  X86AsmPrinter &AsmPrinter = static_cast<X86AsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  AsmPrinter.GetPSI = [&MAM](Module &M) {
    return &MAM.getResult<ProfileSummaryAnalysis>(M);
  };
  AsmPrinter.GetSDPI = [&MAM](Module &M) {
    return &MAM.getResult<StaticDataProfileInfoAnalysis>(M)
                .getStaticDataProfileInfo();
  };
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doInitialization(M);
  return PreservedAnalyses::all();
}

PreservedAnalyses X86AsmPrinterPass::run(MachineFunction &MF,
                                         MachineFunctionAnalysisManager &MFAM) {
  X86AsmPrinter &AsmPrinter = static_cast<X86AsmPrinter &>(
      MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
          .getCachedResult<AsmPrinterAnalysis>(*MF.getFunction().getParent())
          ->getPrinter());
  AsmPrinter.GetPSI = [&MFAM, &MF](Module &M) {
    return MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
        .getCachedResult<ProfileSummaryAnalysis>(M);
  };
  AsmPrinter.GetSDPI = [&MFAM, &MF](Module &M) {
    return &MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
                .getCachedResult<StaticDataProfileInfoAnalysis>(
                    *MF.getFunction().getParent())
                ->getStaticDataProfileInfo();
  };
  setupMachineFunctionAsmPrinter(MFAM, MF, AsmPrinter);
  AsmPrinter.runOnMachineFunction(MF);
  return PreservedAnalyses::all();
}

PreservedAnalyses X86AsmPrinterEndPass::run(Module &M,
                                            ModuleAnalysisManager &MAM) {
  X86AsmPrinter &AsmPrinter = static_cast<X86AsmPrinter &>(
      MAM.getCachedResult<AsmPrinterAnalysis>(M)->getPrinter());
  AsmPrinter.GetPSI = [&MAM](Module &M) {
    return &MAM.getResult<ProfileSummaryAnalysis>(M);
  };
  AsmPrinter.GetSDPI = [&MAM](Module &M) {
    return &MAM.getResult<StaticDataProfileInfoAnalysis>(M)
                .getStaticDataProfileInfo();
  };
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doFinalization(M);
  return PreservedAnalyses::all();
}
