set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(COFFOptionsTableGen)

add_lld_library(lldCOFF
  Binding.cpp
  CallGraphSort.cpp
  Chunks.cpp
  COFFLinkerContext.cpp
  DebugTypes.cpp
  DLL.cpp
  Driver.cpp
  DriverUtils.cpp
  ICF.cpp
  ImportInstructions.cpp
  InputFiles.cpp
  LLDMapFile.cpp
  LTO.cpp
  LTOPartitions.cpp
  MapFile.cpp
  MarkLive.cpp
  MinGW.cpp
  OutputFiles.cpp
  PDB.cpp
  Partitions.cpp
  SymbolTable.cpp
  Symbols.cpp
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
