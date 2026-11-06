SECTIONS {
 .thunks 0xd3900 : { *(.sco_parser_thunk) *(.sco_out_thunks) }
 ASSERT(SIZEOF(.thunks)<=0x100,"Thunk overflow")
 .state 0xd3a00 : { *(.sco_in_stats) *(.sco_in_state) *(.sco_bridge_state) *(.data*) *(.bss*) *(COMMON) }
 ASSERT(SIZEOF(.state)<=0x200,"State overflow")
 .rx 0xd3c00 : { *(.sco_rx_scratch) }
 ASSERT(SIZEOF(.rx)<=0x200,"RX overflow")
 .tx 0xd3e00 : { *(.sco_tx_scratch) }
 ASSERT(SIZEOF(.tx)<=0x200,"TX overflow")
 .text 0xd4000 : { *(.text*) *(.rodata*) }
 ASSERT(.<=0xd8000,"Bridge overflow")
 /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
 native_type2_test = 0x5faa9;
 native_type1 = 0x5fab3;
 native_commit = 0x5fbc3;
 native_return = 0x5fc4d;
 native_dispatch_forward = 0x4414f;
 native_dispatch_resume = 0x44121;
 native_dispatch_equal = 0x44159;
 native_credit_report_resume = 0x37b09;
 native_serializer_resume = 0x5fd41;
}
