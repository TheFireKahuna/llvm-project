//===--- RTDyldObjectLinkingLayerTest.cpp - RTDyld linking layer tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "OrcTestCommon.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/CallTargetRestrictionPlugin.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"
#include <set>
#include <string>

using namespace llvm;
using namespace llvm::orc;

namespace {

// Returns whether a non-alloc section was passed to the memory manager.
static bool testSetProcessAllSections(std::unique_ptr<MemoryBuffer> Obj,
                                      bool ProcessAllSections) {
  class MemoryManagerWrapper : public SectionMemoryManager {
  public:
    MemoryManagerWrapper(bool &NonAllocSeen) : NonAllocSeen(NonAllocSeen) {}
    uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                                 unsigned SectionID, StringRef SectionName,
                                 bool IsReadOnly) override {
      // We check for ".note.GNU-stack" here because it is currently the only
      // non-alloc section seen in the module. If this changes in future any
      // other non-alloc section would do here.
      if (SectionName == ".note.GNU-stack")
        NonAllocSeen = true;
      return SectionMemoryManager::allocateDataSection(
          Size, Alignment, SectionID, SectionName, IsReadOnly);
    }

  private:
    bool &NonAllocSeen;
  };

  bool NonAllocSectionSeen = false;

  ExecutionSession ES(std::make_unique<UnsupportedExecutorProcessControl>());
  auto &JD = ES.createBareJITDylib("main");
  auto Foo = ES.intern("foo");

  RTDyldObjectLinkingLayer ObjLayer(
      ES, [&NonAllocSectionSeen](const MemoryBuffer &) {
        return std::make_unique<MemoryManagerWrapper>(NonAllocSectionSeen);
      });

  auto OnResolveDoNothing = [](Expected<SymbolMap> R) {
    cantFail(std::move(R));
  };

  ObjLayer.setProcessAllSections(ProcessAllSections);
  cantFail(ObjLayer.add(JD, std::move(Obj)));
  ES.lookup(LookupKind::Static, makeJITDylibSearchOrder(&JD),
            SymbolLookupSet(Foo), SymbolState::Resolved, OnResolveDoNothing,
            NoDependenciesToRegister);

  if (auto Err = ES.endSession())
    ES.reportError(std::move(Err));

  return NonAllocSectionSeen;
}

TEST(RTDyldObjectLinkingLayerTest, TestSetProcessAllSections) {
  LLVMContext Context;
  auto M = std::make_unique<Module>("", Context);
  M->setTargetTriple(Triple("x86_64-unknown-linux-gnu"));

  // These values are only here to ensure that the module is non-empty.
  // They are no longer relevant to the test.
  Constant *StrConstant = ConstantDataArray::getString(Context, "forty-two");
  auto *GV =
      new GlobalVariable(*M, StrConstant->getType(), true,
                         GlobalValue::ExternalLinkage, StrConstant, "foo");
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(Align(1));

  // Initialize the native target in case this is the first unit test
  // to try to build a TM.
  OrcNativeTarget::initialize();
  std::unique_ptr<TargetMachine> TM(EngineBuilder().selectTarget(
      M->getTargetTriple(), "", "", SmallVector<std::string, 1>()));
  if (!TM)
    GTEST_SKIP();

  auto Obj = cantFail(SimpleCompiler(*TM)(*M));

  EXPECT_FALSE(testSetProcessAllSections(
      MemoryBuffer::getMemBufferCopy(Obj->getBuffer()), false))
      << "Non-alloc section seen despite ProcessAllSections being false";
  EXPECT_TRUE(testSetProcessAllSections(std::move(Obj), true))
      << "Expected to see non-alloc section when ProcessAllSections is true";
}

TEST(RTDyldObjectLinkingLayerTest, TestOverrideObjectFlags) {

  OrcNativeTarget::initialize();

  std::unique_ptr<TargetMachine> TM(
      EngineBuilder().selectTarget(Triple("x86_64-unknown-linux-gnu"), "", "",
                                   SmallVector<std::string, 1>()));

  if (!TM)
    GTEST_SKIP();

  // Our compiler is going to modify symbol visibility settings without telling
  // ORC. This will test our ability to override the flags later.
  class FunkySimpleCompiler : public SimpleCompiler {
  public:
    FunkySimpleCompiler(TargetMachine &TM) : SimpleCompiler(TM) {}

    Expected<CompileResult> operator()(Module &M) override {
      auto *Foo = M.getFunction("foo");
      assert(Foo && "Expected function Foo not found");
      Foo->setVisibility(GlobalValue::HiddenVisibility);
      return SimpleCompiler::operator()(M);
    }
  };

  // Create a module with two void() functions: foo and bar.
  ThreadSafeModule M;
  {
    auto Ctx = std::make_unique<LLVMContext>();
    ModuleBuilder MB(*Ctx, TM->getTargetTriple().str(), "dummy");
    MB.getModule()->setDataLayout(TM->getTargetTriple().computeDataLayout());

    Function *FooImpl = MB.createFunctionDecl(
        FunctionType::get(Type::getVoidTy(*Ctx), {}, false), "foo");
    BasicBlock *FooEntry = BasicBlock::Create(*Ctx, "entry", FooImpl);
    IRBuilder<> B1(FooEntry);
    B1.CreateRetVoid();

    Function *BarImpl = MB.createFunctionDecl(
        FunctionType::get(Type::getVoidTy(*Ctx), {}, false), "bar");
    BasicBlock *BarEntry = BasicBlock::Create(*Ctx, "entry", BarImpl);
    IRBuilder<> B2(BarEntry);
    B2.CreateRetVoid();

    M = ThreadSafeModule(MB.takeModule(), std::move(Ctx));
  }

  // Create a simple stack and set the override flags option.
  ExecutionSession ES{std::make_unique<UnsupportedExecutorProcessControl>()};
  auto &JD = ES.createBareJITDylib("main");
  auto Foo = ES.intern("foo");
  RTDyldObjectLinkingLayer ObjLayer(ES, [](const MemoryBuffer &) {
    return std::make_unique<SectionMemoryManager>();
  });
  IRCompileLayer CompileLayer(ES, ObjLayer,
                              std::make_unique<FunkySimpleCompiler>(*TM));

  ObjLayer.setOverrideObjectFlagsWithResponsibilityFlags(true);

  cantFail(CompileLayer.add(JD, std::move(M)));
  ES.lookup(
      LookupKind::Static, makeJITDylibSearchOrder(&JD), SymbolLookupSet(Foo),
      SymbolState::Resolved,
      [](Expected<SymbolMap> R) { cantFail(std::move(R)); },
      NoDependenciesToRegister);

  if (auto Err = ES.endSession())
    ES.reportError(std::move(Err));
}

TEST(RTDyldObjectLinkingLayerTest, TestAutoClaimResponsibilityForSymbols) {

  OrcNativeTarget::initialize();

  std::unique_ptr<TargetMachine> TM(
      EngineBuilder().selectTarget(Triple("x86_64-unknown-linux-gnu"), "", "",
                                   SmallVector<std::string, 1>()));

  if (!TM)
    GTEST_SKIP();

  // Our compiler is going to add a new symbol without telling ORC.
  // This will test our ability to auto-claim responsibility later.
  class FunkySimpleCompiler : public SimpleCompiler {
  public:
    FunkySimpleCompiler(TargetMachine &TM) : SimpleCompiler(TM) {}

    Expected<CompileResult> operator()(Module &M) override {
      Function *BarImpl = Function::Create(
          FunctionType::get(Type::getVoidTy(M.getContext()), {}, false),
          GlobalValue::ExternalLinkage, "bar", &M);
      BasicBlock *BarEntry =
          BasicBlock::Create(M.getContext(), "entry", BarImpl);
      IRBuilder<> B(BarEntry);
      B.CreateRetVoid();

      return SimpleCompiler::operator()(M);
    }
  };

  // Create a module with two void() functions: foo and bar.
  ThreadSafeModule M;
  {
    auto Ctx = std::make_unique<LLVMContext>();
    ModuleBuilder MB(*Ctx, TM->getTargetTriple().str(), "dummy");
    MB.getModule()->setDataLayout(TM->getTargetTriple().computeDataLayout());

    Function *FooImpl = MB.createFunctionDecl(
        FunctionType::get(Type::getVoidTy(*Ctx), {}, false), "foo");
    BasicBlock *FooEntry = BasicBlock::Create(*Ctx, "entry", FooImpl);
    IRBuilder<> B(FooEntry);
    B.CreateRetVoid();

    M = ThreadSafeModule(MB.takeModule(), std::move(Ctx));
  }

  // Create a simple stack and set the override flags option.
  ExecutionSession ES{std::make_unique<UnsupportedExecutorProcessControl>()};
  auto &JD = ES.createBareJITDylib("main");
  auto Foo = ES.intern("foo");
  RTDyldObjectLinkingLayer ObjLayer(ES, [](const MemoryBuffer &) {
    return std::make_unique<SectionMemoryManager>();
  });
  IRCompileLayer CompileLayer(ES, ObjLayer,
                              std::make_unique<FunkySimpleCompiler>(*TM));

  ObjLayer.setAutoClaimResponsibilityForObjectSymbols(true);

  cantFail(CompileLayer.add(JD, std::move(M)));
  ES.lookup(
      LookupKind::Static, makeJITDylibSearchOrder(&JD), SymbolLookupSet(Foo),
      SymbolState::Resolved,
      [](Expected<SymbolMap> R) { cantFail(std::move(R)); },
      NoDependenciesToRegister);

  if (auto Err = ES.endSession())
    ES.reportError(std::move(Err));
}

TEST(RTDyldObjectLinkingLayerTest, TestMemoryBufferNamePropagation) {
  OrcNativeTarget::initialize();

  std::unique_ptr<TargetMachine> TM(
      EngineBuilder().selectTarget(Triple("x86_64-unknown-linux-gnu"), "", "",
                                   SmallVector<std::string, 1>()));

  if (!TM)
    GTEST_SKIP();

  // Create a module with two void() functions: foo and bar.
  ThreadSafeModule M;
  {
    auto Ctx = std::make_unique<LLVMContext>();
    ModuleBuilder MB(*Ctx, TM->getTargetTriple().str(), "dummy");
    MB.getModule()->setDataLayout(TM->getTargetTriple().computeDataLayout());

    Function *FooImpl = MB.createFunctionDecl(
        FunctionType::get(Type::getVoidTy(*Ctx), {}, false), "foo");
    BasicBlock *FooEntry = BasicBlock::Create(*Ctx, "entry", FooImpl);
    IRBuilder<> B1(FooEntry);
    B1.CreateRetVoid();

    M = ThreadSafeModule(MB.takeModule(), std::move(Ctx));
  }

  ExecutionSession ES{std::make_unique<UnsupportedExecutorProcessControl>()};
  auto &JD = ES.createBareJITDylib("main");
  auto Foo = ES.intern("foo");
  std::string ObjectIdentifer;

  RTDyldObjectLinkingLayer ObjLayer(
      ES, [&ObjectIdentifer](const MemoryBuffer &Obj) {
        // Capture the name of the object so that we can confirm that it
        // contains the module name.
        ObjectIdentifer = Obj.getBufferIdentifier().str();
        return std::make_unique<SectionMemoryManager>();
      });
  IRCompileLayer CompileLayer(ES, ObjLayer,
                              std::make_unique<SimpleCompiler>(*TM));

  // Capture the module name before we move the module.
  std::string ModuleName = M.getModuleUnlocked()->getName().str();

  cantFail(CompileLayer.add(JD, std::move(M)));
  ES.lookup(
      LookupKind::Static, makeJITDylibSearchOrder(&JD), SymbolLookupSet(Foo),
      SymbolState::Resolved,
      [](Expected<SymbolMap> R) { cantFail(std::move(R)); },
      NoDependenciesToRegister);

  if (auto Err = ES.endSession())
    ES.reportError(std::move(Err));

  EXPECT_TRUE(ObjectIdentifer.find(ModuleName) != std::string::npos);
}

void external() {}

// The listeners see an object before its symbols are published, and the
// memory each section takes includes the stubs RuntimeDyld places after it.
TEST(RTDyldObjectLinkingLayerTest, NotifyObjectLoadedBeforePublishing) {
  OrcNativeTarget::initialize();
  std::unique_ptr<TargetMachine> TM(EngineBuilder().selectTarget(
      Triple("x86_64-pc-windows-msvc"), "", "", SmallVector<std::string, 1>()));
  if (!TM)
    GTEST_SKIP();

  LLVMContext Ctx;
  SMDiagnostic Diag;
  std::unique_ptr<Module> M = parseIR(MemoryBufferRef(R"(
      declare void @external()
      define void @f() {
        call void @external()
        ret void
      })",
                                                      "m"),
                                      Diag, Ctx);
  ExecutionSession ES(std::make_unique<UnsupportedExecutorProcessControl>());
  JITDylib &JD = ES.createBareJITDylib("main");
  cantFail(JD.define(absoluteSymbols(
      {{ES.intern("external"),
        {ExecutorAddr::fromPtr(&external), JITSymbolFlags::Exported}}})));
  RTDyldObjectLinkingLayer ObjLayer(ES, [](const MemoryBuffer &) {
    return std::make_unique<SectionMemoryManager>();
  });
  ObjLayer.setOverrideObjectFlagsWithResponsibilityFlags(true);

  struct Listener : JITEventListener {
    Listener(ExecutionSession &ES, JITDylib &JD) : ES(ES), JD(JD) {}
    void notifyObjectLoaded(ObjectKey K, const object::ObjectFile &Obj,
                            const RuntimeDyld::LoadedObjectInfo &L) override {
      ES.lookup(
          LookupKind::Static, makeJITDylibSearchOrder(&JD),
          SymbolLookupSet(ES.intern("f")), SymbolState::Ready,
          [this](Expected<SymbolMap> R) {
            Found = !cantFail(std::move(R)).empty();
          },
          NoDependenciesToRegister);
      FoundWhenLoaded = Found;
      for (const object::SectionRef &Sec : Obj.sections())
        if (Sec.isText())
          StubSize = L.getSectionAllocationSize(Sec) - Sec.getSize();
    }
    ExecutionSession &ES;
    JITDylib &JD;
    bool Found = false, FoundWhenLoaded = false;
    uint64_t StubSize = 0;
  } L(ES, JD);
  ObjLayer.registerJITEventListener(L);
  cantFail(ObjLayer.add(JD, cantFail(SimpleCompiler(*TM)(*M))));

  // With its dependency ready, f is ready, and found, once published.
  cantFail(ES.lookup({&JD}, "external"));
  cantFail(ES.lookup({&JD}, "f"));
  EXPECT_FALSE(L.FoundWhenLoaded);
  EXPECT_TRUE(L.Found);
  EXPECT_GT(L.StubSize, 0U);
  cantFail(ES.endSession());
}

// The call targets of the code RuntimeDyld loads: each code section with its
// stubs, and as entries its global functions and the functions its
// relocations name, an i386 REL32 and unwind data aside, or, if it was built
// with Control Flow Guard, those its .gfids$y names.
TEST(RTDyldObjectLinkingLayerTest, GetCallTargets) {
  OrcNativeTarget::initialize();
  auto SelectTarget = [](StringRef TT) {
    return std::unique_ptr<TargetMachine>(EngineBuilder().selectTarget(
        Triple(TT), "", "", SmallVector<std::string, 1>()));
  };
  if (!SelectTarget("x86_64-pc-windows-msvc"))
    GTEST_SKIP();

  struct NoResolver : JITSymbolResolver {
    void lookup(const LookupSet &, OnResolvedFunction OnResolved) override {
      OnResolved(LookupResult());
    }
    Expected<LookupSet> getResponsibilitySet(const LookupSet &) override {
      return LookupSet();
    }
  };

  auto Targets = [&](StringRef TT, StringRef Extra) {
    LLVMContext Ctx;
    SMDiagnostic Diag;
    std::string IR = (R"(
      declare void @external()
      define void @f() {
        call void @external()
        call void @called()
        ret void
      }
      define internal void @called() { ret void }
      define internal void @listed() { ret void }
      define internal void @unnamed() { ret void }
    )" + Extra)
                         .str();
    std::unique_ptr<Module> M = parseIR(MemoryBufferRef(IR, "m"), Diag, Ctx);
    M->setTargetTriple(Triple(TT));
    std::unique_ptr<MemoryBuffer> Obj =
        cantFail(SimpleCompiler(*SelectTarget(TT))(*M));
    auto COFFObj = cantFail(object::ObjectFile::createCOFFObjectFile(*Obj));
    SectionMemoryManager MemMgr;
    NoResolver Resolver;
    RuntimeDyld Dyld(MemMgr, Resolver);
    auto L = Dyld.loadObject(*COFFObj);
    SmallVector<sys::MemoryBlock> Code;
    std::vector<uintptr_t> Entries;
    cantFail(getCallTargets(*cast<object::COFFObjectFile>(COFFObj.get()), *L,
                            Code, Entries));

    // The code includes its stubs, and the entries lie in its contents.
    std::set<std::string> Names;
    EXPECT_EQ(Code.size(), 1U);
    for (const object::SectionRef &Sec : COFFObj->sections()) {
      if (!Sec.isText())
        continue;
      uint64_t Base = L->getSectionLoadAddress(Sec);
      EXPECT_EQ(reinterpret_cast<uintptr_t>(Code[0].base()), Base);
      EXPECT_GT(Code[0].allocatedSize(), Sec.getSize());
      for (uintptr_t E : Entries)
        EXPECT_TRUE(E >= Base && E < Base + Sec.getSize());
      for (const object::SymbolRef &Sym : COFFObj->symbols())
        if (cantFail(Sym.getType()) == object::SymbolRef::ST_Function &&
            !(cantFail(Sym.getFlags()) & object::SymbolRef::SF_Undefined) &&
            llvm::is_contained(Entries, Base + cantFail(Sym.getValue())))
          Names.insert(cantFail(Sym.getName()).str());
    }
    return Names;
  };

  using Names = std::set<std::string>;
  StringRef TakesListed = "@p = global ptr @listed\n";
  StringRef Unwinds = R"(
      module asm ".section .pdata,\22dr\22"
      module asm ".rva unnamed")";
  StringRef GuardCF = R"(
      !llvm.module.flags = !{!0}
      !0 = !{i32 2, !"cfguard", i32 2})";
  EXPECT_EQ(Targets("x86_64-pc-windows-msvc", (TakesListed + Unwinds).str()),
            (Names{"called", "f", "listed"}));
  EXPECT_EQ(Targets("x86_64-pc-windows-msvc", (TakesListed + GuardCF).str()),
            (Names{"f", "listed"}));
  EXPECT_EQ(Targets("x86_64-pc-windows-msvc", GuardCF), (Names{"f"}));
  EXPECT_EQ(Targets("i686-pc-windows-msvc", TakesListed),
            (Names{"_f", "_listed"}));
}

} // end anonymous namespace
