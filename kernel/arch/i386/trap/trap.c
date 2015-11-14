/**
 * Authors: 
 *  + John Detter <john@detter.com>
 *  + Amber Arnold <alarnold2@wisc.edu>
 *
 * Trap handling functions.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kern/types.h"
#include "kern/stdlib.h"
#include "idt.h"
#include "trap.h"
#include "file.h"
#include "stdlock.h"
#include "devman.h"
#include "chronos.h"
#include "x86.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "stdarg.h"
#include "syscall.h"
#include "tty.h"
#include "fsman.h"
#include "pipe.h"
#include "proc.h"
#include "cmos.h"
#include "rtc.h"
#include "vm.h"
#include "signal.h"

#define TRAP_COUNT 256
#define INTERRUPT_TABLE_SIZE (sizeof(struct int_gate) * TRAP_COUNT)
#define STACK_TOLERANCE 16

// #define DEBUG

#ifdef DEBUG
#define TRAP_NAME_SZ 20
char* trap_names[] = {
        "DIVIDE ERROR",
        "DEBUG",
        "NON MASKABLE INTERRUPT",
        "BREAK RECEIVED",
        "OVERFLOW CAUGHT",
        "BOUND RANGE EXCEEDED",
        "INVALID OPCODE",
        "DEVICE NOT AVAILABLE",
        "DOUBLE FAULT",
        "COPROCESSOR SEGMENT OVERRUN",
        "INVALID TSS",
        "SEGMENT NOT PRESENT",
        "STACK SEGMENT FAULT",
        "GENERAL PROTECTION FAULT",
        "PAGE FAULT",
        "RESERVED INTERRUPT",
        "FLOATING POINT EXCEPTION",
        "ALIGNMENT CHECK",
        "MACHINE CHECK",
        "SIMD FLOATING POINT EXCEPTION"
};
#endif

extern struct rtc_t k_time;
struct int_gate interrupt_table[TRAP_COUNT];
extern struct proc* rproc;
extern uint trap_handlers[];
extern uint k_ticks;
extern slock_t ptable_lock;
uint __get_cr2__(void);

void trap_return();

void trap_init(void)
{
	int x;
	for(x = 0;x < TRAP_COUNT;x++)
	{
		interrupt_table[x].offset_1 = (uint)(trap_handlers[x] & 0xFFFF);
		interrupt_table[x].offset_2 = 
			(trap_handlers[x] >> 16) & 0xFFFF;
		interrupt_table[x].segment_selector = SEG_KERNEL_CODE << 3;
		interrupt_table[x].flags = GATE_INT_CONST | GATE_USER;
	}

	lidt((uint)interrupt_table, INTERRUPT_TABLE_SIZE);		
}

int trap_pf(uintptr_t address)
{
#ifdef DEBUG
	cprintf("Fault address: 0x%x\n", address);
#endif
	pgflags_t dir_flags = VM_DIR_USRP | VM_DIR_READ | VM_DIR_WRIT;
	pgflags_t tbl_flags = VM_TBL_USRP | VM_TBL_READ | VM_TBL_WRIT;

	uintptr_t stack_bottom = rproc->stack_end;
	uintptr_t stack_tolerance = stack_bottom - STACK_TOLERANCE * PGSIZE;
	if(address < stack_bottom && address >= stack_tolerance){
		uintptr_t address_down = PGROUNDDOWN(address);
		if(address_down <= rproc->heap_end)
			return 1;

		int numOfPgs = (stack_bottom - address_down) / PGSIZE;
		vm_mappages(address_down, numOfPgs * PGSIZE, rproc->pgdir, 
			dir_flags, tbl_flags);
		/* Move the stack end */
		rproc->stack_end -= numOfPgs * PGSIZE;
		return 0;
	}else{
		return 1;
	}
}

void trap_handler(struct trap_frame* tf)
{
	rproc->tf = tf;
	
	int trap = tf->trap_number;
	char fault_string[64];
	int syscall_ret = -1;
	int handled = 0;
	int user_problem = 0;
	int kernel_fault = 0;

	if(!(tf->cs & 0x3))
		kernel_fault = 1;

	/**
	 * A couple of quick optimizations:
	 *  + Clock interrupt is the most common interrupt
	 *  + System call is very common interrupt
	 *  + signal handling shouldn't handle in switch
	 */

#ifdef DEBUG
	if(trap < TRAP_NAME_SZ)
		cprintf("trap: %s\n", trap_names[trap]);
#endif

	handled = 1;
	if(trap == TRAP_SC)
	{
#ifdef DEBUG
		cprintf("trap: SYSTEM CALL\n");
#endif
		strncpy(fault_string, "System Call", 64);
		syscall_ret = syscall_handler((uint*)tf->esp);
		rproc->tf->eax = syscall_ret;
	} else if(trap == INT_PIC_TIMER)
	{
		rproc->user_ticks++;
		k_ticks++;
		pic_eoi(INT_PIC_TIMER_CODE);
		yield();
	} else if(tf->eip == SIG_MAGIC && rproc->sig_handling)
	{
		/* We're done handling this signal! */
		sig_cleanup();
		rproc->sig_handling = 0;
	} else handled = 0; /* Its not any of the above optimizations */

	if(handled) goto TRAP_DONE;


	switch(trap)
	{
		case INT_PIC_KEYBOARD: case INT_PIC_COM1:
			handled = 1;
			break;
			// cprintf("Keyboard interrupt.\n");
			/* Keyboard interrupt */
			tty_handle_keyboard_interrupt();	
			pic_eoi(INT_PIC_TIMER_CODE);
			pic_eoi(INT_PIC_COM1_CODE);
			handled = 1;
			break;
		case INT_PIC_CMOS:
			/* Update the system time */
			pic_eoi(INT_PIC_CMOS_CODE);
			uchar cmos_val = cmos_read_interrupt();
			if(cmos_val == 144)
			{
				/* Clock update */
				rtc_update(&k_time);
			}
			handled = 1;
			break;
		case TRAP_DE:
			strncpy(fault_string, "Divide by 0", 64);
			user_problem = 1;
			break;
		case TRAP_DB:
			strncpy(fault_string, "Debug", 64);
			break;
		case TRAP_N1:
			strncpy(fault_string, "Non maskable interrupt", 64);
			break;
		case TRAP_BP:
			strncpy(fault_string, "Breakpoint", 64);
			break;
		case TRAP_OF:
			strncpy(fault_string, "Overflow", 64);
			user_problem = 1;
			break;
		case TRAP_BR:
			strncpy(fault_string, "Seg Fault", 64);
			user_problem = 1;
			break;
		case TRAP_UD:
			strncpy(fault_string, "Invalid Instruction", 64);
			user_problem = 1;
			break;
		case TRAP_NM:
			strncpy(fault_string, "Device not available", 64);
			break;
		case TRAP_DF:
			strncpy(fault_string, "Double Fault", 64);
			break;
		case TRAP_MF:
			strncpy(fault_string, "Coprocessor Seg fault", 64);
			break;
		case TRAP_TS:
			strncpy(fault_string, "Invalid TSS", 64);
			break;
		case TRAP_NP:
			strncpy(fault_string, "Invalid segment", 64);
			break;
		case TRAP_SS:
			strncpy(fault_string, "Stack Overflow", 64);
			user_problem = 1;
			break;
		case TRAP_GP:
			strncpy(fault_string, "Protection violation", 64);
			user_problem = 1;
			break;
		case TRAP_PF:
			if(kernel_fault) 
			{
				strncpy(fault_string, "Seg Fault", 64);
                                tf->error = __get_cr2__();
				break;
			}

			if(trap_pf(__get_cr2__())){
				strncpy(fault_string, "Seg Fault", 64);
				tf->error = __get_cr2__();
				user_problem = 1;
			}else{
				handled = 1;
			}
			break;
		case TRAP_0F:
			strncpy(fault_string, "Reserved interrupt", 64);
			break;
		case TRAP_FP:
			strncpy(fault_string, "Floating point error", 64);
			user_problem = 1;
			break;
		case TRAP_AC:
			strncpy(fault_string, "Bad alignment", 64);
			user_problem = 1;
			break;
		case TRAP_MC:
			strncpy(fault_string, "Machine Check", 64);
			user_problem = 1;
			break;
		case TRAP_XM:
			strncpy(fault_string, "SIMD Exception", 64);
			break;
		default:
			cprintf("Interrupt number: %d\n", trap);
			strncpy(fault_string, "Invalid interrupt.", 64);
	}

TRAP_DONE:

	if(!handled)
	{
		cprintf("%s: 0x%x", fault_string, tf->error);

		if(user_problem && rproc && !kernel_fault)
		{
			_exit(1);
		} else for(;;);
	}

	/* Do we have any signals waiting? */
	if(rproc->sig_queue && !rproc->sig_handling)
	{
		slock_acquire(&ptable_lock);
		sig_handle();
		slock_release(&ptable_lock);
	}


	/* Make sure that the interrupt flags is set */
	tf->eflags |= EFLAGS_IF;

	//cprintf("Process %d is leaving trap handler.\n", rproc->pid);

	/* While were here, clear the timer interrupt */
	// pic_eoi(INT_PIC_TIMER_CODE);

	/* Warning: Black magic below, this will be fixed later */
	/* Force return */
	asm volatile("movl %ebp, %esp");
	asm volatile("popl %ebp");
	/* add return address and arguments */
	asm volatile("addl $0x08, %esp");
	asm volatile("jmp trap_return");
}