; RUN: llc -mtriple=x86_64-unknown-linux-gnu -atfield-anchor-preparation -atfield-payload-ordinal=3 -o %t.s %s
; RUN: FileCheck %s < %t.s

; CHECK-LABEL: .atfield_function_begin 3, 0
; CHECK: .atfield_unit_begin 0, 0, 3, 0
; CHECK: xchgw
; CHECK: .atfield_unit_begin 1, 2, 2, 0
; CHECK: nop
; CHECK: .atfield_unit_end 2
; CHECK: callq
; CHECK: .atfield_function_end

define void @patchable_inline() #0 {
entry:
  call void asm sideeffect "nop", "~{dirflag},~{fpsr},~{flags}"()
  call void @callee()
  ret void
}

declare void @callee()

attributes #0 = { "atfield-function-ordinal"="0" "patchable-function-entry"="2" }
