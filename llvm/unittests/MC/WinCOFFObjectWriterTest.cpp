//===- llvm/unittest/MC/WinCOFFObjectWriterTest.cpp -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

class WinCOFFObjectWriterTest : public ::testing::Test {
protected:
  Triple TT{"x86_64-pc-windows-msvc"};
  const Target *TheTarget = nullptr;
  std::unique_ptr<MCRegisterInfo> MRI;
  std::unique_ptr<MCAsmInfo> MAI;
  std::unique_ptr<MCSubtargetInfo> STI;
  std::unique_ptr<MCInstrInfo> MII;
  MCTargetOptions Options;

  WinCOFFObjectWriterTest() {
    InitializeAllTargetInfos();
    InitializeAllTargetMCs();

    // Skip the tests if X86 is not built.
    std::string Error;
    TheTarget = TargetRegistry::lookupTarget(TT, Error);
    if (!TheTarget)
      return;
    MRI.reset(TheTarget->createMCRegInfo(TT));
    MAI.reset(TheTarget->createMCAsmInfo(*MRI, TT, Options));
    STI.reset(TheTarget->createMCSubtargetInfo(TT, "", ""));
    MII.reset(TheTarget->createMCInstrInfo());
  }

  // Assembles an object with one instruction, first giving the writer's
  // link-only records the capabilities Caps.
  SmallString<0> assemble(uint64_t Caps) {
    SmallString<0> Object;
    raw_svector_ostream OS(Object);
    MCContext Ctx(TT, *MAI, *MRI, *STI);
    std::unique_ptr<MCObjectFileInfo> MOFI(
        TheTarget->createMCObjectFileInfo(Ctx, /*PIC=*/false));
    Ctx.setObjectFileInfo(MOFI.get());
    std::unique_ptr<MCAsmBackend> MAB(
        TheTarget->createMCAsmBackend(*STI, *MRI, Options));
    std::unique_ptr<MCObjectWriter> OW = MAB->createObjectWriter(OS);
    std::unique_ptr<MCStreamer> Streamer(TheTarget->createMCObjectStreamer(
        TT, Ctx, std::move(MAB), std::move(OW),
        std::unique_ptr<MCCodeEmitter>(
            TheTarget->createMCCodeEmitter(*MII, Ctx)),
        *STI));
    auto &COFFStreamer = static_cast<MCWinCOFFStreamer &>(*Streamer);
    COFFStreamer.getWriter().setLinkRecordCapabilities(Caps);
    Streamer->initSections(*STI);
    Streamer->switchSection(MOFI->getTextSection());
    Streamer->emitBytes("\xc3");
    Streamer->finish();
    return Object;
  }
};

// Returns the object's .llvm_link_records section, or nullptr.
const object::coff_section *findLinkRecords(const object::COFFObjectFile &Obj) {
  for (const object::SectionRef &Sec : Obj.sections())
    if (cantFail(Sec.getName()) == ".llvm_link_records")
      return Obj.getCOFFSection(Sec);
  return nullptr;
}

TEST_F(WinCOFFObjectWriterTest, NoLinkRecords) {
  if (!TheTarget)
    GTEST_SKIP();
  SmallString<0> Object = assemble(0);
  std::unique_ptr<object::COFFObjectFile> Obj = cantFail(
      object::COFFObjectFile::create(MemoryBufferRef(Object, "test.obj")));
  EXPECT_EQ(findLinkRecords(*Obj), nullptr);
}

TEST_F(WinCOFFObjectWriterTest, LinkRecordsHeader) {
  if (!TheTarget)
    GTEST_SKIP();
  SmallString<0> Object = assemble(0x81);
  std::unique_ptr<object::COFFObjectFile> Obj = cantFail(
      object::COFFObjectFile::create(MemoryBufferRef(Object, "test.obj")));
  const object::coff_section *Sec = findLinkRecords(*Obj);
  ASSERT_NE(Sec, nullptr);
  EXPECT_EQ(Sec->Characteristics,
            COFF::IMAGE_SCN_LNK_REMOVE | COFF::IMAGE_SCN_ALIGN_1BYTES);
  ArrayRef<uint8_t> Contents;
  cantFail(Obj->getSectionContents(Sec, Contents));
  // The signature, the version and the capabilities, as ULEB128.
  const uint8_t Expected[] = {'L',  'L', 'R', 'C', COFF::LinkRecordsVersion,
                              0x81, 0x01};
  EXPECT_EQ(Contents, ArrayRef(Expected));
}

} // namespace
