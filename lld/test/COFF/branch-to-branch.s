# REQUIRES: x86, aarch64

## A fork object's direct branch to an unconditional jump is retargeted to its
## destination, up to five deep; branches with addends, to calls, REL32_CALL
## fields, jumps to absolute zero or to overrides, and foreign ones are left.

# RUN: rm -rf %t.dir && split-file %s %t.dir && cd %t.dir
# DEFINE: %{mc} = llvm-mc -filetype=obj
# DEFINE: %{link} = lld-link -entry:main -subsystem:console -debug:symtab \
# DEFINE:   -out:out.exe
# DEFINE: %{dump} = llvm-objdump -d --no-show-raw-insn out.exe

# RUN: %{mc} -triple=x86_64-windows-itanium x64.s -o x64.obj
# RUN: %{link} x64.obj
# RUN: %{dump} | FileCheck --check-prefix=X64 %s
# RUN: llvm-objdump -s -j .tbl out.exe | FileCheck --check-prefix=X64-DATA %s

## Calls made direct through f's and h's pointers reach h; typed and odd are not
## at a jmp's opcode, patched is writable, viashort is passed past its short jmp.
# X64-LABEL: <main>:
# X64-NEXT:  callq 0x[[#%x,H:]] <h>
# X64-NEXT:  jmp 0x[[#H]] <h>
# X64-NEXT:  callq 0x{{.*}} <c{{[12]}}>
# X64-NEXT:  callq 0x{{.*}} <f+0x1>
# X64-NEXT:  callq 0x{{.*}} <calls>
# X64-NEXT:  callq 0x[[#H]] <h>
# X64-NEXT:  callq 0x[[#H]] <h>
# X64-NEXT:  callq 0x{{.*}} <typed>
# X64-NEXT:  callq 0x{{.*}} <odd>
# X64-NEXT:  callq 0x{{.*}} <patched>
# X64-NEXT:  callq 0x{{.*}} <viashort+0x7>
# X64:       <c2>:
# X64-NEXT:  jmp 0x{{.*}} <c{{[12]}}>
# X64:       <g>:
# X64-NEXT:  jmp 0x[[#H]] <h>
# X64:       <f>:
# X64-NEXT:  jmp 0x[[#H]] <h>
## The load of f's pointer and its call, the jne to f, the je to a jump to f.
# X64:       <pair>:
# X64-NEXT:  callq 0x[[#H]] <h>
# X64:       <short>:
# X64-NEXT:  jne 0x[[#H]] <h>
# X64-NEXT:  je 0x[[#H]] <h>
# X64:       <c1>:
# X64-NEXT:  jmp 0x{{.*}} <c{{[12]}}>

## The REL32_CALL field, which holds no addend, still takes f's distance
## from its end, not h's.
# X64-DATA: 140004000 43d0ffff

# RUN: %{mc} -triple=aarch64-windows-itanium a64.s -o a64.obj
# RUN: %{link} -machine:arm64 a64.obj
# RUN: %{dump} | FileCheck --check-prefix=A64 %s

## toweak's jump to absolute zero is not followed.
# A64-LABEL: <main>:
# A64-NEXT:  bl 0x[[#%x,H:]] <h>
# A64-NEXT:  b 0x[[#H]] <h>
# A64-NEXT:  b.eq 0x[[#H]] <h>
# A64-NEXT:  tbz w0, #0x0, 0x[[#H]] <h>
# A64-NEXT:  bl 0x{{.*}} <calls>
# A64-NEXT:  bl 0x{{.*}} <toweak>

# RUN: %{mc} -triple=aarch64-windows-itanium reach.s -o reach.obj
# RUN: %{link} -machine:arm64 -verbose reach.obj 2>&1 | \
# RUN:   FileCheck --check-prefix=REACH-LOG %s
# RUN: %{dump} | FileCheck --check-prefix=REACH %s

## The tbz that far leaves out of its reach goes back to the jump it was taken
## past, with no thunk; the b.eq past the same jump and the tbz to near stay
## retargeted.
# REACH-LOG: Added 0 thunks with margin {{.*}} in 1 passes
# REACH-LABEL: <main>:
# REACH-NEXT:  tbz w0, #0x0, 0x{{.*}} <jfar>
# REACH-NEXT:  b.eq 0x{{.*}} <far>
# REACH-NEXT:  tbz w1, #0x0, 0x{{.*}} <near>

## A foreign object's branch, or a branch to its jump, is not retargeted.
# RUN: %{mc} -triple=aarch64-windows-itanium caller.s -o caller.obj
# RUN: %{mc} -triple=aarch64-windows-itanium callee.s -o callee.obj
# RUN: %{mc} -triple=aarch64-windows-msvc caller.s -o caller-foreign.obj
# RUN: %{mc} -triple=aarch64-windows-msvc callee.s -o callee-foreign.obj
# RUN: %{link} -machine:arm64 caller.obj callee-foreign.obj
# RUN: %{dump} | FileCheck --check-prefix=FOREIGN %s
# RUN: %{link} -machine:arm64 caller-foreign.obj callee.obj
# RUN: %{dump} | FileCheck --check-prefix=FOREIGN %s

# FOREIGN-LABEL: <main>:
# FOREIGN-NEXT:  bl 0x{{.*}} <f>

## A branch to a jump to a function override's stub reaches the default body
## as a site of its record, as the jump does.
# RUN: %{mc} -triple=x86_64-windows-itanium fo.s --defsym X64=1 -o fo-x64.obj
# RUN: %{mc} -triple=x86_64-windows-msvc -I %S/Inputs fo-def.s -o fo-def-x64.obj
# RUN: %{mc} -triple=x86_64-windows-itanium %S/Inputs/loadconfig-cfg-x64.s \
# RUN:   -o lc-x64.obj
# RUN: %{link} -funcoverride fo-x64.obj fo-def-x64.obj lc-x64.obj
# RUN: %{dump} | FileCheck --check-prefix=FO %s
# RUN: %{mc} -triple=aarch64-windows-itanium fo.s -o fo-a64.obj
# RUN: %{mc} -triple=aarch64-windows-msvc -I %S/Inputs fo-def.s -o fo-def-a64.obj
# RUN: %{mc} -triple=aarch64-windows-itanium %S/Inputs/loadconfig-arm64.s \
# RUN:   -o lc-a64.obj
# RUN: %{link} -machine:arm64 -funcoverride fo-a64.obj fo-def-a64.obj lc-a64.obj
# RUN: %{dump} | FileCheck --check-prefix=FO %s

# FO-LABEL: <main>:
# FO-NEXT:  {{callq|bl}} 0x[[#%x,BODY:]]
# FO-LABEL: <ff>:
# FO-NEXT:  {{jmp|b}} 0x[[#BODY]]
# FO-LABEL: <X>:
# FO-NEXT:  {{jmp|b}} 0x[[#BODY]]

## .text$m keeps its size (.Lb - .La would not fit its byte), so its short jump
## sent on to h, out of reach, goes to .Lmid as assembled.
# RUN: %{mc} -triple=x86_64-windows-itanium keep.s -o keep.obj
# RUN: %{link} keep.obj
# RUN: %{dump} | FileCheck --check-prefix=KEEP %s

# KEEP:      jmp 0x[[#%x,MID:]] <main+0x88>
# KEEP-NEXT: int3
# KEEP-NEXT: [[#MID]]: jmp 0x{{.*}} <h>

## Compiled code: a call to a function that only calls another in tail
## position reaches the other.
# RUN: llc -mtriple=x86_64-windows-itanium -function-sections -filetype=obj \
# RUN:   fwd.ll -o fwd-x64.obj
# RUN: %{link} fwd-x64.obj
# RUN: %{dump} | FileCheck --check-prefix=FWD %s
# RUN: llc -mtriple=aarch64-windows-itanium -function-sections -filetype=obj \
# RUN:   fwd.ll -o fwd-a64.obj
# RUN: %{link} -machine:arm64 fwd-a64.obj
# RUN: %{dump} | FileCheck --check-prefix=FWD %s

# FWD-LABEL: <main>:
# FWD:         {{callq|bl}} 0x{{.*}} <h>

#--- x64.s
  .globl main, f, h
main:
  call f
  jmp f
  call c1
  call f+1
  call calls
  call viaptr
  call *__imp_f(%rip)
  call typed
  call odd
  call patched
  call viashort
  ret
  .section .text$f,"xr"
f:
  jmp g
  .section .text$e,"xr"
g:
  jmp h
  .section .text$d,"xr"
h:
  ret
  .section .text$z,"xr"
c1:
  jmp c2
  .section .text$a,"xr"
c2:
  jmp c1
  .section .text$calls,"xr"
calls:
  call h
  ret
  .section .text$viaptr,"xr"
viaptr:
  jmp *__imp_h(%rip)
  .section .text$typed,"xr"
  jne h
typed = . - 5
  .byte 0xe9
  jne h
odd = . - 7
  ret
## Short jumps across resizable code, as sites and at destinations; the fused
## read at the end makes the section's code movable.
  .section .text$short,"xr"
  .globl short
short:
  jne f
  je .Ls
  call *__imp_h(%rip)
.Ls:
  jmp f
viashort:
  jmp .Lv
  call *__imp_h(%rip)
.Lv:
  ret
  movq __imp_h@FUSED(%rip), %rax
## A load and its call that the link binds.
  .section .text$pair,"xr"
  .globl pair
pair:
  movq __imp_f@LOAD_CALL(%rip), %rax
  callq *%rax
  retq
  .section .patch,"xrw"
patched:
  jmp h
  .section .tbl,"xr"
  .long f@REL32_CALL - 4

#--- keep.s
  .section .text$a,"xr"
near:
  retq
  .section .text$m,"xr"
  .globl main, h
main:
  je near
.La:
  .fill 100, 1, 0x90
  .p2align 4
  .fill 21, 1, 0x90
.Lb:
  jmp .Lmid
  int3
.Lmid:
  jmp h
  movq __imp_h@FUSED(%rip), %rax
  .section .text$z,"xr"
  .fill 200, 1, 0x90
h:
  retq
  .section .rdata,"dr"
  .byte .Lb - .La

#--- a64.s
  .globl main
main:
  bl f
  b f
  b.eq f
  tbz w0, #0, f
  bl calls
  bl toweak
  ret
  .section .text$f,"xr"
f:
  b g
  .section .text$e,"xr"
g:
  b h
  .section .text$d,"xr"
h:
  ret
  .section .text$calls,"xr"
calls:
  bl h
  ret
  .section .text$weak,"xr"
toweak:
  b absent
  .weak absent

#--- reach.s
  .globl main
main:
  tbz w0, #0, jfar
  b.eq jfar
  tbz w1, #0, jnear
  ret
  .section .text$a,"xr"
near:
  ret
  .section .text$b,"xr"
jfar:
  b far
  .section .text$c,"xr"
jnear:
  b near
  .section .text$d,"xr"
  .space 0x8000
far:
  ret

#--- caller.s
  .globl main
main:
  bl f
  ret

#--- callee.s
  .globl f
h:
  ret
  .section .text$f,"xr"
f:
  b h

#--- fo.s
  .text
  .globl main
main:
.ifdef X64
  call ff
.else
  bl ff
.endif
  ret
  .section .text$ff,"xr"
ff:
.ifdef X64
  jmp X
.else
  b X
.endif

## X is a function override of another producer's object, and so a stub.
#--- fo-def.s
  .include "function-override-x.s"
  .rva c1
  .section .text$X,"xr"
  .globl X, c1
X:
  ret
  .section .text$c1,"xr"
c1:
  ret

#--- fwd.ll
define dso_local void @h() {
  ret void
}

define dso_local void @f() {
  tail call void @h()
  ret void
}

define i32 @main() {
  call void @f()
  ret i32 0
}
