.syntax unified
.cpu cortex-m3
.thumb
.section .sco_parser_hook,"ax",%progbits
.global sco_parser_hook
.thumb_func
sco_parser_hook:
    b.w sco_parser_thunk

/* Offline link only. The combined bridge provides the real cancellation glue. */
.section .test_reset,"ax",%progbits
.global sco_bridge_reset
.thumb_func
sco_bridge_reset:
    bx lr
