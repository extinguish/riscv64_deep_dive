#ifndef LAB_API_H
#define LAB_API_H

#include <stdint.h>

#include "lab_constants.h"
#include "lab_trap_frame.h"

unsigned long lab_instruction_length(const void *instruction_address);
long lab_handle_syscall(struct lab_trap_frame *frame);
long lab_emulate_instruction(struct lab_trap_frame *frame);
void lab_s_trap_dispatch(struct lab_trap_frame *frame);

void lab_putc(char character);
void lab_puts(const char *string);
void lab_puthex(uint64_t value);
void lab_exit(unsigned int status) __attribute__((noreturn));
unsigned int lab_report(const char *name, int passed);
unsigned int lab_run_helper_tests(void);

extern unsigned char __global_pointer$[];
extern unsigned char __s_trap_stack_bottom[];
extern unsigned char __s_trap_stack_top[];
extern unsigned char __u_stack_bottom[];
extern unsigned char __u_stack_top[];

#endif
