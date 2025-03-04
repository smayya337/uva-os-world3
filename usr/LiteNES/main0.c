/*

for nes0 (image in-kernel), minimum functions. only gfx, no input control

---- Below: comment from NJU OS project ---- 

LiteNES originates from Stanislav Yaglo's mynes project:
  https://github.com/yaglo/mynes

LiteNES is a "more portable" version of mynes.
  all system(library)-dependent code resides in "hal.c" and "main.c"
  only depends on libc's memory moving utilities.

How does the emulator work?
  1) read file name at argv[1]
  2) load the rom file into array rom
  3) call fce_load_rom(rom) for parsing
  4) call fce_init for emulator initialization
  5) call fce_run(), which is a non-exiting loop simulating the NES system
  6) when SIGINT signal is received, it kills itself (TBD)
*/

#include "fce.h"   
#include "common.h"   
#include "../user.h"

#define stderr 2

#include "mario-rom.h"    // built-in rom buffer with Super Mario

struct config cfg; 

// expected args: from exec
// arg0 - prog name (convention)
// arg1 - user VA for mapped fb
// arg2 - fb.vwidth
// arg3 - fb.vheight
// arg4 - fb.pitch
// arg5 - offsetx
// arg6 - offsety
// has to check against SCREEN_HEIGHT, SCREEN_WIDTH
//quest: mario
int main(int argc, char *argv[])
{
    for (int i=0; i<argc; i++)
      printf("arg %d: %s\n", i, argv[i]); // dbg

    unsigned long fb = (unsigned long) argv[1];
    // we don't have sscanf/atol16 (no libc). 
    // we expect config.fb is around 0x3c00_0000, so it SHOULD not overflow
    // sanity check 
    assert(fb > 0x30000000 && fb < 0x40000000); 
    cfg.fb = (char *)fb; 
    cfg.vwidth = argv[2];
    cfg.vheight = argv[3];
    cfg.pitch = argv[4];
    cfg.offsetx = argv[5];
    cfg.offsety = argv[6];
        
    if (fce_load_rom(rom) != 0) { // will load the built-in rom
        fprintf(stderr, "Invalid or unsupported rom.\n");
        exit(1);
    }
    printf("load rom...ok\n"); 

    fce_init();

    printf("running fce ...\n"); 
    
    // project idea: can test stdin. "press a key to start..."
    fce_run();
    return 0;
}
