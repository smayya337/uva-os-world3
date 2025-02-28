#include "sysregs.h"
#include "entry.h"
#include "syscall.h"
#include "plat.h"
#include "param.h"
#include "mmu.h"
#include "sched.h"

	.macro handle_invalid_entry el, type
	// make sure we have a good sp. as the kernel will hang anyway, so 
	// we use the boot stack
	mrs x27, sctlr_el1		// assume nothing valuable in x27/x28...
	orr x27, x27, #1		// sctlr_el1 bit0: MMU on or not. cf sysregs.h
	cmp x27, #0		
	b.eq 99f
	ldr x27, =boot_stacks
	add x27, x27, PAGE_SIZE
	mov x28, #0 
	b 100f 
99: 	// MMU off, use PA 
	ldr x27, =boot_stacks
	add x27, x27, PAGE_SIZE
	mov x28, #VA_START	
100: 
	sub sp, x27, x28
	kernel_entry \el		
	mov	x0, #\type
	mrs	x1, esr_el1
	mrs	x2, elr_el1
	mrs x3, far_el1
	bl	show_invalid_entry_message		// irq.c
	msr	daifset, #0b0010 		// disable irq
	b	err_hang
	.endm

	.macro	ventry	label
	.align	7
	b	\label
	.endm

	/*  
		Now, we need to take exception (svc) from EL0 to EL1. To accommodate this, 
		both `kernel_entry` and `kernel_exit` macros accepts an additional argument `el`, 
		indicating the EL an exception is taken from. 
	*/
	.macro	kernel_entry, el
	sub	sp, sp, #S_FRAME_SIZE
	stp	x0, x1, [sp, #16 * 0]
	stp	x2, x3, [sp, #16 * 1]
	stp	x4, x5, [sp, #16 * 2]
	stp	x6, x7, [sp, #16 * 3]
	stp	x8, x9, [sp, #16 * 4]
	stp	x10, x11, [sp, #16 * 5]
	stp	x12, x13, [sp, #16 * 6]
	stp	x14, x15, [sp, #16 * 7]
	stp	x16, x17, [sp, #16 * 8]
	stp	x18, x19, [sp, #16 * 9]
	stp	x20, x21, [sp, #16 * 10]
	stp	x22, x23, [sp, #16 * 11]
	stp	x24, x25, [sp, #16 * 12]
	stp	x26, x27, [sp, #16 * 13]
	stp	x28, x29, [sp, #16 * 14]

	/* Even for the same task, we are using 2 distinct stacks for EL0 and EL1. 
		This is a common design because we want to separate user/kernel */
	.if	\el == 0
	/* After taking an exception from EL0 to EL1, the CPU automatically starts use 
		the SP for EL1. The SP for EL0 can be found in the `sp_el0` register. 
		The value of this register must be stored and restored upon entering/exiting 
		the kernel, even if the kernel does not  use `sp_el0` in the exception handler. 
		We need to save a copy of `sp_el0` for each task.
	*/
		mrs	x21, sp_el0
	.else
		add	x21, sp, #S_FRAME_SIZE
	.endif /* \el == 0 */

	mrs	x22, elr_el1
	mrs	x23, spsr_el1	
	/* above: save reg0--29. 
	 	below: save reg30, sp, pc(as elr_el1), pstate*/
	stp	x30, x21, [sp, #16 * 15] 
	stp	x22, x23, [sp, #16 * 16]

	/*
	 * For exceptions from EL0, create a final frame record.
	 * For exceptions from EL1, create a synthetic frame record so the
	 * interrupted code shows up in the backtrace.
	 */
	.if \el == 0
		stp	xzr, xzr, [sp, #S_STACKFRAME]
	.else
		stp	x29, x22, [sp, #S_STACKFRAME] // x29 (fp for interrupted func) x22 (lr for it)
	.endif
	add	x29, sp, #S_STACKFRAME // x29 (fp) points to the frame record

	.endm

	.macro	kernel_exit, el
	ldp	x22, x23, [sp, #16 * 16]
	ldp	x30, x21, [sp, #16 * 15] 

	.if	\el == 0
		msr	sp_el0, x21
	.endif

	msr	elr_el1, x22			
	/* This EL level is encoded in the `spsr_el1` register that was saved, 
		e.g. when syscall enters the kernel. So `eret` below always return to the 
		level from which the exception was taken. */
	msr	spsr_el1, x23

	ldp	x0, x1, [sp, #16 * 0]
	ldp	x2, x3, [sp, #16 * 1]
	ldp	x4, x5, [sp, #16 * 2]
	ldp	x6, x7, [sp, #16 * 3]
	ldp	x8, x9, [sp, #16 * 4]
	ldp	x10, x11, [sp, #16 * 5]
	ldp	x12, x13, [sp, #16 * 6]
	ldp	x14, x15, [sp, #16 * 7]
	ldp	x16, x17, [sp, #16 * 8]
	ldp	x18, x19, [sp, #16 * 9]
	ldp	x20, x21, [sp, #16 * 10]
	ldp	x22, x23, [sp, #16 * 11]
	ldp	x24, x25, [sp, #16 * 12]
	ldp	x26, x27, [sp, #16 * 13]
	ldp	x28, x29, [sp, #16 * 14]
	add	sp, sp, #S_FRAME_SIZE		
	eret
	.endm

/*
 * Exception vectors.
 */
.align	11
.globl vectors 
//quest: user helloworld
vectors:
	ventry	sync_invalid_el1t			// Synchronous EL1t
	ventry	irq_invalid_el1t			// IRQ EL1t
	ventry	fiq_invalid_el1t			// FIQ EL1t
	ventry	error_invalid_el1t			// Error EL1t

	ventry	sync_invalid_el1h			// Synchronous EL1h
	ventry	el1_irq						// IRQ EL1h
	ventry	fiq_invalid_el1h			// FIQ EL1h
	ventry	error_invalid_el1h			// Error EL1h

	// Synchronous 64-bit EL0 (used for syscalls)
	ventry sync_invalid_el1h /* TODO: replace this */
	ventry	el0_irq					// IRQ 64-bit EL0
	ventry	fiq_invalid_el0_64			// FIQ 64-bit EL0
	ventry	error_invalid_el0_64			// Error 64-bit EL0

	ventry	sync_invalid_el0_32			// Synchronous 32-bit EL0
	ventry	irq_invalid_el0_32			// IRQ 32-bit EL0
	ventry	fiq_invalid_el0_32			// FIQ 32-bit EL0
	ventry	error_invalid_el0_32			// Error 32-bit EL0

sync_invalid_el1t:
	handle_invalid_entry 1,  SYNC_INVALID_EL1t

irq_invalid_el1t:
	handle_invalid_entry  1, IRQ_INVALID_EL1t

fiq_invalid_el1t:
	handle_invalid_entry  1, FIQ_INVALID_EL1t

error_invalid_el1t:
	handle_invalid_entry  1, ERROR_INVALID_EL1t

sync_invalid_el1h:
	handle_invalid_entry 1, SYNC_INVALID_EL1h

fiq_invalid_el1h:
	handle_invalid_entry  1, FIQ_INVALID_EL1h

error_invalid_el1h:
	handle_invalid_entry  1, ERROR_INVALID_EL1h

fiq_invalid_el0_64:
	handle_invalid_entry  0, FIQ_INVALID_EL0_64

error_invalid_el0_64:
	handle_invalid_entry  0, ERROR_INVALID_EL0_64

sync_invalid_el0_32:
	handle_invalid_entry  0, SYNC_INVALID_EL0_32

irq_invalid_el0_32:
	handle_invalid_entry  0, IRQ_INVALID_EL0_32

fiq_invalid_el0_32:
	handle_invalid_entry  0, FIQ_INVALID_EL0_32

error_invalid_el0_32:
	handle_invalid_entry  0, ERROR_INVALID_EL0_32

el1_irq:
	kernel_entry 1 
	bl	handle_irq
	kernel_exit 1 

el0_irq:
	kernel_entry 0 
	bl	handle_irq
	kernel_exit 0 

// the syscall path (from irq vector)
el0_sync:
	kernel_entry 0
	mrs	x25, esr_el1					// read the syndrome register
	lsr	x24, x25, #ESR_ELx_EC_SHIFT		// exception class
	cmp	x24, #ESR_ELx_EC_SVC64			// is the currrent exception caused by SVC in 64-bit state? 
	b.eq	el0_svc						// if yes, jump to below for handling syscalls 
	cmp	x24, #ESR_ELx_EC_DABT_LOW		// is data abort in EL0?
	b.eq	el0_data_abort						// if yes, handle it
	handle_invalid_entry 0, SYNC_ERROR

sc_nr	.req	x25					// number of system calls
scno	.req	x26					// syscall number
stbl	.req	x27					// syscall table pointer

// the syscall path (cont'd). called from el0_sync above
// quest: user helloworld
el0_svc:
	// NB: up to this point, x0--x8 is untouched by the kernel (still from EL0)
	ldr	stbl, =sys_call_table			// load syscall table pointer
	uxtw	scno, w8					// syscall number in w8
	mov	sc_nr, #__NR_syscalls
	bl	enable_irq
	cmp     scno, sc_nr                 // syscall number >= upper syscall limit?			
	b.hs	ni_sys						// if so, invalid. jump to handle errors

	ldr	x16, [stbl, scno, lsl #3]		// otherwise, invoke the syscall table
	// call sys_* routine
	/* TODO: your code here */
	blr x16
	b	ret_from_syscall

ni_sys:
	handle_invalid_entry 0, SYSCALL_ERROR

ret_from_syscall:	
	// --- deal with killed tasks --  basically implements:
	// if (current->killed) 
	//	exit_process(-1);	
	mov x27, sp
	and x26, x27, #0xfffffffffffff000	// page mask
	add x26, x26, #THREAD_CPU_CONTEXT
	// x26 points to task_struct *current
	add x26, x26, #TASK_STRUCT_KILLED_OFFSET
	// x26 points to current->killed
	ldr w27, [x26]
	cmp w27, #0
	b.eq cont_exit // not killed
	// killed: "exit_process(-1)"
	mov x25, x0 	// preserve x0 
	mov x0, #0xffffffffffffffff	
	bl exit_process // the current task becomes zombie & reschedule a diff task
	mov x0, x25
cont_exit: 
	bl	disable_irq
	str	x0, [sp, #S_X0]	 // returned x0 to the user task (saving x0 to pt_regs)
	kernel_exit 0

el0_data_abort:
	bl	enable_irq
	mrs	x0, far_el1
	mrs	x1, esr_el1			
	mrs x2, elr_el1
	bl	do_mem_abort
	cmp x0, 0
	b.eq 1f		// data abort handled okay
	handle_invalid_entry 0, DATA_ABORT_ERROR
1:
	// --- deal with killed tasks (cf: ret_from_syscall above) ---
	mov x27, sp
	and x26, x27, #0xfffffffffffff000	// page mask
	add x26, x26, #THREAD_CPU_CONTEXT
	// x26 points to task_struct *current
	add x26, x26, #TASK_STRUCT_KILLED_OFFSET
	// x26 points to current->killed
	ldr w27, [x26]
	cmp w27, #0
	b.eq 2f
	// below: call exit_process(-1)
	mov x25, x0 	// preserve x0 
	mov x0, #0xffffffffffffffff	
	bl exit_process // becomes zombie & reschedule a diff task
	mov x0, x25
2: 
	bl disable_irq
	kernel_exit 0
	
// the first instructions to be executed by a new task 
// x19 = fn; x20 = arg, populated by copy_process() 
.globl ret_from_fork
ret_from_fork:
	bl	leave_scheduler
	cbz	x19, ret_to_user  // if the new task is user, jump to below and leave el1
	mov	x0, x20		// new task is kern, load fn/arg as populated by copy_process()
	blr	x19
	// kernel thread returns, and continues below to return to user
ret_to_user:
	bl disable_irq				
	kernel_exit 0 	// will restore previously prepared processor state (pt_regs), hit eret, then EL0

.globl err_hang
err_hang: b err_hang
