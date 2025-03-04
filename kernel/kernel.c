// #define K2_DEBUG_VERBOSE
#define K2_DEBUG_WARN

#include <stddef.h>
#include <stdint.h>

#include "plat.h"
#include "utils.h"
#include "mmu.h"
#include "sched.h"

// unittests.c
extern void test_ktimer(); 
extern void test_sys_sleep();
extern void test_malloc(); 
extern void test_mbox(); 
extern void test_fb(); 
extern void test_kernel_tasks();

extern unsigned long user_begin;	// cf the linker script (linker-xxx.ld)
extern unsigned long user_end;
extern void user_process_hello(); // user.c
extern void user_process_printers(); // user.c
extern void user_process_mario();	// user.c
extern void user_donut(unsigned char *fb /*usr VA*/, int pitch); // user_donut.c
extern void user_donut0(void); 

int move_to_user_mode_donut(unsigned long start, unsigned long size, 
	unsigned long pc); // sched.c

// main body of kernel thread
// quest: (multiple)
void kernel_process() {
	unsigned long begin = (unsigned long)&user_begin;  	// defined in linker script
	unsigned long end = (unsigned long)&user_end;
	int err = 0; 
	
	printf("Kernel process started at EL %d, pid %d\r\n", get_el(), myproc()->pid);
	/* below: call "move_to_user_mode" to switch to user mode (with user code
		start & size). this function maps two pages for user code only...only
		good for simple tasks */ 

	/* TODO: your code here */

	/* alternatively, call "move_to_user_mode_donut". maps usr pages on demand. 
		can launch: donut (kuser), nes0 (binary elf embedded). */

	move_to_user_mode_donut(begin, end - begin, (unsigned long)user_process_mario);
	
	if (err < 0){
		printf("Error while moving process to user mode\n\r");
	} else I("move_to_user_mode ok");
	/* this func is called from ret_from_fork (entry.S). after returning from
	this func, it goes back to ret_from_fork and performs kernel_exit there.
	hence, trapframe populated by move_to_user_mode() will take effect.  */
}

extern void dump_pgdir(void); // vm.c

struct cpu cpus[NCPU]; 

void kernel_main() {
	uart_init();
	init_printf(NULL, putc);	
	printf("------ kernel boot ------  core %d\n\r", cpuid());
	printf("\033[5;32mVA_START %lx\033[0m\n", VA_START);	
	printf("build time (kernel.c) %s %s\n", __DATE__, __TIME__); // simplicity 
		
	paging_init(); 
	dump_pgdir();
	sched_init(); 	// must be before schedule() or timertick() 
	fb_init(); 		// reserve fb memory other page allocations
	consoleinit(); 	
	
	sys_timer_init(); 		// kernel timer: delay, timekeeping...
	enable_interrupt_controller(0/*coreid*/);
	enable_irq();
	
	generic_timer_init();  // sched ticks alive. preemptive scheduler is on

	// now cpu is on its boot stack (boot.S) belonging to the idle task. 
	// schedule() will jump off to kernel stacks belonging to normal tasks
	// (i.e. init_task as set up in sched_init(), sched.c)
	schedule(); 
	// only when scheduler has no normal tasks to run for the current cpu,
	// the cpu switches back to the boot stack and returns here
    while (1) {
        // don't call schedule(), otherwise each irq calls schedule(): too much
        // instead, let timer_tick() throttle & decide when to call schedule()
        V("idle task");
        asm volatile("wfi");
    }
}

// the 1st task (other than "idle"), created by sched_init()
void init(int arg/*ignored*/) {
	int wpid; 
    W("entering init");

	// create a kern task as our launchpad: running kernel tests, launch user
	//  tasks, etc.
	int res = copy_process(PF_KTHREAD, (unsigned long)&kernel_process, 0/*arg*/,
		 "kern-1"); BUG_ON(res<0); 
        
	while (1) {
		wpid = wait(0 /* does not care about status */); 
		if (wpid < 0) {
			W("init: wait failed with %d", wpid);
			panic("init: maybe no child. has nothing to do. bye"); 
		} else {
			I("wait returns pid=%d", wpid);
			// a parentless task 
		}
	}
}
