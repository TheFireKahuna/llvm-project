set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(COFFOptionsTableGen)

add_lld_library(lldCOFF
  CallGraphSort.cpp
  Chunks.cpp
  COFFLinkerContext.cpp
  DebugTypes.cpp
  DLL.cpp
  Driver.cpp
  DriverUtils.cpp
  ICF.cpp
  InputFiles.cpp
  LLDMapFile.cpp
  LTO.cpp
  MapFile.cpp
  MarkLive.cpp
  MinGW.cpp
  PDB.cpp
  Relaxation.cpp
  Relocations.cpp
  SymbolTable.cpp
  Symbols.cpp
  Walker.cpp
  Writer.cpp

  LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  BinaryFormat
  BitWriter
  Core
  DebugInfoCodeView
  DebugInfoDWARF
  DebugInfoMSF
  DebugInfoPDB
  Demangle
  DTLTO
  LibDriver
  LTO
  MC
  MCDisassembler
  Object
  Option
  Passes
  Support
  TargetParser
  WindowsDriver
  WindowsManifest

  LINK_LIBS
  lldCommon
  ${LLVM_PTHREAD_LIB}
  ${LLVM_ATOMIC_LIB}

  DEPENDS
  COFFOptionsTableGen
  intrinsics_gen
  )

# The linker checks a fused site against the description the X86 target
# keeps for the compiler and the assembler.
if("X86" IN_LIST LLVM_TARGETS_TO_BUILD)
  add_dependencies(lldCOFF X86CommonTableGen)
  target_include_directories(lldCOFF PRIVATE
    ${LLVM_MAIN_SRC_DIR}/lib/Target/X86
    ${LLVM_BINARY_DIR}/lib/Target/X86
    )
endif()
