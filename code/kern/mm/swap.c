#include <pmm.h>
#include <vmm.h>
#include <swap.h>
#include <swapfs.h>
#include <slab.h>
#include <assert.h>
#include <stdio.h>
#include <vmm.h>
#include <x86.h>
#include <error.h>
#include <atomic.h>
#include <sync.h>
#include <string.h>
#include <stdlib.h>

/* ------------- swap in/out & page replacement mechanism design&implementation -------------
Hardware Requrirement:
  There should be PRESENT/ACCESSED/DIRTY bit at least in an PTE (Page Table Entry)
    PRESENT bit:   1 :This page is valid (have a corresponding page frame); 0: This page is invalid
    ACCESSED bit:  1 :This page has been accessed (set by CPU's MMU hardware); 0: could be reseted by ucore (software)  
    DIRTY bit:     1 :This page has been modified (set by CPU's MMU hardware); 0: could be reseted by ucore (software) 

  If CPU access a invalid page(PTE is invalid),the page fault exception (intr num: 14) shoud be raised.
-----------------------------------------
Software design:
  First, we design mm & vma struct to build a set of continuous virtual memory area (vma), mm struct is 
used to manage the the vma set which has the same PDT. Each vma struct descripts a  continuous virtual memory area.
--------------------------
Software Design Rule:
 0. ucore should build a PDT, and initialize a mm & a vma for this PDT at least.
 1. if an virtual address ADDR is in one vma's memory range, then this ADDR is a legal ADDR
 2. if an virtual address ADDR is legal, but hasn't a corresponding page frame, then ucore should allocate a
    page frame, and setup a corresponding PDT's pde & PT's pte to map the ADDR & page frame's PHYSICAL ADDR.
 3. if there are no enough free page frames to allocate, then ucore will use a page repalcement algorithm to swap out 
    some page frames which haven't accessed in the past(in reality) or future(in theory) to disk and free these page frames.

Page Replacement algorithm:
----------------------------
(an simplified Linux page replacement algorithm)
  ucore implements a simple modified version of LRU, called the two-list strategy. Instead of maintaining one list, 
the LRU list, ucore keeps two lists: the active list and the inactive list. Pages on the active list are considered “hot” 
and are not available to evict. Pages on the inactive list are available to evict. Pages are placed on the active list 
when ucore need more free pages or when they are accessed while already residing on the inactive list. Both lists are 
maintained in a pseudo-LRU manner: Items are added to the tail and removed from the head, as with a queue. If there are no 
enough free page to alloc, the page items from the active list are moved back to the inactive list, making them available 
to evict. The two-list strategy solves the only-used-once failure in a classic LRU and also enables simpler, pseudo-LRU 
semantics to perform well.This two-list approach is also known as LRU/2; it can be generalized to n-lists, called LRU/n. 

Implementation:
----------------------------
  If an page fault exception is raised, then trap(trap.c)-->pgfault_handler(trap.c) --> do_pgfault (vmm.c) 
to process this exception:  
   1 do_pagefault will find(or alloc) a pte PTE for the legal virtual address ADDR;
   2 if the PTE is invalid && empty, it means ucore should call pgdir_alloc_page(vmm.c) to alloc a page frame PF for this ADDR, and 
     setup PTE to map the ADDR with the corresponding physical addr in this PF.
   3 if the PTE is invalid && not empty, it means the content of corresponding page frame for this ADDR is in the swap space(disk),
     ucore should call swap_in_page to load the contents from the swap space to a free page frame PF, and call page_insert to setup 
     PTE to map the ADDR with the corresponding physical addr in this PF.
  
  If there are no free page frame, then ucore will find&replace some used page frame to swap out to swap space. The key function of 
swap implementation is in kswapd_main(swap.c::proj11::lab3), and the steps are shown below:
  0 ucore first builds two page list (active_list & inactive_list) for active page(hot accessed) frames and inactive page frames
    (not accessed in currently past). ucore wants to evict inactive page frames to produce more free page frames.
  1 try_free_pages(swap.c) will calculate pressure(swap.c) to estimate the number(pressure<<5) of needed page frames in ucore currently, 
     then call kswapd kernel thread.
  2 kswapd kernel thread (wake up by try_free_pages OR timer(sched.[ch]::proj10.4::lab3)) will call kswapd_main to evict N=pressure<<5 
    page frames.
    2.1 call swap_out_mm to try to evict N page frames in inactive page list from each process's mm struct.
    2.2 call page_launder & refill_inactive_scan to try to change some active page frames to inactive page frames and
    swap out some inactive swap page frame to swap space(disk).
*/

// the max offset of swap entry
size_t max_swap_offset;

// the list of pages for swap
typedef struct {
    list_entry_t swap_list;
    size_t nr_pages;
} swap_list_t;

// there are two swap list: active_list & inactive_list
// active page list. Items in active_list may move to inactive_list.
static swap_list_t active_list;
// inactive page list, will be evicted if ucore need more free page frames
static swap_list_t inactive_list;

#define nr_active_pages                 (active_list.nr_pages)
#define nr_inactive_pages               (inactive_list.nr_pages)

// the array element is used to record the offset of swap entry
// the value of array element is the reference number of swap out page
// page->ref+mem_map[offset]= the total reference number of a page(in mem OR swap space)
// the index of array element is the offset of swap space(disk)
static unsigned short *mem_map;

#define SWAP_UNUSED                     0xFFFF
#define MAX_SWAP_REF                    0xFFFE

static volatile bool swap_init_ok = 0;

#define HASH_SHIFT                      10
#define HASH_LIST_SIZE                  (1 << HASH_SHIFT)
#define entry_hashfn(x)                 (hash32(x, HASH_SHIFT))

// the hash list used to find swap page according to swap entry quickly.
static list_entry_t hash_list[HASH_LIST_SIZE];

static void check_swap(void);

static lock_t swap_in_lock;

// swap_list_init - initialize the swap list
static void
swap_list_init(swap_list_t *list) {
    list_init(&(list->swap_list));
    list->nr_pages = 0;
}

// swap_active_list_add - add the page to active_list
static inline void
swap_active_list_add(struct Page *page) {
    assert(PageSwap(page));
    SetPageActive(page);
    swap_list_t *list = &active_list;
    list->nr_pages ++;
    list_add_before(&(list->swap_list), &(page->swap_link));
}

// swap_inactive_list_add - add the page to inactive_list
static inline void
swap_inactive_list_add(struct Page *page) {
    assert(PageSwap(page));
    ClearPageActive(page);
    swap_list_t *list = &inactive_list;
    list->nr_pages ++;
    list_add_before(&(list->swap_list), &(page->swap_link));
}

// swap_list_del - delete page from the swap list
static inline void
swap_list_del(struct Page *page) {
    assert(PageSwap(page));
    (PageActive(page) ? &active_list : &inactive_list)->nr_pages --;
    list_del(&(page->swap_link));
}

// swap_init - init swap fs, two swap lists, alloc memory & init for swap_entry record array mem_map
//           - init the hash list.
void
swap_init(void) {
    swapfs_init();
    swap_list_init(&active_list);
    swap_list_init(&inactive_list);

    if (!(1024 <= max_swap_offset && max_swap_offset < MAX_SWAP_OFFSET_LIMIT)) {
        panic("bad max_swap_offset %08x.\n", max_swap_offset);
    }

    mem_map = kmalloc(sizeof(short) * max_swap_offset);
    assert(mem_map != NULL);

    size_t offset;
    for (offset = 0; offset < max_swap_offset; offset ++) {
        mem_map[offset] = SWAP_UNUSED;
    }

    int i;
    for (i = 0; i < HASH_LIST_SIZE; i ++) {
        list_init(hash_list + i);
    }

    lock_init(&swap_in_lock);

    check_swap();
}

// try_free_pages - calculate pressure to estimate the number(pressure<<5) of needed page frames in ucore currently, 
//                - then call kswapd kernel thread.
bool
try_free_pages(size_t n) {
    if (!swap_init_ok) {
        return 0;
    }
    /* in next lab, swap_init_ok will be set by a kernel thread, named kswapd.
     * but in this lab, this function do no nothing */
    panic("not implemented yet.!\n");
}

static swap_entry_t try_alloc_swap_entry(void);

// swap_page_add - set PG_swap flag in page, set page->index = entry, and add page to hash_list.
//               - if entry==0, It means ???
static bool
swap_page_add(struct Page *page, swap_entry_t entry) {
    assert(!PageSwap(page));
    if (entry == 0) {
        if ((entry = try_alloc_swap_entry()) == 0) {
            return 0;
        }
        assert(mem_map[swap_offset(entry)] == SWAP_UNUSED);
        mem_map[swap_offset(entry)] = 0;
        SetPageDirty(page);
    }
    SetPageSwap(page);
    page->index = entry;
    list_add(hash_list + entry_hashfn(entry), &(page->page_link));
    return 1;
}

// swap_page_del - clear PG_swap flag in page, and del page from hash_list.
static void
swap_page_del(struct Page *page) {
    assert(PageSwap(page));
    ClearPageSwap(page);
    list_del(&(page->page_link));
}

// swap_free_page - call swap_page_del&free_page to generate a free page
static void
swap_free_page(struct Page *page) {
    assert(PageSwap(page) && page_ref(page) == 0);
    swap_page_del(page);
    free_page(page);
}

// swap_hash_find - find page according entry using swap hash list
static struct Page *
swap_hash_find(swap_entry_t entry) {
    list_entry_t *list = hash_list + entry_hashfn(entry), *le= list;
    while ((le = list_next(le)) != list) {
        struct Page *page = le2page(le, page_link);
        if (page->index == entry) {
            return page;
        }
    }
    return NULL;
}

// try_alloc_swap_entry - try to alloc a unused swap entry
static swap_entry_t
try_alloc_swap_entry(void) {
    static size_t next = 1;
    size_t empty = 0, zero = 0, end = next;
    do {
        switch (mem_map[next]) {
        case SWAP_UNUSED:
            empty = next;
            break;
        case 0:
            if (zero == 0) {
                zero = next;
            }
            break;
        }
        if (++ next == max_swap_offset) {
            next = 1;
        }
    } while (empty == 0 && next != end);

    swap_entry_t entry = 0;
    if (empty != 0) {
        entry = (empty << 8);
    }
    else if (zero != 0) {
        entry = (zero << 8);
        struct Page *page = swap_hash_find(entry);
        assert(page != NULL && PageSwap(page));
        swap_list_del(page);
        if (page_ref(page) == 0) {
            swap_free_page(page);
        }
        else {
            swap_page_del(page);
        }
        mem_map[zero] = SWAP_UNUSED;
    }

    static unsigned int failed_counter = 0;
    if (entry == 0 && ((++ failed_counter) % 0x1000) == 0) {
        warn("swap: try_alloc_swap_entry: failed too many times.\n");
    }
    return entry;
}

// swap_remove_entry - call swap_list_del to remove page from swap hash list,
//                   - and call swap_free_page to generate a free page 
void
swap_remove_entry(swap_entry_t entry) {
    size_t offset = swap_offset(entry);
    assert(mem_map[offset] > 0);
    if (-- mem_map[offset] == 0) {
        struct Page *page = swap_hash_find(entry);
        if (page != NULL) {
            if (page_ref(page) != 0) {
                return ;
            }
            swap_list_del(page);
            swap_free_page(page);
        }
        mem_map[offset] = SWAP_UNUSED;
    }
}

// swap_page_count - get reference number of swap page frame
int
swap_page_count(struct Page *page) {
    if (!PageSwap(page)) {
        return 0;
    }
    size_t offset = swap_offset(page->index);
    assert(mem_map[offset] >= 0);
    return mem_map[offset];
}

// swap_duplicate - reference number of mem_map[offset] ++
void
swap_duplicate(swap_entry_t entry) {
    size_t offset = swap_offset(entry);
    assert(mem_map[offset] >= 0 && mem_map[offset] < MAX_SWAP_REF);
    mem_map[offset] ++;
}

// swap_in_page - swap in a content of a page frame from swap space to memory
//              - set the PG_swap flag in this page and add this page to swap active list
int
swap_in_page(swap_entry_t entry, struct Page **pagep) {
    if (pagep == NULL) {
        return -E_INVAL;
    }
    size_t offset = swap_offset(entry);
    assert(mem_map[offset] >= 0);

    int ret;
    struct Page *page, *newpage;
    if ((page = swap_hash_find(entry)) != NULL) {
        goto found;
    }

    newpage = alloc_page();

    lock(&(swap_in_lock));
    if ((page = swap_hash_find(entry)) != NULL) {
        if (newpage != NULL) {
            free_page(newpage);
        }
        goto found_unlock;
    }
    if (newpage == NULL) {
        ret = -E_NO_MEM;
        goto failed_unlock;
    }
    page = newpage;
    if (swapfs_read(entry, page) != 0) {
        free_page(page);
        ret = -E_SWAP_FAULT;
        goto failed_unlock;
    }
    swap_page_add(page, entry);
    swap_active_list_add(page);

found_unlock:
    unlock(&swap_in_lock);
found:
    *pagep = page;
    return 0;

failed_unlock:
    unlock(&swap_in_lock);
    return ret;
}

// swap_copy_entry - copy a content of swap out page frame to a new page
//                 - set this new page PG_swap flag and add to swap active list
int
swap_copy_entry(swap_entry_t entry, swap_entry_t *store) {
    if (store == NULL) {
        return -E_INVAL;
    }

    int ret = -E_NO_MEM;
    struct Page *page, *newpage;
    swap_duplicate(entry);
    if ((newpage = alloc_page()) == NULL) {
        goto failed;
    }
    if ((ret = swap_in_page(entry, &page)) != 0) {
        goto failed_free_page;
    }
    ret = -E_NO_MEM;
    if (!swap_page_add(newpage, 0)) {
        goto failed_free_page;
    }
    swap_active_list_add(newpage);
    memcpy(page2kva(newpage), page2kva(page), PGSIZE);
    *store = newpage->index;
    ret = 0;
out:
    swap_remove_entry(entry);
    return ret;

failed_free_page:
    free_page(newpage);
failed:
    goto out;
}

// try_free_swap_entry - if mem_map[offset] == 0 (no reference), then mem_map[offset] = SWAP_UNUSED
static bool
try_free_swap_entry(swap_entry_t entry) {
    size_t offset = swap_offset(entry);
    if (mem_map[offset] == 0) {
        mem_map[offset] = SWAP_UNUSED;
        return 1;
    }
    return 0;
}

// page_launder - try to move page to swap_active_list OR swap_inactive_list, 
//              - and call swap_fs_write to swap out pages in swap_inactive_list
static int
page_launder(void) {
    size_t maxscan = nr_inactive_pages, free_count = 0;
    list_entry_t *list = &(inactive_list.swap_list), *le = list_next(list);
    while (maxscan -- > 0 && le != list) {
        struct Page *page = le2page(le, swap_link);
        le = list_next(le);
        if (!(PageSwap(page) && !PageActive(page))) {
            panic("inactive: wrong swap list.\n");
        }
        swap_list_del(page);
        if (page_ref(page) != 0) {
            swap_active_list_add(page);
            continue ;
        }
        swap_entry_t entry = page->index;
        if (!try_free_swap_entry(entry)) {
            if (PageDirty(page)) {
                ClearPageDirty(page);
                swap_duplicate(entry);
                if (swapfs_write(entry, page) != 0) {
                    SetPageDirty(page);
                }
                mem_map[swap_offset(entry)] --;
                if (page_ref(page) != 0) {
                    swap_active_list_add(page);
                    continue ;
                }
                if (PageDirty(page)) {
                    swap_inactive_list_add(page);
                    continue ;
                }
                try_free_swap_entry(entry);
            }
        }
        free_count ++;
        swap_free_page(page);
    }
    return free_count;
}

// refill_inactive_scan - try to move page in swap_active_list into swap_inactive_list
static void
refill_inactive_scan(void) {
    size_t maxscan = nr_active_pages;
    list_entry_t *list = &(active_list.swap_list), *le = list_next(list);
    while (maxscan -- > 0 && le != list) {
        struct Page *page = le2page(le, swap_link);
        le = list_next(le);
        if (!(PageSwap(page) && PageActive(page))) {
            panic("active: wrong swap list.\n");
        }
        if (page_ref(page) == 0) {
            swap_list_del(page);
            swap_inactive_list_add(page);
        }
    }
}

// swap_out_vma - try unmap pte & move pages into swap active list.
static int
swap_out_vma(struct mm_struct *mm, struct vma_struct *vma, uintptr_t addr, size_t require) {
    if (require == 0 || !(addr >= vma->vm_start && addr < vma->vm_end)) {
        return 0;
    }
    uintptr_t end;
    size_t free_count = 0;
    addr = ROUNDDOWN(addr, PGSIZE), end = ROUNDUP(vma->vm_end, PGSIZE);
    while (addr < end && require != 0) {
        pte_t *ptep = get_pte(mm->pgdir, addr, 0);
        if (ptep == NULL) {
            addr = ROUNDDOWN(addr + PTSIZE, PTSIZE);
            continue ;
        }
        if (*ptep & PTE_P) {
            struct Page *page = pte2page(*ptep);
            assert(!PageReserved(page));
            if (*ptep & PTE_A) {
                *ptep &= ~PTE_A;
                tlb_invalidate(mm->pgdir, addr);
                goto try_next_entry;
            }
            if (!PageSwap(page)) {
                if (!swap_page_add(page, 0)) {
                    goto try_next_entry;
                }
                swap_active_list_add(page);
            }
            else if (*ptep & PTE_D) {
                SetPageDirty(page);
            }
            swap_entry_t entry = page->index;
            swap_duplicate(entry);
            page_ref_dec(page);
            *ptep = entry;
            tlb_invalidate(mm->pgdir, addr);
            mm->swap_address = addr + PGSIZE;
            free_count ++, require --;
        }
    try_next_entry:
        addr += PGSIZE;
    }
    return free_count;
}

// swap_out_mm - call swap_out_vma to try to unmap a set of vma ('require' NUM pages).
static int
swap_out_mm(struct mm_struct *mm, size_t require) {
    assert(mm != NULL);
    if (require == 0 || mm->map_count == 0) {
        return 0;
    }
    assert(!list_empty(&(mm->mmap_list)));

    uintptr_t addr = mm->swap_address;
    struct vma_struct *vma;

    if ((vma = find_vma(mm, addr)) == NULL) {
        addr = mm->swap_address = 0;
        vma = le2vma(list_next(&(mm->mmap_list)), list_link);
    }
    assert(vma != NULL && addr <= vma->vm_end);

    if (addr < vma->vm_start) {
        addr = vma->vm_start;
    }

    int i;
    size_t free_count = 0;
    for (i = 0; i <= mm->map_count; i ++) {
        int ret = swap_out_vma(mm, vma, addr, require);
        free_count += ret, require -= ret;
        if (require == 0) {
            break;
        }
        list_entry_t *le = list_next(&(vma->list_link));
        if (le == &(mm->mmap_list)) {
            le = list_next(le);
        }
        vma = le2vma(le, list_link);
        addr = vma->vm_start;
    }
    return free_count;
}

// check_swap - check the correctness of swap & page replacement algorithm
static void
check_swap(void) {
    size_t nr_free_pages_store = nr_free_pages();
    size_t slab_allocated_store = slab_allocated();

    size_t offset;
    for (offset = 2; offset < max_swap_offset; offset ++) {
        mem_map[offset] = 1;
    }

    struct mm_struct *mm = mm_create();
    assert(mm != NULL);

    extern struct mm_struct *check_mm_struct;
    assert(check_mm_struct == NULL);

    check_mm_struct = mm;

    pde_t *pgdir = mm->pgdir = boot_pgdir;
    assert(pgdir[0] == 0);

    struct vma_struct *vma = vma_create(0, PTSIZE, VM_WRITE | VM_READ);
    assert(vma != NULL);

    insert_vma_struct(mm, vma);

    struct Page *rp0 = alloc_page(), *rp1 = alloc_page();
    assert(rp0 != NULL && rp1 != NULL);

    uint32_t perm = PTE_U | PTE_W;
    int ret = page_insert(pgdir, rp1, 0, perm);
    assert(ret == 0 && page_ref(rp1) == 1);

    page_ref_inc(rp1);
    ret = page_insert(pgdir, rp0, 0, perm);
    assert(ret == 0 && page_ref(rp1) == 1 && page_ref(rp0) == 1);

    // check try_alloc_swap_entry

    swap_entry_t entry = try_alloc_swap_entry();
    assert(swap_offset(entry) == 1);
    mem_map[1] = 1;
    assert(try_alloc_swap_entry() == 0);

    // set rp1, Swap, Active, add to hash_list, active_list

    swap_page_add(rp1, entry);
    swap_active_list_add(rp1);
    assert(PageSwap(rp1));

    mem_map[1] = 0;
    entry = try_alloc_swap_entry();
    assert(swap_offset(entry) == 1);
    assert(!PageSwap(rp1));

    // check swap_remove_entry

    assert(swap_hash_find(entry) == NULL);
    mem_map[1] = 2;
    swap_remove_entry(entry);
    assert(mem_map[1] == 1);

    swap_page_add(rp1, entry);
    swap_inactive_list_add(rp1);
    swap_remove_entry(entry);
    assert(PageSwap(rp1));
    assert(rp1->index == entry && mem_map[1] == 0);

    // check page_launder, move page from inactive_list to active_list

    assert(page_ref(rp1) == 1);
    assert(nr_active_pages == 0 && nr_inactive_pages == 1);
    assert(list_next(&(inactive_list.swap_list)) == &(rp1->swap_link));

    page_launder();
    assert(nr_active_pages == 1 && nr_inactive_pages == 0);
    assert(PageSwap(rp1) && PageActive(rp1));

    entry = try_alloc_swap_entry();
    assert(swap_offset(entry) == 1);
    assert(!PageSwap(rp1) && nr_active_pages == 0);
    assert(list_empty(&(active_list.swap_list)));

    // set rp1 inactive again

    assert(page_ref(rp1) == 1);
    swap_page_add(rp1, 0);
    assert(PageSwap(rp1) && swap_offset(rp1->index) == 1);
    swap_inactive_list_add(rp1);
    mem_map[1] = 1;
    assert(nr_inactive_pages == 1);
    page_ref_dec(rp1);

    size_t count = nr_free_pages();
    swap_remove_entry(entry);
    assert(nr_inactive_pages == 0 && nr_free_pages() == count + 1);

    // check swap_out_mm

    pte_t *ptep0 = get_pte(pgdir, 0, 0), *ptep1;
    assert(ptep0 != NULL && pte2page(*ptep0) == rp0);

    ret = swap_out_mm(mm, 0);
    assert(ret == 0);

    ret = swap_out_mm(mm, 10);
    assert(ret == 1 && mm->swap_address == PGSIZE);

    ret = swap_out_mm(mm, 10);
    assert(ret == 0 && *ptep0 == entry && mem_map[1] == 1);
    assert(PageDirty(rp0) && PageActive(rp0) && page_ref(rp0) == 0);
    assert(nr_active_pages == 1 && list_next(&(active_list.swap_list)) == &(rp0->swap_link));

    // check refill_inactive_scan()

    refill_inactive_scan();
    assert(!PageActive(rp0) && page_ref(rp0) == 0);
    assert(nr_inactive_pages == 1 && list_next(&(inactive_list.swap_list)) == &(rp0->swap_link));

    page_ref_inc(rp0);
    page_launder();
    assert(PageActive(rp0) && page_ref(rp0) == 1);
    assert(nr_active_pages == 1 && list_next(&(active_list.swap_list)) == &(rp0->swap_link));

    page_ref_dec(rp0);
    refill_inactive_scan();
    assert(!PageActive(rp0));

    // save data in rp0

    int i;
    for (i = 0; i < PGSIZE; i ++) {
        ((char *)page2kva(rp0))[i] = (char)i;
    }

    page_launder();
    assert(nr_inactive_pages == 0 && list_empty(&(inactive_list.swap_list)));
    assert(mem_map[1] == 1);

    rp1 = alloc_page();
    assert(rp1 != NULL);
    ret = swapfs_read(entry, rp1);
    assert(ret == 0);

    for (i = 0; i < PGSIZE; i ++) {
        assert(((char *)page2kva(rp1))[i] == (char)i);
    }

    // page fault now

    *(char *)0 = 0xEF;

    rp0 = pte2page(*ptep0);
    assert(page_ref(rp0) == 1);
    assert(PageSwap(rp0) && PageActive(rp0));

    entry = try_alloc_swap_entry();
    assert(swap_offset(entry) == 1 && mem_map[1] == SWAP_UNUSED);
    assert(!PageSwap(rp0) && nr_active_pages == 0 && nr_inactive_pages == 0);

    // clear accessed flag

    assert(rp0 == pte2page(*ptep0));
    assert(!PageSwap(rp0));

    ret = swap_out_mm(mm, 10);
    assert(ret == 0);
    assert(!PageSwap(rp0) && (*ptep0 & PTE_P));

    // change page table

    ret = swap_out_mm(mm, 10);
    assert(ret == 1);
    assert(*ptep0 == entry && page_ref(rp0) == 0 && mem_map[1] == 1);

    count = nr_free_pages();
    refill_inactive_scan();
    page_launder();
    assert(count + 1 == nr_free_pages());

    ret = swapfs_read(entry, rp1);
    assert(ret == 0 && *(char *)(page2kva(rp1)) == (char)0xEF);
    free_page(rp1);

    // duplictate *ptep0

    assert(mem_map[1] == 1);
    ptep1 = get_pte(pgdir, PGSIZE, 0);
    assert(ptep1 != NULL && *ptep1 == 0);
    swap_duplicate(*ptep0);
    *ptep1 = *ptep0;

    // page fault again

    *(char *)0 = 0xFF;
    *(char *)(PGSIZE + 1) = 0x88;
    assert(pte2page(*ptep0) == pte2page(*ptep1));
    rp0 = pte2page(*ptep0);
    assert(*(char *)1 == (char)0x88 && *(char *)PGSIZE == (char)0xFF);

    assert(page_ref(rp0) == 2 && rp0->index == entry && mem_map[1] == 0);

    assert(PageSwap(rp0) && PageActive(rp0));
    entry = try_alloc_swap_entry();
    assert(swap_offset(entry) == 1 && mem_map[1] == SWAP_UNUSED);
    assert(!PageSwap(rp0));
    assert(list_empty(&(active_list.swap_list)));
    assert(list_empty(&(inactive_list.swap_list)));

    // check swap_out_mm

    *(char *)0 = *(char *)PGSIZE = 0xEE;
    mm->swap_address = PGSIZE * 2;
    ret = swap_out_mm(mm, 2);
    assert(ret == 0);
    assert((*ptep0 & PTE_P) && !(*ptep0 & PTE_A));
    assert((*ptep1 & PTE_P) && !(*ptep1 & PTE_A));

    ret = swap_out_mm(mm, 2);
    assert(ret == 2);
    assert(mem_map[1] == 2 && page_ref(rp0) == 0);

    refill_inactive_scan();
    page_launder();
    assert(mem_map[1] == 2 && swap_hash_find(entry) == NULL);

    // check copy entry

    swap_remove_entry(entry);
    *ptep1 = 0;
    assert(mem_map[1] == 1);

    swap_entry_t store;
    ret = swap_copy_entry(entry, &store);
    assert(ret == -E_NO_MEM);
    mem_map[2] = SWAP_UNUSED;

    ret = swap_copy_entry(entry, &store);
    assert(ret == 0 && swap_offset(store) == 2 && mem_map[2] == 0);
    mem_map[2] = 1;
    *ptep1 = store;

    assert(*(char *)PGSIZE == (char)0xEE && *(char *)(PGSIZE + 1)== (char)0x88);

    *(char *)PGSIZE = 1, *(char *)(PGSIZE + 1) = 2;
    assert(*(char *)0 == (char)0xEE && *(char *)1 == (char)0x88);

    ret = swap_in_page(entry, &rp0);
    assert(ret == 0);
    ret = swap_in_page(store, &rp1);
    assert(ret == 0);
    assert(rp1 != rp0);

    // free memory

    swap_list_del(rp0), swap_list_del(rp1);
    swap_page_del(rp0), swap_page_del(rp1);

    assert(page_ref(rp0) == 1 && page_ref(rp1) == 1);
    assert(nr_active_pages == 0 && list_empty(&(active_list.swap_list)));
    assert(nr_inactive_pages == 0 && list_empty(&(inactive_list.swap_list)));

    for (i = 0; i < HASH_LIST_SIZE; i ++) {
        assert(list_empty(hash_list + i));
    }

    page_remove(pgdir, 0);
    page_remove(pgdir, PGSIZE);

    free_page(pa2page(pgdir[0]));
    pgdir[0] = 0;

    mm->pgdir = NULL;
    mm_destroy(mm);
    check_mm_struct = NULL;

    assert(nr_active_pages == 0 && nr_inactive_pages == 0);
    for (offset = 0; offset < max_swap_offset; offset ++) {
        mem_map[offset] = SWAP_UNUSED;
    }

    assert(nr_free_pages_store == nr_free_pages());
    assert(slab_allocated_store == slab_allocated());

    cprintf("check_swap() succeeded.\n");
}

