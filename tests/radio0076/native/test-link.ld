/* Offline proof placement only. Root owns the combined hardware link. */
ENTRY(sco_parser_thunk)
native_type2_test = 0x5faa8;
native_type1 = 0x5fab2;
native_commit = 0x5fbc2;
native_return = 0x5fc4c;
SECTIONS {
 .sco_parser_hook 0x5faa4 : { *(.sco_parser_hook) }
 .test_reset 0x88f00 : { *(.test_reset) }
 .sco_parser_thunk 0xd3980 : { *(.sco_parser_thunk) }
 .sco_in_stats 0xd3a00 : { *(.sco_in_stats) }
 .sco_in_state 0xd3a30 : { *(.sco_in_state) }
 .sco_rx_scratch 0xd3c00 : { *(.sco_rx_scratch) }
 .text 0xd4000 : { *(.text*) *(.rodata*) }
 /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.note*) }
}
ASSERT(SIZEOF(.sco_parser_thunk)<=128,"Offline thunk slot")
ASSERT(SIZEOF(.sco_parser_hook)==4,"Complete native type1 compare/branch pair")
ASSERT(SIZEOF(.text)<=16384,"Combined code budget")
