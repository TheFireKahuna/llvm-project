# REQUIRES: x86
## Under -import-slots, a word of static data that holds an address inside an
## import for which its DLL exports no name is written by the image's residual
## fill, which the C initializer table runs ahead of every initializer. A
## read-only word moves with its chunk, slots and all, to .sealed, which the
## fill makes read-only once written, returning NtProtectVirtualMemory's
## status so that a failed seal fails the image's start-up; a writable word
## stays in place, and a fill with no read-only word is a leaf that returns 0.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# RUN: lld-link -def:lib.def -out:lib.lib -machine:x64
# RUN: lld-link -def:ntdll.def -out:ntdll.lib -machine:x64
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium crt.s -o crt.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium ro.s -o ro.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium rw.s -o rw.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium tls.s -o tls.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium named.s -o named.obj
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-itanium cfg.s -o cfg.obj

# RUN: lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj rw.obj \
# RUN:   lib.lib ntdll.lib
# RUN: llvm-readobj --coff-imports out.dll > out.txt
# RUN: llvm-readobj --sections out.dll >> out.txt
# RUN: llvm-objdump -d --no-show-raw-insn out.dll >> out.txt
# RUN: FileCheck --input-file=out.txt %s
# RUN: llvm-objdump -s -j .CRT out.dll | FileCheck --check-prefix=TABLE %s
# RUN: llvm-readobj --unwind out.dll | FileCheck --check-prefix=UNWIND %s

## arr keeps its entry, which the fill reads, and its slot at ro + 8 is a run
## of its own in .sealed.
# CHECK:      Name: lib.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,ARR:]]
# CHECK-NEXT: Symbol: arr
# CHECK:      Name: lib.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,SLOT:]]
# CHECK-NEXT: Symbol: arr
# CHECK:      Name: ntdll.dll
# CHECK-NEXT: ImportLookupTableRVA:
# CHECK-NEXT: ImportAddressTableRVA: 0x[[#%X,PROTECT:]]
# CHECK-NEXT: Symbol: NtProtectVirtualMemory

# CHECK:      Name: .data
# CHECK-NEXT: VirtualSize:
# CHECK-NEXT: VirtualAddress: 0x[[#%X,DATA:]]
# CHECK:      Name: .sealed
# CHECK-NEXT: VirtualSize: 0x10
# CHECK-NEXT: VirtualAddress: 0x[[#%X,SLOT-8]]
# CHECK:      Characteristics [
# CHECK-NEXT:   IMAGE_SCN_CNT_INITIALIZED_DATA
# CHECK-NEXT:   IMAGE_SCN_MEM_READ
# CHECK-NEXT:   IMAGE_SCN_MEM_WRITE
# CHECK-NEXT: ]

## 6442450944 is the image base, 0x180000000.
# CHECK:      180001000: subq $0x48, %rsp
# CHECK-NEXT:            movq {{.*}}(%rip), %rax # 0x[[#%x,ARR+6442450944]]
# CHECK-NEXT:            addq $0xc, %rax
# CHECK-NEXT:            movq %rax, {{.*}}(%rip) # 0x[[#%x,SLOT-8+6442450944]]
# CHECK-NEXT:            movq {{.*}}(%rip), %rax # 0x[[#%x,ARR+6442450944]]
# CHECK-NEXT:            movabsq $0x100000000, %rcx
# CHECK-NEXT:            addq %rcx, %rax
# CHECK-NEXT:            movq %rax, {{.*}}(%rip) # 0x[[#%x,DATA+6442450944]]
# CHECK-NEXT:            leaq {{.*}}(%rip), %rax # 0x[[#%x,SLOT-8+6442450944]]
# CHECK-NEXT:            movq %rax, 0x30(%rsp)
# CHECK-NEXT:            movq $0x10, 0x38(%rsp)
# CHECK-NEXT:            leaq 0x40(%rsp), %rax
# CHECK-NEXT:            movq %rax, 0x20(%rsp)
# CHECK-NEXT:            movq $-0x1, %rcx
# CHECK-NEXT:            leaq 0x30(%rsp), %rdx
# CHECK-NEXT:            leaq 0x38(%rsp), %r8
# CHECK-NEXT:            movl $0x2, %r9d
# CHECK-NEXT:            callq *{{.*}}(%rip) # 0x[[#%x,PROTECT+6442450944]]
# CHECK-NEXT:            addq $0x48, %rsp
# CHECK-NEXT:            retq

## The fill follows the table's start, ahead of the other initializer.
# TABLE:      11111111 11111111 00100080 01000000
# TABLE-NEXT: 22222222 22222222 33333333 33333333

# UNWIND:      StartAddress: (0x180001000)
# UNWIND-NEXT: EndAddress: (0x180001074)
# UNWIND:      PrologSize: 4
# UNWIND:      0x04: ALLOC_SMALL size=72

## Control Flow Guard lets the table call it.
# RUN: lld-link -dll -noentry -import-slots -guard:cf -out:guard.dll crt.obj \
# RUN:   ro.obj cfg.obj lib.lib ntdll.lib
# RUN: llvm-readobj --coff-load-config guard.dll | \
# RUN:   FileCheck --check-prefix=GUARD %s

# GUARD:      GuardFidTable [
# GUARD-NEXT:   0x180001000
# GUARD-NEXT: ]

## With only writable words, the fill neither seals nor needs a frame.
# RUN: lld-link -dll -noentry -import-slots -out:leaf.dll crt.obj rw.obj \
# RUN:   lib.lib
# RUN: llvm-objdump -d --no-show-raw-insn leaf.dll | \
# RUN:   FileCheck --check-prefix=LEAF %s
# RUN: llvm-readobj --coff-imports --sections --unwind leaf.dll | \
# RUN:   FileCheck --check-prefix=LEAF-IMAGE %s

# LEAF:      180001000: movq {{.*}}(%rip), %rax
# LEAF-NEXT:            movabsq $0x100000000, %rcx
# LEAF-NEXT:            addq %rcx, %rax
# LEAF-NEXT:            movq %rax, {{.*}}(%rip)
# LEAF-NEXT:            xorl %eax, %eax
# LEAF-NEXT:            retq
# LEAF-IMAGE-NOT: ntdll.dll
# LEAF-IMAGE-NOT: .sealed
# LEAF-IMAGE-NOT: RuntimeFunction

# RUN: not lld-link -dll -noentry -import-slots -out:out.dll ro.obj lib.lib \
# RUN:   ntdll.lib 2>&1 | FileCheck --check-prefix=NOTABLE %s
# RUN: not lld-link -dll -noentry -import-slots -out:out.dll crt.obj ro.obj \
# RUN:   lib.lib 2>&1 | FileCheck --check-prefix=NOPROTECT %s
# RUN: not lld-link -dll -noentry -import-slots -align:512 -out:out.dll \
# RUN:   crt.obj ro.obj lib.lib ntdll.lib 2>&1 | \
# RUN:   FileCheck --check-prefix=ALIGN %s
# RUN: not lld-link -dll -noentry -import-slots -out:out.dll crt.obj tls.obj \
# RUN:   named.obj lib.lib ntdll.lib 2>&1 | FileCheck --check-prefix=PLACE %s

# NOTABLE: error: ro.obj: .rdata holds the address of arr plus 12, imported from lib.dll, which exports no name for it, and the image has no C initializer table (__xi_a) to write it
# NOPROTECT: error: ro.obj: .rdata holds the address of arr plus 12, imported from lib.dll, which exports no name for it, and the image cannot make it read-only once written without NtProtectVirtualMemory from ntdll.lib
# ALIGN: error: ro.obj: .rdata holds the address of arr plus 12, imported from lib.dll, which exports no name for it, and its section alignment is smaller than a page, which the image cannot make read-only once written
# PLACE: error: tls.obj: .tls$ holds the address of arr plus 4, imported from lib.dll, which exports no name for it, in thread-local data
# PLACE: error: named.obj: .rdata$x holds the address of arr plus 4, imported from lib.dll, which exports no name for it, in a read-only section that cannot move

#--- lib.def
LIBRARY lib.dll
EXPORTS
  arr DATA

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
  NtProtectVirtualMemory

#--- crt.s
  .section .CRT$XIA,"dr"
  .globl __xi_a
__xi_a:
  .quad 0x1111111111111111
  .section .CRT$XIC,"dr"
  .quad 0x2222222222222222
  .section .CRT$XIZ,"dr"
  .globl __xi_z
__xi_z:
  .quad 0x3333333333333333

#--- ro.s
  .section .rdata,"dr"
  .p2align 3
  .quad arr+12
  .quad arr

#--- rw.s
  .data
  .p2align 3
  .quad arr+0x100000000

#--- tls.s
  .section .tls$,"dw"
  .p2align 3
  .quad arr+4

#--- named.s
  .section .rdata$x,"dr"
  .p2align 3
  .quad arr+4

#--- cfg.s
  .section .rdata,"dr"
  .p2align 3
  .globl _load_config_used
_load_config_used:
  .long 256
  .fill 124, 1, 0
  .quad __guard_fids_table
  .quad __guard_fids_count
  .long __guard_flags
  .fill 12, 1, 0
  .quad __guard_iat_table
  .quad __guard_iat_count
  .quad __guard_longjmp_table
  .quad __guard_fids_count
  .fill 84, 1, 0
