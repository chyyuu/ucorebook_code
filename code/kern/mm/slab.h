#ifndef __KERN_MM_SLAB_H__
#define __KERN_MM_SLAB_H__

#include <types.h>

#define KMALLOC_MAX_ORDER       10

void slab_init(void);

void *kmalloc(size_t n);
void kfree(void *objp);

size_t slab_allocated(void);

#endif /* !__KERN_MM_SLAB_H__ */

