// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>

extern uint32_t _stack_top, _sidata, _sdata, _edata, _sbss, _ebss;
extern int main(void);

static int semihost(int operation, void *argument) {
    register int r0 __asm__("r0") = operation;
    register void *r1 __asm__("r1") = argument;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void probe_emit(const char *text) {
    semihost(4, (void *)text);
}

__attribute__((noreturn)) void _exit(int code) {
    uint32_t argument[] = {0x20026, (uint32_t)code};
    semihost(0x20, argument);
    for (;;) {}
}

static void hex(uint32_t value) {
    char text[] = "00000000\n";
    for (unsigned i = 0; i < 8; ++i)
        text[i] = "0123456789abcdef"[(value >> (28 - i * 4)) & 15];
    probe_emit(text);
}

void fault_report(uint32_t *stack) {
    probe_emit("FAULT pc=");
    hex(stack[6]);
    probe_emit("CFSR=");
    hex(*(volatile uint32_t *)0xe000ed28);
    _exit(2);
}

__attribute__((naked)) void Fault_Handler(void) {
    __asm__ volatile("tst lr,#4\n ite eq\n mrseq r0,msp\n mrsne r0,psp\n b fault_report");
}

void Reset_Handler(void) {
    *(volatile uint32_t *)0xe000ed88 = 0xfu << 20;
    __asm__ volatile("dsb; isb");
    uint32_t *destination = &_sdata, *source = &_sidata;
    while (destination < &_edata) *destination++ = *source++;
    destination = &_sbss;
    while (destination < &_ebss) *destination++ = 0;
    _exit(main());
}

__attribute__((section(".vectors"), used)) const uintptr_t vectors[16] = {
    (uintptr_t)&_stack_top, (uintptr_t)Reset_Handler,
    (uintptr_t)Fault_Handler, (uintptr_t)Fault_Handler,
    (uintptr_t)Fault_Handler, (uintptr_t)Fault_Handler,
    (uintptr_t)Fault_Handler,
};

void _fini(void) {}
