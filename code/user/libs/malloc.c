#include <types.h>
#include <ulib.h>
#include <syscall.h>
#include <malloc.h>

union header {
    struct {
        union header *ptr;
        size_t size;
    } s;
    uint32_t align[16];
};

typedef union header header_t;

static header_t base;
static header_t *freep = NULL;

static bool
morecore(size_t nu) {
    static_assert(sizeof(header_t) == 0x40);
    static uintptr_t brk = 0;
    if (brk == 0) {
        if (sys_brk(&brk) != 0 || brk == 0) {
            return 0;
        }
    }
    uintptr_t newbrk = brk + nu * sizeof(header_t);
    if (sys_brk(&newbrk) != 0 || newbrk <= brk) {
        return 0;
    }
    header_t *p = (void *)brk;
    p->s.size = (newbrk - brk) / sizeof(header_t);
    free((void *)(p + 1));
    brk = newbrk;
    return 1;
}

void *
malloc(size_t size) {
    header_t *p, *prevp;
    size_t nunits;

    nunits = (size + sizeof(header_t) - 1) / sizeof(header_t) + 1;
    if ((prevp = freep) == NULL) {
        base.s.ptr = freep = prevp = &base;
        base.s.size = 0;
    }

    for (p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {
        if (p->s.size >= nunits) {
            if (p->s.size == nunits) {
                prevp->s.ptr = p->s.ptr;
            }
            else {
                header_t *np = prevp->s.ptr = (p + nunits);
                np->s.ptr = p->s.ptr;
                np->s.size = p->s.size - nunits;
                p->s.size = nunits;
            }
            freep = prevp;
            return (void *)(p + 1);
        }
        if (p == freep) {
            if (!morecore(nunits)) {
                return NULL;
            }
        }
    }
}

void
free(void *ap) {
    header_t *bp = ((header_t *)ap) - 1, *p;

    for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr) {
        if (p >= p->s.ptr && (bp > p || bp < p->s.ptr)) {
            break;
        }
    }

    if (bp + bp->s.size != p->s.ptr) {
        bp->s.ptr = p->s.ptr;
    }
    else {
        bp->s.size += p->s.ptr->s.size;
        bp->s.ptr = p->s.ptr->s.ptr;
    }

    if (p + p->s.size != bp) {
        p->s.ptr = bp;
    }
    else {
        p->s.size += bp->s.size;
        p->s.ptr = bp->s.ptr;
    }

    freep = p;
}

