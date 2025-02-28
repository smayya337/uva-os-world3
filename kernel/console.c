/*
 * Minimum console for lab3, goal: allow user tasks to write/read uart, w/o
 * going through a vfs layer.
 *
 * Implemented:
 * - copyin/out across u/k boundary
 * - sleep/wakeup management (blocking read/write)
 * - buffer (a queue, which showcases r/w lock)
 *
 * Removed: interface with vfs
 * No dependency on: inode and underlying filesystem, to keep things simple
 */

/*
 * Console input and output, to the uart.
 * Reads are line at a time.
 * Implements special input characters:
 *   newline -- end of line
 *   control-h -- backspace
 *   control-u -- kill line
 *   control-d -- end of file
 *   control-p -- print process list
 *
 * from xv6. a mini driver supports: concurrent users accesses,
 * buffered user writes, concurrent syscall/irq (with locks)
 * it is decoupled with the uart hw (e.g. pl011.c, mini_uart.c)
 */

#include <stdarg.h>

#include "utils.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "sched.h"

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

/*
 * Send one character to the uart.
 * Called by printf(), and to echo input characters,
 * but not from write().
 */
void consputc(int c) {
    if (c == BACKSPACE) {
        // if the user typed backspace, overwrite with a space. fxl: assumes \b is rendered by terminal program...
        uartputc_sync('\b');
        uartputc_sync(' ');
        uartputc_sync('\b');
    } else {
        uartputc_sync(c);
    }
}

struct {
  struct spinlock lock;   // protects the buf below
  
  // input
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

/* user write()s to the console go here.
 user_src=1 means src is user va */
// quest: user helloworld
int consolewrite(int user_src, uint64 src, int off, int n, void *content/*ignored*/)
{
  int i;

  // for simplicity: one char at a time; no buffer (hence no lock). 
  //  would be better: copy all n chars in one copyin() call
  for(i = 0; i < n; i++){
    char c = 0;
    if (either_copyin(&c,user_src,src+i,1) == -1) /* TODO: replace this */
      break;
    uartputc(c);
  }

  return i;
}

/* user read()s from the console go here.
copy (up to) a whole input line to dst.
user_dist indicates whether dst is a user
or kernel address.
user_dst: =1 means dst is user va */
int consoleread(int user_dst, uint64 dst, int off, int n, char blocking, void *content)
{
  uint target;
  int c;
  char cbuf;

  BUG_ON(!blocking); // TBD nonblocking read

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

/*
 * The console input interrupt handler.
 * uartintr() calls this for input character.
 * Do erase/kill processing, append to cons.buf,
 * wake up consoleread() if a whole line has arrived.
 * 
 * (i.e. called per incoming char. store char to buf (by default) for special
 * char like backspace, modify the buf. only wakeup user reader when a whole
 * line arrives.)
 */
void consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE); 
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        cons.w = cons.e;
        wakeup(&cons.r);
      }
    }   // if buf full, just drop the new char
    break;
  }
  
  release(&cons.lock);
}

void consoleinit(void)
{
  initlock(&cons.lock, "cons");
  // uartinit();    // fxl: already init for kernel printf
}

// from sysfile.c 
// simplified: shortcut it to console 
// quest: user helloworld   cf consolewrite() definition
int sys_write(int fd /*ignored*/, unsigned long p /*user va*/, int n) { 
  return consolewrite(1, p, 0, n, 0); /* TODO: replace this */
}

// simplified: shortcut to console
int sys_read(int fd, unsigned long p /*user va*/, int n) {  
  return consoleread(1/*user_dst*/, p, 0/*off,ignored*/, n,
    1/*blocking*/, 0/*ignored*/);
}
