/*
 * Run at EL0 in its own VM. But compiled as part of kernel source.
 * file must be named as user_XXX for code/text to be placed between user_start/end
 * (cf the linker script linker-rpi3qemu.ld or linker-rpi3.ld)
 *
 * Must be 100% self contained. cannot call kernel functions (--> mem fault)
 *
 * Draw a rotating donut on text console.
 *
 * dependency:
 *     delay
 *     fb (uart may work? depending on terminal program)
 * 
 * CREDITS: see end of the file
 *
 * Understand how to debug things like:
 * irq.c:180 Unhandled EL0 sync exception, cpu0, esr: 0x000000006232c061, elr: 0x00000000000012f8, far: 0x0000000007ffff40
 * irq.c:182 online esr decoder: https://esr.arm64.dev/#0x000000006232c061
 * how to map the elr to the kernel symbol
 */

#include "kuser_sys.h"
#include "plat.h"
#include "utils.h"

#define PIXELSIZE 4 /*ARGB, expected by /dev/fb*/
typedef unsigned int PIXEL;
#define NN 640 // canvas dimension. NN by NN

// cannot call kernel's strlen which may emit long jmp with absolute addr
static unsigned int user_strlen(const char *s)    
{
  int n;
  for(n = 0; s[n]; n++)
    ;
  return n;
}

// c: the fill value (byte); n: size, in bytes
static void* mymemset(void *dst, int c, uint n)
{
  char *cdst = (char *) dst;
  int i;
  for(i = 0; i < n; i++){
    cdst[i] = c;
  }
  return dst;
}

static void print_to_console(char *msg) {
	call_sys_write(1 /*stdout*/, msg, user_strlen(msg)); 
}
static void myprintf(const char *fmt, ...); 

static inline void setpixel(unsigned char *buf, int x, int y, int pit, PIXEL p) {
    assert(x >= 0 && y >= 0);
    *(PIXEL *)(buf + y * pit + x * PIXELSIZE) = p;
}

#define R(mul, shift, x, y)              \
    _ = x;                               \
    x -= mul * y >> shift;               \
    y += mul * _ >> shift;               \
    _ = (3145728 - x * x - y * y) >> 11; \
    x = x * _ >> 10;                     \
    y = y * _ >> 10;

// cannot use global vars. will alloc via sbrk() below
// static char b[1760];        // text buffer (W 80 H 22?
// static signed char z[1760]; // z buffer
#define BUFSIZE 1760

static PIXEL int2rgb (int value); 

// draw dots on canvas, closer to the original js version (see comment at the end)

// accepting args which are populated by move_to_user_mode()
// x0-x7 (per arm64 calling convention)
//  unpopulated args will have garbage values 
//quest: "user donut"
void user_donut(unsigned char *fb /*usr VA*/, int pitch) {
    int sA = 1024, cA = 0, sB = 1024, cB = 0, _;
    char *b, *z; 

    // validate: debugging works
    print_to_console("Donut process entry\n\r");
    myprintf("fb %p pitch %d", fb, pitch);
  
    // for usage of sbrk(), cf "man sbrk" also search for "sbrk" in usertests.c
    b = call_sys_sbrk(0); /* TODO: replace this */
    if (b == -1) {
      myprintf("sbrk for b failed\n"); call_sys_exit(-1); 
    }
    z = call_sys_sbrk(0); /* TODO: replace this */
    if (z == -1) {
      myprintf("sbrk for z failed\n"); call_sys_exit(-1); 
    }

    while (1) {
        mymemset(b, 0, 1760);  // text buffer 0: black bkgnd
        mymemset(z, 127, 1760); // z buffer
        int sj = 0, cj = 1024;
        for (int j = 0; j < 90; j++) {
            int si = 0, ci = 1024; // sine and cosine of angle i
            for (int i = 0; i < 324; i++) {
                int R1 = 1, R2 = 2048, K2 = 5120 * 1024;

                int x0 = R1 * cj + R2,
                    x1 = ci * x0 >> 10,
                    x2 = cA * sj >> 10,
                    x3 = si * x0 >> 10,
                    x4 = R1 * x2 - (sA * x3 >> 10),
                    x5 = sA * sj >> 10,
                    x6 = K2 + R1 * 1024 * x5 + cA * x3,
                    x7 = cj * si >> 10,
                    x = 25 + 30 * (cB * x1 - sB * x4) / x6,
                    y = 12 + 15 * (cB * x4 + sB * x1) / x6,
                    // N = (((-cA * x7 - cB * ((-sA * x7 >> 10) + x2) - ci * (cj * sB >> 10)) >> 10) - x5) >> 7;
                    lumince = (((-cA * x7 - cB * ((-sA * x7 >> 10) + x2) - ci * (cj * sB >> 10)) >> 10) - x5); 
                    // range likely: <0..~1408, scale to 0..255
                    lumince = lumince<0? 0 : lumince/5; 
                    lumince = lumince<255? lumince : 255; 

                int o = x + 80 * y; // fxl: 80 chars per row
                signed char zz = (x6 - K2) >> 15;
                if (22 > y && y > 0 && x > 0 && 80 > x && zz < z[o]) { // fxl: z depth will control visibility
                    z[o] = zz;
                    // luminance_index is now in the range 0..11 (8*sqrt(2) = 11.3)
                    // now we lookup the character corresponding to the
                    // luminance and plot it in our output:
                    // b[o] = ".,-~:;=!*#$@"[N > 0 ? N : 0];
                    b[o] = lumince;                    
                }
                R(5, 8, ci, si) // rotate i
            }
            R(9, 7, cj, sj) // rotate j
        }
        R(5, 7, cA, sA);
        R(5, 8, cB, sB);

        int y = 0, x = 0;
        for (int k = 0; 1761 > k; k++) {
            if (k % 80) {
                if (x < 50) {
                    // to display, scale x by K, y by 2K (so we have a round donut)
                    int K=6, xx=x*K, yy=y*K*2;
                    PIXEL clr = int2rgb(b[k]); // to a color spectrum
                    setpixel(fb, xx, yy, pitch, clr);
                    setpixel(fb, xx+1, yy, pitch, clr);
                    setpixel(fb, xx, yy+1, pitch, clr);
                    setpixel(fb, xx+1, yy+1, pitch, clr);
                }
                x++;
            } else { 
                y++;
                x = 1;
            }
        }

        // Below cannot work, b/c we are at EL0 
        // instead, tap into syscall to flush cache
        // user_flush_dcache_range(fb, (char*)fb + NN *pitch);
        
        /* TODO: your code here */
        
        // not as fast as expected? possible reason: this code is compiled -0O
    }
}

// map luminance [0..255] to rgb color
// value: 0..255, PIXEL: argb. Color is 
static PIXEL int2rgb(int value) {
    int r, g, b;
    if (value >= 0 && value <= 63) {
        // Black to White
        r = g = b = (value * 255) / 63;
    } else if (value >= 64 && value <= 127) {
        // White to Light Gold
        r = 255;
        g = 255 - ((value - 64) * (32)) / 63;  // 255 to 223
        b = 255 - ((value - 64) * (128)) / 63; // 255 to 127
    } else if (value >= 128 && value <= 191) {
        // Light Gold to Dark Goldenrod
        r = 255 - ((value - 128) * (71)) / 63; // 255 to 184
        g = 223 - ((value - 128) * (89)) / 63; // 223 to 134
        b = 127 - ((value - 128) * (116)) / 63; // 127 to 11
    } else if (value >= 192 && value <= 255) {
        // Dark Goldenrod to Red
        r = 184 + ((value - 192) * (71)) / 63; // 184 to 255
        g = 134 - ((value - 192) * (134)) / 63; // 134 to 0
        b = 11 - ((value - 192) * (11)) / 63;   // 11 to 0
    } else {
        // Value out of range
        r = g = b = 0;
    }
    return (r << 16) | (g << 8) | b;
}

// -------  mini printf() for debugging -----------
// code adapted from xv6: usr/printf.c
// CANNOT use global vars. all vars must be on stack

static void myputc(int fd/*ignored*/, char c) {
    call_sys_write(1 /*stdout*/, &c, 1);
}

static void
myprintint(int fd, int xx, int base, int sgn)
{
  char buf[16];
  int i, neg;
  uint x;

  char digits[] = "0123456789ABCDEF";

  neg = 0;
  if(sgn && xx < 0){
    neg = 1;
    x = -xx;
  } else {
    x = xx;
  }

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);
  if(neg)
    buf[i++] = '-';

  while(--i >= 0)
    myputc(fd, buf[i]);
}

static void
myprintptr(int fd, uint64 x) {
  int i;
  char digits[] = "0123456789ABCDEF";
  myputc(fd, '0');
  myputc(fd, 'x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    myputc(fd, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the given fd. Only understands %d, %x, %p, %s.
static void
myvprintf(int fd, const char *fmt, va_list ap)
{
  char *s;
  int c, i, state;

  state = 0;
  for(i = 0; fmt[i]; i++){
    c = fmt[i] & 0xff;
    if(state == 0){
      if(c == '%'){
        state = '%';
      } else {
        myputc(fd, c);
      }
    } else if(state == '%'){
      if(c == 'd'){
        myprintint(fd, va_arg(ap, int), 10, 1);
      } else if(c == 'l') {
        myprintint(fd, va_arg(ap, uint64), 10, 0);
      } else if(c == 'x') {
        myprintint(fd, va_arg(ap, int), 16, 0);
      } else if(c == 'p') {
        myprintptr(fd, va_arg(ap, uint64));
      } else if(c == 's'){
        s = va_arg(ap, char*);
        if(s == 0)
          s = "(null)";
        while(*s != 0){
          myputc(fd, *s);
          s++;
        }
      } else if(c == 'c'){
        myputc(fd, va_arg(ap, uint));
      } else if(c == '%'){
        myputc(fd, c);
      } else {
        // Unknown % sequence.  Print it to draw attention.
        myputc(fd, '%');
        myputc(fd, c);
      }
      state = 0;
    }
  }
}

// side quest: complete below
static void
myprintf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  myvprintf(1, fmt, ap);
}

/// project idea: use ktimer to drive the animation. periodically

/**
 * Original author:
 * https://twitter.com/a1k0n
 * https://www.a1k0n.net/2021/01/13/optimizing-donut.html
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-09-15     Andy Sloane  First version
 * 2011-07-20     Andy Sloane  Second version
 * 2021-01-13     Andy Sloane  Third version
 * 2021-03-25     Meco Man     Port to RT-Thread RTOS
 *
 *
 *  js code for both canvas & text version
 *  https://www.a1k0n.net/js/donut.js
 *
 *  ported by FL
 * From the NJU OS project:
 * https://github.com/NJU-ProjectN/am-kernels/blob/master/kernels/demo/src/donut/donut.c
 *
 */
