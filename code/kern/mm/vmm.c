#include <vmm.h>
#include <sync.h>
#include <slab.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <error.h>
#include <pmm.h>
#include <x86.h>

/* 
  vmm design include two parts: mm_struct (mm) & vma_struct (vma)
  mm is the memory manager for the set of continuous virtual memory  
  area which have the same PDT. vma is a continuous virtual memory area.
  There a linear link list for vma & a redblack link list for vma in mm.
---------------
  mm related functions:
   golbal functions
     struct mm_struct * mm_create(void)
     void mm_destroy(struct mm_struct *mm)
     int do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr)
--------------
  vma related functions:
   global functions
     struct vma_struct * vma_create (uintptr_t vm_start, uintptr_t vm_end,...)
     void insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma)
     struct vma_struct * find_vma(struct mm_struct *mm, uintptr_t addr)
   local functions
     inline void check_vma_overlap(struct vma_struct *prev, struct vma_struct *next)
     inline struct vma_struct * find_vma_rb(rb_tree *tree, uintptr_t addr) 
     inline void insert_vma_rb(rb_tree *tree, struct vma_struct *vma, ....
     inline int vma_compare(rb_node *node1, rb_node *node2)
---------------
   check correctness functions
     void check_vmm(void);
     void check_vma_struct(void);
     void check_pgfault(void);
*/

static void check_vmm(void);
static void check_vma_struct(void);
static void check_pgfault(void);

// mm_create -  alloc a mm_struct & initialize it.
struct mm_struct *
mm_create(void) {
    struct mm_struct *mm = kmalloc(sizeof(struct mm_struct));
    if (mm != NULL) {
        list_init(&(mm->mmap_list));
        mm->mmap_tree = NULL;
        mm->mmap_cache = NULL;
        mm->pgdir = NULL;
        mm->map_count = 0;
    }
    return mm;
}

// vma_create - alloc a vma_struct & initialize it. (addr range: vm_start~vm_end)
struct vma_struct *
vma_create(uintptr_t vm_start, uintptr_t vm_end, uint32_t vm_flags) {
    struct vma_struct *vma = kmalloc(sizeof(struct vma_struct));
    if (vma != NULL) {
        vma->vm_start = vm_start;
        vma->vm_end = vm_end;
        vma->vm_flags = vm_flags;
    }
    return vma;
}

// find_vma_rb - find a vma  (vma->vm_start <= addr <= vma_vm_end) in rb tree
static inline struct vma_struct *
find_vma_rb(rb_tree *tree, uintptr_t addr) {
    rb_node *node = rb_node_root(tree);
    struct vma_struct *vma = NULL, *tmp;
    while (node != NULL) {
        tmp = rbn2vma(node, rb_link);
        if (tmp->vm_end > addr) {
            vma = tmp;
            if (tmp->vm_start <= addr) {
                break;
            }
            node = rb_node_left(tree, node);
        }
        else {
            node = rb_node_right(tree, node);
        }
    }
    return vma;
}

// find_vma - find a vma  (vma->vm_start <= addr <= vma_vm_end)
struct vma_struct *
find_vma(struct mm_struct *mm, uintptr_t addr) {
    struct vma_struct *vma = NULL;
    if (mm != NULL) {
        vma = mm->mmap_cache;
        if (!(vma != NULL && vma->vm_start <= addr && vma->vm_end > addr)) {
            if (mm->mmap_tree != NULL) {
                vma = find_vma_rb(mm->mmap_tree, addr);
            }
            else {
                bool found = 0;
                list_entry_t *list = &(mm->mmap_list), *le = list;
                while ((le = list_next(le)) != list) {
                    vma = le2vma(le, list_link);
                    if (addr < vma->vm_end) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    vma = NULL;
                }
            }
        }
        if (vma != NULL) {
            mm->mmap_cache = vma;
        }
    }
    return vma;
}

// vma_compare - compare vma1->vm_start < vma2->vm_start ?
static inline int
vma_compare(rb_node *node1, rb_node *node2) {
    struct vma_struct *vma1 = rbn2vma(node1, rb_link);
    struct vma_struct *vma2 = rbn2vma(node2, rb_link);
    uintptr_t start1 = vma1->vm_start, start2 = vma2->vm_start;
    return (start1 < start2) ? -1 : (start1 > start2) ? 1 : 0;
}

// check_vma_overlap - check if vma1 overlaps vma2 ?
static inline void
check_vma_overlap(struct vma_struct *prev, struct vma_struct *next) {
    assert(prev->vm_start < prev->vm_end);
    assert(prev->vm_end <= next->vm_start);
    assert(next->vm_start < next->vm_end);
}

// insert_vma_rb - insert vma in rb tree according vma->start_addr
static inline void
insert_vma_rb(rb_tree *tree, struct vma_struct *vma, struct vma_struct **vma_prevp) {
    rb_node *node = &(vma->rb_link), *prev;
    rb_insert(tree, node);
    if (vma_prevp != NULL) {
        prev = rb_node_prev(tree, node);
        *vma_prevp = (prev != NULL) ? rbn2vma(prev, rb_link) : NULL;
    }
}

// insert_vma_struct -insert vma in mm's rb tree link & list link
void
insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma) {
    assert(vma->vm_start < vma->vm_end);
    list_entry_t *list = &(mm->mmap_list);
    list_entry_t *le_prev = list, *le_next;
    if (mm->mmap_tree != NULL) {
        struct vma_struct *mmap_prev;
        insert_vma_rb(mm->mmap_tree, vma, &mmap_prev);
        if (mmap_prev != NULL) {
            le_prev = &(mmap_prev->list_link);
        }
    }
    else {
        list_entry_t *le = list;
        while ((le = list_next(le)) != list) {
            struct vma_struct *mmap_prev = le2vma(le, list_link);
            if (mmap_prev->vm_start > vma->vm_start) {
                break;
            }
            le_prev = le;
        }
    }

    le_next = list_next(le_prev);

    /* check overlap */
    if (le_prev != list) {
        check_vma_overlap(le2vma(le_prev, list_link), vma);
    }
    if (le_next != list) {
        check_vma_overlap(vma, le2vma(le_next, list_link));
    }

    vma->vm_mm = mm;
    list_add_after(le_prev, &(vma->list_link));

    mm->map_count ++;
    if (mm->mmap_tree == NULL && mm->map_count >= RB_MIN_MAP_COUNT) {

        /* try to build red-black tree now, but may fail. */
        mm->mmap_tree = rb_tree_create(vma_compare);

        if (mm->mmap_tree != NULL) {
            list_entry_t *list = &(mm->mmap_list), *le = list;
            while ((le = list_next(le)) != list) {
                insert_vma_rb(mm->mmap_tree, le2vma(le, list_link), NULL);
            }
        }
    }
}

// mm_destroy - free mm and mm internal fields
void
mm_destroy(struct mm_struct *mm) {
    if (mm->mmap_tree != NULL) {
        rb_tree_destroy(mm->mmap_tree);
    }
    list_entry_t *list = &(mm->mmap_list), *le;
    while ((le = list_next(list)) != list) {
        list_del(le);
        kfree(le2vma(le, list_link));
    }
    kfree(mm);
}

// vmm_init - initialize virtual memory management
//          - now just call check_vmm to check correctness of vmm
void
vmm_init(void) {
    check_vmm();
}

// check_vmm - check correctness of vmm
static void
check_vmm(void) {
    size_t nr_free_pages_store = nr_free_pages();
    size_t slab_allocated_store = slab_allocated();

    check_vma_struct();
    check_pgfault();

    assert(nr_free_pages_store == nr_free_pages());
    assert(slab_allocated_store == slab_allocated());

    cprintf("check_vmm() succeeded.\n");
}

static void
check_vma_struct(void) {
    size_t nr_free_pages_store = nr_free_pages();
    size_t slab_allocated_store = slab_allocated();

    struct mm_struct *mm = mm_create();
    assert(mm != NULL);

    int step1 = RB_MIN_MAP_COUNT * 2, step2 = step1 * 10;

    int i;
    for (i = step1; i >= 0; i --) {
        struct vma_struct *vma = vma_create(i * 5, i * 5 + 2, 0);
        assert(vma != NULL);
        insert_vma_struct(mm, vma);
    }

    for (i = step1 + 1; i <= step2; i ++) {
        struct vma_struct *vma = vma_create(i * 5, i * 5 + 2, 0);
        assert(vma != NULL);
        insert_vma_struct(mm, vma);
    }

    list_entry_t *le = list_next(&(mm->mmap_list));

    for (i = 0; i <= step2; i ++) {
        assert(le != &(mm->mmap_list));
        struct vma_struct *mmap = le2vma(le, list_link);
        assert(mmap->vm_start == i * 5 && mmap->vm_end == i * 5 + 2);
        le = list_next(le);
    }

    for (i = 0; i < 5 * step2 + 2; i ++) {
        struct vma_struct *vma = find_vma(mm, i);
        assert(vma != NULL);
        int j = i / 5;
        if (i >= 5 * j + 2) {
            j ++;
        }
        assert(vma->vm_start == j * 5 && vma->vm_end == j * 5 + 2);
    }

    mm_destroy(mm);

    assert(nr_free_pages_store == nr_free_pages());
    assert(slab_allocated_store == slab_allocated());

    cprintf("check_vma_struct() succeeded!\n");
}

struct mm_struct *check_mm_struct;

// check_pgfault - check correctness of pgfault handler
static void
check_pgfault(void) {
    size_t nr_free_pages_store = nr_free_pages();
    size_t slab_allocated_store = slab_allocated();

    check_mm_struct = mm_create();
    assert(check_mm_struct != NULL);

    struct mm_struct *mm = check_mm_struct;
    pde_t *pgdir = mm->pgdir = boot_pgdir;
    assert(pgdir[0] == 0);

    struct vma_struct *vma = vma_create(0, PTSIZE, VM_WRITE);
    assert(vma != NULL);

    insert_vma_struct(mm, vma);

    uintptr_t addr = 0x100;
    assert(find_vma(mm, addr) == vma);

    int i, sum = 0;
    for (i = 0; i < 100; i ++) {
        *(char *)(addr + i) = i;
        sum += i;
    }
    for (i = 0; i < 100; i ++) {
        sum -= *(char *)(addr + i);
    }
    assert(sum == 0);

    page_remove(pgdir, ROUNDDOWN(addr, PGSIZE));
    free_page(pa2page(pgdir[0]));
    pgdir[0] = 0;

    mm->pgdir = NULL;
    mm_destroy(mm);
    check_mm_struct = NULL;

    assert(nr_free_pages_store == nr_free_pages());
    assert(slab_allocated_store == slab_allocated());

    cprintf("check_pgfault() succeeded!\n");
}

// do_pgfault - interrupt handler to process the page fault execption
int
do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr) {
    int ret = -E_INVAL;
    struct vma_struct *vma = find_vma(mm, addr);
    if (vma == NULL || vma->vm_start > addr) {
        goto failed;
    }

    switch (error_code & 3) {
    default:
            /* default is 3: write, present */
    case 2: /* write, not present */
        if (!(vma->vm_flags & VM_WRITE)) {
            goto failed;
        }
        break;
    case 1: /* read, present */
        goto failed;
    case 0: /* read, not present */
        if (!(vma->vm_flags & (VM_READ | VM_EXEC))) {
            goto failed;
        }
    }

    uint32_t perm = PTE_U;
    if (vma->vm_flags & VM_WRITE) {
        perm |= PTE_W;
    }
    addr = ROUNDDOWN(addr, PGSIZE);

    ret = -E_NO_MEM;

    if (pgdir_alloc_page(mm->pgdir, addr, perm) == 0) {
        goto failed;
    }
    ret = 0;

failed:
    return ret;
}

