ENTRY(sco_credit_vendor_thunk)
native_credit_report_resume = 0x37b09;
SECTIONS {
    . = 0xd4a00;
    .text : { *(.text.credit_vendor_thunk) *(.text*) *(.rodata*) }
    . = ALIGN(4);
    .state : { *(.sco_bridge_state) }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
