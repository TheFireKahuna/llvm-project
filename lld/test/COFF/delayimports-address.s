# REQUIRES: x86, aarch64

## An observed delay-loaded function's address is its DLL's own: code loads it
## from the entry behind the DLL's gate, read-only words are runs with
## descriptors of their own, and writable words are listed for the DLL's
## ensure stub.

# RUN: rm -rf %t && split-file %s %t && cd %t
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -dll -noentry -guard:cf -include:main \
# DEFINE:   -call-graph-profile-sort:none -delayload:d.dll -delayload:e.dll
# DEFINE: %{dump} = llvm-readobj --coff-imports --coff-load-config --unwind

# RUN: lld-link -machine:x64 -def:d.def -out:d.lib
# RUN: lld-link -machine:x64 -def:e.def -out:e.lib
# RUN: cat x64.s data.s > main.s
# RUN: %{mc} -triple=x86_64-windows-itanium -mattr=+egpr main.s -o main.obj
# RUN: %{mc} -triple=x86_64-windows-itanium -I %S/Inputs rt.s -o rt.obj
# RUN: %{mc} -triple=x86_64-windows-msvc foreign.s -o foreign.obj
# RUN: %{link} d.lib e.lib main.obj rt.obj foreign.obj -include:other \
# RUN:   -out:x64.dll
# RUN: llvm-objdump -d x64.dll > x64.txt
# RUN: %{dump} x64.dll >> x64.txt
# RUN: llvm-objdump -s -j .rdata -j .data -j .zrun x64.dll >> x64.txt
# RUN: FileCheck --check-prefixes=CHECK,X64 %s < x64.txt

## The address of f or g is loaded from its entry behind the gate: inline
## where the flags are dead in code that moves, else by a call of a leaf for
## the entry and register. h's is its thunk's, and the calls are as they were.
# X64:      <.text>:
# X64-NEXT: callq 0x[[#%x,LEAF:]]
# X64-NEXT: nop
# X64-NEXT: leaq {{.*}}(%rip), %rdx {{.*}}# 0x[[#%x,GTHUNK:]]
# X64-NEXT: cmpq %rdx, {{.*}}(%rip) {{.*}}# 0x[[#%x,GATE:]]
# X64-NEXT: jne 0x[[#%x,LOAD:]]
# X64-NEXT: callq 0x[[#%x,ENSURE:]]
# X64-NEXT: [[#LOAD]]: {{.*}} movq {{.*}}(%rip), %rdx {{.*}}# 0x180007010
# X64-NEXT: addq %rdx, %rax
# X64-NEXT: callq 0x[[#%x,LEAF2:]]
# X64-NEXT: nop
# X64-NEXT: callq 0x[[#%x,LEAF3:]]
# X64-NEXT: nopl (%rax)
# X64-NEXT: leaq {{.*}}(%rip), %rcx {{.*}}<.text+0x[[#%x,]]>
# X64-NEXT: callq
# X64-NEXT: retq
## So does a foreign object's lea.
# X64:      callq 0x[[#LEAF]]
# X64-NEXT: nop
# X64-NEXT: retq
## A leaf pushes rax for rcx and rcx for any other register, and rcx is zero
## exactly while the gate holds its thunk; the flags are untouched.
# X64:      [[#LEAF]]: 50 pushq %rax
# X64-NEXT: leaq {{.*}}(%rip), %rax {{.*}}# 0x[[#GTHUNK]]
# X64-NEXT: notq %rax
# X64-NEXT: movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#GATE]]
# X64-NEXT: leaq 0x1(%rcx,%rax), %rcx
# X64-NEXT: jrcxz 0x[[#%x,SLOW:]]
# X64-NEXT: [[#%x,BACK:]]: {{.*}} movq {{.*}}(%rip), %rcx {{.*}}# 0x180007008
# X64-NEXT: popq %rax
# X64-NEXT: retq
# X64-NEXT: [[#SLOW]]: {{.*}} callq 0x[[#ENSURE]]
# X64-NEXT: jmp 0x[[#BACK]]
# X64-NEXT: [[#LEAF2]]: 51 pushq %rcx
# X64:      movq {{.*}}(%rip), %r12 {{.*}}# 0x[[#GATE]]
# X64-NEXT: leaq 0x1(%r12,%rcx), %rcx
# X64:      [[#LEAF3]]: 51 pushq %rcx
# X64:      d5 48 8b 0d {{.*}} movq {{.*}}(%rip), %r17 {{.*}}# 0x[[#GATE]]
# X64-NEXT: d5 18 8d 4c 09 01 leaq 0x1(%r17,%rcx), %rcx
# X64-NEXT: jrcxz
# X64-NEXT: d5 48 8b 0d {{.*}} movq {{.*}}(%rip), %r17 {{.*}}# 0x180007008
## The stub keeps the flags, the registers and the extended state.
# X64:      [[#ENSURE]]: 55 pushq %rbp
# X64-NEXT: movq %rsp, %rbp
# X64-NEXT: pushfq
# X64:      cld
# X64-NEXT: movl 0x7ffe03e8, %ecx
# X64:      xsavec64 (%rsp)
# X64-NEXT: leaq 0x[[#%x,]](%rip), %rcx {{.*}}# 0x[[#%x,LIST:]]
# X64-NEXT: movq %rcx, %rdi
# X64-NEXT: subq $0x20, %rsp
# X64-NEXT: callq 0x{{[0-9a-f]+}} <.text+0x{{[0-9a-f]+}}>
# X64:      xrstor64 (%rsp)
# X64:      popfq
# X64-NEXT: popq %rbp
# X64-NEXT: retq

## ARM64: in code that moves, a guard before the adrp loads the gate into
## its register and tests bit 0, which only the gate's unbound value, its
## thunk plus one, has; the guard after the section saves x30 and the site's
## address, calls the ensure stub, and redoes the adrp. h's is its thunk's.
# A64:      <.text>:
# A64-NEXT: [[#%x,LEAF:]]: {{.*}} adrp x0, 0x[[#%x,PAGE:]]
# A64-NEXT: ldr x0, [x0, #0x28]
# A64-NEXT: tbnz w0, #0x0, 0x[[#%x,GUARD:]]
# A64-NEXT: adrp x0, 0x[[#PAGE]]
# A64-NEXT: [[#%x,BACK:]]: {{.*}} ldr x0, [x0, #0x8]
# A64-NEXT: adr x1, 0x[[#%x,]]
# A64-NEXT: bl
# A64-NEXT: ret
# A64-NEXT: adrp x0, 0x[[#PAGE]]
# A64-NEXT: ldr x0, [x0, #0x28]
# A64-NEXT: tbz w0, #0x0, 0x[[#%x,TAIL:]]
# A64-NEXT: [[#GUARD]]: {{.*}} adr x0, 0x[[#LEAF]]
# A64-NEXT: stp x30, x0, [sp, #-0x10]!
# A64-NEXT: bl 0x[[#%x,ENSURE:]]
# A64-NEXT: ldr x30, [sp], #0x10
# A64-NEXT: [[#TAIL]]: {{.*}} adrp x0, 0x[[#PAGE]]
# A64-NEXT: b 0x[[#BACK]]
## So in a frame, with the site's own register.
# A64:      stp x29, x30, [sp, #-0x10]!
# A64-NEXT: mov x29, sp
# A64-NEXT: adrp x8, 0x[[#PAGE]]
# A64-NEXT: ldr x8, [x8, #0x28]
# A64-NEXT: tbnz w8, #0x0, 0x[[#%x,]]
# A64-NEXT: adrp x8, 0x[[#PAGE]]
# A64-NEXT: ldr x8, [x8, #0x10]
## Code that cannot grow, as a foreign object's, branches to its guard, which
## tests the gate itself.
# A64:      str x19, [sp, #0x8]
# A64-NEXT: [[#%x,SITE:]]: {{.*}} b 0x[[#%x,DETOUR:]]
# A64-NEXT: [[#%x,BACK:]]: {{.*}} ldr x0, [x0, #0x8]
# A64:      [[#DETOUR]]: {{.*}} adrp x0, 0x[[#PAGE]]
# A64-NEXT: ldr x0, [x0, #0x28]
# A64-NEXT: tbz w0, #0x0, 0x[[#%x,TAIL:]]
# A64-NEXT: adr x0, 0x[[#SITE]]
# A64-NEXT: stp x30, x0, [sp, #-0x10]!
# A64-NEXT: bl 0x[[#ENSURE]]
# A64-NEXT: ldr x30, [sp], #0x10
# A64-NEXT: [[#TAIL]]: {{.*}} adrp x0, 0x[[#PAGE]]
# A64-NEXT: b 0x[[#BACK]]
## So is a function whose unwind data is packed.
# A64-NEXT: str x19, [sp, #-0x10]!
# A64-NEXT: b 0x[[#%x,PACKED:]]
# A64:      [[#ENSURE]]: a9bf7bfd stp x29, x30, [sp, #-0x10]!
# A64-NEXT: mov x29, sp
# A64-NEXT: sub sp, sp, #0x2a0
# A64:      stp q30, q31, [sp, #0x280]
# A64-NEXT: adrp x0, 0x[[#%x,]]
# A64-NEXT: add x0, x0
# A64-NEXT: adrp x16, 0x180001000
# A64-NEXT: add x16, x16, #0x[[#%x,]]
# A64-NEXT: blr x16
# A64:      add sp, sp, #0x2a0
# A64-NEXT: mov sp, x29
# A64-NEXT: ldp x29, x30, [sp], #0x10
# A64-NEXT: ret

## Version 2 on x86-64: the tail merge's epilogue is its jump through rax,
## the ensure stub's its last pop and return.
# X64:          Version: 2
# X64:          PrologSize: 24
# X64:            0x01: EPILOG atend=no, length=0x1
# X64-NEXT:       0x02: EPILOG offset=0x2
# X64-NEXT:       0x18: ALLOC_SMALL size=104
## A leaf's one push, and its epilogue, which its slow path's call follows.
# X64:      StartAddress: (0x[[#%X,LEAF]])
# X64-NEXT: EndAddress: (0x[[#%X,LEAF+41]])
# X64:        Version: 2
# X64:        PrologSize: 1
# X64:          0x02: EPILOG atend=no, length=0x2
# X64-NEXT:     0x09: EPILOG offset=0x9
# X64-NEXT:     0x01: PUSH_NONVOL reg=RAX
# X64:      StartAddress: (0x[[#%X,LEAF2]])
# X64:          0x01: PUSH_NONVOL reg=RCX
# X64:      StartAddress: (0x[[#%X,LEAF3]])
# X64-NEXT: EndAddress: (0x[[#%X,LEAF3+44]])
# X64:      StartAddress: (0x[[#%X,ENSURE]])
# X64-NEXT: EndAddress: (0x[[#%X,ENSURE+140]])
# X64:        Version: 2
# X64:        PrologSize: 4
# X64-NEXT:   FrameRegister: RBP
# X64:          0x02: EPILOG atend=yes, length=0x2
# X64-NEXT:     0x00: EPILOG padding
# X64-NEXT:     0x04: SET_FPREG reg=RBP, offset=0x0
# X64-NEXT:     0x01: PUSH_NONVOL reg=RBP
## A guard's head and tail run the codes that ntdll runs at its site, none
## for a function with no entry, after an end_c. The call and the restore
## take the site's address for the PC, then x30 and the 16 bytes.
# A64:        Function: 0x[[#%X,GUARD-12]]
# A64:        FunctionLength: 20
# A64:        Prologue [
# A64-NEXT:     0xe5 ; end_c
# A64-NEXT:     0xe4 ; end
# A64:        Function: 0x[[#%X,GUARD+8]]
# A64:        FunctionLength: 8
# A64:        Prologue [
# A64-NEXT:     0xe5 ; end_c
# A64-NEXT:     0xd2c1 ; str x30, [sp, #8]
# A64-NEXT:     0xec ; clear unwound to call
# A64-NEXT:     0xd2c0 ; str x30, [sp, #0]
# A64-NEXT:     0x01 ; sub sp, #16
# A64-NEXT:     0xe4 ; end
# A64:        Function: 0x[[#%X,GUARD+16]]
# A64:        FunctionLength: 8
# A64:        Prologue [
# A64-NEXT:     0xe5 ; end_c
# A64-NEXT:     0xe4 ; end
## In the frame, whose function grew by the guard's 12 bytes, the frame's.
# A64:        FunctionLength: 40
# A64:        FunctionLength: 20
# A64:        Prologue [
# A64-NEXT:     0xe5 ; end_c
# A64-NEXT:     0xe1 ; mov fp, sp
# A64-NEXT:     0x81 ; stp x29, x30, [sp, #-16]!
# A64-NEXT:     0xe4 ; end
## The packed function's guard runs the codes ntdll expands its data to.
# A64:        Function: 0x[[#%X,PACKED]]
# A64:        FunctionLength: 20
# A64:        Prologue [
# A64-NEXT:     0xe5 ; end_c
# A64-NEXT:     0xd401 ; str x19, [sp, #-16]!
# A64-NEXT:     0xe4 ; end
# A64:        FunctionLength: 272
# A64:        EpiloguePacked: Yes
# A64:        Prologue [
# A64-NEXT:     0xc02a ; sub sp, #672
# A64-NEXT:     0xe1 ; mov fp, sp
# A64-NEXT:     0x81 ; stp x29, x30, [sp, #-16]!
# A64-NEXT:     0xe4 ; end
# A64-NEXT:   ]

## d.dll's descriptor, its gate's naming the list as the bound table and
## holding f's thunk (plus one on ARM64), e.dll's, then the runs'; every
## d.dll descriptor names one module handle.
# CHECK:      DelayImport {
# CHECK-NEXT:   Name: d.dll
# CHECK-NEXT:   Attributes: 0x1
# CHECK-NEXT:   ModuleHandle: 0x[[#%X,HANDLE:]]
# CHECK-NEXT:   ImportAddressTable: 0x7000
# CHECK:        Import {
# CHECK-NEXT:     Symbol: call (0)
# CHECK:          Symbol: f (1)
# CHECK-NEXT:     Address: 0x[[#%X,GATETHUNK:]]
# CHECK:          Symbol: g (2)
# CHECK:          Symbol: h (3)
# CHECK:      DelayImport {
# CHECK-NEXT:   Name: d.dll
# CHECK-NEXT:   Attributes: 0x1
# CHECK-NEXT:   ModuleHandle: 0x[[#HANDLE]]
# CHECK-NEXT:   ImportAddressTable: 0x7028
# CHECK-NEXT:   ImportNameTable:
# CHECK-NEXT:   BoundDelayImportTable: 0x22BC
# CHECK:        Import {
# CHECK-NEXT:     Symbol: f (1)
# X64-NEXT:       Address: 0x[[#GATETHUNK]]
# A64-NEXT:       Address: 0x[[#%X,GATETHUNK+1]]
# CHECK-NEXT:   }
# CHECK-NEXT: }
# CHECK-NEXT: DelayImport {
# CHECK-NEXT:   Name: e.dll
# CHECK:          Symbol: e (0)
# CHECK-NOT:      Symbol:
## The run of r1 goes on into r2, where a word other than zero follows it.
# CHECK:      DelayImport {
# CHECK-NEXT:   Name: d.dll
# CHECK-NEXT:   Attributes: 0x1
# CHECK-NEXT:   ModuleHandle: 0x[[#HANDLE]]
# CHECK-NEXT:   ImportAddressTable: 0x3000
# CHECK:        Import {
# CHECK-NEXT:     Symbol: f (1)
# CHECK:          Symbol: g (2)
# CHECK:          Symbol: f (1)
# CHECK:          Symbol: (65535)
# CHECK-NEXT:     Address: 0x[[#%X,MAIN:]]
# CHECK-NEXT:   }
# CHECK-NEXT: }
## r3's, which ends its section.
# CHECK-NEXT: DelayImport {
# CHECK-NEXT:   Name: d.dll
# CHECK-NEXT:   Attributes: 0x1
# CHECK-NEXT:   ModuleHandle: 0x[[#HANDLE]]
# CHECK-NEXT:   ImportAddressTable: 0x3030
# CHECK:        Import {
# CHECK-NEXT:     Symbol: g (2)
# CHECK-NEXT:     Address:
# CHECK-NEXT:   }
# CHECK-NEXT: }
## That of .zrun, which keeps its place.
# CHECK-NEXT: DelayImport {
# CHECK-NEXT:   Name: d.dll
# CHECK:        ImportAddressTable: 0x8000
# CHECK:        Import {
# CHECK-NEXT:     Symbol: g (2)
# CHECK-NEXT:     Address:
# CHECK-NEXT:   }
# CHECK-NEXT: }
# CHECK-NOT:  DelayImport {

# CHECK:      GuardFlags [
# CHECK:        PROTECT_DELAYLOAD_IAT
# CHECK:      GuardIatTable [
# CHECK-NEXT:   0x180003000
# CHECK-NEXT:   0x180003008
# CHECK-NEXT:   0x180003010
# CHECK-NEXT:   0x180003030
# CHECK-NEXT:   0x180007008
# CHECK-NEXT:   0x180007010
# CHECK-NEXT:   0x180008000
# CHECK-NEXT: ]

## The list: the DLL's and the runs' descriptors with first words and lengths,
## the writable word with f's entry and its thunk, and the gate's.
# CHECK:      Contents of section .rdata:
# CHECK:      {{ }}04000000 {{.*}}
# CHECK-NEXT: {{ }}01000000 [[#%x,]] 00700000 04000000
# CHECK-NEXT: {{ }}[[#%x,]] 00300000 03000000 [[#%x,]]
# CHECK-NEXT: {{ }}30300000 01000000 [[#%x,]] 00800000
# CHECK-NEXT: {{ }}01000000 00500000 08700000 [[THUNK:[0-9a-f]+]]0000
# CHECK-NEXT: {{ }}[[#%x,]] 28700000 01000000
## The runs are on pages of their own in .rdata: r1 and r2, 16 bytes of r2
## that are no run, r3 and its zero word.
# CHECK:      180003000 [[THUNK]]0080 01000000 {{[0-9a-f]+}} 01000000
# CHECK-NEXT: 180003010 [[THUNK]]0080 01000000 {{[0-9a-f]+}} 01000000
# CHECK-NEXT: 180003020 00000000 00000000 07000000 00000000
# CHECK-NEXT: 180003030 {{[0-9a-f]+}} 01000000 00000000 00000000
# CHECK-NEXT: 180003040 00000000
# CHECK:      180004000
# CHECK:      Contents of section .data:
# CHECK-NEXT: 180005000 [[THUNK]]0080 01000000 {{[0-9a-f]+}} 01000000
# CHECK:      Contents of section .zrun:
# CHECK-NEXT: 180008000 {{[0-9a-f]+}} 01000000 00000000 00000000

## The thunks' addresses stay under /delay:nocanonical, without a protected
## table, and where the image refers to the DLL-unloading function.
# RUN: %{link} d.lib e.lib main.obj rt.obj -out:no.dll -delay:nocanonical:D.DLL
# RUN: llvm-objdump -d no.dll > no.txt && %{dump} no.dll >> no.txt
# RUN: FileCheck --check-prefix=KEPT %s < no.txt
# RUN: %{mc} -triple=x86_64-windows-itanium rt.s --defsym UNSAFE=1 -o unsafe.obj
# RUN: lld-link -dll -noentry -include:main -delayload:d.dll -delayload:e.dll \
# RUN:   d.lib e.lib main.obj unsafe.obj -out:no.dll
# RUN: llvm-objdump -d no.dll > no.txt && %{dump} no.dll >> no.txt
# RUN: FileCheck --check-prefix=KEPT %s < no.txt
# RUN: %{link} d.lib e.lib main.obj rt.obj -out:no.dll -delay:unload \
# RUN:   -include:__FUnloadDelayLoadedDLL2
# RUN: llvm-objdump -d no.dll > no.txt && %{dump} no.dll >> no.txt
# RUN: FileCheck --check-prefix=KEPT %s < no.txt
## Without the reference the tables to restore take nothing away.
# RUN: %{link} d.lib e.lib main.obj rt.obj -out:unload.dll -delay:unload
# RUN: %{dump} unload.dll | FileCheck --check-prefix=UNLOAD %s

# KEPT:      <.text>:
# KEPT-NEXT: leaq
# KEPT-NEXT: movq
# KEPT-NOT:  xsavec64
# KEPT:      DelayImport {
# KEPT-NEXT:   Name: d.dll
# KEPT:      DelayImport {
# KEPT-NEXT:   Name: e.dll
# KEPT-NOT:  DelayImport {

# UNLOAD-COUNT-5: Name: d.dll

## A run is bound where it lies only in read-only data, a writable word only
## in writable data, and the runtime's function is needed.
# RUN: not %{link} d.lib e.lib main.obj rt.obj -out:bad.dll \
# RUN:   -merge:.rdata=.data 2>&1 | FileCheck --check-prefix=BAD %s
# BAD: error: main.obj: the address of f, delay-loaded from d.dll, at offset 0x0 of .rdata is in section .data, which is not read-only data, where the loader cannot bind it in place; keep its section apart from /merge and /section
# RUN: not %{link} d.lib e.lib main.obj rt.obj -out:bad.dll \
# RUN:   -section:.data,r 2>&1 | FileCheck --check-prefix=BAD2 %s
# BAD2: error: main.obj: the address of f, delay-loaded from d.dll, at offset 0x0 of .data is in section .data, which is read-only, where the runtime cannot write it; keep its section apart from /merge and /section
# RUN: %{mc} -triple=x86_64-windows-itanium -I %S/Inputs rt.s --defsym SAFE=1 \
# RUN:   -o safe.obj
# RUN: not %{link} d.lib e.lib main.obj safe.obj -out:bad.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=BAD3 %s
# BAD3: error: the address of a delay-loaded function is its DLL's own, which needs __llvm_delay_resolve; link the builtins library, or keep the thunk's with /delay:nocanonical:<dll>

## The runtime's function, with what it imports, is part of the image only
## where a stub calls it.
# RUN: lld-link -machine:x64 -def:ntdll.def -out:ntdll.lib
# RUN: %{mc} -triple=x86_64-windows-itanium resolve.s -o resolve.obj
# RUN: %{link} d.lib e.lib ntdll.lib main.obj safe.obj resolve.obj \
# RUN:   -out:resolve.dll
# RUN: llvm-readobj --coff-imports resolve.dll | FileCheck --check-prefix=RESOLVE %s
# RUN: %{link} d.lib e.lib ntdll.lib main.obj safe.obj resolve.obj \
# RUN:   -out:no.dll -delay:nocanonical:d.dll
# RUN: llvm-readobj --coff-imports no.dll | \
# RUN:   FileCheck --implicit-check-not=ntdll.dll --check-prefix=NO-RESOLVE %s
# RESOLVE:    Name: ntdll.dll
# RESOLVE:    Symbol: LdrResolveDelayLoadedAPI (
# NO-RESOLVE: Name: d.dll

## A site that reaches a delay-bound section tests the gate of each DLL whose
## words the section holds or, through words of data, addresses: t1 holds f
## and e, w an address in t1, and p one in t2, which holds g. Nothing guards
## t3, which holds h, whose address is no one's.
# RUN: %{mc} -triple=x86_64-windows-itanium closure.s -o closure.obj
# RUN: %{link} d.lib e.lib closure.obj rt.obj -out:closure.dll
# RUN: llvm-objdump -d closure.dll > closure.txt
# RUN: llvm-readobj --unwind closure.dll >> closure.txt
# RUN: FileCheck --check-prefix=CLOSURE %s < closure.txt
# RUN: %{mc} -triple=x86_64-windows-itanium closure.s --defsym BAD=1 -o bad.obj
# RUN: not %{link} d.lib e.lib bad.obj rt.obj -out:bad.dll 2>&1 | \
# RUN:   FileCheck --check-prefix=CLOSURE-BAD %s

## A lea of t1, a load from it and a load of p call leaves; the load's
## REX.W begins it, whatever byte comes before.
# CLOSURE:      <.text>:
# CLOSURE-NEXT: callq 0x[[#%x,LEA:]]
# CLOSURE-NEXT: nop
# CLOSURE-NEXT: b0 48 movb $0x48, %al
# CLOSURE-NEXT: callq 0x[[#%x,LOAD:]]
# CLOSURE-NEXT: nop
# CLOSURE-NEXT: callq 0x[[#%x,P:]]
# CLOSURE-NEXT: nop
## A fused read of t2's pointer is the lea of t2 behind a leaf, and the
## access through its register stays.
# CLOSURE-NEXT: callq 0x[[#%x,T2:]]
# CLOSURE-NEXT: nop
# CLOSURE-NEXT: movq 0x8(%rax), %rax
## A load from t2 before a test calls a leaf, no fact stating the flags dead.
# CLOSURE-NEXT: callq
# CLOSURE-NEXT: nop
# CLOSURE-NEXT: testq %rcx, %rcx
## The load of w's pointer before an add is inline: each gate's test, then
## the lea of w.
# CLOSURE-NEXT: leaq {{.*}}(%rip), %rcx {{.*}}# 0x[[#%x,DTHUNK:]]
# CLOSURE-NEXT: cmpq %rcx, {{.*}}(%rip) {{.*}}# 0x[[#%x,DGATE:]]
# CLOSURE-NEXT: jne
# CLOSURE-NEXT: callq 0x[[#%x,DENSURE:]]
# CLOSURE-NEXT: leaq {{.*}}(%rip), %rcx {{.*}}# 0x[[#%x,ETHUNK:]]
# CLOSURE-NEXT: cmpq %rcx, {{.*}}(%rip) {{.*}}# 0x[[#%x,EGATE:]]
# CLOSURE-NEXT: jne
# CLOSURE-NEXT: callq 0x[[#%x,EENSURE:]]
# CLOSURE-NEXT: leaq {{.*}}(%rip), %rcx
# CLOSURE-NEXT: addq %rcx, %rax
# CLOSURE-NEXT: leaq {{.*}}(%rip), %r9
## Other reads of t2 call leaves that replay them, a standalone lock with
## its instruction; a store to t2 stays.
# CLOSURE-NEXT: callq 0x[[#%x,CMP:]]
# CLOSURE-NEXT: nop
# CLOSURE-NEXT: callq 0x[[#%x,XCHG:]]
# CLOSURE-NEXT: nopl (%rax)
# CLOSURE-NEXT: movl %ecx, {{.*}}(%rip)
# CLOSURE-NEXT: callq
## The lea's leaf tests d.dll's gate, then e.dll's, and calls the ensure stub
## of each that is unbound.
# CLOSURE:      [[#LEA]]: 50 pushq %rax
# CLOSURE-NEXT: leaq {{.*}}(%rip), %rax {{.*}}# 0x[[#DTHUNK]]
# CLOSURE-NEXT: notq %rax
# CLOSURE-NEXT: movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#DGATE]]
# CLOSURE-NEXT: leaq 0x1(%rcx,%rax), %rcx
# CLOSURE-NEXT: jrcxz 0x[[#%x,DSLOW:]]
# CLOSURE-NEXT: [[#%x,EBACK:]]: {{.*}} leaq {{.*}}(%rip), %rax {{.*}}# 0x[[#ETHUNK]]
# CLOSURE-NEXT: notq %rax
# CLOSURE-NEXT: movq {{.*}}(%rip), %rcx {{.*}}# 0x[[#EGATE]]
# CLOSURE-NEXT: leaq 0x1(%rcx,%rax), %rcx
# CLOSURE-NEXT: jrcxz 0x[[#%x,ESLOW:]]
# CLOSURE-NEXT: [[#%x,VALUE:]]: {{.*}} leaq {{.*}}(%rip), %rcx {{.*}}# 0x[[#%x,T1:]]
# CLOSURE-NEXT: popq %rax
# CLOSURE-NEXT: retq
# CLOSURE-NEXT: [[#DSLOW]]: {{.*}} callq 0x[[#DENSURE]]
# CLOSURE-NEXT: jmp 0x[[#EBACK]]
# CLOSURE-NEXT: [[#ESLOW]]: {{.*}} callq 0x[[#EENSURE]]
# CLOSURE-NEXT: jmp 0x[[#VALUE]]
## The load's leaf loads t1's second word; p's tests d.dll's gate alone.
# CLOSURE:      [[#LOAD]]: 51 pushq %rcx
# CLOSURE:      jrcxz
# CLOSURE:      jrcxz
# CLOSURE-NEXT: movq {{.*}}(%rip), %rdx {{.*}}# 0x[[#T1+8]]
# CLOSURE:      [[#P]]: 51 pushq %rcx
# CLOSURE-NEXT: leaq {{.*}}(%rip), %rcx {{.*}}# 0x[[#DTHUNK]]
# CLOSURE:      jrcxz
# CLOSURE-NEXT: movq {{.*}}(%rip), %r8
# CLOSURE-NEXT: popq %rcx
# CLOSURE:      [[#T2]]: 51 pushq %rcx
# CLOSURE:      jrcxz
# CLOSURE-NEXT: leaq {{.*}}(%rip), %rax
## A replay saves rcx and rax, tests the gate, restores them and jumps to its
## instruction, after its function's end, and returns from it.
# CLOSURE:      [[#CMP]]: 51 pushq %rcx
# CLOSURE-NEXT: 50 pushq %rax
# CLOSURE-NEXT: leaq {{.*}}(%rip), %rcx {{.*}}# 0x[[#DTHUNK]]
# CLOSURE-NEXT: notq %rcx
# CLOSURE-NEXT: movq {{.*}}(%rip), %rax {{.*}}# 0x[[#DGATE]]
# CLOSURE-NEXT: leaq 0x1(%rax,%rcx), %rcx
# CLOSURE-NEXT: jrcxz 0x[[#%x,CSLOW:]]
# CLOSURE-NEXT: [[#%x,CBACK:]]: {{.*}} popq %rax
# CLOSURE-NEXT: popq %rcx
# CLOSURE-NEXT: jmp 0x[[#%x,CEXEC:]]
# CLOSURE-NEXT: [[#CSLOW]]: {{.*}} callq 0x[[#DENSURE]]
# CLOSURE-NEXT: jmp 0x[[#CBACK]]
# CLOSURE-NEXT: [[#CEXEC]]: {{.*}} cmpq %rcx, {{.*}}(%rip) {{.*}}# 0x[[#%x,T2W:]]
# CLOSURE-NEXT: retq
# CLOSURE-NEXT: [[#XCHG]]: 51 pushq %rcx
# CLOSURE:      jmp 0x[[#%x,XEXEC:]]
# CLOSURE:      [[#XEXEC]]: f0 lock
# CLOSURE-NEXT: 48 0f b1 0d {{.*}} cmpxchgq %rcx, {{.*}}(%rip) {{.*}}# 0x[[#T2W]]
# CLOSURE-NEXT: retq
## A leaf of two gates has its epilogue before two calls of ensure stubs.
# CLOSURE:      StartAddress: (0x[[#%X,LEA]])
# CLOSURE-NEXT: EndAddress: (0x[[#%X,LEA+72]])
# CLOSURE:        0x02: EPILOG atend=no, length=0x2
# CLOSURE-NEXT:   0x10: EPILOG offset=0x10
# CLOSURE-NEXT:   0x01: PUSH_NONVOL reg=RAX
## A replay's ends at its jump to the instruction, its epilogue the pops and
## the jump.
# CLOSURE:      StartAddress: (0x[[#%X,CMP]])
# CLOSURE-NEXT: EndAddress: (0x[[#%X,CEXEC]])
# CLOSURE:        0x04: EPILOG atend=no, length=0x4
# CLOSURE-NEXT:   0x0B: EPILOG offset=0xB
# CLOSURE-NEXT:   0x02: PUSH_NONVOL reg=RAX
# CLOSURE-NEXT:   0x01: PUSH_NONVOL reg=RCX

# CLOSURE-BAD: error: bad.obj: .text+0x64: IMAGE_REL_AMD64_REL32 reaches t2, which holds the address of a function of a delay-loaded DLL, in an instruction whose start its object does not state, as an object from an older compiler

## A function imported by ordinal alone: its run's word holds its load thunk
## until bound, as its entry and the gate do.
# RUN: lld-link -machine:x64 -def:o.def -out:o.lib
# RUN: %{mc} -triple=x86_64-windows-itanium ord.s -o ord.obj
# RUN: lld-link -dll -noentry -guard:cf -include:main -delayload:o.dll \
# RUN:   o.lib ord.obj rt.obj -out:ord.dll
# RUN: llvm-readobj --coff-imports ord.dll | FileCheck --check-prefix=ORD %s
# ORD:      Symbol:  (5)
# ORD-NEXT: Address: 0x[[#%X,T:]]
# ORD:      Symbol:  (5)
# ORD-NEXT: Address: 0x[[#T]]
# ORD:      Symbol:  (5)
# ORD-NEXT: Address: 0x[[#T]]

# RUN: lld-link -machine:arm64 -def:d.def -out:d-a64.lib
# RUN: lld-link -machine:arm64 -def:e.def -out:e-a64.lib
# RUN: cat arm64.s data.s > main-a64.s
# RUN: %{mc} -triple=aarch64-windows-itanium main-a64.s -o main-a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium -I %S/Inputs rt.s -o rt-a64.obj
# RUN: %{mc} -triple=aarch64-windows-msvc foreign-a64.s -o foreign-a64.obj
# RUN: %{link} d-a64.lib e-a64.lib main-a64.obj rt-a64.obj foreign-a64.obj \
# RUN:   -include:leaf -include:framed -include:saves -include:other \
# RUN:   -include:packed -out:arm64.dll
# RUN: llvm-objdump -d arm64.dll > arm64.txt
# RUN: %{dump} arm64.dll >> arm64.txt
# RUN: llvm-objdump -s -j .rdata -j .data -j .zrun arm64.dll >> arm64.txt
# RUN: FileCheck --check-prefixes=CHECK,A64 %s < arm64.txt


#--- d.def
LIBRARY d.dll
EXPORTS
call
f
g
h

#--- e.def
LIBRARY e.dll
EXPORTS
e

#--- x64.s
.text
.globl main
main:
  leaq f(%rip), %rcx
  movq __imp_g(%rip), %rdx
  addq %rdx, %rax
  movq __imp_f(%rip), %r12
  movq __imp_f(%rip), %r17
  leaq h(%rip), %rcx
  callq call
  ret
## A fused read makes the code move.
  movq __imp_call@FUSED(%rip), %rax

#--- foreign.s
  .def other; .scl 2; .type 32; .endef
  .text
  .globl other
other:
  leaq f(%rip), %rcx
  ret

#--- closure.s
.text
.globl main
main:
  leaq t1(%rip), %rcx
  movb $0x48, %al
  movq t1+8(%rip), %rdx
  movq p(%rip), %r8
  movq __imp_t2@FUSED(%rip), %rax
  .reloc ., IMAGE_REL_LLVM_FUSED_ACCESS, __imp_t2
  movq 8(%rax), %rax
  movq t2(%rip), %rcx
  testq %rcx, %rcx
  movq __imp_w(%rip), %rcx
  addq %rcx, %rax
  leaq t3(%rip), %r9
  cmpq %rcx, t2(%rip)
  lock; cmpxchgq %rcx, t2(%rip)
  movl %ecx, t2(%rip)
  callq call
  ret
  movq __imp_call@FUSED(%rip), %rax
.ifdef BAD
  .byte 0x2e
  cmpq %rcx, t2(%rip)
.endif
.addrsig
.addrsig_sym f
.addrsig_sym g
.addrsig_sym e
.globl t1, t2, p, w, t3
.section .rdata,"dr",one_only,t1
t1:
  .quad f
  .quad e
.section .rdata,"dr",one_only,t3
t3:
  .quad h
.data
.p2align 3
t2:
  .quad g
.section .data,"dw",one_only,p
p:
  .quad t2
.section .data,"dw",one_only,w
w:
  .quad t1
.section .drectve,"yn"
  .ascii " -include:t1 -include:p -include:w -include:t3"

#--- o.def
LIBRARY o.dll
EXPORTS
n @5 NONAME

#--- ord.s
.text
.globl main
main:
  ret
.addrsig
.addrsig_sym n
.section .rdata,"dr",one_only,ro
.globl ro
ro:
  .quad n
.section .drectve,"yn"
  .ascii " -include:ro"

#--- arm64.s
.text
.globl main
main:
  adrp x0, f
  add x0, x0, :lo12:f
  adrp x1, h
  add x1, x1, :lo12:h
  bl call
  ret

.section .text,"xr",one_only,leaf
.globl leaf
leaf:
  adrp x9, __imp_f
  ldr x0, [x9, :lo12:__imp_f]
  ret

.section .text,"xr",one_only,framed
.globl framed
.seh_proc framed
framed:
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  mov x29, sp
  .seh_set_fp
  .seh_endprologue
  adrp x8, __imp_g
  ldr x8, [x8, :lo12:__imp_g]
  bl call
  .seh_startepilogue
  ldp x29, x30, [sp], #16
  .seh_save_fplr_x 16
  .seh_endepilogue
  ret
.seh_endproc

.section .text,"xr",one_only,saves
.globl saves
.seh_proc saves
saves:
  str x19, [sp, #-16]!
  .seh_save_reg_x x19, 16
  .seh_endprologue
  adrp x19, __imp_f
  ldr x19, [x19, :lo12:__imp_f]
  mov x0, x19
  .seh_startepilogue
  ldr x19, [sp], #16
  .seh_save_reg_x x19, 16
  .seh_endepilogue
  ret
.seh_endproc

#--- foreign-a64.s
.section .text,"xr",one_only,other
.globl other
.seh_proc other
other:
  sub sp, sp, #16
  .seh_stackalloc 16
  str x19, [sp, #8]
  .seh_save_reg x19, 8
  .seh_endprologue
  adrp x0, f
  add x0, x0, :lo12:f
  .seh_startepilogue
  ldr x19, [sp, #8]
  .seh_save_reg x19, 8
  add sp, sp, #16
  .seh_stackalloc 16
  .seh_endepilogue
  ret
.seh_endproc
.section .text,"xr",one_only,packed
.globl packed
.seh_proc packed
packed:
  str x19, [sp, #-16]!
  .seh_save_reg_x x19, 16
  .seh_endprologue
  adrp x0, g
  add x0, x0, :lo12:g
  .seh_startepilogue
  ldr x19, [sp], #16
  .seh_save_reg_x x19, 16
  .seh_endepilogue
  ret
.seh_endproc

#--- data.s
.addrsig
.addrsig_sym f
.addrsig_sym g
.globl r1, r2, r3, r4
.section .rdata,"dr",one_only,r1
r1:
  .quad f
  .quad g
.section .rdata,"dr",one_only,r2
r2:
  .quad f
  .quad main
  .quad 0
  .quad 7
.section .rdata,"dr",one_only,r3
r3:
  .quad g
.section .rdata,"dr",one_only,r4
r4:
  .quad h
  .quad e
.section .zrun,"dr"
  .quad g
.data
.p2align 3
  .quad f
  .quad h
.section .drectve,"yn"
  .ascii " -include:r1 -include:r2 -include:r3 -include:r4"

#--- rt.s
.text
.ifndef SAFE
.globl __llvm_delay_resolve
__llvm_delay_resolve:
.endif
.globl __delayLoadHelper2
__delayLoadHelper2:
  ret
.section .text,"xr",one_only,__FUnloadDelayLoadedDLL2
.globl __FUnloadDelayLoadedDLL2
__FUnloadDelayLoadedDLL2:
  ret
.ifndef UNSAFE
.data
.globl __MSSafeDelayLoadIAT
__MSSafeDelayLoadIAT:
  .long 0
.include "loadconfig-cfg-iat.s"
.endif

#--- ntdll.def
LIBRARY ntdll.dll
EXPORTS
LdrResolveDelayLoadedAPI

#--- resolve.s
.section .text,"xr",one_only,__llvm_delay_resolve
.globl __llvm_delay_resolve
__llvm_delay_resolve:
  jmpq *__imp_LdrResolveDelayLoadedAPI(%rip)
