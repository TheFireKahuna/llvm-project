# REQUIRES: x86
## Under -import-slots a DLL exports addresses past the ends of its data, such
## as the end of an array another image's data holds, which the loader would
## read as the name of another DLL's export were the export directory to
## follow the data. The export directory goes first in .rdata, and an export
## whose address is inside it still is an error.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium a.s -o a.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium t.s -o t.obj

# RUN: lld-link -import-slots -dll -noentry a.obj -out:a.dll
# RUN: llvm-readobj --file-headers --coff-exports a.dll | FileCheck %s
# CHECK:      ExportTableRVA: 0x1000
# CHECK:      Name: arr
# CHECK-NEXT: RVA: 0x[[#%X,ARR:]]
# CHECK:      Name: arr$so16
# CHECK-NEXT: RVA: 0x[[#%X,ARR+16]]

## Elsewhere the export directory follows the data.
# RUN: lld-link -dll -noentry a.obj -out:plain.dll
# RUN: llvm-readobj --file-headers plain.dll | FileCheck --check-prefix=PLAIN %s
# PLAIN: ExportTableRVA: 0x1010

# RUN: not lld-link -import-slots -dll -noentry t.obj -out:t.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=ERR %s
# ERR: error: cannot export te: its address is inside the export directory, which the loader reads as the name of another DLL's export

#--- a.s
  .section .rdata,"dr",one_only,arr
  .globl arr, arr$so16
  .p2align 4
arr:
  .long 1, 2, 3, 4
arr$so16:

  .section .drectve,"yni"
  .ascii " -export:arr,DATA -export:arr$so16,DATA"

#--- t.s
  .text
  .globl te
  .fill 4096, 1, 0xcc
te:
  .section .drectve,"yni"
  .ascii " -export:te"
