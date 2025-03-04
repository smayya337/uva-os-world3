// #define K2_DEBUG_VERBOSE
#define K2_DEBUG_WARN

#include "plat.h"
#include "utils.h"
#include "sched.h"
// #include "fcntl.h"
#include "syscall.h"
#include "mmu.h"

// in-kernel syscall impl
// we have a simplified mechanism getting syscall arguments & return values.

/*
	assumption 1: from kernel entry to sys_XXX, x0-x8 are untouched. 
	this allows syscalls to read args direcly 

	assumption 2: the user's va is mapped during the exec of the syscall.
	(and the user pointers are not used in irq context, in which the user
	va may not be mapped)	

	in general we shall not trust the user pointers passed to syscalls.  
	hence, the kernel must check user pointers, ie walking the backing pagetables 
	and copying contents to kernel va). 
	This allows the kernel to catch invalid user buffers and reject it as needed. 
	(^^ This does not avoid TOCTOU problems, which are a different topic)

*/ 

// copy to/from userspace: although the code is there (vm.c) and is in use, 
// it is not needed. 

// invoked from el0_svc (entry.S)
// x0..x9 were intact since kernel entry (svc). hence they can be directly used as arguments
// upon return, ret_from_syscall (entry.S) will copy x0 to trapframe, as return value to user

// Fetch the uint64 at addr from the current process.
int fetchaddr(uint64 addr, uint64 *ip) {
    struct task_struct *p = myproc();
    // fxl: maybe check against brk and stack bottom? our task memlayout differs TODO:
    //   if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
    //     return -1;
    if (copyin(p->mm, (char *)ip, addr, sizeof(*ip)) != 0)
        return -1;
    return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{  
  if(copyinstr(myproc()->mm, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

int argstr(uint64 addr, char *buf, int max) {return fetchstr(addr, buf, max);};

int sys_fork(void) {
	return copy_process(0 /*clone_flags*/, 0 /*fn*/, 0 /*arg*/, 0/*inherit name*/);
}

int sys_exit(int c){
	I("exit called. pid %d code %d", myproc()->pid, c);
	exit_process(c);
	return 0; 
}

int sys_wait(unsigned long p /* user va*/) {	
	return wait(p); 
}

// implement this & enable nes0 or user_donut, 
// which needs sbrk() to work
// cf: https://linux.die.net/man/2/sbrk
// quest: user donut
unsigned long sys_sbrk(int incr) {
	unsigned long sz, sz0; 
	struct trapframe *regs = task_pt_regs(myproc());
	
	acquire(&myproc()->mm->lock); 
	sz = myproc()->mm->sz; sz0=sz;  
	
	// various sanity checks
	V("cal sys_sbrk, requestsed brk %lu", sz + incr);
	// all kinds of checks. careful: sz is unsigned; incr is signed
	if (incr < 0 && sz + incr < myproc()->mm->codesz) {
		W("sys_sbrk failed: requested brk too small. into code/data region"); 
		goto bad; 
	}
    /* NB: for multithreading case, sp can be anywhere in the addr space, e.g. a global array. so we react iff old sz is below sp and new sz approches or exceeds sp */
    if (incr > 0 && sz < regs->sp && sz + incr > regs->sp - PAGE_SIZE) {
		W("sys_sbrk failed: requested brk %lx too large. too close to sp %lx", 
			sz+incr, regs->sp); 
		goto bad; 
	} 
	if (incr == 0) { // shortcut 
		V("pid %d user_pages_count %d", myproc()->pid, myproc()->mm->user_pages_count);
		release(&myproc()->mm->lock); return sz;
	}

	sz = growproc(myproc()->mm, incr);
	release(&myproc()->mm->lock);
	if (sz == (unsigned long)-1) 
		W("sys_sbrk failed. requestsed brk %lx", sz0 + incr); 
	return sz; 
bad:
	release(&myproc()->mm->lock);
	return (unsigned long)(void *)-1; 	
}

int sys_kill(int pid) {
	return kill(pid); 
}

int sys_getpid(void) {
	return myproc()->pid; 
}

// return: # of ms
int sys_uptime(void) {
    uint sec, msec; 
	current_time(&sec, &msec); 
    return (int)(sec * 1000 + msec);  // fxl: loss of precision ...
}

// sysfile.c
// extern int sys_dup(int fd); 
extern int sys_read(int fd, unsigned long p /*user va*/, int n); // console.c
extern int sys_write(int fd, unsigned long p /*user va*/, int n); // console.c
// extern int sys_lseek(int fd, int offset, int whence); 
// extern int sys_close(int fd); 
// extern int sys_fstat(int fd, unsigned long st /*user va*/);
// extern int sys_link(unsigned long userold, unsigned long usernew);
// extern int sys_unlink(unsigned long upath /*user va*/);
// extern int sys_open(unsigned long upath, int omode);
// extern int sys_mkdir(unsigned long upath);
// extern int sys_mknod(unsigned long upath, short major, short minor);
// extern int sys_chdir(unsigned long upath);
extern int sys_exec(unsigned long upath, unsigned long uargv);
// extern int sys_pipe(unsigned long fdarray);

extern int sys_fork();		// sys.c

extern unsigned long sys_sbrk(int incr); // vm.c
extern int sys_sleep(int ms); // timer.c

// our placeholder. NB: syscall handlers (sys_XXX) can have arbitrary signatures
int sys_unimplemented(void) {
	BUG(); 
	return -1; 
}

/* An array of pointers to all syscall handlers. 
	Each syscall has a "syscall number" (sys.h) — which is just an index in this array 		
*/
// quest: (multiple)
void *const sys_call_table[] = {
    [SYS_fork] sys_fork, //! STUDENT_WILL_SEE(	[SYS_fork]    sys_unimplemented,)
    [SYS_exit] sys_exit, //! STUDENT_WILL_SEE(	[SYS_exit]    sys_unimplemented,)
    [SYS_wait] sys_wait,
    [SYS_pipe] sys_unimplemented, // sys_pipe,
    [SYS_read] sys_read,
    [SYS_kill] sys_unimplemented,  // sys_kill,
    [SYS_exec] sys_exec,           //! STUDENT_WILL_SEE(	[SYS_exec]    sys_unimplemented,)
    [SYS_fstat] sys_unimplemented, // sys_fstat,
    [SYS_chdir] sys_unimplemented, // sys_chdir,
    [SYS_dup] sys_unimplemented,   // sys_dup,
    [SYS_getpid] sys_getpid,
    [SYS_sbrk] sys_sbrk,   //! STUDENT_WILL_SEE(	[SYS_sbrk]    sys_unimplemented,)
    [SYS_sleep] sys_sleep, //! STUDENT_WILL_SEE(	[SYS_sleep]    sys_unimplemented,)
    [SYS_uptime] sys_uptime,
    [SYS_open] sys_unimplemented,      // sys_open,
    [SYS_write] sys_write,             //! STUDENT_WILL_SEE(	[SYS_write]   sys_unimplemented,)
    [SYS_mknod] sys_unimplemented,     // sys_mknod,
    [SYS_unlink] sys_unimplemented,    // sys_unlink,
    [SYS_link] sys_unimplemented,      // sys_link,
    [SYS_mkdir] sys_unimplemented,     // sys_mkdir,
    [SYS_close] sys_unimplemented,     // sys_close,
    [SYS_lseek] sys_unimplemented,     // sys_lseek,
    [SYS_clone] sys_unimplemented,     // sys_clone,
    [SYS_semcreate] sys_unimplemented, // sys_semcreate,
    [SYS_semfree] sys_unimplemented,   // sys_semfree,
    [SYS_semp] sys_unimplemented,      // sys_semp,
    [SYS_semv] sys_unimplemented,      // sys_semv,
};
