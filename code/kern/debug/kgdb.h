#ifndef __KERN_DEBUG_KGDB_H__
#define __KERN_DEBUG_KGDB_H__

#include <trap.h>

void kgdb_init(void);
void kgdb_debug(struct trapframe *tf);
void kgdb_intr(struct trapframe *tf);

#endif /* !__KERN_DEBUG_KGDB_H__ */

