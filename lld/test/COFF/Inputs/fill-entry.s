## The runtime's fill entries, which name the list and the entry that they
## run after it, for a test's image that writes cells as it starts.
.macro entry name
.section .text,"xr",one_only,\name
.globl \name
\name:
  ret
  .rva __llvm_fill_list
  .rva __llvm_fill_target
.endm
  entry __llvm_fill_dll
  entry __llvm_fill_dll_imports
.section .text,"xr",one_only,__llvm_fill_true
.globl __llvm_fill_true
__llvm_fill_true:
  ret
