# Tests of lld/COFF's own parts. The walker's decoder is checked against
# LLVM's x86 encoder, whose tables and fixups are internal to the target.

set(LLVM_LINK_COMPONENTS
  MC
  Support
  TargetParser
  X86
)

add_lld_unittests(LLDCOFFTests
  WalkerTest.cpp
)

target_include_directories(LLDCOFFTests
  PRIVATE
  ${LLD_SOURCE_DIR}/COFF
  ${LLVM_MAIN_SRC_DIR}/lib/Target/X86
  ${LLVM_BINARY_DIR}/lib/Target/X86
)

target_link_libraries(LLDCOFFTests
  PRIVATE
  lldCommon
  lldCOFF
)
