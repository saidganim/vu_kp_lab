/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/memlayout.h>

#include <kern/pmap.h>
#include <kern/kclock.h>
#include <kern/env.h>
#include <kern/spinlock.h>
#include <kern/ide.h>

/* These variables are set by i386_detect_memory() */
size_t npages;                  /* Amount of physical memory (in pages) */
static size_t npages_basemem;   /* Amount of base memory (in pages) */

/* These variables are set in mem_init() */
pde_t *kern_pgdir;                       /* Kernel's initial page directory */
struct page_info *pages;                 /* Physical page state array */
struct page_info *page_free_list; /* Free list of physical pages */
struct page_info *page_used_clock; /* Free list of physical pages */
struct pg_swap_entry *pgswap_free_list;
struct pg_swap_entry *pgswaps;
struct swap_queue_entry *swapq_entries;
struct swap_queue_entry *sqe_free_list;
struct swap_queue_entry *sqe_fifo;
size_t premapped_rbound = KERNBASE + HUGE_PGSIZE;
static unsigned int swap_slots[SWAP_SLOTS_NUMBER];
static struct pg_swap_entry* swap_env_map[SWAP_SLOTS_NUMBER];

static spinlock_t pg_swap_lock;
static spinlock_t sqe_lock;
static spinlock_t disk_lock;

/***************************************************************
 * Detect machine's physical memory setup.
 ***************************************************************/

static int nvram_read(int r)
{
    return mc146818_read(r) | (mc146818_read(r + 1) << 8);
}

static void i386_detect_memory(void)
{
    size_t npages_extmem;

    /* Use CMOS calls to measure available base & extended memory.
     * (CMOS calls return results in kilobytes.) */
    npages_basemem = (nvram_read(NVRAM_BASELO) * 1024) / PGSIZE;
    npages_extmem = (nvram_read(NVRAM_EXTLO) * 1024) / PGSIZE;

    /* Calculate the number of physical pages available in both base and
     * extended memory. */
    if (npages_extmem)
        npages = (EXTPHYSMEM / PGSIZE) + npages_extmem;
    else
        npages = npages_basemem;

    cprintf("Physical memory: %uK available, base = %uK, extended = %uK\n",
        npages * PGSIZE / 1024,
        npages_basemem * PGSIZE / 1024,
        npages_extmem * PGSIZE / 1024);
}


/***************************************************************
 * Set up memory mappings above UTOP.
 ***************************************************************/

static void mem_init_mp(void);
static void pgswaps_init();
static void check_page_free_list(bool only_low_memory);
static void check_page_alloc(void);
static void check_kern_pgdir(void);
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va);
static void check_page(void);
static void check_page_installed_pgdir(void);
static void check_page_hugepages(void);

/* This simple physical memory allocator is used only while JOS is setting up
 * its virtual memory system.  page_alloc() is the real allocator.
 *
 * If n>0, allocates enough pages of contiguous physical memory to hold 'n'
 * bytes.  Doesn't initialize the memory.  Returns a kernel virtual address.
 *
 * If n==0, returns the address of the next free page without allocating
 * anything.
 *
 * If we're out of memory, boot_alloc should panic.
 * This function may ONLY be used during initialization, before the
 * page_free_list list has been set up. */
static void *boot_alloc(uint32_t n)
{
	static char *nextfree;  /* virtual address of next byte of free memory */
	char *result;
	if (!nextfree) {
			extern char end[];
			nextfree = ROUNDUP((char *) end, PGSIZE);
	}

 result = nextfree;
 nextfree = ROUNDUP((char *)nextfree + n, PGSIZE);
 return result;
}

/*
 * Set up a two-level page table:
 *    kern_pgdir is its linear (virtual) address of the root
 *
 * This function only sets up the kernel part of the address space (ie.
 * addresses >= UTOP).  The user part of the address space will be setup later.
 *
 * From UTOP to ULIM, the user is allowed to read but not write.
 * Above ULIM the user cannot read or write.
 */
void mem_init(void)
{
    uint32_t cr0;
    size_t n;

    /* Find out how much memory the machine has (npages & npages_basemem). */
    i386_detect_memory();

    /*********************************************************************
     * create initial page directory.
     */
    kern_pgdir = (pde_t *) boot_alloc(PGSIZE);
    memset(kern_pgdir, 0, PGSIZE);

    /*********************************************************************
     * Recursively insert PD in itself as a page table, to form a virtual page
     * table at virtual address UVPT.
     * (For now, you don't have understand the greater purpose of the following
     * line.)
     */

    /* Permissions: kernel R, user R */
    kern_pgdir[PDX(UVPT)] = PADDR(kern_pgdir) | PTE_U | PTE_P;

    /*********************************************************************
     * Allocate an array of npages 'struct page_info's and store it in 'pages'.
     * The kernel uses this array to keep track of physical pages: for each
     * physical page, there is a corresponding struct page_info in this array.
     * 'npages' is the number of physical pages in memory.  Your code goes here.
     */

     /*********************************************************************
     * Make 'envs' point to an array of size 'NENV' of 'struct env'.
     * LAB 3: Your code here.
     */
		 envs = boot_alloc(NENV * sizeof(struct env));
		 pages = boot_alloc(npages * sizeof(struct page_info));
    /*********************************************************************
     * Now that we've allocated the initial kernel data structures, we set
     * up the list of free physical pages. Once we've done so, all further
     * memory management will go through the page_* functions. In particular, we
     * can now map memory using boot_map_region or page_insert.
     */
    page_init();
    //check_page_free_list(1);
    //check_page_alloc();
    //check_page();

    /*********************************************************************
     * Now we set up virtual memory */

    /*********************************************************************
     * Map 'pages' read-only by the user at linear address UPAGES
     * Permissions:
     *    - the new image at UPAGES -- kernel R, user R
     *      (ie. perm = PTE_U | PTE_P)
     *    - pages itself -- kernel RW, user NONE
     * Your code goes here:
     */

		boot_map_region(kern_pgdir, UPAGES, sizeof(struct page_info) * npages, PADDR(pages), PTE_W);

    /* This is set up already by the identity mapping below. */
    /*
    boot_map_region(kern_pgdir, (uintptr_t)pages,
            ROUNDUP(sizeof(struct page_info) * npages), PADDR(pages), PTE_W);
    */

    /*********************************************************************
     * Map the 'envs' array read-only by the user at linear address UENVS
     * (ie. perm = PTE_U | PTE_P).
     * Permissions:
     *    - the new image at UENVS  -- kernel R, user R
     *    - envs itself -- kernel RW, user NONE
     * LAB 3: Your code here.
     */
		 boot_map_region(kern_pgdir, UENVS, sizeof(struct env) * NENV, PADDR(envs), PTE_U);
		 boot_map_region(kern_pgdir, (unsigned int)envs, sizeof(struct env) * NENV, PADDR(envs), PTE_W);

    /*********************************************************************
     * Use the physical memory that 'bootstack' refers to as the kernel
     * stack.  The kernel stack grows down from virtual address KSTACKTOP.
     * We consider the entire range from [KSTACKTOP-PTSIZE, KSTACKTOP)
     * to be the kernel stack, but break this into two pieces:
     *     * [KSTACKTOP-KSTKSIZE, KSTACKTOP) -- backed by physical memory
     *     * [KSTACKTOP-PTSIZE, KSTACKTOP-KSTKSIZE) -- not backed; so if
     *       the kernel overflows its stack, it will fault rather than
     *       overwrite memory.  Known as a "guard page".
     *     Permissions: kernel RW, user NONE
     * Your code goes here:
     *
     * LAB 6 Update:
     * We now move to initializing kernel stacks in mem_init_mp().
     * So, we must remove this bootstack initialization from here.
     */
    /* Note: Dont map anything between KSTACKTOP - PTSIZE and KSTACKTOP - KTSIZE
     * leaving this as guard region.
     */
		 boot_map_region(kern_pgdir, KERNBASE, MAX_VA - KERNBASE, 0x0, PTE_W);
    /*********************************************************************
     * Map all of physical memory at KERNBASE.
     * Ie.  the VA range [KERNBASE, 2^32) should map to
     *      the PA range [0, 2^32 - KERNBASE)
     * We might not have 2^32 - KERNBASE bytes of physical memory, but
     * we just set up the mapping anyway.
     * Permissions: kernel RW, user NONE
     * Your code goes here:
     */

    /* Enable Page Size Extensions for huge page support */
    lcr4(rcr4() | CR4_PSE);

    /* Initialize the SMP-related parts of the memory map. */
    mem_init_mp();

    /* Check that the initial page directory has been set up correctly. */
    //check_kern_pgdir();

    /* Switch from the minimal entry page directory to the full kern_pgdir
    / * page table we just created.  Our instruction pointer should be
     * somewhere between KERNBASE and KERNBASE+4MB right now, which is
     * mapped the same way by both page tables.
     *
     * If the machine reboots at this point, you've probably set up your
     * kern_pgdir wrong. */
    lcr3(PADDR(kern_pgdir));
		premapped_rbound = MAX_VA;
    //check_page_free_list(0);

    /* entry.S set the really important flags in cr0 (including enabling
     * paging).  Here we configure the rest of the flags that we care about. */
    cr0 = rcr0();
    cr0 |= CR0_PE|CR0_PG|CR0_AM|CR0_WP|CR0_NE|CR0_MP;
    cr0 &= ~(CR0_TS|CR0_EM);
    lcr0(cr0);

    /* Some more checks, only possible after kern_pgdir is installed. */
    //check_page_installed_pgdir();

    /* Check for huge page support */
    //check_page_hugepages();
    pgswaps_init();
}

/*
 * Modify mappings in kern_pgdir to support SMP
 *   - Map the per-CPU stacks in the region [KSTACKTOP-PTSIZE, KSTACKTOP)
 */
static void mem_init_mp(void)
{
    /*
     * Map per-CPU stacks starting at KSTACKTOP, for up to 'NCPU' CPUs.
     *
     * For CPU i, use the physical memory that 'percpu_kstacks[i]' refers
     * to as its kernel stack. CPU i's kernel stack grows down from virtual
     * address kstacktop_i = KSTACKTOP - i * (KSTKSIZE + KSTKGAP), and is
     * divided into two pieces, just like the single stack you set up in
     * mem_init:
     *     * [kstacktop_i - KSTKSIZE, kstacktop_i)
     *          -- backed by physical memory
     *     * [kstacktop_i - (KSTKSIZE + KSTKGAP), kstacktop_i - KSTKSIZE)
     *          -- not backed; so if the kernel overflows its stack,
     *             it will fault rather than overwrite another CPU's stack.
     *             Known as a "guard page".
     *     Permissions: kernel RW, user NONE
     *
     * LAB 6: Your code here:
     */
		 for(int cpu_i = 0; cpu_i < NCPU; ++cpu_i){
			 boot_map_region(kern_pgdir, (unsigned int)(KSTACKTOP - cpu_i * (KSTKSIZE + KSTKGAP) -  KSTKSIZE), KSTKSIZE, PADDR(percpu_kstacks[cpu_i]), PTE_W);
		 }
}

/***************************************************************
 * Tracking of physical pages.
 * The 'pages' array has one 'struct page_info' entry per physical page.
 * Pages are reference counted, and free pages are kept on a linked list.
 ***************************************************************/

/*
 * Initialize page structure and memory free list.
 * After this is done, NEVER use boot_alloc again.  ONLY use the page
 * allocator functions below to allocate and deallocate physical
 * memory via the page_free_list.
 */
void page_init(void)
{
    /*
     * The example code here marks all physical pages as free.
     * However this is not truly the case.  What memory is free?
     *  1) Mark physical page 0 as in use.
     *     This way we preserve the real-mode IDT and BIOS structures in case we
     *     ever need them.  (Currently we don't, but...)
     *  2) The rest of base memory, [PGSIZE, npages_basemem * PGSIZE) is free.
     *  3) Then comes the IO hole [IOPHYSMEM, EXTPHYSMEM), which must never be
     *     allocated.
     *  4) Then extended memory [EXTPHYSMEM, ...).
     *     Some of it is in use, some is free. Where is the kernel in physical
     *     memory?  Which pages are already in use for page tables and other
     *     data structures?
     *
     * Change the code to reflect this.
     * NB: DO NOT actually touch the physical memory corresponding to free
     *     pages! */

    /* LAB 6: Change your code to mark the physical page at MPENTRY_PADDR as
     *       in-use. */

    size_t i;
		page_free_list = 0;
		page_used_clock = 0;
		extern char mpentry_start[], mpentry_end[];
		int in_io_area;
		int in_kern_area;
		int in_mp_entry_area;
    for (i = 1; i < npages; ++i) {
				in_io_area = (i >= PGNUM(IOPHYSMEM) && i < PGNUM(EXTPHYSMEM));
				in_kern_area = (i >= PGNUM(EXTPHYSMEM) && i < (PGNUM(boot_alloc(0) - KERNBASE)));
				in_mp_entry_area = (i >= PGNUM(MPENTRY_PADDR) && i < (PGNUM(MPENTRY_PADDR + mpentry_end - mpentry_start)));
				if(!(in_io_area || in_kern_area || in_mp_entry_area)){
					pages[i].pp_ref = 0;
	        pages[i].pp_link = page_free_list;
	        page_free_list = &pages[i];
				}
    }
}


static void pgswaps_init(){
  pgswap_free_list = 0;
  struct page_info *pp = page_alloc(ALLOC_HUGE),  *pp2 = page_alloc(ALLOC_HUGE);
  if(!pp || !pp2)
   panic("Not enough memory for swap structures");

  pgswaps = (struct pg_swap_entry*)(page2kva(pp));
  swapq_entries = (struct swap_queue_entry*)(page2kva(pp2));
  for(struct pg_swap_entry *pg_swap_tmp = pgswaps ;
  (void*)pg_swap_tmp < ((void*)pgswaps + HUGE_PGSIZE);
    ++pg_swap_tmp){
      pg_swap_tmp->pse_next = pgswap_free_list;
      pgswap_free_list = pg_swap_tmp;
  }
  sqe_free_list = 0;
  for(struct swap_queue_entry *tmp = swapq_entries;
    (void*)tmp < (void*)swapq_entries + HUGE_PGSIZE; ++tmp){
    tmp->sqe_next = sqe_free_list;
    sqe_free_list = tmp;
  }
  memset(swap_slots, 0x0, sizeof(unsigned int) * SWAP_SLOTS_NUMBER);
}

struct swap_queue_entry* sqe_alloc(){
  struct swap_queue_entry *res = 0;
  lock_pagealloc();
  if(!sqe_free_list)
    goto release;
  res = sqe_free_list;
  sqe_free_list = sqe_free_list->sqe_next;
  memset(res, 0x0, sizeof(struct swap_queue_entry));
release:
  unlock_pagealloc();
  return res;
}

void sqe_free(struct swap_queue_entry* sqe_e){
  lock_pagealloc();
  sqe_e->sqe_next = sqe_free_list;
  sqe_free_list = sqe_e;
  unlock_pagealloc();

}

struct pg_swap_entry* pgswap_alloc(){
  struct pg_swap_entry *res = 0;
  lock_pagealloc();
  if(!pgswap_free_list)
    goto release;
  res = pgswap_free_list;
  pgswap_free_list = pgswap_free_list->pse_next;
  memset(res, 0x0, sizeof(struct pg_swap_entry));
release:
  unlock_pagealloc();
  return res;
}

void pgswap_free(struct pg_swap_entry* pg_s){
  lock_pagealloc();
  pg_s->pse_next = pgswap_free_list;
  pgswap_free_list = pg_s;
  unlock_pagealloc();

}

void page_swap_out(struct env *env, struct page_info* pp){
  spin_lock(&sqe_lock);
  //lock_env();
  //env->env_status = ENV_NOT_RUNNABLE;
  //unlock_env();
  struct swap_queue_entry *sqe_e = sqe_alloc();
  if(!sqe_e)
    panic("OUT OF STRUCTURE CACHE[SWAP_QUEUE_ENTRY]");
  sqe_e->sqe_env = env;
  sqe_e->sqe_pp = pp;
  sqe_e->sqe_next = sqe_fifo;
  sqe_fifo = sqe_e;
  spin_unlock(&sqe_lock);
};

void page_swap_in(struct env *env, void *va){
  spin_lock(&sqe_lock);
  env->env_status = ENV_NOT_RUNNABLE;
  remove_entry_from_list(struct env, env, env_run_list, env_link);
  struct swap_queue_entry *sqe_e = sqe_alloc();
  if(!sqe_e)
    panic("OUT OF STRUCTURE CACHE[SWAP_QUEUE_ENTRY]");
  sqe_e->sqe_env = env;
  sqe_e->sqe_va = va;
  sqe_e->sqe_next = sqe_fifo;
  sqe_fifo = sqe_e;
  spin_unlock(&sqe_lock);
}


void ide_write_page_blocking(size_t sector, char* buf){
  spin_lock(&disk_lock);
  int nsectors = PGSIZE/SECTSIZE;
  ide_start_readwrite(sector, nsectors, 1);
  for (int i = 0; i < nsectors; i++) {
    while (!ide_is_ready()){};
      ide_write_sector(buf + i*SECTSIZE);
    }
  spin_unlock(&disk_lock);
}

void ide_write_page(size_t sector, char* buf){
  spin_lock(&disk_lock);
  int nsectors = PGSIZE/SECTSIZE;
  ide_start_readwrite(sector, nsectors, 1);
  for (int i = 0; i < nsectors; i++) {
    while (!ide_is_ready()){
      spin_unlock(&disk_lock);
      kernel_thread_desched();
      spin_lock(&disk_lock);
    };
      ide_write_sector(buf + i*SECTSIZE);
    }
  spin_unlock(&disk_lock);
}

void ide_read_page(size_t sector, char* buf){
  spin_lock(&disk_lock);
  int nsectors = PGSIZE/SECTSIZE;
  ide_start_readwrite(sector, nsectors, 0);
  for (int i = 0; i < nsectors; i++) {
    while (!ide_is_ready()){
      spin_unlock(&disk_lock);
      kernel_thread_desched();
      spin_lock(&disk_lock);
    };
      ide_read_sector(buf + i*SECTSIZE);
    }
  spin_unlock(&disk_lock);
}


size_t __page_swap_out_nonblocking(struct page_info* pp){
  struct pg_swap_entry *pg_swap = pp->pp_pse, *tmp;
  pte_t *pte;
  size_t res = 0;
  unsigned int slot_i = 1;
  cprintf("KSWAPD: SWAPPED OUT BEGINNING\n");
  while(swap_slots[slot_i] > 0){
      ++slot_i;
      if(slot_i > SWAP_SLOTS_NUMBER)
        goto release; //Invalid slot number (first slot is used for sanity check)
  }
  while(pg_swap){
    tmp = pg_swap;
    if(!pg_swap->pse_env->env_pgdir)
      continue;
    pte = pgdir_walk(pg_swap->pse_env->env_pgdir, pg_swap->pse_va, 0);
    if(!pte || !(*pte & PTE_P))
      goto release; //panic("page swap entry is incorrect");
    *pte = (slot_i << 12) | (*pte & 0xFFF);
    pg_swap->pse_env->env_mm.mm_pf_count -= PGSIZE;
    *pte &= ~PTE_P;
    pg_swap = pg_swap->pse_next;
    page_decref(pp);
    ++swap_slots[slot_i];
    //tlb_invalidate(pg_swap->pse_env->env_pgdir, pg_swap->pse_va);
    //pgswap_free(tmp);
  }
  swap_env_map[slot_i] = pp->pp_pse;
  pp->pp_pse = 0;
  // Writing page frame on disk
  ide_write_page_blocking(slot_i * PGSIZE / SECTSIZE, page2kva(pp));
  // for(int i = 0 ; i < (PGSIZE - 10); ++i)
  //   cprintf("%02x ",((char*)page2kva(pp))[i]);
  // cprintf("END\n");
  res = slot_i;
release:
  cprintf("KSWAPD: SWAPPED OUT PAGE\n");
  return res;
}

size_t __page_swap_out(struct page_info* pp){
  struct pg_swap_entry *pg_swap = pp->pp_pse, *tmp;
  pte_t *pte;
  size_t res = 0;
  unsigned int slot_i = 1;
  cprintf("KSWAPD: SWAPPED OUT BEGINNING\n");
  spin_lock(&pg_swap_lock);
  lock_env();
  while(swap_slots[slot_i] > 0){
      ++slot_i;
      if(slot_i > SWAP_SLOTS_NUMBER)
        goto release; //Invalid slot number (first slot is used for sanity check)
  }
  while(pg_swap){
    tmp = pg_swap;
    if(!pg_swap->pse_env->env_pgdir)
      continue;
    pte = pgdir_walk(pg_swap->pse_env->env_pgdir, pg_swap->pse_va, 0);
    if(!pte || !(*pte & PTE_P))
      goto release; //panic("page swap entry is incorrect");
    spin_lock(&pg_swap->pse_env->env_mm.mm_lock);
    *pte = (slot_i << 12) | (*pte & 0xFFF);
    pg_swap->pse_env->env_mm.mm_pf_count -= PGSIZE;
    *pte &= ~PTE_P;
    spin_unlock(&pg_swap->pse_env->env_mm.mm_lock);
    pg_swap = pg_swap->pse_next;
    page_decref(pp);
    ++swap_slots[slot_i];
    //tlb_invalidate(pg_swap->pse_env->env_pgdir, pg_swap->pse_va);
    //pgswap_free(tmp);
  }
  swap_env_map[slot_i] = pp->pp_pse;
  pp->pp_pse = 0;
  // Writing page frame on disk
  spin_unlock(&pg_swap_lock);
  unlock_env();
  ide_write_page(slot_i * PGSIZE / SECTSIZE, page2kva(pp));
  spin_lock(&pg_swap_lock);
  lock_env();
  // for(int i = 0 ; i < (PGSIZE - 10); ++i)
  //   cprintf("%02x ",((char*)page2kva(pp))[i]);
  // cprintf("END\n");
  res = slot_i;
release:
  cprintf("KSWAPD: SWAPPED OUT PAGE\n");
  unlock_env();
  spin_unlock(&pg_swap_lock);
  return res;
}

struct page_info* __page_swap_in(struct env *env, void *va){
  pte_t *pte = pgdir_walk(env->env_pgdir, va, 0);
  struct page_info *res = NULL;
  struct pg_swap_entry *pg_swap;
  int slot_i;
  cprintf("KSWAPD: SWAPPED IN BEGINNING\n");
  spin_lock(&pg_swap_lock);
  lock_env();
  if(!pte)
    goto release;
  slot_i = PTE_ADDR(*pte) >> 12;
  if(slot_i <=0)
    goto release;
  res = page_alloc(ALLOC_PREMAPPED);
  if(!res)
    goto release;
  spin_unlock(&pg_swap_lock);
  unlock_env();
  ide_read_page(slot_i * PGSIZE / SECTSIZE, page2kva(res));
  lock_env();
  spin_lock(&pg_swap_lock);
  pg_swap = swap_env_map[slot_i];
  while(pg_swap){
    pte = pgdir_walk(pg_swap->pse_env->env_pgdir, pg_swap->pse_va, 0);
    // *pte = (unsigned int)page2pa(res) | (*pte & 0xFFF);
    // *pte |= PTE_P;
    page_insert(pg_swap->pse_env->env_pgdir, res, pg_swap->pse_va, (*pte & 0xFFF));
    pg_swap = pg_swap->pse_next;
    //tlb_invalidate(pg_swap->pse_env->env_pgdir, pg_swap->pse_va);
  }
  res->pp_pse = swap_env_map[slot_i];
  swap_env_map[slot_i] = 0;
  swap_slots[slot_i] = 0;
// for(int i = 0 ; i < PGSIZE - 1; ++i)
  //   cprintf("%02x ",((char*)page2kva(res))[i]);
  // cprintf("END\n");
release:
  cprintf("KSWAPD: SWAPPED IN PAGE\n");

  unlock_env();
  spin_unlock(&pg_swap_lock);
  return res;
}


void direct_page_reclaim(){
  struct env *tmp;
	pte_t *pte;
	void* va;
	struct page_info *pp;
  //lock_env();
  cprintf("DIRECT PAGE RECLAIMING\n");
  tmp = env_run_list;
  struct page_info *min_pp = NULL;
  while(tmp){
    if(tmp->env_type == ENV_TYPE_KERNEL){
      tmp = tmp->env_link;
      continue;
    }
    struct vma *vma = tmp->env_mm.mm_vma;
    while(vma){
      //spin_lock(&tmp->env_mm.mm_lock);
      va = vma->vma_va;
      for(; va < (vma->vma_va + vma->vma_len);){
        pte = pgdir_walk(tmp->env_pgdir, va, 0);
        if(!pte || !(*pte & PTE_P)){
          va += PGSIZE;
          continue;
        }
        int pg_size = (*pte & PTE_PS)? PGSIZE : HUGE_PGSIZE;
        pp = pa2page(PTE_ADDR(*pte));
        pp->pp_lru_counter = ((*pte & PTE_A)? 1 : 0) <<
         (sizeof(pp->pp_lru_counter) - 1) ||
         (pp->pp_lru_counter >> 1);
        if(!min_pp || pp->pp_lru_counter < min_pp->pp_lru_counter){
          min_pp = pp;
        }
        *pte &= ~PTE_A;
        va += pg_size;
      }
      vma = vma->vma_next;
      //spin_unlock(&tmp->env_mm.mm_lock);
    }
     tmp = tmp->env_link;
  }
  if(min_pp)
    __page_swap_out_nonblocking(min_pp);
  else{
    oom_kill_default();
  }
  //unlock_env();
  cprintf("DIRECT PAGE RECLAIMING FINISHED\n");
}

void kswapd(void *arg){
	struct env *tmp;
	pte_t *pte;
	void* va;
	struct page_info *pp;
	//kernel_thread_sleep(10000000);
	while(1){
		lock_env();
		tmp = env_run_list;
		while(tmp){
			if(tmp->env_type == ENV_TYPE_KERNEL){
				tmp = tmp->env_link;
				continue;
			}
			struct vma *vma = tmp->env_mm.mm_vma;
			while(vma){
				spin_lock(&tmp->env_mm.mm_lock);
				va = vma->vma_va;
				for(; va < (vma->vma_va + vma->vma_len);){
					pte = pgdir_walk(tmp->env_pgdir, va, 0);
					if(!pte || !(*pte & PTE_P)){
						va += PGSIZE;
						continue;
					}
					int pg_size = (*pte & PTE_PS)? PGSIZE : HUGE_PGSIZE;
					pp = pa2page(PTE_ADDR(*pte));
					pp->pp_lru_counter = ((*pte & PTE_A)? 1 : 0) <<
					 (sizeof(pp->pp_lru_counter) - 1) ||
					 (pp->pp_lru_counter >> 1);
					if(!pp->pp_lru_counter){
						page_swap_out(tmp, pp);
            tmp->env_mm.mm_pf_count -= PGSIZE;
					}
					*pte &= ~PTE_A;
					va += pg_size;
				}
				vma = vma->vma_next;
				spin_unlock(&tmp->env_mm.mm_lock);
			}
			 tmp = tmp->env_link;
		}
	sleep:
		unlock_env();
		kernel_thread_sleep(10000000);
	}
}

void __kswapd(void* arg){
  while(true){
    //spin_lock(&sqe_lock);
    if(sqe_fifo){
      struct swap_queue_entry *tmp = sqe_fifo;
      sqe_fifo = sqe_fifo->sqe_next;
      if(tmp->sqe_va){
        // SWAP IN
        __page_swap_in(tmp->sqe_env, tmp->sqe_va);
        lock_env();
        tmp->sqe_env->env_link = env_run_list;
        env_run_list = tmp->sqe_env;
        tmp->sqe_env->env_status = ENV_RUNNABLE;
        unlock_env();
      } else {
        // SWAP OUT
        cprintf("SWAP OUT %p\n", tmp->sqe_pp);
        __page_swap_out(tmp->sqe_pp);
      }
      sqe_free(tmp);
    }
    //spin_unlock(&sqe_lock);
    kernel_thread_desched();
  }
}

void __add_to_clock_list(struct page_info *pp){
	// CLOCK FOR PAGE REPLACEMENTS
	if(page_used_clock){
		pp->pp_clock_next = page_used_clock;
		pp->pp_clock_prev = page_used_clock->pp_clock_prev;
		pp->pp_clock_prev->pp_clock_next = pp;
	} else {
		page_used_clock = pp;
		pp->pp_clock_next = pp;
		pp->pp_clock_prev = pp;
	}
}

void __remove_from_clock_list(struct page_info *pp){
	// CLOCK FOR PAGE REPLACEMENTS
	struct page_info *tmp = page_used_clock;
	struct page_info *first = page_used_clock;
	while(tmp){
		if(tmp == pp){
			if(tmp == tmp->pp_clock_next){
				// Only one element in CLOCK
				tmp->pp_clock_next = tmp->pp_clock_prev = page_used_clock = 0;
				return;
			}
			pp->pp_clock_prev->pp_clock_next = pp->pp_clock_next;
			pp->pp_clock_next->pp_clock_prev = pp->pp_clock_prev;
			pp->pp_clock_next = pp->pp_clock_prev = 0;
		}
	}
}

struct page_info *page_alloc(int alloc_flags)
{
   	struct page_info *result = NULL;
		extern pde_t entry_pgdir[];
		pde_t *curr_pgdir;
		int _pgsize = alloc_flags & ALLOC_HUGE? HUGE_PGSIZE : PGSIZE;
  beg:
    lock_pagealloc();
    if(alloc_flags & ALLOC_PREMAPPED){
			result = page_free_list;
			while(result){
				if(page2pa(result) >= 0x0 && page2pa(result) < PADDR((void*)premapped_rbound)){
					remove_entry_from_list(struct page_info, result, page_free_list, pp_link);
					goto found_page;
				}
				result = result->pp_link;
			}
			result = NULL;
			goto release;
		} else if(alloc_flags & ALLOC_HUGE){
			// HUGE PAGE allocation
			for(size_t curr_page_i = 0; curr_page_i < npages; ++curr_page_i){
				if(pages[curr_page_i].pp_link != NULL && (page2pa(&pages[curr_page_i]) % HUGE_PGSIZE) == 0){
					size_t huge_page_i;
					for(huge_page_i = curr_page_i + 1; huge_page_i < PGNUM(HUGE_PGSIZE) + curr_page_i; ++huge_page_i){
						if(pages[huge_page_i].pp_link == NULL) break;
					}
					if(huge_page_i - curr_page_i == PGNUM(HUGE_PGSIZE)){
						// Huge page is founded
						result = &pages[curr_page_i];
						result->pp_flags = ALLOC_HUGE;
						for(huge_page_i = curr_page_i; huge_page_i < PGNUM(HUGE_PGSIZE) + curr_page_i; ++huge_page_i){
							remove_entry_from_list(struct page_info, &pages[huge_page_i], page_free_list, pp_link);
						}
						goto found_page;
					}
				}
			}
			goto release;
		} else {
			// NORMAL PAGE allocation
		normal_page:
			if(!page_free_list)
				goto release;
			result = page_free_list;
			remove_entry_from_list(struct page_info, page_free_list, page_free_list, pp_link);
			goto found_page;
		}
	found_page:
			//__add_to_clock_list(result);
      result->pp_lru_counter = 0;
			if(page2pa(result) < PADDR((void*)premapped_rbound))
				memset(page2kva(result), 0x0, _pgsize);
	release:
    unlock_pagealloc();
    if(!result){
      direct_page_reclaim();
      goto beg;
    }
    return result;
}

/*
 * Return a page to the free list.
 * (This function should only be called when pp->pp_ref reaches 0.)
 */
void page_free(struct page_info *pp){
	lock_pagealloc();
	if(pp->pp_link != NULL)
		panic("Invalid/ Double deallocating page");
	if(pp->pp_flags & ALLOC_HUGE){
		// HUGE PAGE deallocation
		for(size_t huge_page_i = 0; huge_page_i < PGNUM(HUGE_PGSIZE); ++huge_page_i)
			add_page_free_entry(pp++);
	} else {
		// NORMAL PAGE deallocation
		add_page_free_entry(pp);
	}
	//__remove_from_clock_list(pp);
	unlock_pagealloc();
}

/*
 * Decrement the reference count on a page,
 * freeing it if there are no more refs.
 */
void page_decref(struct page_info* pp)
{
    if (--pp->pp_ref == 0)
        page_free(pp);
}

/*
 * Given 'pgdir', a pointer to a page directory, pgdir_walk returns
 * a pointer to the page table entry (PTE) for linear address 'va'.
 * This requires walking the two-level page table structure.
 *
 * For normal 4K paging support:
 *  The relevant page table page might not exist yet.
 *  If this is true, and create == 0, then pgdir_walk returns NULL.
 *  Otherwise, if CREATE_NORMAL flag is set in the 'create' argument,
 *  pgdir_walk allocates a new page table page with page_alloc.
 *    - If the allocation fails, pgdir_walk returns NULL.
 *    - Otherwise, the new page's reference count is incremented,
 *      the page is cleared,
 *      and pgdir_walk returns a pointer into the new page table page.
 *
 * For huge 4MB paging support:
 *  Is two-level walk required in this case?
 *  If the relevant page table entry does not exist and create == false,
 *  then, pgdir_walk returns NULL.
 *  Otherwise, if CREATE_HUGE flag is set in the 'create' argument,
 *  pgdir_walk() returns a pointer to page table entry for the huge page.

 * Hint 1: you can turn a struct page_info* into the physical address of the
 * page it refers to with page2pa() from kern/pmap.h.
 *
 * Hint 2: the x86 MMU checks permission bits in both the page directory
 * and the page table, so it's safe to leave permissions in the page
 * more permissive than strictly necessary.
 *
 * Hint 3: look at inc/mmu.h for useful macros that manipulate page
 * table and page directory entries.
 */
pte_t *pgdir_walk(pde_t *pgdir, const void *va, int create)
{
		struct page_info *pp;
    pgdir = pgdir + PDX(va);
		if(*pgdir & PTE_P)
			goto release;
		if(!create)
			return NULL;
		if(create & CREATE_HUGE)
			*pgdir = PTE_PS;
		else{
			pp = page_alloc(ALLOC_PREMAPPED | ALLOC_ZERO);
			if(!pp)
				return NULL;
			++pp->pp_ref;
			*pgdir = page2pa(pp) | PTE_P | PTE_W;
		}
	release:
		return *pgdir & PTE_PS? (pte_t*)pgdir : KADDR(PTE_ADDR(*pgdir) + PTX(va) * sizeof(pte_t));
}

/*
 * Map [va, va+size) of virtual address space to physical [pa, pa+size)
 * in the page table rooted at pgdir.  Size is a multiple of PGSIZE.
 * Use permission bits perm|PTE_P for the entries.
 *
 * This function is only intended to set up the ``static'' mappings
 * above UTOP. As such, it should *not* change the pp_ref field on the
 * mapped pages.
 *
 * Hint: the TA solution uses pgdir_walk
 */
void boot_map_region(pde_t *pgdir, uintptr_t va, size_t size, physaddr_t pa, int perm)
{
		pte_t *pte;
    for(size_t item = 0; item < size; item += PGSIZE){
			pte = pgdir_walk(pgdir, (void*)(va + item), CREATE_NORMAL);
			if(!pte)
				return;

			pgdir[PDX(va + item)] |= perm;
			*pte = (pa + item) | PTE_P | perm;
		}
}

/*
 * Map the physical page 'pp' at virtual address 'va'.
 * The permissions (the low 12 bits) of the page table entry
 * should be set to 'perm|PTE_P'.
 *
 * Requirements
 *   - If there is already a page mapped at 'va', it should be page_remove()d.
 *   - If necessary, on demand, a page table should be allocated and inserted
 *     into 'pgdir'.
 *   - pp->pp_ref should be incremented if the insertion succeeds.
 *   - The TLB must be invalidated if a page was formerly present at 'va'.
 *
 * Corner-case hint: Make sure to consider what happens when the same
 * pp is re-inserted at the same virtual address in the same pgdir.
 * However, try not to distinguish this case in your code, as this
 * frequently leads to subtle bugs; there's an elegant way to handle
 * everything in one code path.
 *
 * RETURNS:
 *   0 on success
 *   -E_NO_MEM, if page table couldn't be allocated
 *
 * Hint: The TA solution is implemented using pgdir_walk, page_remove,
 * and page2pa.
 *
 * Also add support for huge page insertion.
 */
int page_insert(pde_t *pgdir, struct page_info *pp, void *va, int perm)
{
    pte_t *pte;
		int mismatch_pages;
		int page_size = perm & PTE_PS? CREATE_HUGE : CREATE_NORMAL;
		// PTE_PS only for pages with ALLOC_HUGE flag
		if((perm & PTE_PS) ^ (pp->pp_flags & ALLOC_HUGE? PTE_PS : 0x0))
			panic("Mismatched types of page and perm flags");
		pte = pgdir_walk(pgdir, va, 0);
		++(pp->pp_ref);
		if(pte && (*pte & PTE_P)){ // Page is already mapped
				mismatch_pages = (perm & PTE_PS) && !(*pte & PTE_PS)? NPTENTRIES : 1; // amount of pages to be removed
				for(int i = 0; i < mismatch_pages; ++i)
					page_remove(pgdir, va + i * PGSIZE);
		}

		pte = pgdir_walk(pgdir, va, page_size);
		if(!pte){
			--pp->pp_ref;
			return -E_NO_MEM;
		}
		*(pgdir + PDX(va)) |= perm;
		*pte = page2pa(pp) | PTE_P | perm;
    return 0;
}

/*
 * Return the page mapped at virtual address 'va'.
 * If pte_store is not zero, then we store in it the address
 * of the pte for this page.  This is used by page_remove and
 * can be used to verify page permissions for syscall arguments,
 * but should not be used by most callers.
 *
 * Return NULL if there is no page mapped at va.
 *
 * Hint: the TA solution uses pgdir_walk and pa2page.
 */
struct page_info *page_lookup(pde_t *pgdir, void *va, pte_t **pte_store)
{
    pte_t *pte = pgdir_walk(pgdir, va, 0);
		if(!pte || !(*pte & PTE_P))
			return NULL;
    if(pte_store)
			*pte_store = pte;
		return pa2page(PTE_ADDR(*pte));
}

/*
 * Unmaps the physical page at virtual address 'va'.
 * If there is no physical page at that address, silently does nothing.
 *
 * Details:
 *   - The ref count on the physical page should decrement.
 *   - The physical page should be freed if the refcount reaches 0.
 *   - The pg table entry corresponding to 'va' should be set to 0.
 *     (if such a PTE exists)
 *   - The TLB must be invalidated if you remove an entry from
 *     the page table.
 *
 * Hint: The TA solution is implemented using page_lookup,
 *  tlb_invalidate, and page_decref.
 */
void page_remove(pde_t *pgdir, void *va)
{
	 	struct page_info *pp;
		pte_t *pte = pgdir_walk(pgdir, va, 0);
	 	if(!pte || !(*pte & PTE_P))
	 		return;
		pp = page_lookup(pgdir, va, NULL);
		if(!pp)
			return;
		page_decref(pp);
		*pte = 0x0;
		tlb_invalidate(pgdir, va);
}

/*
 * Invalidate a TLB entry, but only if the page tables being
 * edited are the ones currently in use by the processor.
 */
void tlb_invalidate(pde_t *pgdir, void *va)
{
    /* Flush the entry only if we're modifying the current address space. */
    if (!curenv || curenv->env_pgdir == pgdir)
        invlpg(va);
}

/*
 * Reserve size bytes in the MMIO region and map [pa,pa+size) at this
 * location.  Return the base of the reserved region.  size does *not*
 * have to be multiple of PGSIZE.
 */
void *mmio_map_region(physaddr_t pa, size_t size)
{
    /*
     * Where to start the next region.  Initially, this is the
     * beginning of the MMIO region.  Because this is static, its
     * value will be preserved between calls to mmio_map_region
     * (just like nextfree in boot_alloc).
     */
    static void* mmio_base;
		void* result;
		size = ROUNDUP(size, PGSIZE);
		pa = ROUNDDOWN(pa, PGSIZE);

		if(!mmio_base)
			mmio_base = (void*)MMIOBASE;

		if(mmio_base + size > (void*)MMIOLIM)
			panic("Overflow MMIOLIM");
		result = (void*)mmio_base;
		mmio_base += size;
		boot_map_region(kern_pgdir, (unsigned int)result, size, pa, PTE_W | PTE_PCD | PTE_PWT | PTE_P);
		return result;
		/*
     * Reserve size bytes of virtual memory starting at base and map physical
     * pages [pa,pa+size) to virtual addresses [base,base+size).  Since this is
     * device memory and not regular DRAM, you'll have to tell the CPU that it
     * isn't safe to cache access to this memory.  Luckily, the page tables
     * provide bits for this purpose; simply create the mapping with
     * PTE_PCD|PTE_PWT (cache-disable and write-through) in addition to PTE_W.
     * (If you're interested in more details on this, see section 10.5 of IA32
     * volume 3A.)
     *
     * Be sure to round size up to a multiple of PGSIZE and to handle if this
     * reservation would overflow MMIOLIM (it's okay to simply panic if this
     * happens).
     *
     * Hint: The staff solution uses boot_map_region.
     *
     * LAB 5: Your code here:
     */
    //panic("mmio_map_region not implemented");
}

static uintptr_t user_mem_check_addr;

/*
 * Check that an environment is allowed to access the range of memory
 * [va, va+len) with permissions 'perm | PTE_P'.
 * Normally 'perm' will contain PTE_U at least, but this is not required.
 * 'va' and 'len' need not be page-aligned; you must test every page that
 * contains any of that range.  You will test either 'len/PGSIZE',
 * 'len/PGSIZE + 1', or 'len/PGSIZE + 2' pages.
 *
 * A user program can access a virtual address if (1) the address is below
 * ULIM, and (2) the page table gives it permission.  These are exactly
 * the tests you should implement here.
 *
 * If there is an error, set the 'user_mem_check_addr' variable to the first
 * erroneous virtual address.
 *
 * Returns 0 if the user program can access this range of addresses,
 * and -E_FAULT otherwise.
 */
int user_mem_check(struct env *env, const void *va, size_t len, int perm)
{
		struct page_info *pp;
		pte_t *pte;
		size_t page_number;
	  len = ROUNDUP(len, PGSIZE);
		page_number = len / PGSIZE;
		for(int page_i = 0; page_i <  page_number; ++page_i){
			pp = page_lookup(env->env_pgdir, (void*)va, &pte);
			if(!pp || ((*pte & perm) != perm)){
				user_mem_check_addr = (unsigned int)va;
				return -E_FAULT;
			}
			va = ROUNDDOWN((void*)va + PGSIZE, PGSIZE);
		}
    return 0;
}

/*
 * Checks that environment 'env' is allowed to access the range
 * of memory [va, va+len) with permissions 'perm | PTE_U | PTE_P'.
 * If it can, then the function simply returns.
 * If it cannot, 'env' is destroyed and, if env is the current
 * environment, this function will not return.
 */
void user_mem_assert(struct env *env, const void *va, size_t len, int perm)
{
    if (user_mem_check(env, va, len, perm | PTE_U) < 0) {
        cprintf("[%08x] user_mem_check assertion failure for "
            "va %08x\n", env->env_id, user_mem_check_addr);
        env_destroy(env);   /* may not return */
    }
}


/***************************************************************
 * Checking functions.
 ***************************************************************/

/*
 * Check that the pages on the page_free_list are reasonable.
 */
static void check_page_free_list(bool only_low_memory)
{
    struct page_info *pp;
    unsigned pdx_limit = only_low_memory ? 1 : NPDENTRIES;
    int nfree_basemem = 0, nfree_extmem = 0;
    char *first_free_page;

    if (!page_free_list)
        panic("'page_free_list' is a null pointer!");

    if (only_low_memory) {
        /* Move pages with lower addresses first in the free list, since
         * entry_pgdir does not map all pages. */
        struct page_info *pp1, *pp2;
        struct page_info **tp[2] = { &pp1, &pp2 };
        for (pp = page_free_list; pp; pp = pp->pp_link) {
            int pagetype = PDX(page2pa(pp)) >= pdx_limit;
            *tp[pagetype] = pp;
            tp[pagetype] = &pp->pp_link;
        }
        *tp[1] = 0;
        *tp[0] = pp2;
        page_free_list = pp1;
    }

    /* if there's a page that shouldn't be on the free list,
     * try to make sure it eventually causes trouble. */
    for (pp = page_free_list; pp; pp = pp->pp_link)
        if (PDX(page2pa(pp)) < pdx_limit)
            memset(page2kva(pp), 0x97, 128);

    first_free_page = (char *) boot_alloc(0);
    for (pp = page_free_list; pp; pp = pp->pp_link) {
        /* check that we didn't corrupt the free list itself */
        assert(pp >= pages);
        assert(pp < pages + npages);
        assert(((char *) pp - (char *) pages) % sizeof(*pp) == 0);

        /* check a few pages that shouldn't be on the free list */
        assert(page2pa(pp) != 0);
        assert(page2pa(pp) != IOPHYSMEM);
        assert(page2pa(pp) != EXTPHYSMEM - PGSIZE);
        assert(page2pa(pp) != EXTPHYSMEM);
        assert(page2pa(pp) < EXTPHYSMEM || (char *) page2kva(pp) >= first_free_page);

        if (page2pa(pp) < EXTPHYSMEM)
            ++nfree_basemem;
        else
            ++nfree_extmem;
    }

    assert(nfree_basemem > 0);
    assert(nfree_extmem > 0);
}

/*
 * Check the physical page allocator (page_alloc(), page_free(),
 * and page_init()).
 */
static void check_page_alloc(void)
{
    struct page_info *pp, *pp0, *pp1, *pp2;
    struct page_info *php0, *php1, *php2;
    int nfree, total_free;
    struct page_info *fl;
    char *c;
    int i;

    if (!pages)
        panic("'pages' is a null pointer!");

    /* check number of free pages */
    for (pp = page_free_list, nfree = 0; pp; pp = pp->pp_link)
        ++nfree;
    total_free = nfree;

    /* should be able to allocate three pages */
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));

    assert(pp0);
    assert(pp1 && pp1 != pp0);
    assert(pp2 && pp2 != pp1 && pp2 != pp0);
    assert(page2pa(pp0) < npages*PGSIZE);
    assert(page2pa(pp1) < npages*PGSIZE);
    assert(page2pa(pp2) < npages*PGSIZE);

    /* temporarily steal the rest of the free pages.
     *
     * Lab 1 Bonus:
     * For the bonus, if you go for a different design for the page allocator,
     * then do update here suitably to simulate a no-free-memory situation */
    fl = page_free_list;
    page_free_list = 0;

    /* should be no free memory */
    assert(!page_alloc(0));

    /* free and re-allocate? */
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));
    assert(pp0);
    assert(pp1 && pp1 != pp0);
    assert(pp2 && pp2 != pp1 && pp2 != pp0);
    assert(!page_alloc(0));

    /* test flags */
    memset(page2kva(pp0), 1, PGSIZE);
    page_free(pp0);
    assert((pp = page_alloc(ALLOC_ZERO)));
    assert(pp && pp0 == pp);
    c = page2kva(pp);
    for (i = 0; i < PGSIZE; i++)
        assert(c[i] == 0);

    /* give free list back */
    page_free_list = fl;

    /* free the pages we took */
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);

    /* number of free pages should be the same */
    for (pp = page_free_list; pp; pp = pp->pp_link)
        --nfree;
    assert(nfree == 0);

    cprintf("[4K] check_page_alloc() succeeded!\n");

    /* test allocation of huge page */
    pp0 = pp1 = php0 = 0;
    assert((pp0 = page_alloc(0)));
    assert((php0 = page_alloc(ALLOC_HUGE)));
    assert((pp1 = page_alloc(0)));
    assert(pp0);
    assert(php0 && php0 != pp0);
    assert(pp1 && pp1 != php0 && pp1 != pp0);
    assert(0 == (page2pa(php0) % (1024*PGSIZE)));
    if (page2pa(pp1) > page2pa(php0)) {
        assert(page2pa(pp1) - page2pa(php0) >= 1024*PGSIZE);
    }

    /* free and reallocate 2 huge pages */
    page_free(php0);
    page_free(pp0);
    page_free(pp1);
    php0 = php1 = pp0 = pp1 = 0;
    assert((php0 = page_alloc(ALLOC_HUGE)));
    assert((php1 = page_alloc(ALLOC_HUGE)));

    /* Is the inter-huge-page difference right? */
    if (page2pa(php1) > page2pa(php0)) {
        assert(page2pa(php1) - page2pa(php0) >= 1024*PGSIZE);
    } else {
        assert(page2pa(php0) - page2pa(php1) >= 1024*PGSIZE);
    }

    /* free the huge pages we took */
    page_free(php0);
    page_free(php1);

    /* number of free pages should be the same */
    nfree = total_free;
    for (pp = page_free_list; pp; pp = pp->pp_link)
        --nfree;
    assert(nfree == 0);

    cprintf("[4M] check_page_alloc() succeeded!\n");
}

/*
 * Checks that the kernel part of virtual address space
 * has been setup roughly correctly (by mem_init()).
 *
 * This function doesn't test every corner case,
 * but it is a pretty good sanity check.
 */
static void check_kern_pgdir(void)
{
    uint32_t i, n;
    pde_t *pgdir;

    pgdir = kern_pgdir;

    /* check pages array */
    n = ROUNDUP(npages*sizeof(struct page_info), PGSIZE);
    for (i = 0; i < n; i += PGSIZE)
        assert(check_va2pa(pgdir, UPAGES + i) == PADDR(pages) + i);

    /* check envs array (new test for lab 3) */
    n = ROUNDUP(NENV*sizeof(struct env), PGSIZE);
    for (i = 0; i < n; i += PGSIZE)
        assert(check_va2pa(pgdir, UENVS + i) == PADDR(envs) + i);

    /* check phys mem */
    for (i = 0; i < npages * PGSIZE; i += PGSIZE)
        assert(check_va2pa(pgdir, KERNBASE + i) == i);

    /* check kernel stack */
    /* (updated in LAB 6 to check per-CPU kernel stacks) */
    for (n = 0; n < NCPU; n++) {
        uint32_t base = KSTACKTOP - (KSTKSIZE + KSTKGAP) * (n + 1);
        for (i = 0; i < KSTKSIZE; i += PGSIZE)
            assert(check_va2pa(pgdir, base + KSTKGAP + i)
                == PADDR(percpu_kstacks[n]) + i);
        for (i = 0; i < KSTKGAP; i += PGSIZE)
            assert(check_va2pa(pgdir, base + i) == ~0);
    }

    /* check PDE permissions */
    for (i = 0; i < NPDENTRIES; i++) {
        switch (i) {
        case PDX(UVPT):
        case PDX(KSTACKTOP-1):
        case PDX(UPAGES):
        case PDX(UENVS):
        case PDX(MMIOBASE):
            assert(pgdir[i] & PTE_P);
            break;
        default:
            if (i >= PDX(KERNBASE)) {
                assert(pgdir[i] & PTE_P);
                assert(pgdir[i] & PTE_W);
            } else
                assert(pgdir[i] == 0);
            break;
        }
    }
    cprintf("check_kern_pgdir() succeeded!\n");
}

/*
 * This function returns the physical address of the page containing 'va',
 * defined by the page directory 'pgdir'.  The hardware normally performs
 * this functionality for us!  We define our own version to help check
 * the check_kern_pgdir() function; it shouldn't be used elsewhere.
 */
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va)
{
    pte_t *p;

    pgdir = &pgdir[PDX(va)];
    if (!(*pgdir & PTE_P))
        return ~0;
    p = (pte_t*) KADDR(PTE_ADDR(*pgdir));
    if (!(p[PTX(va)] & PTE_P))
        return ~0;
    return PTE_ADDR(p[PTX(va)]);
}


/* Check page_insert, page_remove, &c */
static void check_page(void)
{
    struct page_info *pp, *pp0, *pp1, *pp2;
    struct page_info *fl;
    pte_t *ptep, *ptep1;
    void *va;
    uintptr_t mm1, mm2;
    int i;
    extern pde_t entry_pgdir[];

    /* should be able to allocate three pages */
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(ALLOC_PREMAPPED)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));

    assert(pp0);
    assert(pp1 && pp1 != pp0);
    assert(pp2 && pp2 != pp1 && pp2 != pp0);

    /* temporarily steal the rest of the free pages.
     *
     * Lab 1 Bonus:
     * For the bonus, if you had chosen a different design for
     * the page allocator, then do update here suitably to
     * simulate a no-free-memory situation */
    fl = page_free_list;
    page_free_list = 0;

    /* should be no free memory */
    assert(!page_alloc(0));

    /* there is no page allocated at address 0 */
    assert(page_lookup(kern_pgdir, (void *) 0x0, &ptep) == NULL);

    /* there is no free memory, so we can't allocate a page table */
    assert(page_insert(kern_pgdir, pp1, 0x0, PTE_W) < 0);

    /* free pp0 and try again: pp0 should be used for page table */
    page_free(pp0);
    assert(page_insert(kern_pgdir, pp1, 0x0, PTE_W) == 0);
    assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
    assert(check_va2pa(kern_pgdir, 0x0) == page2pa(pp1));
    assert(pp1->pp_ref == 1);
    assert(pp0->pp_ref == 1);

    /* should be able to map pp2 at PGSIZE because pp0 is already allocated for page table */
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
    assert(pp2->pp_ref == 1);

    /* should be no free memory */
    assert(!page_alloc(0));

    /* should be able to map pp2 at PGSIZE because it's already there */
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
    assert(pp2->pp_ref == 1);

    /* pp2 should NOT be on the free list
     * could happen in ref counts are handled sloppily in page_insert */
    assert(!page_alloc(0));

    /* check that pgdir_walk returns a pointer to the pte */
    ptep = (pte_t *) KADDR(PTE_ADDR(kern_pgdir[PDX(PGSIZE)]));
    assert(pgdir_walk(kern_pgdir, (void*)PGSIZE, 0) == ptep+PTX(PGSIZE));

    /* should be able to change permissions too. */
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W|PTE_U) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
    assert(pp2->pp_ref == 1);
    assert(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_U);
    assert(kern_pgdir[0] & PTE_U);

    /* should be able to remap with fewer permissions */
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W) == 0);
    assert(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_W);
    assert(!(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_U));

    /* Should not be able to map at PTSIZE because need free page for page
     * table. */

    assert(page_insert(kern_pgdir, pp0, (void*) PTSIZE, PTE_W) < 0);

    /* insert pp1 at PGSIZE (replacing pp2) */
    assert(page_insert(kern_pgdir, pp1, (void*) PGSIZE, PTE_W) == 0);
    assert(!(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_U));

    /* should have pp1 at both 0 and PGSIZE, pp2 nowhere, ... */
    assert(check_va2pa(kern_pgdir, 0) == page2pa(pp1));
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp1));
    /* ... and ref counts should reflect this */
    assert(pp1->pp_ref == 2);
    assert(pp2->pp_ref == 0);

    /* pp2 should be returned by page_alloc */
    assert((pp = page_alloc(0)) && pp == pp2);

    /* unmapping pp1 at 0 should keep pp1 at PGSIZE */
    page_remove(kern_pgdir, 0x0);
    assert(check_va2pa(kern_pgdir, 0x0) == ~0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp1));
    assert(pp1->pp_ref == 1);
    assert(pp2->pp_ref == 0);

    /* test re-inserting pp1 at PGSIZE */
    assert(page_insert(kern_pgdir, pp1, (void*) PGSIZE, 0) == 0);
    assert(pp1->pp_ref);
    assert(pp1->pp_link == NULL);

    /* unmapping pp1 at PGSIZE should free it */
    page_remove(kern_pgdir, (void*) PGSIZE);
    assert(check_va2pa(kern_pgdir, 0x0) == ~0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == ~0);
    assert(pp1->pp_ref == 0);
    assert(pp2->pp_ref == 0);

    /* so it should be returned by page_alloc */
    assert((pp = page_alloc(0)) && pp == pp1);

    /* should be no free memory */
    assert(!page_alloc(0));

    /* forcibly take pp0 back */
    assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
    kern_pgdir[0] = 0;
    assert(pp0->pp_ref == 1);
    pp0->pp_ref = 0;

    /* check pointer arithmetic in pgdir_walk */
    page_free(pp0);
    va = (void*)(PGSIZE * NPDENTRIES + PGSIZE);
    ptep = pgdir_walk(kern_pgdir, va, 1);
    ptep1 = (pte_t *) KADDR(PTE_ADDR(kern_pgdir[PDX(va)]));
    assert(ptep == ptep1 + PTX(va));
    kern_pgdir[PDX(va)] = 0;
    pp0->pp_ref = 0;

    /* check that new page tables get cleared */
    memset(page2kva(pp0), 0xFF, PGSIZE);
    page_free(pp0);
    pgdir_walk(kern_pgdir, 0x0, 1);
    ptep = (pte_t *) page2kva(pp0);
    for(i=0; i<NPTENTRIES; i++)
        assert((ptep[i] & PTE_P) == 0);
    kern_pgdir[0] = 0;
    pp0->pp_ref = 0;

    /* give free list back */
    page_free_list = fl;

    /* free the pages we took */
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);

    /* test mmio_map_region */
    mm1 = (uintptr_t) mmio_map_region(0, 4097);
    mm2 = (uintptr_t) mmio_map_region(0, 4096);
    /* check that they're in the right region */
    assert(mm1 >= MMIOBASE && mm1 + 8096 < MMIOLIM);
    assert(mm2 >= MMIOBASE && mm2 + 8096 < MMIOLIM);
    /* check that they're page-aligned */
    assert(mm1 % PGSIZE == 0 && mm2 % PGSIZE == 0);
    /* check that they don't overlap */
    assert(mm1 + 8096 <= mm2);
    /* check page mappings */
    assert(check_va2pa(kern_pgdir, mm1) == 0);
    assert(check_va2pa(kern_pgdir, mm1+PGSIZE) == PGSIZE);
    assert(check_va2pa(kern_pgdir, mm2) == 0);
    assert(check_va2pa(kern_pgdir, mm2+PGSIZE) == ~0);
    /* check permissions */
    assert(*pgdir_walk(kern_pgdir, (void*) mm1, 0) & (PTE_W|PTE_PWT|PTE_PCD));
    assert(!(*pgdir_walk(kern_pgdir, (void*) mm1, 0) & PTE_U));
    /* clear the mappings */
    *pgdir_walk(kern_pgdir, (void*) mm1, 0) = 0;
    *pgdir_walk(kern_pgdir, (void*) mm1 + PGSIZE, 0) = 0;
    *pgdir_walk(kern_pgdir, (void*) mm2, 0) = 0;

    cprintf("check_page() succeeded!\n");
}

/* Check page_insert, page_remove, &c, with an installed kern_pgdir */
static void check_page_installed_pgdir(void)
{
    struct page_info *pp, *pp0, *pp1, *pp2;
    struct page_info *fl;
    pte_t *ptep, *ptep1;
    uintptr_t va;
    int i;

    /* check that we can read and write installed pages */
    pp1 = pp2 = 0;
    assert((pp0 = page_alloc(ALLOC_PREMAPPED)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));
    page_free(pp0);
    memset(page2kva(pp1), 1, PGSIZE);
    memset(page2kva(pp2), 2, PGSIZE);
    page_insert(kern_pgdir, pp1, (void*) PGSIZE, PTE_W);
    assert(pp1->pp_ref == 1);
    assert(*(uint32_t *)PGSIZE == 0x01010101U);
    page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_W);
    assert(*(uint32_t *)PGSIZE == 0x02020202U);
    assert(pp2->pp_ref == 1);
    assert(pp1->pp_ref == 0);
    *(uint32_t *)PGSIZE = 0x03030303U;
    assert(*(uint32_t *)page2kva(pp2) == 0x03030303U);
    page_remove(kern_pgdir, (void*) PGSIZE);
    assert(pp2->pp_ref == 0);

    /* forcibly take pp0 back */
    assert(PTE_ADDR(kern_pgdir[0]) == page2pa(pp0));
    kern_pgdir[0] = 0;
    assert(pp0->pp_ref == 1);
    pp0->pp_ref = 0;

    /* free the pages we took */
    page_free(pp0);

    cprintf("check_page_installed_pgdir() succeeded!\n");
}

/* Check pgdir_walk() for huge page support */
static void check_page_hugepages(void)
{
    struct page_info *php0;
    assert(php0 = page_alloc(ALLOC_HUGE));
    assert(page_insert(kern_pgdir, php0, (void *)(1024*PGSIZE), PTE_W | PTE_PS) == 0);
    assert(php0->pp_ref == 1);
    memset(page2kva(php0), 1, PGSIZE);
    assert(*(uint32_t *)(1024*PGSIZE) == 0x01010101U);

    /* Access the second 4K-page within the huge page */
    memset(page2kva(php0+1), 2, PGSIZE);
    assert(*(uint32_t *)(1025*PGSIZE) == 0x02020202U);

    /* Writing to the last 4K-page within the huge page works? */
    *(uint32_t *)(2*1024*PGSIZE - 42) = 0x42424242U;
    assert(*(uint32_t *)(2*1024*PGSIZE - 42) == 0x42424242U);

    /* Are the underlying frames consecutive? */
    memset(page2kva(php0+1021), 0x37, PGSIZE);
    memset(page2kva(php0+1022), 0x38, PGSIZE);
    assert(*(uint32_t *)((1024+1021)*PGSIZE) == 0x37373737U);
    assert(*(uint32_t *)((1024+1022)*PGSIZE) == 0x38383838U);

    /* Check pgdir_walk for the page and the PSE bit */
    pte_t *p_pte1, *p_pte2;
    p_pte1 = pgdir_walk(kern_pgdir, (void*)(1024*PGSIZE), 0);
    assert(NULL != p_pte1);
    assert(*p_pte1 & PTE_PS);
    p_pte2 = pgdir_walk(kern_pgdir, (void*)(1025*PGSIZE), 0);
    assert(NULL != p_pte2);
    assert(p_pte1 == p_pte2);

    /* check page_remove() on the huge page */
    page_remove(kern_pgdir, (void*) (1024*PGSIZE));
    assert(php0->pp_ref == 0);
    assert((php0+1022)->pp_ref == 0);

    /* check CREATE_HUGE flag */
    p_pte1 = pgdir_walk(kern_pgdir, (void*)(1024*PGSIZE), CREATE_HUGE);
    assert(NULL != p_pte1);
    assert(php0 = page_alloc(ALLOC_HUGE));
    assert(page_insert(kern_pgdir, php0, (void *)(2*1024*PGSIZE), PTE_W | PTE_PS) == 0);
    page_remove(kern_pgdir, (void*) (2*1024*PGSIZE));
    assert(php0->pp_ref == 0);

    cprintf("check_page_hugepages() succeeded!\n");
}
