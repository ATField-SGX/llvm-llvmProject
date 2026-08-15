; RUN: llc -mtriple=x86_64-unknown-linux-gnu -atfield-anchor-preparation -atfield-payload-ordinal=5 -o %t.s %s
; RUN: FileCheck %s < %t.s
; RUN: llvm-mc -triple=x86_64-unknown-linux-gnu -filetype=obj -o %t.o %t.s
; RUN: llvm-objdump -s -j .note.atfield.anchors %t.o | FileCheck %s --check-prefix=NOTE

; CHECK-LABEL: .atfield_function_begin 5, 0
; CHECK: .atfield_unit_begin 1, 0, 2, 0
; CHECK: .atfield_unit_begin 0, 1, 1, 0
; CHECK: retq
; CHECK: .atfield_function_end
; NOTE: 41464e32 01005000 03000000

define void @empty_inline() #0 {
entry:
  call void asm sideeffect "", ""()
  ret void
}

attributes #0 = { "atfield-function-ordinal"="0" }
