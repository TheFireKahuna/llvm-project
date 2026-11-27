; REQUIRES: x86, aarch64

;; A DLL offers (exports every non-hidden name of) the vague entities it keeps,
;; decided before the offers are added; an executable offers nothing.

; RUN: rm -rf %t && split-file %s %t && cd %t

; DEFINE: %{t} = x86_64
; DEFINE: %{dll} = lld-link -dll -noentry
; DEFINE: %{exports} = llvm-readobj --coff-exports
; DEFINE: %{run} = \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj a.ll -o a.obj && \
; DEFINE:   llc -mtriple=%{t}-windows-itanium -filetype=obj b.ll -o b.obj && \
; DEFINE:   %{dll} a.obj -out:a.dll -implib:a.lib && \
; DEFINE:   %{exports} a.dll | FileCheck --check-prefix=OFFERS %s \
; DEFINE:     --implicit-check-not=Name: && \
; DEFINE:   llvm-nm a.lib | FileCheck --check-prefix=LIB %s && \
; DEFINE:   lld-link -entry:use -subsystem:console a.obj -out:exe.exe && \
; DEFINE:   %{exports} exe.exe | FileCheck --check-prefix=EXE %s \
; DEFINE:     --implicit-check-not=Name: && \
; DEFINE:   opt -mtriple=%{t}-windows-itanium a.ll -o a.bc && \
; DEFINE:   %{dll} a.bc -out:lto.dll -implib:lto.lib && \
; DEFINE:   %{exports} lto.dll | FileCheck --check-prefix=OFFERS %s \
; DEFINE:     --implicit-check-not=Name: && \
; DEFINE:   %{dll} a.obj b.obj -out:ab.dll -implib:ab.lib && \
; DEFINE:   %{dll} b.obj a.obj -out:ab.dll -implib:ba.lib && \
; DEFINE:   cmp ab.lib ba.lib && \
; DEFINE:   %{dll} b.obj a.lib -out:b.dll -implib:b.lib && \
; DEFINE:   %{exports} b.dll | FileCheck --check-prefix=FORWARD %s && \
; DEFINE:   %{dll} b.obj a.lib -out:b.dll -implib:b.lib -export:iv,DATA && \
; DEFINE:   %{exports} b.dll | FileCheck --check-prefix=FORWARD %s && \
; DEFINE:   %{dll} a.obj -out:a.dll -implib:ord.lib -export:iv,@7,NONAME,DATA && \
; DEFINE:   %{dll} b.obj ord.lib -out:b.dll -implib:b.lib && \
; DEFINE:   %{exports} b.dll | FileCheck --check-prefix=ORDINAL %s && \
; DEFINE:   %{dll} a.obj -out:a.dll -implib:dot.lib -export:iv,DATA,EXPORTAS,i.v && \
; DEFINE:   not %{dll} b.obj dot.lib -out:b.dll -implib:b.lib 2>&1 \
; DEFINE:     | FileCheck --check-prefix=DOT %s

; RUN: %{run}
; REDEFINE: %{t} = aarch64
; RUN: %{run}

;; Offered: iv with guard, interior name and visible aliases, prot, _ZTI1A, the
;; address-taken fn; not dead, hid, call, _ZTS1A or the vtable; LTO alike.
; OFFERS: Name: _ZGV2iv
; OFFERS: Name: _ZTI1A
; OFFERS: Name: exp
; OFFERS: Name: fn
; OFFERS: Name: iv
; OFFERS: Name: iv$so4
; OFFERS: Name: prot
; OFFERS: Name: use

;; The import library holds a member per offer, a function's with its thunk.
; LIB-DAG: D __imp_iv
; LIB-DAG: D __imp__ZGV2iv
; LIB-DAG: D __imp_iv$so4
; LIB-DAG: T fn

; EXE: Name: exp
; EXE: Name: use

;; Reading another DLL's offer forwards the name there (by ordinal where it has
;; no name, never to a dotted name), and offers only what it alone keeps.
; FORWARD:      Name: iv
; FORWARD-NEXT: ForwardedTo: a.dll.iv
; FORWARD:      Name: own
; FORWARD-NOT:  ForwardedTo

; ORDINAL:      Name: iv
; ORDINAL-NEXT: ForwardedTo: a.dll.#7

; DOT: error: dot.lib(a.dll): cannot forward the offer of iv, which a.dll exports as i.v, a name with a dot

;; An entity an ARM64EC image would offer is an error.
; RUN: llc -mtriple=x86_64-windows-itanium -filetype=obj ec.ll -o ec.obj
; RUN: not lld-link -machine:arm64ec -dll -noentry ec.obj -out:ec.dll 2>&1 \
; RUN:   | FileCheck --check-prefix=EC %s
; EC: error: ec.obj: iv is a vague entity, which an ARM64EC or ARM64X image cannot share with another; give it hidden visibility

;; A definition a version script localises or --exclude-libs hides is not
;; offered.
; RUN: lld-link -dll -noentry a.obj -out:local.dll -version-script:fn.map
; RUN: llvm-readobj --coff-exports local.dll | FileCheck --check-prefix=LOCAL %s
; RUN: rm -f a.a && llvm-ar rcs a.a a.obj
; RUN: lld-link -dll -noentry a.a -include:use -out:excluded.dll \
; RUN:   -exclude-libs:a.a
; RUN: llvm-readobj --coff-exports excluded.dll | FileCheck --check-prefix=NONE %s
; LOCAL:     Name: _ZTI1A
; LOCAL-NOT: Name: fn
; LOCAL:     Name: iv
; NONE-NOT:  Name:

;--- fn.map
{ local: fn; };

;--- a.ll
$iv = comdat any
$dead = comdat any
$hid = comdat any
$prot = comdat any
$exp = comdat any
$fn = comdat any
$call = comdat any
$_ZTV1A = comdat any
$_ZTI1A = comdat any
$_ZTS1A = comdat any

@iv = linkonce_odr global [2 x i32] [i32 1, i32 2], comdat
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)
@"iv$so4" = alias i32, getelementptr (i32, ptr @iv, i64 1)
@dead = linkonce_odr global i32 3, comdat
@hid = linkonce_odr hidden global i32 4, comdat
@prot = weak_odr protected global i32 5, comdat
@exp = weak_odr dllexport global i32 6, comdat
@exph = hidden alias i32, ptr @exp
@_ZTV1A = linkonce_odr unnamed_addr constant [1 x ptr] [ptr @_ZTI1A], comdat
@_ZTI1A = linkonce_odr constant ptr @_ZTS1A, comdat #0
@_ZTS1A = linkonce_odr constant [3 x i8] c"1A\00", comdat

define linkonce_odr void @fn() comdat {
  ret void
}

define linkonce_odr void @call() local_unnamed_addr comdat {
  ret void
}

define dllexport ptr @use() {
  call void @call()
  %a = load volatile i32, ptr @iv
  %b = load volatile i64, ptr @_ZGV2iv
  %c = load volatile i32, ptr @hid
  %d = load volatile i32, ptr @prot
  %e = load volatile ptr, ptr @_ZTV1A
  ret ptr @fn
}

attributes #0 = { "type-info" }

;--- b.ll
$iv = comdat any
$own = comdat any
$fn = comdat any

@iv = linkonce_odr global [2 x i32] [i32 1, i32 2], comdat
@_ZGV2iv = linkonce_odr global i64 0, comdat($iv)
@"iv$so4" = alias i32, getelementptr (i32, ptr @iv, i64 1)
@own = linkonce_odr global i32 7, comdat

define linkonce_odr void @fn() comdat {
  ret void
}

define dllexport ptr @useb() {
  %a = load volatile i32, ptr @iv
  %b = load volatile i32, ptr @own
  ret ptr @fn
}

;--- ec.ll
$iv = comdat any
@iv = linkonce_odr global i32 1, comdat
@keep = dllexport global ptr @iv
