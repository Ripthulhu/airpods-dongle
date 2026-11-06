.syntax unified
.cpu cortex-m3
.thumb
.section .sco_parser_thunk,"ax",%progbits
.global sco_parser_thunk
.thumb_func
sco_parser_thunk:
    cmp r1,#3
    beq type3
    // Replay both complete native instructions displaced at5faa4.
    cmp r1,#1
    beq native_command
    b.w native_type2_test
native_command:
    push.w {r0-r3,r12,lr}
    sub sp,#8
    str r10,[sp]
    mov r0,r4
    mov r1,r8
    mov r2,r6
    mov r3,r7
    bl sco_inbound_command
    add sp,#8
    pop.w {r0-r3,r12,lr}
    // Restore the native command-compare flags after the optional Reset helper.
    cmp r1,#1
    b.w native_type1
type3:
    push.w {r0-r3,r12,lr}
    sub sp,#8
    str r10,[sp]
    mov r0,r4
    mov r1,r8
    mov r2,r6
    mov r3,r7
    bl sco_inbound_process
    cmp r0,#0
    add sp,#8
    pop.w {r0-r3,r12,lr}
    beq retry
    mov.w r11,#1
    cmp r6,#0
    ite ne
    addne r4,r7,r6
    moveq r4,r9
    b.w native_commit
retry:
    b.w native_return
.size sco_parser_thunk,.-sco_parser_thunk
