.syntax unified
.cpu cortex-m3
.thumb
.section .text.credit_vendor_thunk,"ax",%progbits
.global sco_credit_vendor_thunk
.thumb_func
sco_credit_vendor_thunk:
    push {r0-r3, r12, lr}
    ldr r3, =0xd1b61
    cmp lr, r3
    bne original_report
    sub sp, sp, #24
    str r5, [sp]
    str r6, [sp, #4]
    str r7, [sp, #8]
    str r8, [sp, #12]
    str lr, [sp, #16]
    mov r3, sp
    bl sco_credit_vendor_recount
    str r0, [sp, #28] /* Replace saved R1 only; preserve native R0/R2/R3. */
    add sp, sp, #24
original_report:
    pop {r0-r3, r12, lr}
    ldr r3, =0x202dbc
    push {r4, r5}
    b.w native_credit_report_resume
.ltorg
