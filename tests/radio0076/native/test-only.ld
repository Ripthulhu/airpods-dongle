/* Synthetic Unicorn locations only; root owns all production link addresses. */
ENTRY(sco_outbound_send)
native_dispatch_forward = 0x4414e;
native_dispatch_equal = 0x44158;
native_dispatch_resume = 0x44120;
native_serializer_resume = 0x5fd40;
SECTIONS {
 .text 0x90000 : { *(.text*) *(.rodata*) }
 .sco_out_thunks 0x98000 : { *(.sco_out_thunks) }
 .data 0x21f000 : { *(.data*) }
 .bss ALIGN(8) : { *(.bss*) *(COMMON) }
 /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) }
}
ASSERT(SIZEOF(.text) <= 0x4000, "Test core exceeds root proposed code region size")
ASSERT(SIZEOF(.sco_out_thunks) <= 256, "Thunks exceed proposed region")
