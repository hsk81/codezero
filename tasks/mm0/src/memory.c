/*
 * Initialise the memory structures.
 *
 * Copyright (C) 2007, 2008 Bahadir Balban
 */
#include <init.h>
#include <memory.h>
#include <l4/macros.h>
#include <l4/config.h>
#include <l4/types.h>
#include <l4/api/errno.h>
#include <l4/generic/space.h>
#include <l4lib/arch/syslib.h>
#include INC_GLUE(memory.h)
#include INC_SUBARCH(mm.h)
#include <memory.h>
#include <file.h>
#include <user.h>


struct address_pool pager_vaddr_pool;

void *phys_to_virt(void *addr)
{
	return addr + INITTASK_OFFSET;
}

void *virt_to_phys(void *addr)
{
	return addr - INITTASK_OFFSET;
}

/* Maps a page from a vm_file to the pager's address space */
void *pager_map_page(struct vm_file *f, unsigned long page_offset)
{
	int err;
	struct page *p;

	if ((err = read_file_pages(f, page_offset, page_offset + 1)) < 0)
		return PTR_ERR(err);

	if ((p = find_page(&f->vm_obj, page_offset)))
		return (void *)l4_map_helper((void *)page_to_phys(p), 1);
	else
		return 0;
}

/* Unmaps a page's virtual address from the pager's address space */
void pager_unmap_page(void *addr)
{
	l4_unmap_helper(addr, 1);
}

int pager_address_pool_init(void)
{
	int err;

	/* Initialise the global shm virtual address pool */
	if ((err = address_pool_init(&pager_vaddr_pool,
				     (unsigned long)0xD0000000,
				     (unsigned long)0xE0000000)) < 0) {
		printf("Pager virtual address pool initialisation failed.\n");
		return err;
	}
	return 0;
}

void *pager_new_address(int npages)
{
	return address_new(&pager_vaddr_pool, npages);
}

int pager_delete_address(void *virt_addr, int npages)
{
	return address_del(&pager_vaddr_pool, virt_addr, npages);
}

/* Maps a page from a vm_file to the pager's address space */
void *pager_map_pages(struct vm_file *f, unsigned long page_offset, unsigned long npages)
{
	int err;
	struct page *p;
	void *addr_start, *addr;

	/* Get the pages */
	if ((err = read_file_pages(f, page_offset, page_offset + npages)) < 0)
		return PTR_ERR(err);

	/* Get the address range */
	if (!(addr_start = pager_new_address(npages)))
		return PTR_ERR(-ENOMEM);
	addr = addr_start;

	/* Map pages contiguously one by one */
	for (unsigned long pfn = page_offset; pfn < page_offset + npages; pfn++) {
		BUG_ON(!(p = find_page(&f->vm_obj, pfn)))
			l4_map((void *)page_to_phys(p), addr, 1, MAP_USR_RW_FLAGS, self_tid());
			addr += PAGE_SIZE;
	}

	return addr_start;
}

/* Unmaps a page's virtual address from the pager's address space */
void pager_unmap_pages(void *addr, unsigned long npages)
{
	/* Align to page if unaligned */
	if (!is_page_aligned(addr))
		addr = (void *)page_align(addr);

	/* Unmap so many pages */
	l4_unmap_helper(addr, npages);

	/* Free the address range */
	pager_delete_address(addr, npages);
}

/*
 * Maps multiple pages on a contiguous virtual address range,
 * returns pointer to byte offset in the file.
 */
void *pager_map_file_range(struct vm_file *f, unsigned long byte_offset,
			   unsigned long size)
{
	unsigned long mapsize = (byte_offset & PAGE_MASK) + size;

	void *page = pager_map_pages(f, __pfn(byte_offset), __pfn(page_align_up(mapsize)));

	return (void *)((unsigned long)page | (PAGE_MASK & byte_offset));
}

void *pager_validate_map_user_range2(struct tcb *user, void *userptr,
				    unsigned long size, unsigned int vm_flags)
{
	unsigned long start = page_align(userptr);
	unsigned long end = page_align_up(userptr + size);
	unsigned long npages = __pfn(end - start);
	void *virt, *virt_start;
	void *mapped = 0;

	/* Validate that user task owns this address range */
	if (pager_validate_user_range(user, userptr, size, vm_flags) < 0)
		return 0;

	/* Get the address range */
	if (!(virt_start = pager_new_address(npages)))
		return PTR_ERR(-ENOMEM);
	virt = virt_start;

	/* Map every page contiguously in the allocated virtual address range */
	for (unsigned long addr = start; addr < end; addr += PAGE_SIZE) {
		struct page *p = task_virt_to_page(user, addr);

		if (IS_ERR(p)) {
			/* Unmap pages mapped so far */
			l4_unmap_helper(virt_start, __pfn(addr - start));

			/* Delete virtual address range */
			pager_delete_address(virt_start, npages);

			return p;
		}

		l4_map((void *)page_to_phys(task_virt_to_page(user, addr)),
		       virt, 1, MAP_USR_RW_FLAGS, self_tid());
		virt += PAGE_SIZE;
	}

	/* Set the mapped pointer to offset of user pointer given */
	mapped = virt_start;
	mapped = (void *)(((unsigned long)mapped) |
			  ((unsigned long)(PAGE_MASK &
				  	   (unsigned long)userptr)));

	/* Return the mapped pointer */
	return mapped;
}


