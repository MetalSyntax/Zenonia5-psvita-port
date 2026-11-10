    .syntax unified
    .arm
    .text

@ Import sin implementar: el thunk deja su índice en r12. Registra el nombre y devuelve 0
@ (r0 = r1 = 0, que también sirve como "nil" para ObjC y 0 para retornos de 64 bits).
    .global ml_unimpl_entry
    .type ml_unimpl_entry, %function
ml_unimpl_entry:
    push    {r4, lr}
    mov     r4, sp
    bic     sp, sp, #7          @ ml_unimpl_log es código de la Vita (ver ml_align_call)
    mov     r0, r12
    blx     ml_unimpl_log
    mov     sp, r4
    pop     {r4, lr}
    mov     r0, #0
    mov     r1, #0
    bx      lr

@ uint32_t call_guest(void *fn, a0, a1, a2, a3): llama al juego guardando r9, que el código
@ iOS 3 trata como temporal.
    .global call_guest
    .type call_guest, %function
call_guest:
    push    {r4, r9, r10, lr}
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldr     r3, [sp, #16]
    blx     r12
    pop     {r4, r9, r10, pc}

@ uint32_t call_guest6(void *fn, a0, a1, a2, a3, a4, a5): igual que call_guest, con 2 argumentos
@ más por pila (render callback de AudioUnit).
    .global call_guest6
    .type call_guest6, %function
call_guest6:
    push    {r4, r5, r9, r10, r11, lr}
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldr     r3, [sp, #24]
    ldr     r4, [sp, #28]
    ldr     r5, [sp, #32]
    sub     sp, sp, #8
    str     r4, [sp]
    str     r5, [sp, #4]
    blx     r12
    add     sp, sp, #8
    pop     {r4, r5, r9, r10, r11, pc}

@ Entrada a toda función nuestra llamada por el juego (imports y hooks), con ip = destino.
@ El ABI de iOS ARMv6 solo garantiza pila alineada a 4; el código de la Vita (newlib, vitaGL)
@ asume 8 (AAPCS) y falla con pila desalineada (NEON con alineación, va_arg de 64 bits). Si sp
@ está desalineado, se arma un marco alineado copiando 16 palabras de argumentos de pila.
    .global ml_align_call
    .type ml_align_call, %function
ml_align_call:
    tst     sp, #4
    bxeq    ip
    push    {r4-r8, r10, r11, lr}
    mov     r4, sp
    sub     sp, sp, #64
    bic     sp, sp, #7
    add     r5, r4, #32
    mov     lr, sp
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    blx     ip
    mov     sp, r4
    pop     {r4-r8, r10, r11, pc}

@ ---------------------------------------------------------------------------- ObjC
@ objc_msgSend(self, _cmd, ...): busca la IMP (objc_lookup_imp, C) y salta con r0-r3 y la pila
@ intactos. IMP del juego (>= 0x98000000) -> salto directo; IMP nativa (nuestro C) -> por
@ ml_align_call (alinea la pila a 8 para newlib/vitaGL). self nil -> devuelve 0 (r0 = r1 = 0).
    .global objc_msgSend
    .type objc_msgSend, %function
objc_msgSend:
    cmp     r0, #0
    moveq   r1, #0
    bxeq    lr
    push    {r0-r3, r7, lr}
    mov     r7, sp
    bic     sp, sp, #7
    blx     objc_lookup_imp
    mov     ip, r0
    mov     sp, r7
    pop     {r0-r3, r7, lr}
    b       ml_dispatch_imp

@ objc_msgSend_stret(ret*, self, _cmd, ...)
    .global objc_msgSend_stret
    .type objc_msgSend_stret, %function
objc_msgSend_stret:
    cmp     r1, #0
    bxeq    lr
    push    {r0-r3, r7, lr}
    mov     r7, sp
    bic     sp, sp, #7
    mov     r0, r1
    mov     r1, r2
    blx     objc_lookup_imp
    mov     ip, r0
    mov     sp, r7
    pop     {r0-r3, r7, lr}
    b       ml_dispatch_imp

@ objc_msgSendSuper2(struct objc_super *{receiver, cls}, _cmd, ...): busca desde cls->superclass.
    .global objc_msgSendSuper2
    .type objc_msgSendSuper2, %function
objc_msgSendSuper2:
    push    {r0-r3, r7, lr}
    mov     r7, sp
    bic     sp, sp, #7
    blx     objc_lookup_super2
    mov     ip, r0
    mov     sp, r7
    pop     {r0-r3, r7, lr}
    ldr     r0, [r0]
    b       ml_dispatch_imp

@ objc_msgSendSuper2_stret(ret*, struct objc_super*, _cmd, ...)
    .global objc_msgSendSuper2_stret
    .type objc_msgSendSuper2_stret, %function
objc_msgSendSuper2_stret:
    push    {r0-r3, r7, lr}
    mov     r7, sp
    bic     sp, sp, #7
    mov     r0, r1
    mov     r1, r2
    blx     objc_lookup_super2
    mov     ip, r0
    mov     sp, r7
    pop     {r0-r3, r7, lr}
    ldr     r1, [r1]
    b       ml_dispatch_imp

@ ip = IMP. Del juego: salto directo. Nuestra: por ml_align_call (ip = destino).
ml_dispatch_imp:
    cmp     ip, #0x98000000
    bxhs    ip
    b       ml_align_call

@ uint32_t objc_call_imp(imp, self, sel, a, b, c, d): llamada desde nuestro C a una IMP del juego
@ (o nativa) con 4 argumentos extra; preserva r9 como call_guest.
    .global objc_call_imp
    .type objc_call_imp, %function
objc_call_imp:
    push    {r4-r6, r9, r10, lr}
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldr     r3, [sp, #24]
    ldr     r4, [sp, #28]
    ldr     r5, [sp, #32]
    ldr     r6, [sp, #36]
    sub     sp, sp, #16
    str     r4, [sp]
    str     r5, [sp, #4]
    str     r6, [sp, #8]
    blx     r12
    add     sp, sp, #16
    pop     {r4-r6, r9, r10, pc}

@ ---------------------------------------------------------------------------- setjmp
@ setjmp/longjmp de Darwin ARM sobre el jmp_buf del juego (_JBLEN = 27 palabras). Tienen que
@ entrar sin thunk de alineación (si no, setjmp guardaría nuestro marco y no el del juego).
@ Layout propio: r4-r11, sp, lr, d8-d15 = 10 + 16 = 26 palabras.
    .global ml_setjmp
    .type ml_setjmp, %function
ml_setjmp:
    stmia   r0!, {r4-r11, sp, lr}
    vstmia  r0, {d8-d15}
    mov     r0, #0
    bx      lr

    .global ml_longjmp
    .type ml_longjmp, %function
ml_longjmp:
    ldmia   r0!, {r4-r11, sp, lr}
    vldmia  r0, {d8-d15}
    movs    r0, r1
    moveq   r0, #1
    bx      lr

@ uint32_t objc_call_imp6(imp, self, sel, a, b, c, d, e, f): como objc_call_imp con 6 argumentos
@ extra (p. ej. glkView:drawInRect: = vista + CGRect por valor).
    .global objc_call_imp6
    .type objc_call_imp6, %function
objc_call_imp6:
    push    {r4-r8, r9, r10, lr}
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldr     r3, [sp, #32]
    ldr     r4, [sp, #36]
    ldr     r5, [sp, #40]
    ldr     r6, [sp, #44]
    ldr     r7, [sp, #48]
    sub     sp, sp, #16
    stm     sp, {r4-r7}
    blx     r12
    add     sp, sp, #16
    pop     {r4-r8, r9, r10, pc}
