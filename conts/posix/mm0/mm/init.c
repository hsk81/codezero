/*
 * Initialise the system.
 *
 * Copyright (C) 2007 - 2009 Bahadir Balban
 */
#include <l4lib/arch/syscalls.h>
#include <l4lib/arch/syslib.h>
#include <l4lib/utcb.h>
#include <l4lib/exregs.h>
#include <l4/lib/list.h>
#include <l4/generic/cap-types.h>	/* TODO: Move this to API */
#include <l4/api/capability.h>

#include <stdio.h>
#include <string.h>
#include <mm/alloc_page.h>
#include <lib/malloc.h>
#include <lib/bit.h>

#include <task.h>
#include <shm.h>
#include <file.h>
#include <init.h>
#include <test.h>
#include <utcb.h>
#include <bootm.h>
#include <vfs.h>
#include <init.h>
#include <memory.h>
#include <capability.h>
#include <linker.h>
#include <mmap.h>
#include <file.h>
#include <syscalls.h>
#include <linker.h>

/* Kernel data acquired during initialisation */
__initdata struct initdata initdata;


/* Physical memory descriptors */
struct memdesc physmem;		/* Initial, primitive memory descriptor */
struct membank membank[1];	/* The memory bank */
struct page *page_array;	/* The physical page array based on mem bank */


/* Capability descriptor list */
struct cap_list capability_list;

/* Memory region capabilities */
struct container_memory_regions cont_mem_regions;

__initdata static struct capability *caparray;
__initdata static int total_caps = 0;

void print_pfn_range(int pfn, int size)
{
	unsigned int addr = pfn << PAGE_BITS;
	unsigned int end = (pfn + size) << PAGE_BITS;
	printf("Used: 0x%x - 0x%x\n", addr, end);
}


/*
 * This sets up the mm0 task struct and memory environment but omits
 * bits that are already done such as creating a new thread, setting
 * registers.
 */
int pager_setup_task(void)
{
	struct tcb *task;
	struct task_ids ids;
	struct exregs_data exregs;
	void *mapped;
	int err;

	/*
	 * The thread itself is already known by the kernel,
	 * so we just allocate a local task structure.
	 */
	if (IS_ERR(task = tcb_alloc_init(TCB_NO_SHARING))) {
		printf("FATAL: "
		       "Could not allocate tcb for pager.\n");
		BUG();
	}

	/* Set up own ids */
	l4_getid(&ids);
	task->tid = ids.tid;
	task->spid = ids.spid;
	task->tgid = ids.tgid;

	/* Initialise vfs specific fields. */
	task->fs_data->rootdir = vfs_root.pivot;
	task->fs_data->curdir = vfs_root.pivot;

	/* Text markers */
	task->text_start = (unsigned long)__start_text;
	task->text_end = (unsigned long)__end_rodata;

	/* Data markers */
	task->stack_end = (unsigned long)__stack;
	task->stack_start = (unsigned long)__start_stack;

	/* Stack markers */
	task->data_start = (unsigned long)__start_data;
	task->data_end = (unsigned long)__end_data;

	/* BSS markers */
	task->bss_start = (unsigned long)__start_bss;
	task->bss_end = (unsigned long)__end_bss;

	/* Task's region available for mmap as  */
	task->map_start = PAGER_MMAP_START;
	task->map_end = PAGER_MMAP_END;

	/* Task's total map boundaries */
	task->start = __pfn_to_addr(cont_mem_regions.pager->start);
	task->end = __pfn_to_addr(cont_mem_regions.pager->end);

	/*
	 * Map all regions as anonymous (since no real
	 * file could back) All already-mapped areas
	 * are mapped at once.
	 */
	if (IS_ERR(mapped =
		   do_mmap(0, 0, task, task->start,
			   VMA_ANONYMOUS | VM_READ | VMA_FIXED |
			   VM_WRITE | VM_EXEC | VMA_PRIVATE,
			   __pfn(page_align_up(task->map_start) -
				 task->start)))) {
		printf("FATAL: do_mmap: failed with %d.\n", (int)mapped);
		BUG();
	}


	/* Set pager as child and parent of itself */
	list_insert(&task->child_ref, &task->children);
	task->parent = task;

	/* Allocate and set own utcb */
	task_setup_utcb(task);
	memset(&exregs, 0, sizeof(exregs));
	exregs_set_utcb(&exregs, task->utcb_address);
	if ((err = l4_exchange_registers(&exregs, task->tid)) < 0) {
		printf("FATAL: Pager could not set own utcb. "
		       "UTCB address: 0x%lx, error: %d\n",
		       task->utcb_address, err);
		BUG();
	}

	/* Pager must prefault its utcb */
	prefault_page(task, task->utcb_address,
		      VM_READ | VM_WRITE);

	/* Add the task to the global task list */
	global_add_task(task);

	return 0;
}

/* Copy all init-memory allocated capabilities */
void copy_boot_capabilities(int ncaps)
{
	struct capability *cap;

	link_init(&capability_list.caps);
	for (int i = 0; i < total_caps; i++) {
		cap = kzalloc(sizeof(struct capability));

		/* This copies kernel-allocated unique cap id as well */
		memcpy(cap, &caparray[i], sizeof(struct capability));

		/* Initialize capability list */
		link_init(&cap->list);

		/* Add capability to global cap list */
		list_insert(&cap->list, &capability_list.caps);
	}
	capability_list.ncaps = ncaps;
}

int read_pager_capabilities()
{
	int ncaps;
	int err;
	struct capability *cap;

	/* Read number of capabilities */
	if ((err = l4_capability_control(CAP_CONTROL_NCAPS,
					 0, &ncaps)) < 0) {
		printf("l4_capability_control() reading # of"
		       " capabilities failed.\n Could not "
		       "complete CAP_CONTROL_NCAPS request.\n");
		BUG();
	}
	total_caps = ncaps;

	/* Allocate array of caps from boot memory */
	caparray = alloc_bootmem(sizeof(struct capability) * ncaps, 0);

	/* Read all capabilities */
	if ((err = l4_capability_control(CAP_CONTROL_READ,
					 0, caparray)) < 0) {
		printf("l4_capability_control() reading of "
		       "capabilities failed.\n Could not "
		       "complete CAP_CONTROL_READ_CAPS request.\n");
		BUG();
	}

	/* Share all of them with paged children */
	if ((err = l4_capability_control(CAP_CONTROL_SHARE,
					 CAP_SHARE_CONTAINER,
					 0)) < 0) {
		printf("l4_capability_control() sharing of "
		       "capabilities failed.\n Could not "
		       "complete CAP_CONTROL_SHARE request.\n");
		BUG();
	}

	/* Copy them to real allocated structures */
	copy_boot_capabilities(ncaps);

	memset(&cont_mem_regions, 0, sizeof(cont_mem_regions));

	/* Set up pointers to important capabilities */
	list_foreach_struct(cap, &capability_list.caps, list) {
		/* Physical memory bank */
		if ((cap->type & CAP_RTYPE_MASK)
		    == CAP_RTYPE_PHYSMEM)
			cont_mem_regions.physmem = cap;

		/* Virtual regions */
		if ((cap->type & CAP_RTYPE_MASK)
		    == CAP_RTYPE_VIRTMEM) {

			/* Pager address region (get from linker-defined) */
			if (__pfn_to_addr(cap->start)
			    == (unsigned long)virtual_base)
				cont_mem_regions.pager = cap;

			/* UTCB address region */
			else if (UTCB_REGION_START ==
				 __pfn_to_addr(cap->start)) {
				if (UTCB_REGION_END !=
				    __pfn_to_addr(cap->end)) {
					printf("FATAL: Region designated "
					       "for UTCB allocation does not "
					       "match on start/end marks");
					BUG();
				}

				if (!(cap->access & CAP_MAP_UTCB_BIT)) {
					printf("FATAL: Region designated "
					       "for UTCB allocation does not "
					       "have UTCB map permissions");
					BUG();
				}
				cont_mem_regions.utcb = cap;
			}

			/* Shared memory disjoint region */
			else if (SHMEM_REGION_START ==
				 __pfn_to_addr(cap->start)) {
				if (SHMEM_REGION_END !=
				    __pfn_to_addr(cap->end)) {
					printf("FATAL: Region designated "
					       "for SHM allocation does not "
					       "match on start/end marks");
					BUG();
				}

				cont_mem_regions.shmem = cap;
			}

			/* Task memory region */
			else if (TASK_REGION_START ==
				 __pfn_to_addr(cap->start)) {
				if (TASK_REGION_END !=
				    __pfn_to_addr(cap->end)) {
					printf("FATAL: Region designated "
					       "for Task address space does"
					       "not match on start/end mark.");
					BUG();
				}
				cont_mem_regions.task = cap;
			}
		}
	}

	if (!cont_mem_regions.task ||
	    !cont_mem_regions.shmem ||
	    !cont_mem_regions.utcb ||
	    !cont_mem_regions.physmem ||
	    !cont_mem_regions.pager) {
		printf("%s: Error, pager does not have one of the required"
	 	       "mem capabilities defined. (TASK, SHM, PHYSMEM, UTCB)\n",
		       __TASKNAME__);
		printf("%p, %p, %p, %p, %p\n", cont_mem_regions.task,
		       cont_mem_regions.shmem, cont_mem_regions.utcb,
		       cont_mem_regions.physmem, cont_mem_regions.pager);
		BUG();
	}

	return 0;
}

/*
 * Copy all necessary data from initmem to real memory,
 * release initdata and any init memory used
 */
void release_initdata()
{
	/* Free and unmap init memory:
	 *
	 * FIXME: We can and do safely unmap the boot
	 * memory here, but because we don't utilize it yet,
	 * it remains as if it is a used block
	 */

	l4_unmap(__start_init,
		 __pfn(page_align_up(__end_init - __start_init)),
		 self_tid());
}

static void init_page_map(struct page_bitmap *pmap,
			  unsigned long pfn_start,
			  unsigned long pfn_end)
{
	pmap->pfn_start = pfn_start;
	pmap->pfn_end = pfn_end;
	set_page_map(pmap, pfn_start,
		     pfn_end - pfn_start, 0);
}

/*
 * Marks pages in the global page_map as used or unused.
 *
 * @start = start page address to set, inclusive.
 * @numpages = number of pages to set.
 */
int set_page_map(struct page_bitmap *page_map,
		 unsigned long pfn_start,
		 int numpages, int val)
{
	unsigned long pfn_end = pfn_start + numpages;
	unsigned long pfn_err = 0;

	if (page_map->pfn_start > pfn_start ||
	    page_map->pfn_end < pfn_start) {
		pfn_err = pfn_start;
		goto error;
	}
	if (page_map->pfn_end < pfn_end ||
	    page_map->pfn_start > pfn_end) {
		pfn_err = pfn_end;
		goto error;
	}

	/* Adjust bases so words get set from index 0 */
	pfn_start -= page_map->pfn_start;
	pfn_end -= page_map->pfn_start;

	if (val)
		for (int i = pfn_start; i < pfn_end; i++)
			page_map->map[BITWISE_GETWORD(i)] |= BITWISE_GETBIT(i);
	else
		for (int i = pfn_start; i < pfn_end; i++)
			page_map->map[BITWISE_GETWORD(i)] &= ~BITWISE_GETBIT(i);

	return 0;

error:
	BUG_MSG("Given page area is out of system page_map range: 0x%lx\n",
		pfn_err << PAGE_BITS);
	return -1;
}

/*
 * Allocates page descriptors and
 * initialises them using page_map information
 */
void init_physmem_secondary(struct membank *membank)
{
	struct page_bitmap *pmap = initdata.page_map;
	int npages = pmap->pfn_end - pmap->pfn_start;

	/*
	 * Allocation marks for the struct
	 * page array; npages, start, end
	 */
	int pg_npages, pg_spfn, pg_epfn;
	unsigned long ffree_addr;

	membank[0].start = __pfn_to_addr(pmap->pfn_start);
	membank[0].end = __pfn_to_addr(pmap->pfn_end);

	/* First find the first free page after last used page */
	for (int i = 0; i < npages; i++)
		if ((pmap->map[BITWISE_GETWORD(i)] & BITWISE_GETBIT(i)))
			membank[0].free = membank[0].start + (i + 1) * PAGE_SIZE;
	BUG_ON(membank[0].free >= membank[0].end);

	/*
	 * One struct page for every physical page.
	 * Calculate how many pages needed for page
	 * structs, start and end pfn marks.
	 */
	pg_npages = __pfn(page_align_up((sizeof(struct page) * npages)));


	/* These are relative pfn offsets
	 * to the start of the memory bank
	 *
	 * FIXME:
	 * 1.) These values were only right
	 *     when membank started from pfn 0.
	 *
	 * 2.) Use set_page_map to set page map
	 *     below instead of manually.
	 */
	pg_spfn = __pfn(membank[0].free);
	pg_epfn = pg_spfn + pg_npages;

	/*
	 * Use free pages from the bank as
	 * the space for struct page array
	 */
	membank[0].page_array =
		l4_map_helper((void *)membank[0].free,
			      pg_npages);

	/* Update free memory left */
	membank[0].free += pg_npages * PAGE_SIZE;

	/* Update page bitmap for the pages used for the page array */
+	set_page_map(pmap, pg_spfn, pg_epfn - pg_spfn, 1);

	/* Initialise the page array */
	for (int i = 0; i < npages; i++) {
		link_init(&membank[0].page_array[i].list);

		/*
		 * Set use counts for pages the
		 * kernel has already used up
		 */
		if (!(pmap->map[BITWISE_GETWORD(i)]
		      & BITWISE_GETBIT(i)))
			membank[0].page_array[i].refcnt = -1;
		else	/* Last page used +1 is free */
			ffree_addr = membank[0].start + (i + 1) * PAGE_SIZE;
	}

	/*
	 * First free address must
	 * come up the same for both
	 */
	BUG_ON(ffree_addr != membank[0].free);

	/* Set global page array to this bank's array */
	page_array = membank[0].page_array;

	/* Test that page/phys macros work */
	BUG_ON(phys_to_page(page_to_phys(&page_array[5]))
			    != &page_array[5])
}


/* Fills in the physmem structure with free physical memory information */
void init_physmem_primary()
{
	unsigned long pfn_start, pfn_end, pfn_images_end = 0;
	struct bootdesc *bootdesc = initdata.bootdesc;

	/* Allocate page map structure */
	initdata.page_map =
		alloc_bootmem(sizeof(struct page_bitmap) +
			      ((cont_mem_regions.physmem->end -
			        cont_mem_regions.physmem->start)
			       >> 5) + 1, 0);

	/* Initialise page map from physmem capability */
	init_page_map(initdata.page_map,
		      cont_mem_regions.physmem->start,
		      cont_mem_regions.physmem->end);

	/* Mark pager and other boot task areas as used */
	for (int i = 0; i < bootdesc->total_images; i++) {
		pfn_start =
			__pfn(page_align_up(bootdesc->images[i].phys_start));
		pfn_end =
			__pfn(page_align_up(bootdesc->images[i].phys_end));

		if (pfn_end > pfn_images_end)
			pfn_images_end = pfn_end;
		set_page_map(initdata.page_map, pfn_start,
			     pfn_end - pfn_start, 1);
	}

	physmem.start = cont_mem_regions.physmem->start;
	physmem.end = cont_mem_regions.physmem->end;

	physmem.free_cur = pfn_images_end;
	physmem.free_end = physmem.end;
	physmem.numpages = physmem.end - physmem.start;
}

void init_physmem(void)
{
	init_physmem_primary();

	init_physmem_secondary(membank);

	init_page_allocator(membank[0].free, membank[0].end);
}

/*
 * To be removed later: This file copies in-memory elf image to the
 * initialized and formatted in-memory memfs filesystem.
 */
void copy_init_process(void)
{
	int fd;
	struct svc_image *init_img;
	unsigned long img_size;
	void *init_img_start, *init_img_end;
	struct tcb *self = find_task(self_tid());
	void *mapped;
	int err;

	if ((fd = sys_open(self, "/test0", O_TRUNC |
			   O_RDWR | O_CREAT, 0)) < 0) {
		printf("FATAL: Could not open file "
		       "to write initial task.\n");
		BUG();
	}

	init_img = bootdesc_get_image_byname("test0");
	img_size = page_align_up(init_img->phys_end) -
				 page_align(init_img->phys_start);

	init_img_start = l4_map_helper((void *)init_img->phys_start,
				       __pfn(img_size));
	init_img_end = init_img_start + img_size;

	/*
	 * Map an anonymous region and prefault it.
	 * Its got to be from __end, because we haven't
	 * unmapped .init section yet (where map_start normally lies).
	 */
	if (IS_ERR(mapped =
		   do_mmap(0, 0, self, page_align_up(__end),
			   VMA_ANONYMOUS | VM_READ | VMA_FIXED |
			   VM_WRITE | VM_EXEC | VMA_PRIVATE,
			   __pfn(img_size)))) {
		printf("FATAL: do_mmap: failed with %d.\n",
		       (int)mapped);
		BUG();
	}

	 /* Prefault it */
	if ((err = prefault_range(self, (unsigned long)mapped,
				  img_size,
				  VM_READ | VM_WRITE)) < 0) {
		printf("FATAL: Prefaulting init image failed.\n");
		BUG();
	}

	/* Copy the raw image to anon region */
	memcpy(mapped, init_img_start, img_size);

	/* Write it to real file from anon region */
	sys_write(find_task(self_tid()), fd, mapped, img_size);

	/* Close file */
	sys_close(find_task(self_tid()), fd);

	/* Unmap anon region */
	do_munmap(self, (unsigned long)mapped, __pfn(img_size));

	/* Unmap raw virtual range for image memory */
	l4_unmap_helper(init_img_start,__pfn(img_size));

}

void start_init_process(void)
{
	/* Copy raw test0 elf image from memory to memfs first */
	copy_init_process();

	init_execve("/test0");
}

void init(void)
{
	read_pager_capabilities();

	pager_address_pool_init();

	read_boot_params();

	init_physmem();

	init_devzero();

	shm_pool_init();

	utcb_pool_init();

	vfs_init();

	pager_setup_task();

	start_init_process();

	release_initdata();

	mm0_test_global_vm_integrity();
}

