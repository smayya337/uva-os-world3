/* linker script for rpi3 emulated by qemu */

SECTIONS
{
	/* quest: kernel virtaddr */
	. = 0xffff000000080000; /* TODO: replace this */
	.text.boot : { *(.text.boot) }

	/* include user text/data compiled as part of kernel 
		e.g. user.c user_sys.S user_donut.c */
	. = ALIGN(0x00001000);	
	user_begin = .;
	.text.user : { build-rpi3qemu/kuser* (.text) }
	.rodata.user : { build-rpi3qemu/kuser* (.rodata) }
	.data.user : { build-rpi3qemu/kuser* (.data) }
	.bss.user : { build-rpi3qemu/kuser* (.bss) }
	user_end = .;
	
	. = ALIGN(512);		/*sector aligned*/
	ramdisk_start = .;
	.ramdisk : {build-rpi3qemu/sd* (.data)}		/* must come before generic *(.data) */	
	ramdisk_end = .;	
	
	/* quest: "nes0", cf other part of this linker script  */
	/* must come before generic *(.data) */
	. = ALIGN(512);		/*sector aligned*/
	nes_start = .; 
	/* TODO: your code here */
	/* we don't need to set "nes_end" */
	
	. = ALIGN(512);		/*sector aligned*/
	.text :  { *(.text) }
	.rodata : { *(.rodata) }
	.data : { *(.data) }
	
	. = ALIGN(0x8);	 /* bss begin/end must be aligned: cleared with memzero_aligned*/
	bss_begin = .;
	.bss : { *(.bss*) } 
	bss_end = .;
	. = ALIGN(0x00001000);
	idmap_dir = .;
	.data.idmapd : {. += (3* (1 << 12));}
	. = ALIGN(0x00001000);
	pg_dir = .;
	.data.pgd : { . += (4 * (1 << 12)); }	/* PG_DIR_SIZE */
	kernel_end = .; 
}

