%ifdef CONFIG
{
  "RegData": {
    "RAX": "0"
  }
}
%endif

; SPDX-License-Identifier: MIT
; Untranslatable code reached while single-stepping: with the trap flag set the
; dispatcher compiles one instruction at a time (CompileSingleStep), a third path that
; used to jump to host address 0 when nothing could be translated. `int 0x80` in a
; 64-bit process is refused by FEX, so the run must stop with
; FEXMAC_STOP_UNSUPPORTED_INSTRUCTION at it. Its correct outcome is that stop, recorded in
; unittests/ASM/Known_Failures_macOS.
mov rsp, 0xe0001000
pushfq
or qword [rsp], 0x100
mov rax, 265
popfq
int 0x80
hlt
