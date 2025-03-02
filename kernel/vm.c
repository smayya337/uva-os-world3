#define K2_DEBUG_INFO

// virtual memory, based on xv6 design with modifications

#include "plat.h"
#include "utils.h"
#include "mmu.h"
#include "spinlock.h"
#include "sched.h"

/* 
 * Allocate and map a page to user. Return the kernel virtual address of the 
 * page, or 0 if failed.
 * 
 * @mm: the user's memory management structure
 * @va: the user virtual address to map the page to
 * @perm: the permissions for the page
 *
 * Caller must hold mm->lock
 */
void *allocate_user_page_mm(struct mm_struct *mm, unsigned long va, unsigned long perm) {
	unsigned long page;
	if (mm->user_pages_count == MAX_TASK_USER_PAGES) { // no need to go further
		E("reached limit of MAX_TASK_USER_PAGES %d", MAX_TASK_USER_PAGES); 
		E("consider increase the limit in param.h");
		return 0; 
	}

	page = get_free_page();
	if (page == 0) {
		W("get_free_page failed");
		return 0;
	}
	if (map_page(mm, va, page, 1 /*alloc*/, perm))
		return PA2VA(page);
	else {
		W("map_page failed");
		free_page(page);
		return 0; 
	}
}

/* 
 * Set (or find) a PTE, i.e., the entry in a bottom-level page table.
 * 
 * @pte: The 0-th PTE of that page table, kernel virtual address.
 * @va: The virtual address corresponding to @pte.
 * @pa: If 0, don't touch PTE (just return its address); otherwise, update PTE.
 * @perm: Permission bits to be written to PTE (cf mmu.h).
 * 
 * Return: Kernel virtual address of the PTE set or found.
 * 
 * Note: The found PTE can be invalid.
 */
static 
unsigned long * map_table_entry(unsigned long *pte, unsigned long va, 
	unsigned long pa, unsigned long perm) {
	unsigned long index = va >> PAGE_SHIFT;
	index = index & (PTRS_PER_TABLE - 1);
	if (pa) {
		unsigned long entry = pa | MMU_PTE_FLAGS | perm; 
		pte[index] = entry;
	}
	return pte + index; 
}

/* 
 * Extract table index from the virtual address and prepare a descriptor in the
 * parent table that points to the child table. Allocate the child table as needed.
 * 
 * @table: a (virtual) pointer to the parent page table. This page table is
 *         assumed to be already allocated, but might contain empty entries.
 * @shift: indicates where to find the index bits in a virtual address corresponding
 *         to the target page table level.
 * @va: the virtual address of the page to be mapped.
 * @alloc [in|out]: in: 1 means allocate a new table if needed; 0 means don't allocate.
 *                  out: 1 means a new page table is allocated; 0 otherwise.
 * @desc [out]: pointer (kernel virtual address) to the page table descriptor
 *              installed/located.
 * 
 * Return: the physical address of the next page table. 0 if failed to allocate.
 */
static unsigned long map_table(unsigned long *table, unsigned long shift, 
	unsigned long va, int* alloc, unsigned long **pdesc) {
	unsigned long index = va >> shift;

	index = index & (PTRS_PER_TABLE - 1);
	if (pdesc)
		*pdesc = table + index; 
	if (!table[index]) { /* next level pgtable absent. */
		if (*alloc) { /* asked to alloc. then alloc a page & install */
			unsigned long next_level_table = get_free_page();
			if (!next_level_table) {
				*alloc = 0; 
				return 0; 
			}
			unsigned long entry = next_level_table | MM_TYPE_PAGE_TABLE;
			table[index] = entry;
			return next_level_table;
		} else { /* asked not to alloc. bail out */
			*alloc = 0; /* didn't alloc */
			return 0; 
		}
	} else {  /* next lv pgtable exists */
		*alloc = 0; /* didn't alloc */
		return PTE_TO_PA(table[index]);
	}
}

/* 
 * Walk a task's pgtable tree. Find and (optionally) update the PTE
 * corresponding to a given user virtual address.
 * 
 * @mm: the user address space under question, can be obtained via task_struct::mm
 * @va: the given user virtual address
 * @page: the physical address of the page start. If 0, do not map (just locate the PTE)
 * @alloc: if 1, allocate any absent pgtables if needed
 * @perm: permission, only matters if page != 0 & alloc != 0
 * 
 * Return: Kernel virtual address of the PTE set or found; 0 if failed (cannot proceed without
 *         pgtable allocation). The located PTE could be invalid.
 * 
 * Caller must hold mm->lock.
 * Only operates on one page.
 * 
 * (maybe better named as "locate_page"?)
 */
unsigned long *map_page(struct mm_struct *mm, unsigned long va, 
	unsigned long page, int alloc, unsigned long perm) {
	unsigned long *desc; 
	BUG_ON(!mm); 

	// record pgtable descriptors installed & ker pages allocated during walk. 
	// for reversing them in case we bail out. most four (pud..pte)
	int nk = mm->kernel_pages_count; 
	unsigned long *descs[4] = {0}; 	
	unsigned long ker_pages[4] = {0}; // pa

	// reached limit for user pages 
	if (page != 0 && alloc == 1 && mm->user_pages_count >= MAX_TASK_USER_PAGES)
		return 0; 

	/* start from the task's top-level pgtable. allocate if absent 
		this is how a task's pgtable tree gets allocated
	*/
	if (!mm->pgd) { 
		if (alloc) {
			if (nk == MAX_TASK_KER_PAGES)
				goto fail; 
			if (!(mm->pgd = get_free_page()))
				goto fail; 
			ker_pages[0] = mm->pgd; 
			descs[0] = &(mm->pgd); 
			// mm->kernel_pages[mm->kernel_pages_count++] = mm->pgd;
		} else 
			goto fail; 
	} 
	
	__attribute__((unused)) const char *lvs[] = {"pgd","pud","pmd","pte"};
	const int shifts [] = {0, PGD_SHIFT, PUD_SHIFT, PMD_SHIFT}; 
	unsigned long table = mm->pgd; 	// pa of a pgd/pud/pmd/pte
	int allocated; 

	for (int i = 1; i < 4; i++) { // pud->pmd->pte
		allocated = alloc; 
		table = map_table(PA2VA(table), shifts[i], va, &allocated, &desc); 
		if (table) { 
			if (allocated) { 
				ker_pages[i] = table; 
				descs[i] = desc; 
				if (nk+i > MAX_TASK_KER_PAGES) { /* exceeding the limit, bail out*/
					W("MAX_TASK_KER_PAGES %d reached", MAX_TASK_KER_PAGES); 
					goto fail; 
				}
			} else 
				; /* use existing table -- fine */
		} else { /*!table*/
			if (!alloc)
				W("%s: failed b/c we reached nonexisting pgtable, and asked not to alloc",
					lvs[i]);
			else
				W("%s: asked to alloc but still failed. low kernel mem?", lvs[i]);
			goto fail; 
		}
	}	

	/* Now, pgtables at all levels are in place. table points to a pte, 
		the bottom level of pgtable tree. Install the actual user page */
	unsigned long *pte_va = 
		map_table_entry(PA2VA(table), va, page /* 0 for finding entry only*/, perm);
	if (page) { /* a page just installed, bookkeeping.. */
		BUG_ON(mm->user_pages_count >= MAX_TASK_USER_PAGES); // shouldn't happen, as we checked above
		mm->user_pages_count++; 
	}

	// success: bookkeep the kern pages ever allocated
	for (int i = 0; i < 4; i++) {
		if (ker_pages[i] == 0) 
			continue; 	
		mm->kernel_pages[mm->kernel_pages_count++] = ker_pages[i]; 
		V("now has %d kern pages", mm->kernel_pages_count); 
	}
	return pte_va;

fail:
	for (int i = 0; i < 4; i++) {
		if (ker_pages[i] == 0) 
			continue; 	 		
		BUG_ON(!alloc || !descs[i]); 
		free_page(ker_pages[i]); 
		*descs = 0; 	// nuke the descriptor 
	}
	W("failed. reverse allocated pgtables during tree walk"); 
	return 0; 	
}


/* 
 * Duplicate the contents of the current ("src") task's user pages to the "dst"
 * task, at the same virtual address. Allocate and map pages for "dst" on demand.
 * 
 * Return 0 on success.
 * 
 * Assumption: current->mm is active.
 * Caller must hold dstmm->lock.
 */
//quest: "two user printers"
int dup_current_virt_memory(struct mm_struct *dstmm) {
	struct trapframe *regs = task_pt_regs(myproc());
	struct mm_struct* srcmm = myproc()->mm; BUG_ON(!srcmm); 

	acquire(&srcmm->lock); 

	V("pid %d src>mm %lx p->mm->sz %lu", myproc()->pid, (unsigned long)srcmm, srcmm->sz);

	/* Iterate through the source task's virtual pages, allocate and map
	   physical pages for the destination task, and copy the content from the
	   corresponding virtual address of the source task.

	   We will copy page by page. During the copy process, since the source
	   task's user virtual address space is active, the source address can be
	   the user virtual address. However, the destination task's user virtual
	   address space is inactive, so the destination address must be a kernel
	   virtual address. */
	for (unsigned long i = 0; i < srcmm->sz; i += PAGE_SIZE) {
		// locate src pte, extract perm bits, copy over. 
		unsigned long *pte = map_page(srcmm, i, 0/*just locate*/, 0/*no alloc*/, 0); 
		BUG_ON(!pte);  // bad user mapping? 
		unsigned long perm = PTE_TO_PERM(*pte); 
		// NB: "i" is a userva in the src address space. this assumes the src's userva is active
		V("dup user page at userva %lx", i);  
		void *kernel_va = allocate_user_page_mm(dstmm->kernel_pages[i / PAGE_SIZE], i * PAGE_SIZE, 0); /* TODO: replace this */
		if(kernel_va == 0)
			goto no_mem;  
		// copy the page content from the src to the dst. be careful with the memmove() arg order
		memmove(kernel_va, srcmm->kernel_pages[i / PAGE_SIZE], PAGE_SIZE); /* TODO: replace this */
	}

	// copy user stack from src to dst. 
	V("regs->sp %lx", regs->sp);
	for (unsigned long i = PGROUNDDOWN(regs->sp); i < USER_VA_END; i+=PAGE_SIZE) {
		unsigned long *pte = map_page(srcmm, i, 0/*just locate*/, 0/*no alloc*/, 0); 
		BUG_ON(!pte);  // bad user mapping (stack)?
		void *kernel_va = allocate_user_page_mm(dstmm->kernel_pages[i / PAGE_SIZE], i * PAGE_SIZE, 0); /* TODO: replace this */
		if(kernel_va == 0)
			goto no_mem; 
		// NB: "i" is a userva in the src address space. this assumes the src's userva is active
		V("kern va %lx i %x", kernel_va, i);
		memmove(kernel_va, srcmm->kernel_pages[i / PAGE_SIZE], PAGE_SIZE); /* TODO: replace this */
	}

	dstmm->sz = srcmm->sz; dstmm->codesz = srcmm->codesz;

	release(&srcmm->lock);
	return 0;
no_mem: 	// XXX: revserse allocation .. 
	release(&srcmm->lock);
	return -1; 
}

/* 
 * Look up a virtual address, return the physical address, or 0 if not mapped or
 * invalid. Can only be used to look up *user* pages.
 *
 * Caller must hold mm->lock 
 */
unsigned long walkaddr(struct mm_struct *mm, unsigned long va) {
    unsigned long *pte = map_page(mm, va, 0 /*dont map, just locate*/, 0 /*don't alloc*/, 0 /*perm*/);
    if (!pte)
        return 0;
    if ((*pte & (~PAGE_MASK) & (~(unsigned long)MM_AP_MASK)) != MMU_PTE_FLAGS) {
        W("fail: pte found but invalid %016lx, %016x",
          (*pte & (~PAGE_MASK) & (~(unsigned long)MM_AP_MASK)), MMU_PTE_FLAGS);
        return 0;
    }
    BUG_ON(PTE_TO_PA(*pte) < PHYS_BASE ||
           PTE_TO_PA(*pte) >= PHYS_BASE + PHYS_SIZE);
    return PTE_TO_PA(*pte);
}

/* 
 * Copy from kernel to user. Called by various syscalls, drivers.
 * Copy len bytes from src to virtual address dstva in a given page table.
 * Return 0 on success, -1 on error.
 *
 * Caller must NOT hold mm->lock 
 */
int copyout(struct mm_struct * mm, uint64_t dstva, char *src, uint64_t len) {
    uint64_t n, va0, pa0;

	if (dstva > USER_VA_END) {
		W("illegal user va. a kernel va??");
		return -1; 
	} 

	acquire(&mm->lock); 
    while (len > 0) {
        va0 = PGROUNDDOWN(dstva); // va0 pagebase
        pa0 = walkaddr(mm, va0);
        if (pa0 == 0)
            {release(&mm->lock); return -1;}
        n = PAGE_SIZE - (dstva - va0); // n: remaining bytes on the page
        if (n > len)
            n = len;
        memmove(PA2VA(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PAGE_SIZE;
    }
	release(&mm->lock);
    return 0;
}

/* 
 * Copy from user to kernel. Called by various syscalls, drivers.
 * Copy len bytes to dst from virtual address srcva in a given page table.
 * Return 0 on success, -1 on error.
 *
 * Caller must NOT hold mm->lock 
 */
int copyin(struct mm_struct * mm, char *dst, uint64 srcva, uint64 len) {
    uint64 n, va0, pa0;

	if (srcva > USER_VA_END) {
		W("illegal user va. is it a kernel va??");
		return -1; 
	}
	V("%lx %lx", srcva, len);

	acquire(&mm->lock); 
    while (len > 0) {
        va0 = PGROUNDDOWN(srcva);  // fxl: user virt page base...
        pa0 = walkaddr(mm, va0); // fxl: phys addr for user va pagebase
        if (pa0 == 0)
            {release(&mm->lock); return -1;}
        n = PAGE_SIZE - (srcva - va0);
        if (n > len)
            n = len;
        memmove(dst, PA2VA(pa0 + (srcva - va0)), n);

        len -= n;
        dst += n;
        srcva = va0 + PAGE_SIZE;
    }
	release(&mm->lock);
    return 0;
}

/* 
 * Copy a null-terminated string from user to kernel.
 * Copy bytes to dst from virtual address srcva in a given page table,
 * until a '\0', or max.
 * 
 * Return 0 on success, -1 on error.
 * 
 * Caller must NOT hold mm->lock 
 */
int copyinstr(struct mm_struct *mm, char *dst, uint64 srcva, uint64 max) {
    uint64 n, va0, pa0;
    int got_null = 0;

    if (srcva > USER_VA_END) {
		W("// illegal user va. a kernel va??");
		return -1; 
	}

	acquire(&mm->lock); 
    while (got_null == 0 && max > 0) {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(mm, va0);
        if (pa0 == 0)
            {release(&mm->lock); return -1;}
        n = PAGE_SIZE - (srcva - va0);
        if (n > max)
            n = max;

        char *p = (char *)PA2VA(pa0 + (srcva - va0));
        while (n > 0) {
            if (*p == '\0') {
                *dst = '\0';
                got_null = 1;
                break;
            } else {
                *dst = *p;
            }
            --n;
            --max;
            p++;
            dst++;
        }

        srcva = va0 + PAGE_SIZE;
    }
	release(&mm->lock);
    if (got_null) {
        return 0;
    } else {
        return -1;
    }
}

/* 
 * Copy to either a user address or kernel address depending on usr_dst.
 * 
 * @user_dst: if 1, dst is a kernel virtual address (not physical address).
 * 
 * Returns 0 on success, -1 on error.
 * 
 * cf: readi() and its callers.
 */
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct task_struct *p = myproc();
  if(user_dst){
    return copyout(p->mm, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

/* 
 * Copy from either a user address, or kernel address,
 * depending on usr_src.
 * 
 * Returns 0 on success, -1 on error.
 * 
 * cf above 
 */
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct task_struct *p = myproc();
  if(user_src){
    return copyin(p->mm, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

/* 
 * Free user pages only if useronly=1, otherwise free user/kernel pages.
 * NB: won't update pgtables for unmapping freed user pages.
 * Used to destroy a task's existing virtual memory.
 * 
 * Caller must hold mm->lock.
 */
void free_task_pages(struct mm_struct *mm, int useronly) {
	unsigned long page; 
	unsigned long sz; 
	BUG_ON(!mm); 

	sz = mm->sz; V("%s enter sz %lu", __func__, sz);

	if (growproc(mm, -sz) == (unsigned long)(void *)-1) {
		BUG(); 
		return; 
	}
	// XXX: free stack pages.... 	
	if (!useronly) {
		// free kern pages. must handle with care. 
		// NB: task_struct and mm no longer live on the kernel pages. so we won't
		//	be corrupting them here 
		for (int i = 0; i < mm->kernel_pages_count; i++) {
			page = mm->kernel_pages[i]; 
			BUG_ON(!page);
			free_page(page); 
		}
		memzero(&mm->kernel_pages, sizeof(mm->kernel_pages));
		mm->kernel_pages_count = 0;
		mm->pgd = 0; // should we do this? 
	}
}

/* 
 * Unmap and free user pages, e.g., to shrink task VM.
 * Return the number of freed pages on success, -1 on failure.
 * 
 * 1. Caller later must flush TLB (e.g., via set_pgd(mm->pgd)).
 * 2. Caller must hold mm->lock.
 * 3. start_va and size must be page aligned.
 * 
 * XXX: Make it transactional. Now if any page fails to unmap, it will abort there.
 * XXX: Need to grab any lock?
 */
static int free_user_page_range(struct mm_struct *mm, unsigned long start_va, 
		unsigned long size) {
	unsigned long *pte; 
	unsigned long page; // pa; 
	int cnt = 0; 

	BUG_ON((start_va & (~PAGE_MASK)) || (size & (~PAGE_MASK)) || !mm);

	for (unsigned long i = start_va; i < start_va + size; i+= PAGE_SIZE, cnt++) {		
		pte = map_page(mm, i, 0/*only locate pte*/, 0/*no alloc*/, 0/*dont care*/); 
		if (!pte)
			goto bad; 
		page = PTE_TO_PA(*pte); 
		free_page(page); 
		mm->user_pages_count --;
		*pte = 0; // nuke pte				
	}
	return cnt; 
bad: 
	BUG(); 
	return -1; 	
}

/* 
	Core of sbrk(). 

	incr can be positive or negative. 
	allows incr == -sz, i.e. free all user pages.
	cf: https://linux.die.net/man/2/sbrk
	"On success, sbrk() returns the previous program break. 
	(If the break was increased, then this value is a pointer to the start of 
	the newly allocated memory). On error, (void *) -1 is returned"

	caller must hold mm->lock
*/
//quest: user donut
unsigned long growproc (struct mm_struct *mm, int incr) {
	unsigned long sz = mm->sz, sz1; 
	void *kva; 
	int ret; 
	
	// careful: sz is unsigned; incr is signed
	if (1) { /* TODO: replace this */
		W("incr too small"); 
		W("sz 0x%lx %ld (dec) incr %d (dec). requested new brk 0x%lx", 
			sz, sz, incr, sz+incr); 
		goto bad; 
	}
	if (1) { /* TODO: replace this */
		W("incr too large"); 
		W("sz 0x%lx %ld (dec) incr %d (dec). requested new brk 0x%lx", 
		sz, sz, incr, sz+incr); 
		goto bad; 		
	}

	if (incr >= 0) {		// brk grows
		for (; ; ) { /* TODO: replace this */
			kva = allocate_user_page_mm(mm, sz1, MM_AP_RW | MM_XN); 
			if (!kva) {
				W("allocate_user_page_mm failed");
				goto reverse; 
			}
		}
	} else {	// brk shrinks
		// since it's shrinking, unmap from the next page boundary of the new brk, 
		// to the page start of the old brk (sz)
		// side quest idea: below args
		int ret = free_user_page_range(mm, PGROUNDUP(sz+incr),  
			PGROUNDDOWN(sz) - PGROUNDUP(sz+incr)); 
		BUG_ON(ret == -1); // user va has bad mapping
	}
	sz1 = mm->sz; // old sz
	mm->sz += incr; 	
	if (myproc()->mm == mm)
		set_pgd(mm->pgd); // forces a tlb flush

	V("succeeds. return old brk %lx new brk %lx", sz1, mm->sz); 
	return sz1; 	

reverse: 	
	// sz was old brk, sz1 was the failed va to allocate 	
	// side quest: below args
	ret = free_user_page_range(mm, PGROUNDUP(sz), sz1 - PGROUNDUP(sz)); 
	BUG_ON(ret == -1); // user va has bad mapping.
	V("reversed user page allocation %d pages sz %lx sz1 %lx", ret, sz, sz1);
bad: 
	W("growproc failed");	 
	return (unsigned long)(void *)-1; 	
}

/* 
 * Called from el0_da, which was from data abort exception 
 * return 0 on handled (inc task killed), otherwise causes kernel panic
 *
 * @addr: FAR from the exception 
 * @esr: value of error syndrome register, indicating the error reason
 * fxl: XXX check whether @addr is a legal user va;
 *      XXX check if @addr is the same addr (diff addr may be ok	
 *      XXX @ind is global. at least, it should be per task or per addr (?) 
 */
static int ind = 1; // # of times we tried memory access
int do_mem_abort(unsigned long addr, unsigned long esr, unsigned long elr) {
	 __attribute__((unused))  struct trapframe *regs = task_pt_regs(myproc());	 
	unsigned long dfs = (esr & 0b111111);

	if (addr > USER_VA_END) {
		E("do_mem_abort: bad user va? faulty addr 0x%lx > USER_VA_END %x", addr, 
			USER_VA_END); 
		E("esr 0x%lx, elr 0x%lx", esr, elr); 		
		goto bad; 
	}

	/* whether the current exception is actually a translation fault? */		
	if ((dfs & 0b111100) == 0b100) { /* translation fault */
		unsigned long page = get_free_page();
		if (page == 0) {
			E("do_mem_abort: insufficient mem"); 
			goto bad; 
		}
		acquire(&myproc()->mm->lock);
		map_page(myproc()->mm, addr & PAGE_MASK, page, 1/*alloc*/, 
			MMU_PTE_FLAGS | MM_AP_RW); // XXX: set perm (XN?) based on addr 
		release(&myproc()->mm->lock);
		ind++; // return to user, give it a second chance
		if (ind > 5) {  // repeated fault
		    E("do_mem_abort: pid %d too many mem faults. ind %d. killed", 
				myproc()->pid, ind); 
			goto bad; 	
		}
		I("demand paging at user va 0x%lx, elr 0x%lx", addr, regs->pc);
		return 0;
	}
	/* other causes, e.g. permission... */
	E("do_mem_abort: cannot handle: not translation fault.\r\nFAR 0x%016lx\r\nESR 0x%08lx\r\nELR 0x%016lx", 
		addr, esr, elr); 
	E("online esr decoder: %s0x%016lx", "https://esr.arm64.dev/#", esr);
	debug_hexdump((void *)elr, 32); 
bad:
	show_stack_user(); 
	setkilled(myproc());
	return 0; // handled
}

////////////////////////////////////////////////////////////////////////////////
// ------------ pgtable utilities (cf boot-pgtable.S)-------------------------//
// NB 
// - we are still on PA
// - only works for our simple pgtable (PGD|PUD|PMD1|PMD2)

/* Given a current level pgtable (either PGD or PUD) and a virt addr to map,
setting up the corresponding pgtable entry pointing to the next lv pgtable. 

    @tbl:  pointing to the "current" pgtable in a memory region
    @virt: the virtual address that we are currently mapping
    @shift: for the "current" pgtable lv. 39 in case of PGD and 30 in case of PUD
           apply to the virtual address in order to extract current table index. 
    @off: the offset between "tbl" and the next lv pgtable. 1 or 2 
*/
static void create_table_entry(unsigned long *tbl, unsigned long virt, int shift, int off) {
	unsigned long desc, idx; 
	idx = (virt >> shift) & (PTRS_PER_TABLE-1); // extracted table index in the current lv. 	
	desc = (unsigned long)(tbl + off*PTRS_PER_TABLE); // addr of a next level pgtable (PUD or PMD). 
	desc |= MM_TYPE_PAGE_TABLE; 
	tbl[idx] = desc; 
}

/* Populating entries in a PUD or PMD table for a given virt addr range 
	"block map": mappings larger than 4KB, e.g. 1GB or 2MB

	@tbl: pointing to the base of PUD/PMD table
	@phys: the start of the physical region to be mapped
	@start/@end: virtual address of the first/last section to be mapped
	@flags: to be copied into lower attributes of the block descriptor
	@shift: SUPERSECTION_SHIFT for PUD, SECTION_SHIFT for PMD 
*/
// quest: kernel virtaddr
static void _create_block_map(unsigned long *tbl, 
	unsigned long phys, unsigned long start, unsigned long end, 
	unsigned long flags, int shift) {

	start = (start >> shift) & (PTRS_PER_TABLE - 1);
	end = (end >> shift) & (PTRS_PER_TABLE - 1);
	phys >>= shift;
	while (start <= end) {
		unsigned long entry = (phys << shift) | flags;
		tbl[start] = entry;
		start++;
		phys++;
	}
}

// quest: kernel virtaddr
#define create_block_map_supersection(tbl, phys, start, end, flags) \
	_create_block_map(tbl, phys, start, end, flags, SUPERSECTION_SHIFT); /* TODO: replace this */

#define create_block_map_section(tbl, phys, start, end, flags) \
	_create_block_map(tbl, phys, start, end, flags, SECTION_SHIFT); /* TODO: replace this */

// NB: we are still on PA
// kern pgtable dir layout: PGD|PUD|PMD1|PMD2	each one page. total 4 pages
// quest: kernel virtaddr
void create_kern_pgtables(void) {
	unsigned long *pgd = (unsigned long *)VA2PA(&pg_dir); 
	unsigned long *pud = pgd + PTRS_PER_TABLE;
	unsigned long *pmd1 = pgd + 2*PTRS_PER_TABLE, *pmd2 = pgd + 3*PTRS_PER_TABLE;
	
	// clear the mem region backing pgtables
	memzero_aligned(pgd, PG_DIR_SIZE); /* TODO: replace this */

	// allocate PUD & PMD1; link PGD (pg_dir)->PUD, and PUD->PMD1
	create_table_entry(pgd, VA_START, PGD_SHIFT, 1); 
	/* TODO: your code here */
	create_table_entry(pud, VA_START, PUD_SHIFT, 1);

	// 1. kernel mem (PMD1). Phys addr range: 0--DEVICE_BASE
	create_block_map_section(pmd1, 0, VA_START, 
		VA_START + DEVICE_BASE - SECTION_SIZE, MMU_FLAGS); 

	// 2. device memory (PMD1). Phys addr range: DEVICE_BASE--DEVICE_LOW(0x40000000)	
	create_block_map_section(pmd1, DEVICE_BASE, VA_START+DEVICE_BASE, 
		VA_START+DEVICE_LOW - SECTION_SIZE, MMU_DEVICE_FLAGS); /* TODO: replace this */
	
	// link PUD->PMD2
	/* TODO: your code here */
	create_table_entry(pud, VA_START + DEVICE_LOW, PUD_SHIFT, 2);

	// 3. extra device mem (PMD2). Phys addr range: DEVICE_LOW--+SECTION_SIZE
	create_block_map_section(pmd2, DEVICE_LOW, 
		VA_START + DEVICE_LOW, VA_START + DEVICE_LOW, MMU_DEVICE_FLAGS); /* TODO: replace this */
}

/* A workaround for QEMU's quirks on MMU emulation, which also showcases how
__create_page_tables can be used. 

As soon as the MMU is on and CPU switches from physical addresses to virtual
addresses, the emulated CPU seems to be still fetching next (few) instructions
using the physical addresses of those instructions. These addresses will go
through MMU for translation as if they are virtual addresses. Of course our
kernel pgtables do not have translation for these addresses (TTBR1 is for
translating virtual addresses at 0xffff...). That causes MMU to throw a Prefetch
abort. (prefetch == instruction loading)

Real Rpi3 hardware has no such a problem: after MMU is on, it will not fetch
instructions at addresses calculated before MMU is on. 

The workaround is to set an "identity" mapping. That is, we create an additional
pgtable tree loaded at TTBR0 that maps all physical DRAM (0 -- PHYS_MEMORY_SIZE)
to virtual addresses with the same values. That keeps translation going on at
the switch of MMU. 

Cf: https://github.com/s-matyukevich/raspberry-pi-os/issues/8
https://www.raspberrypi.org/forums/viewtopic.php?t=222408
*/

// PGD|PUD, each one page. total 2 pages. use 1 supersection
extern unsigned long idmap_dir;  // allocated in linker-qemu.ld
// quest: kernel virtaddr
void create_kern_idmap(void) {
	unsigned long *pgd = (unsigned long *)VA2PA(&idmap_dir); 
	unsigned long *pud = pgd + PTRS_PER_TABLE;

	memzero_aligned(pgd, 3 * (1 << 12)); /* TODO: replace this */

	// allocate one PUD; link PGD (pg_dir)->PUD. 
	create_table_entry(pgd, VA_START, PGD_SHIFT, 1); 

	//1. kernel mem (PUD). Phys addr range: PHYS_BASE, +PHYS_SIZE
	// TODO: this is going into a spinlock
	create_block_map_supersection(pud, PHYS_BASE, PHYS_BASE, 
		PHYS_BASE + PHYS_SIZE - SUPERSECTION_SIZE, MMU_FLAGS); 
}

#define N 4	// # of entries to dump per table
void dump_pgdir(void) {
#if K2_ACTUAL_DEBUG_LEVEL <= 20     // "V"
	unsigned long *p = (unsigned long *)&pg_dir; 

	printf("PGD va %lx\n", (unsigned long)&pg_dir); 
	for (int i =0; i<N; i++)
		printf("	PGD[%d] %lx\n", i, p[i]); 
	
	p += PTRS_PER_TABLE; 
	printf("PUD va %lx\n", (unsigned long)p); 
	for (int i =0; i<N; i++)
		printf("	PUD[%d] %lx\n", i, p[i]); 

	p += PTRS_PER_TABLE; 
	printf("PMD1 va %lx\n", (unsigned long)p); 
	for (int i =0; i<N; i++)
		printf("	PMD[%d] %lx\n", i, p[i]); 

	p += PTRS_PER_TABLE; 
	printf("PMD2 va %lx\n", (unsigned long)p); 
	for (int i =0; i<N; i++)
		printf("	PMD[%d] %lx\n", i, p[i]); 		

	unsigned long nFlags;
	asm volatile ("mrs %0, sctlr_el1" : "=r" (nFlags));
	printf("sctlr_el1 %016lx\n", nFlags); 
	asm volatile ("mrs %0, tcr_el1" : "=r" (nFlags));
	printf("tcr_el1 %016lx\n", nFlags); 
#endif	
}
