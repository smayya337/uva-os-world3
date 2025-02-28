/*  kernel boot code, shall be used with newer rpi3 firmware that builds in
	armstub, which boots kernel at 0x80000 and EL2

	single core, has mmu/pgtable, has bootstack (for multitasking)

	NB: qemu/gdb cannot do source-level debugging for boot.S (before MMU is on),
	b/c the cpu is on phys address -- until MMU is on */

#include "param.h"
#include "sysregs.h"
#include "plat.h"
#include "mmu.h"

.section ".text.boot"

.globl _start
_start:		
	// MMU off, until we set pgtables. cf: sysregs.h
	ldr	x0, =SCTLR_VALUE_MMU_DISABLED  
	msr	sctlr_el1, x0
	
	// No trapping EL0/EL1 access to advanced simd, fp registers.
	// cf: https://developer.arm.com/documentation/ddi0595/2021-03/AArch64-Registers/CPACR-EL1--Architectural-Feature-Access-Control-Register
	//	some code may contain them (e.g. usblib StringFormat uses q0/q1...)
	MRS     x1, CPACR_EL1
	ORR     x1, x1, #(0x3 << 20)    // Write CPACR_EL1.FPEN bits
	MSR     CPACR_EL1, x1
	ISB

	// For Cortex A53 SMP coherency, setting S3_1_c15_c2_1 is needed. 
	// However we assume that it's already done by firmware (armstub)

	/* -------- Exception level switch -------------- */
	// Check the current exception level: EL3 or EL2?	
	mrs x0, CurrentEL
  	lsr x0, x0, #2
	cmp x0, #3
	beq el3

	# Current EL: EL2 
	# set EL1 to be running in AArch64
	mrs	x0, hcr_el2
	orr	x0, x0, #HCR_RW  
	msr	hcr_el2, x0

	# prepare to switch to EL1
	mov x0, #SPSR_VALUE
	msr	spsr_el2, x0

	adr	x0, el1_entry
	msr	elr_el2, x0
	eret	// switch to EL1

el3: 		// Current EL: EL3
	// 	With rpi3 firmware (armstub) or qemu, kernel always starts in EL2; 
	//  We leave EL3 code here for completeness
  	ldr x0, =HCR_VALUE
  	msr hcr_el2, x0

	ldr	x0, =SCR_VALUE
	msr	scr_el3, x0

	# prepare to switch to EL1
	ldr	x0, =SPSR_VALUE
	msr	spsr_el3, x0

	adr	x0, el1_entry		
	msr	elr_el3, x0	
	eret	// switch to EL1				

	/* ------- Start of EL1 execution ------- */
el1_entry:	
	// Below: cpu0 only. Clean up bss region. 
	// bss_begin/end are linking addr (kernel virt). convert them to phys.
	// they are (at least) 8 bytes aligned in link script
	ldr	x0, =bss_begin
	ldr	x1, =bss_end
	ldr x2, =VA_START
	sub x0, x0, x2
	sub x1, x1, x2
	sub	x1, x1, x0
	bl 	memzero_aligned
	
	// NEW: Create kern pgtables
	// for readability, written in C. 
	// hence, grab a temporary SP (at the end of phys mem)
	ldr x0, =PHYS_SIZE
	mov sp, x0 	
	bl create_kern_pgtables
#if defined(PLAT_RPI3QEMU)
	bl	create_kern_idmap // needed. see its comment
#endif	
	// pgtables are set up, but not effective yet

// quest: kernel virtaddr
setup_sp: 	
	// Set virt sp = 4096 + boot_stacks
	// (assuming linear mapping, phys 0 --> virt VA_START)	
	/* TODO: your code here */
	// NB: we aren't using sp yet -- until mmu is on
	ldr x0, =boot_stacks
	add x0, x0, #4096
	mov sp, x0
	// below is new vs. boot.S in lab1/2
	// load kernel's pgtable
	adrp	x0, pg_dir		// @pg_dir: the virtual base address of pgtables. cf utils.h
	msr		ttbr1_el1, x0	
	isb 		// memory barrier 

	// TCR: translation control reg. (translate granule, user/kernel split, etc)	
	ldr	x0, =(TCR_VALUE)
	msr	tcr_el1, x0  // tcr_el1: Translation Control Register. cf mmu.h
	isb 		// memory barrier 
	
	// MAIR: memory attributes. cf mmu.h
	ldr	x0, =(MAIR_VALUE)	
	msr	mair_el1, x0

#ifdef PLAT_RPI3QEMU
	adrp	x0, idmap_dir	// install identity mem mapping
	msr	ttbr0_el1, x0	
#endif

	// install irq vectors
	ldr x0, =vectors	// load VBAR_EL1 vector table addr (kern VA)
	msr	vbar_el1, x0	
	
	// load the linking (virt) addr of kernel_main
	/* TODO: your code here */
	ldr x2,=kernel_main
	ldr x0, =SCTLR_VALUE // Turn on MMU. cf sysregs.h
	msr	sctlr_el1, x0	// BOOM! after this, this core runs on virtual addr
	br 	x2				// Jump to kernel_main() (kernel.c)