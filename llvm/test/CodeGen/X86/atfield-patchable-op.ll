; RUN: llc -mtriple=x86_64-unknown-linux-gnu -atfield-anchor-preparation -atfield-payload-ordinal=4 -o %t.s %s
; RUN: FileCheck %s < %t.s

; CHECK-LABEL: .atfield_function_begin 4, 0
; CHECK: .atfield_unit_begin 0, 0, 3, 0
; CHECK: xchgw
; CHECK-NOT: .atfield_unit_begin 0, 1, 1, 0
; CHECK: retq
; CHECK: .atfield_function_end

define void @patchable_op_pair() #0 {
entry:
  ret void
}

attributes #0 = { "atfield-function-ordinal"="0" "patchable-function"="prologue-short-redirect" }
