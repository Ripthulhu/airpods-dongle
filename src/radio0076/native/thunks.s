.syntax unified
.cpu cortex-m3
.thumb
.section .sco_out_thunks,"ax",%progbits
.global dispatch_7074_thunk
.thumb_func
dispatch_7074_thunk:
    push {r1}
    movw r1,#0x7074
    cmp r0,r1
    bne original_dispatch
    ldr r1,=sco_out_transport_context
    str r5,[r1]
    pop {r1}
    b.w native_dispatch_forward
original_dispatch:
    pop {r1}
    cmp r0,r2
    beq.w native_dispatch_equal
    b.w native_dispatch_resume
    .ltorg
.size dispatch_7074_thunk,.-dispatch_7074_thunk
.global serializer_7074_thunk
.thumb_func
serializer_7074_thunk:
    push {r4,lr}
    ldr r2,[r0,#4]
    movw r1,#0x7074
    cmp r2,r1
    beq sco_message
    push {r0-r3}
    bl sco_bridge_observe_control
    pop {r0-r3}
    b.w native_serializer_resume
sco_message:
    ldr r1,=sco_outbound_env
    bl sco_outbound_send
    pop {r4,pc}
    .ltorg
.size serializer_7074_thunk,.-serializer_7074_thunk
